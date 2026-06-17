/*
 * This file is part of the DSView project.
 * Copyright (C) 2024
 *
 * UART VCD driver — TCP-only, protocol v3, 24MHz samplerate.
 */

#define _GNU_SOURCE
#include "uart_vcd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netdb.h>
#include <assert.h>
#include "../../log.h"

#undef LOG_PREFIX
#define LOG_PREFIX "uart_vcd: "

#define UART_VCD_READ_BUF_SIZE          65536
#define UART_VCD_EVENT_LIMIT            65536
#define UART_VCD_DELTA_CLAMP            0x00ffffffu
#define UART_VCD_MAX_EVENT_SIZE         264
#define UART_VCD_SYNC_FRAME_SIZE        12
#define UART_VCD_HEADER_SYNC            0xA0
#define UART_VCD_ERROR_DUMP_SIZE        32
#define UART_VCD_TEXT_BUF_SIZE          1200

SR_PRIV struct sr_dev_driver uart_vcd_driver_info;
static struct sr_dev_driver *di = &uart_vcd_driver_info;

static void flush_event_batch(struct uart_vcd_context *ctx,
                              const struct sr_dev_inst *sdi);

static void init_default_labels(struct uart_vcd_context *ctx)
{
    int channel;
    for (channel = 0; channel < 8; channel++) {
        snprintf(ctx->labels[channel], sizeof(ctx->labels[channel]),
                 "RX%d:", channel);
        ctx->labels_received[channel] = FALSE;
    }
}

static void record_state(struct uart_vcd_context *ctx,
                         const struct sr_dev_inst *sdi)
{
    struct sr_logic_sparse_event *last;
    ctx->activity_mask |= ctx->recorded_state ^ ctx->output_state;
    ctx->recorded_state = ctx->output_state;

    if (ctx->event_count) {
        last = &ctx->event_buf[ctx->event_count - 1];
        if (last->sample == ctx->collected_samples) {
            last->state = ctx->output_state;
            return;
        }
        if (last->state == ctx->output_state)
            return;
    }

    if (ctx->event_count >= UART_VCD_EVENT_BATCH_SIZE - 1)
        flush_event_batch(ctx, sdi);

    ctx->event_buf[ctx->event_count].sample = ctx->collected_samples;
    ctx->event_buf[ctx->event_count].state = ctx->output_state;
    ctx->event_buf[ctx->event_count].reserved = 0;
    ctx->event_count++;
}

static void flush_event_batch(struct uart_vcd_context *ctx,
                              const struct sr_dev_inst *sdi)
{
    struct sr_datafeed_packet pkt;
    struct sr_datafeed_logic log;
    struct sr_logic_sparse_event *last;

    if (!ctx->event_count ||
        ctx->event_buf[ctx->event_count - 1].sample != ctx->collected_samples) {
        last = &ctx->event_buf[ctx->event_count++];
        last->sample = ctx->collected_samples;
        last->state = ctx->output_state;
        last->reserved = 0;
    }

    pkt.type = SR_DF_LOGIC;
    pkt.status = SR_PKT_OK;
    pkt.payload = &log;
    log.format = LA_SPARSE_EVENTS;
    log.index = 0;
    log.order = 0;
    log.length = (uint64_t)ctx->event_count * sizeof(*ctx->event_buf);
    log.unitsize = sizeof(*ctx->event_buf);
    log.data_error = 0;
    log.error_pattern = 0;
    log.data = ctx->event_buf;
    ds_data_forward(sdi, &pkt);
    ctx->event_count = 0;
}

static void advance_time(struct uart_vcd_context *ctx, uint64_t target)
{
    ctx->collected_samples = target;
}

static void send_event_end(struct uart_vcd_context *ctx, const struct sr_dev_inst *sdi)
{
    if (ctx->end_sent)
        return;

    if (ctx->parsed_events || ctx->activity_mask) {
        sr_info("final parsed=%llu, sample=%llu, activity=0x%08x, "
                "bad_packets=%llu, recovered=%llu, dropped_input=%llu",
                (unsigned long long)ctx->parsed_events,
                (unsigned long long)ctx->collected_samples,
                ctx->activity_mask,
                (unsigned long long)ctx->bad_packets,
                (unsigned long long)ctx->recovered_packets,
                (unsigned long long)ctx->dropped_input_bytes);
    }

    flush_event_batch(ctx, sdi);
    struct sr_datafeed_packet pkt;
    pkt.type=SR_DF_END; pkt.status=SR_PKT_OK;
    ds_data_forward(sdi, &pkt);
    ctx->end_sent=TRUE;
    ctx->collecting=FALSE;
}

static void protocol_error(struct uart_vcd_context *ctx, const char *reason,
                           const uint8_t *data, uint64_t len)
{
    char dump[UART_VCD_ERROR_DUMP_SIZE * 3 + 1];
    uint64_t count = len;
    uint64_t i;

    if (count > UART_VCD_ERROR_DUMP_SIZE)
        count = UART_VCD_ERROR_DUMP_SIZE;
    for (i = 0; i < count; i++)
        snprintf(dump + i * 3, sizeof(dump) - i * 3, "%02X ", data[i]);
    dump[count * 3] = '\0';

    ctx->bad_packets++;
    ctx->dropped_input_bytes += len;
    sr_err("bad MCU packet: %s; dropped=%llu; data=%s%s",
           reason, (unsigned long long)len, dump,
           len > count ? "..." : "");
}

static int ev3_frame_size(const struct uart_vcd_context *ctx,
                          const uint8_t *p, int len)
{
    uint32_t delta_raw;
    uint8_t header;
    int payload_onwire;
    int payload_padded;
    int frame_size;
    int i;

    if (len < 4)
        return 0;

    delta_raw = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                ((uint32_t)p[2] << 16);
    if (!ctx->first_event && delta_raw > UART_VCD_DELTA_CLAMP)
        return -1;

    header = p[3];
    if (!(header & 0x80)) {
        const uint8_t sub = (header >> 5) & 3;
        const uint8_t channel = header & 0x1F;
        if (sub > 1 || channel > 23)
            return -1;
        return 4;
    }

    if (header == UART_VCD_HEADER_SYNC) {
        uint32_t state, inv_state;
        if (len < UART_VCD_SYNC_FRAME_SIZE)
            return 0;
        state = (uint32_t)p[4] | ((uint32_t)p[5] << 8) |
                ((uint32_t)p[6] << 16);
        inv_state = (uint32_t)p[7] | ((uint32_t)p[8] << 8) |
                    ((uint32_t)p[9] << 16);
        if (((state ^ inv_state) & 0x00ffffffu) != 0x00ffffffu)
            return -1;
        if (p[10] != 0x55 || p[11] != 0xAA)
            return -1;
        return UART_VCD_SYNC_FRAME_SIZE;
    }
    if (header != 0x80 && header != 0xC0 && header != 0xE0)
        return -1;

    if (len < 6)
        return 0;
    if (p[4] > 7)
        return -1;
    if (header == 0x80 && p[5] > UART_VCD_MAX_LABEL_LEN - 1)
        return -1;

    payload_onwire = 2 + p[5];
    payload_padded = (payload_onwire + 3) & ~3;
    frame_size = 4 + payload_padded;
    if (frame_size > UART_VCD_MAX_EVENT_SIZE)
        return -1;
    if (len < frame_size)
        return 0;

    for (i = 4 + payload_onwire; i < frame_size; i++) {
        if (p[i] != 0)
            return -1;
    }
    return frame_size;
}

static void append_escaped_ascii(char *dst, size_t dst_size, size_t *pos,
                                 uint8_t value)
{
    if (*pos >= dst_size)
        return;

    if ((value >= 0x20 && value <= 0x7e) ||
        value == '\r' || value == '\n' || value == '\t') {
        if (*pos + 1 < dst_size)
            dst[(*pos)++] = (char)value;
        return;
    }

    if (*pos + 4 < dst_size) {
        static const char hexc[] = "0123456789ABCDEF";
        dst[(*pos)++] = '\\';
        dst[(*pos)++] = 'x';
        dst[(*pos)++] = hexc[(value >> 4) & 0x0F];
        dst[(*pos)++] = hexc[value & 0x0F];
    }
}

static void emit_text_annotation(struct uart_vcd_context *ctx,
                                 const struct sr_dev_inst *sdi,
                                 int channel, int render_hex,
                                 const uint8_t *data, int data_len)
{
    struct sr_datafeed_packet pkt;
    struct sr_datafeed_uart_vcd_text text;
    char out[UART_VCD_TEXT_BUF_SIZE];
    size_t pos = 0;
    int i;

    if (channel < 0 || channel > 7)
        return;

    for (i = 0; ctx->labels[channel][i] != '\0' &&
                pos + 1 < sizeof(out); i++)
        out[pos++] = ctx->labels[channel][i];

    if (render_hex) {
        static const char hexc[] = "0123456789ABCDEF";
        for (i = 0; i < data_len && pos + 2 < sizeof(out); i++) {
            out[pos++] = hexc[(data[i] >> 4) & 0x0F];
            out[pos++] = hexc[data[i] & 0x0F];
        }
    } else {
        for (i = 0; i < data_len && pos + 1 < sizeof(out); i++)
            append_escaped_ascii(out, sizeof(out), &pos, data[i]);
    }
    out[pos < sizeof(out) ? pos : sizeof(out) - 1] = '\0';

    text.start_sample = ctx->collected_samples;
    text.end_sample = ctx->collected_samples + 1;
    text.channel = (uint8_t)channel;
    memset(text.reserved, 0, sizeof(text.reserved));
    text.text = out;

    pkt.type = SR_DF_UART_VCD_TEXT;
    pkt.status = SR_PKT_OK;
    pkt.payload = &text;
    pkt.bExportOriginalData = 0;
    ds_data_forward(sdi, &pkt);
}

static int ev3_blow_buf(struct uart_vcd_context *ctx,
                        const struct sr_dev_inst *sdi,
                        const uint8_t *p, int len)
{
    const int frame_size = ev3_frame_size(ctx, p, len);
    uint32_t delta_raw;
    uint64_t delta_samples;
    uint8_t header;

    if (frame_size <= 0)
        return frame_size;

    delta_raw = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                ((uint32_t)p[2] << 16);
    delta_samples = delta_raw;
    if (ctx->first_event) {
        ctx->first_event = FALSE;
        delta_samples = 1;
    } else if (delta_samples > ctx->total_samples) {
        delta_samples = 1;
    }

    header = p[3];
    if (!(header & 0x80)) {
        const uint8_t sub = (header >> 5) & 3;
        const int channel = header & 0x1F;
        uint64_t target = ctx->collected_samples + delta_samples;
        if (!ctx->is_loop && target > ctx->total_samples)
            target = ctx->total_samples;
        advance_time(ctx, target);
        if (channel <= 23) {
            const uint32_t mask = 1u << channel;
            if (sub == 0)
                ctx->gpio_state &= ~mask;
            else
                ctx->gpio_state |= mask;
            ctx->output_state =
                (ctx->output_state & 0xff000000u) | ctx->gpio_state;
            record_state(ctx, sdi);
        }
    } else if (header == UART_VCD_HEADER_SYNC) {
        const uint32_t state = (uint32_t)p[4] |
            ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16);
        uint64_t target = ctx->collected_samples + delta_samples;
        if (!ctx->is_loop && target > ctx->total_samples)
            target = ctx->total_samples;
        advance_time(ctx, target);
        ctx->gpio_state = state & 0x00ffffffu;
        ctx->output_state =
            (ctx->output_state & 0xff000000u) | ctx->gpio_state;
        record_state(ctx, sdi);
    } else if (header == 0x80) {
        const int channel = p[4];
        const int label_len = p[5];
        int copy_len = label_len;
        if (copy_len > UART_VCD_MAX_LABEL_LEN - 1)
            copy_len = UART_VCD_MAX_LABEL_LEN - 1;
        memcpy(ctx->labels[channel], p + 6, (size_t)copy_len);
        ctx->labels[channel][copy_len] = '\0';
        ctx->labels_received[channel] = TRUE;
    } else {
        const int channel = p[4];
        const int data_len = p[5];
        uint64_t target = ctx->collected_samples + delta_samples;
        if (!ctx->is_loop && target > ctx->total_samples)
            target = ctx->total_samples;
        advance_time(ctx, target);
        emit_text_annotation(ctx, sdi, channel, header == 0xC0,
                             p + 6, data_len);
    }

    ctx->parsed_events++;
    if (ctx->collected_samples - ctx->activity_report_sample >=
        ctx->samplerate / 4) {
        sr_info("parsed=%llu, sample=%llu, activity=0x%08x",
                (unsigned long long)ctx->parsed_events,
                (unsigned long long)ctx->collected_samples,
                ctx->activity_mask);
        ctx->parsed_events = 0;
        ctx->activity_mask = 0;
        ctx->activity_report_sample = ctx->collected_samples;
    }

    if (!ctx->is_loop && ctx->collected_samples >= ctx->total_samples)
        send_event_end(ctx, sdi);

    return frame_size;
}

static uint64_t ev3_resync_offset(const struct uart_vcd_context *ctx,
                                  const uint8_t *p, uint64_t len)
{
    uint64_t offset;

    for (offset = 1; offset + UART_VCD_SYNC_FRAME_SIZE <= len; offset++) {
        int frame_size = ev3_frame_size(ctx, p + offset, (int)(len - offset));
        if (frame_size == UART_VCD_SYNC_FRAME_SIZE &&
            p[offset + 3] == UART_VCD_HEADER_SYNC)
            return offset;
    }

    return len > UART_VCD_SYNC_FRAME_SIZE - 1 ?
        len - (UART_VCD_SYNC_FRAME_SIZE - 1) : 0;
}

/* ─── TCP ─── */
static int tcp_connect(const char *host, int port)
{
    int fd; struct sockaddr_in addr; struct hostent *he;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd<0) { sr_err("TCP socket: %s", strerror(errno)); return -1; }
    he = gethostbyname(host);
    if (!he) { sr_err("TCP resolve: %s", host); close(fd); return -1; }
    memset(&addr,0,sizeof(addr)); addr.sin_family=AF_INET;
    addr.sin_port=htons(port); memcpy(&addr.sin_addr,he->h_addr,he->h_length);

    { int fl=fcntl(fd,F_GETFL,0); fcntl(fd,F_SETFL,fl|O_NONBLOCK); }
    if (connect(fd,(struct sockaddr*)&addr,sizeof(addr))<0) {
        if (errno!=EINPROGRESS) {
            sr_err("TCP connect %s:%d: %s",host,port,strerror(errno)); close(fd); return -1;
        }
        struct timeval tv; tv.tv_sec=1; tv.tv_usec=0;
        fd_set wset; FD_ZERO(&wset); FD_SET(fd,&wset);
        int ret=select(fd+1,NULL,&wset,NULL,&tv);
        if (ret<=0) {
            sr_err("TCP connect %s:%d timeout",host,port); close(fd); return -1;
        }
        int err=0; socklen_t len=sizeof(err);
        getsockopt(fd,SOL_SOCKET,SO_ERROR,&err,&len);
        if (err) { sr_err("TCP connect %s:%d: %s",host,port,strerror(err)); close(fd); return -1; }
    }

    sr_info("TCP connected to %s:%d", host, port);
    return fd;
}

static int tcp_reconnect(struct uart_vcd_context *ctx)
{
    if (ctx->tcp_fd>=0) { close(ctx->tcp_fd); ctx->tcp_fd=-1; }
    sr_info("TCP connecting to %s:%d...", ctx->tcp_host, ctx->tcp_port);
    ctx->tcp_fd=tcp_connect(ctx->tcp_host, ctx->tcp_port);
    if (ctx->tcp_fd<0) { sr_err("TCP connect failed"); return SR_ERR; }
    { int fl=fcntl(ctx->tcp_fd,F_GETFL,0); fcntl(ctx->tcp_fd,F_SETFL,fl|O_NONBLOCK); }
    { int rcvbuf=524288; setsockopt(ctx->tcp_fd,SOL_SOCKET,SO_RCVBUF,&rcvbuf,sizeof(rcvbuf)); }
    { uint8_t d[1024]; while (read(ctx->tcp_fd,d,sizeof(d))>0); }
    ctx->input_len=0; ctx->input_offset=0; ctx->gpio_state=0;
    ctx->first_event=TRUE;
    return SR_OK;
}

/* ─── Driver ─── */
static int hw_init(struct sr_context *sr_ctx) { return std_hw_init(sr_ctx, di, LOG_PREFIX); }
static int hw_clean_up(void) { return SR_OK; }

static GSList *hw_scan(GSList *options)
{
    struct sr_dev_inst *sdi; struct uart_vcd_context *ctx; struct drv_context *drvc;
    GSList *devices=NULL; (void)options;
    drvc=di->priv; if (drvc->instances) return devices;
    ctx=calloc(1,sizeof(*ctx)); if (!ctx) return devices;
    sdi=sr_dev_inst_new(LOGIC,SR_ST_INACTIVE,"UART_VCD","Uart VCD",NULL);
    if (!sdi) { free(ctx); return NULL; }
    sdi->priv=ctx; sdi->driver=di; sdi->dev_type=DEV_TYPE_USB;
    ctx->tcp_fd=-1; ctx->tcp_port=UART_VCD_DEFAULT_TCP_PORT;
    g_strlcpy(ctx->tcp_host, UART_VCD_DEFAULT_TCP_HOST, sizeof(ctx->tcp_host));
    ctx->samplerate=UART_VCD_EVENT_SAMPLERATE_DEFAULT;
    ctx->total_samples=UART_VCD_EVENT_DEFAULT_TOTAL_SAMPLES;
    ctx->num_probes=UART_VCD_NUM_PROBES;
    init_default_labels(ctx);
    sdi->path=g_strdup("tcp");
    drvc->instances=g_slist_append(drvc->instances,sdi);
    devices=g_slist_append(devices,sdi);
    sr_info("uart_vcd device created: samplerate=%llu, total_samples=%llu",
            (unsigned long long)ctx->samplerate,(unsigned long long)ctx->total_samples);
    return devices;
}

static const GSList *hw_dev_mode_list(const struct sr_dev_inst *sdi)
    { (void)sdi; GSList *l=NULL; l=g_slist_append(l,(gpointer)&sr_mode_list[0]); return l; }

static int hw_dev_open(struct sr_dev_inst *sdi)
{
    struct sr_channel *probe; int i; struct uart_vcd_context *ctx;
    assert(sdi); assert(sdi->priv); ctx=sdi->priv;
    if (sdi->status==SR_ST_ACTIVE) return SR_OK;
    sr_dev_probes_free(sdi);
    for (i=0; i<ctx->num_probes; i++) {
        if (!(probe=sr_channel_new(i,SR_CHANNEL_LOGIC,TRUE,uart_vcd_probe_names[i])))
            { sr_err("create channel failed"); sr_dev_inst_free(sdi); return SR_ERR; }
        sdi->channels=g_slist_append(sdi->channels,probe);
    }
    sdi->status=SR_ST_ACTIVE; return SR_OK;
}

static int hw_dev_close(struct sr_dev_inst *sdi)
{
    struct uart_vcd_context *ctx;
    if (sdi && sdi->priv) {
        ctx=sdi->priv;
        if (ctx->tcp_fd>=0) { close(ctx->tcp_fd); ctx->tcp_fd=-1; }
        free(ctx->input_buf);
        free(ctx->event_buf);
        ctx->input_buf = NULL;
        ctx->event_buf = NULL;
        sdi->status=SR_ST_INACTIVE; return SR_OK;
    }
    return SR_ERR_CALL_STATUS;
}

static int dev_destroy(struct sr_dev_inst *sdi)
{
    struct drv_context *drvc;
    hw_dev_close(sdi);
    free(sdi->priv); sdi->priv=NULL;
    drvc=di->priv; if (drvc) drvc->instances=g_slist_remove(drvc->instances,sdi);
    sr_dev_inst_free(sdi); return SR_OK;
}

static int config_get(int id, GVariant **data, const struct sr_dev_inst *sdi,
                      const struct sr_channel *ch, const struct sr_channel_group *cg)
{
    (void)cg; struct uart_vcd_context *ctx; assert(sdi->priv); ctx=sdi->priv;
    switch (id) {
    case SR_CONF_SAMPLERATE: *data=g_variant_new_uint64(ctx->samplerate); break;
    case SR_CONF_LIMIT_SAMPLES: *data=g_variant_new_uint64(ctx->total_samples); break;
    case SR_CONF_DEVICE_MODE: *data=g_variant_new_int16(sdi->mode); break;
    case SR_CONF_HW_DEPTH: *data=g_variant_new_uint64(UART_VCD_MAX_HW_DEPTH); break;
    case SR_CONF_UNIT_BITS: *data=g_variant_new_byte(1); break;
    case SR_CONF_VLD_CH_NUM: *data=g_variant_new_int16(ctx->num_probes); break;
    case SR_CONF_PROBE_EN: if (ch) *data=g_variant_new_boolean(TRUE); else return SR_ERR; break;
    case SR_CONF_HAVE_ZERO: *data=g_variant_new_boolean(FALSE); break;
    case SR_CONF_LOAD_DECODER: *data=g_variant_new_boolean(TRUE); break;
    case SR_CONF_RLE: *data=g_variant_new_boolean(FALSE); break;
    case SR_CONF_INSTANT: *data=g_variant_new_boolean(FALSE); break;
    case SR_CONF_OPERATION_MODE: *data=g_variant_new_int16(LO_OP_STREAM); break;
    case SR_CONF_LOOP_MODE: *data=g_variant_new_boolean(ctx->is_loop); break;
    case SR_CONF_TCP_HOST: *data=g_variant_new_string(ctx->tcp_host); break;
    case SR_CONF_USB_SPEED: *data=g_variant_new_int32(LIBUSB_SPEED_HIGH); break;
    case SR_CONF_USB30_SUPPORT: *data=g_variant_new_boolean(FALSE); break;
    default: return SR_ERR_NA;
    }
    return SR_OK;
}

static int config_set(int id, GVariant *data, struct sr_dev_inst *sdi,
                      struct sr_channel *ch, struct sr_channel_group *cg)
{
    (void)cg; struct uart_vcd_context *ctx; assert(sdi->priv); ctx=sdi->priv;
    switch (id) {
    case SR_CONF_SAMPLERATE: ctx->samplerate=g_variant_get_uint64(data); break;
    case SR_CONF_LIMIT_SAMPLES: ctx->total_samples=g_variant_get_uint64(data); break;
    case SR_CONF_PROBE_EN: (void)ch; break; /* always enabled */
    case SR_CONF_DEVICE_MODE: sdi->mode=g_variant_get_int16(data); break;
    case SR_CONF_LOOP_MODE: ctx->is_loop=g_variant_get_boolean(data); break;
    case SR_CONF_TCP_HOST:
        g_strlcpy(ctx->tcp_host, g_variant_get_string(data, NULL), sizeof(ctx->tcp_host));
        break;
    default: break;
    }
    return SR_OK;
}

static int config_list(int key, GVariant **data, const struct sr_dev_inst *sdi,
                       const struct sr_channel_group *cg)
{
    (void)cg;(void)sdi; GVariant *gvar; GVariantBuilder gvb;
    switch (key) {
    case SR_CONF_DEVICE_OPTIONS:
        *data=g_variant_new_from_data(G_VARIANT_TYPE("ai"),uart_vcd_hwoptions,
                ARRAY_SIZE(uart_vcd_hwoptions)*sizeof(int32_t),TRUE,NULL,NULL); break;
    case SR_CONF_DEVICE_SESSIONS:
        *data=g_variant_new_from_data(G_VARIANT_TYPE("ai"),uart_vcd_sessions,
                ARRAY_SIZE(uart_vcd_sessions)*sizeof(int32_t),TRUE,NULL,NULL); break;
    case SR_CONF_SAMPLERATE:
        g_variant_builder_init(&gvb,G_VARIANT_TYPE("a{sv}"));
        gvar=g_variant_new_from_data(G_VARIANT_TYPE("at"),uart_vcd_samplerates,
                ARRAY_SIZE(uart_vcd_samplerates)*sizeof(uint64_t),TRUE,NULL,NULL);
        g_variant_builder_add(&gvb,"{sv}","samplerates",gvar);
        *data=g_variant_builder_end(&gvb); break;
    default: return SR_ERR_ARG;
    }
    return SR_OK;
}

static int hw_dev_acquisition_start(struct sr_dev_inst *sdi, void *cb_data);
static int receive_data_event(int fd, int revents, const struct sr_dev_inst *sdi);

static int hw_dev_acquisition_start(struct sr_dev_inst *sdi, void *cb_data)
{
    (void)cb_data; struct uart_vcd_context *ctx; assert(sdi->priv); ctx=sdi->priv;

    ctx->collected_samples=0; ctx->collecting=TRUE; ctx->end_sent=FALSE;
    ctx->recorded_state=0xff000000u; ctx->activity_mask=0;
    ctx->activity_report_sample=0; ctx->parsed_events=0;
    ctx->bad_packets=0; ctx->recovered_packets=0;
    ctx->dropped_input_bytes=0;
    ctx->gpio_state=0; ctx->output_state=0xff000000u;
    ctx->input_len=0; ctx->input_offset=0; ctx->event_count=0;
    init_default_labels(ctx);

    if (tcp_reconnect(ctx)!=SR_OK) { ctx->collecting=FALSE; return SR_ERR; }

    free(ctx->input_buf);
    free(ctx->event_buf);
    ctx->input_buf=malloc(UART_VCD_BUFSIZE);
    ctx->event_buf=malloc(sizeof(*ctx->event_buf) * UART_VCD_EVENT_BATCH_SIZE);
    if (!ctx->input_buf||!ctx->event_buf) {
        ctx->collecting=FALSE;
        sr_err("malloc failed");
        return SR_ERR_MALLOC;
    }
    ctx->event_buf[0].sample=0;
    ctx->event_buf[0].state=ctx->output_state;
    ctx->event_buf[0].reserved=0;
    ctx->event_count=1;

    sr_info("Start acquisition on TCP port %d, samplerate=%llu",
            ctx->tcp_port,(unsigned long long)ctx->samplerate);
    sr_session_source_add(ctx->tcp_fd, G_IO_IN, 100, receive_data_event, sdi);
    return SR_OK;
}

static int hw_dev_acquisition_stop(const struct sr_dev_inst *sdi, void *cb_data)
{
    (void)cb_data; struct uart_vcd_context *ctx; assert(sdi->priv); ctx=sdi->priv;
    if (ctx->collecting)
        send_event_end(ctx, sdi);
    if (ctx->tcp_fd>=0) { close(ctx->tcp_fd); ctx->tcp_fd=-1; }
    return SR_OK;
}

static int hw_dev_status_get(const struct sr_dev_inst *sdi, struct sr_status *status, gboolean prg)
    { (void)prg; if (sdi&&status) { memset(status,0,sizeof(*status)); return SR_OK; } return SR_ERR; }

/* ─── Receive (TCP) ─── */
static int receive_data_event(int fd, int revents, const struct sr_dev_inst *sdi)
{
    struct uart_vcd_context *ctx; struct sr_datafeed_packet pkt;
    uint8_t read_buf[UART_VCD_READ_BUF_SIZE];
    assert(sdi->priv); ctx=sdi->priv;

    if (!ctx->collecting) {
        if (!ctx->end_sent) {
            pkt.type=SR_DF_END; pkt.status=SR_PKT_OK;
            ds_data_forward(sdi,&pkt);
            ctx->end_sent=TRUE;
        }
        return FALSE;
    }
    if (!(revents & G_IO_IN)) return TRUE;

    {
        int ec=0;

        while (ctx->collecting && ec < UART_VCD_EVENT_LIMIT) {

            while (ctx->collecting && ctx->input_len >= 4 &&
                   ec < UART_VCD_EVENT_LIMIT) {
                int c=ev3_blow_buf(ctx, sdi,
                    ctx->input_buf + ctx->input_offset,
                    (int)ctx->input_len);
                if (c==0) break;
                if (c < 0) {
                    uint64_t skip = ev3_resync_offset(ctx,
                        ctx->input_buf + ctx->input_offset, ctx->input_len);
                    if (!skip)
                        break;
                    protocol_error(ctx, "invalid framing",
                        ctx->input_buf + ctx->input_offset, skip);
                    if (skip + UART_VCD_SYNC_FRAME_SIZE <= ctx->input_len &&
                        ctx->input_buf[ctx->input_offset + skip + 3] ==
                            UART_VCD_HEADER_SYNC)
                        ctx->recovered_packets++;
                    ctx->input_offset += skip;
                    ctx->input_len -= skip;
                    continue;
                }
                ctx->input_offset += c;
                ctx->input_len   -= c;
                ec++;
            }

            if (!ctx->collecting) break;
            if (ec >= UART_VCD_EVENT_LIMIT) break;

            if (ctx->input_offset) {
                if (ctx->input_len > 0)
                    memmove(ctx->input_buf, ctx->input_buf + ctx->input_offset, ctx->input_len);
                ctx->input_offset = 0;
            }

            {
                ssize_t n = read(fd, read_buf, sizeof(read_buf));
                if (n < 0) { if (errno==EAGAIN||errno==EWOULDBLOCK) break;
                             sr_err("read error: %s",strerror(errno)); return FALSE; }
                if (n == 0) break;

                if (ctx->input_len + (uint64_t)n > UART_VCD_BUFSIZE) {
                    protocol_error(ctx, "input buffer overflow",
                                   ctx->input_buf, ctx->input_len);
                    ctx->input_len = 0;
                    ctx->input_offset = 0;
                }

                memcpy(ctx->input_buf + ctx->input_len, read_buf, n);
                ctx->input_len += (uint64_t)n;
            }
        }
    }
    if (ctx->collecting)
        flush_event_batch(ctx, sdi);
    return ctx->collecting ? TRUE : FALSE;
}

SR_PRIV struct sr_dev_driver uart_vcd_driver_info = {
    .name="uart-vcd", .longname="Uart VCD",
    .api_version=1, .driver_type=DRIVER_TYPE_HARDWARE,
    .init=hw_init, .cleanup=hw_clean_up, .scan=hw_scan,
    .dev_mode_list=hw_dev_mode_list,
    .config_get=config_get, .config_set=config_set, .config_list=config_list,
    .dev_open=hw_dev_open, .dev_close=hw_dev_close, .dev_destroy=dev_destroy,
    .dev_status_get=hw_dev_status_get,
    .dev_acquisition_start=hw_dev_acquisition_start,
    .dev_acquisition_stop=hw_dev_acquisition_stop,
    .priv=NULL,
};
