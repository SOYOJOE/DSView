/*
 * This file is part of the DSView project.
 * Copyright (C) 2024
 *
 * UART VCD driver — TCP-only, protocol v2, 24MHz samplerate.
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
#define UART_VCD_DELTA_CLAMP            12000000
#define UART_VCD_MAX_EVENT_SIZE         264
#define UART_VCD_ERROR_DUMP_SIZE        32

SR_PRIV struct sr_dev_driver uart_vcd_driver_info;
static struct sr_dev_driver *di = &uart_vcd_driver_info;

static void flush_event_batch(struct uart_vcd_context *ctx,
                              const struct sr_dev_inst *sdi);

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

static void uart_set_level(struct uart_vcd_context *ctx, int channel, int high)
{
    const uint32_t mask = 1u << (24 + channel);
    if (high)
        ctx->output_state |= mask;
    else
        ctx->output_state &= ~mask;
}

static gboolean uart_fifo_push(struct uart_vcd_context *ctx, int channel,
                               uint8_t value)
{
    const uint8_t next = (ctx->uart_fifo_head[channel] + 1) & 0x3F;
    if (next == ctx->uart_fifo_tail[channel]) {
        ctx->uart_fifo_overflow = TRUE;
        ctx->dropped_uart_bytes++;
        return FALSE;
    }

    ctx->uart_fifo[channel][ctx->uart_fifo_head[channel]] = value;
    ctx->uart_fifo_head[channel] = next;
    return TRUE;
}

static void uart_start_byte(struct uart_vcd_context *ctx, int channel)
{
    ctx->uart_tx_data[channel] =
        ctx->uart_fifo[channel][ctx->uart_fifo_tail[channel]];
    ctx->uart_fifo_tail[channel] =
        (ctx->uart_fifo_tail[channel] + 1) & 0x3F;
    ctx->uart_tx_bit[channel] = 0;
    ctx->uart_tx_next_sample[channel] =
        ctx->collected_samples + ctx->uart_tx_samp_per_bit;
    uart_set_level(ctx, channel, FALSE);
}

static void uart_advance(struct uart_vcd_context *ctx,
                         const struct sr_dev_inst *sdi, uint64_t target)
{
    while (ctx->uart_tx_active) {
        uint64_t next = UINT64_MAX;
        uint32_t old_state;
        int channel;

        for (channel = 0; channel < 8; channel++) {
            if (ctx->uart_tx_bit[channel] >= 0 &&
                ctx->uart_tx_next_sample[channel] < next)
                next = ctx->uart_tx_next_sample[channel];
        }
        if (next > target)
            break;

        ctx->collected_samples = next;
        old_state = ctx->output_state;

        for (channel = 0; channel < 8; channel++) {
            int bit;
            if (ctx->uart_tx_bit[channel] < 0 ||
                ctx->uart_tx_next_sample[channel] != next)
                continue;

            bit = ++ctx->uart_tx_bit[channel];
            if (bit < 9) {
                uart_set_level(ctx, channel,
                    (ctx->uart_tx_data[channel] & (1u << (bit - 1))) != 0);
                ctx->uart_tx_next_sample[channel] =
                    next + ctx->uart_tx_samp_per_bit;
            } else if (bit == 9) {
                uart_set_level(ctx, channel, TRUE);
                ctx->uart_tx_next_sample[channel] =
                    next + ctx->uart_tx_samp_per_bit;
            } else if (ctx->uart_fifo_head[channel] !=
                       ctx->uart_fifo_tail[channel]) {
                uart_start_byte(ctx, channel);
            } else {
                ctx->uart_tx_bit[channel] = -1;
                ctx->uart_tx_next_sample[channel] = UINT64_MAX;
                ctx->uart_tx_active--;
                uart_set_level(ctx, channel, TRUE);
            }
        }

        if (ctx->output_state != old_state)
            record_state(ctx, sdi);
    }

    ctx->collected_samples = target;
}

static void send_event_end(struct uart_vcd_context *ctx, const struct sr_dev_inst *sdi)
{
    if (ctx->end_sent)
        return;

    if (ctx->parsed_events || ctx->activity_mask) {
        sr_info("final parsed=%llu, sample=%llu, activity=0x%08x, "
                "bad_packets=%llu, recovered=%llu, dropped_input=%llu, "
                "dropped_uart=%llu",
                (unsigned long long)ctx->parsed_events,
                (unsigned long long)ctx->collected_samples,
                ctx->activity_mask,
                (unsigned long long)ctx->bad_packets,
                (unsigned long long)ctx->recovered_packets,
                (unsigned long long)ctx->dropped_input_bytes,
                (unsigned long long)ctx->dropped_uart_bytes);
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

static int ev2_frame_size(const struct uart_vcd_context *ctx,
                          const uint8_t *p, int len)
{
    uint32_t delta_raw;
    uint8_t header;

    if (len < 4)
        return 0;

    delta_raw = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                ((uint32_t)p[2] << 16);
    if (!ctx->first_event && delta_raw > UART_VCD_DELTA_CLAMP)
        return -1;

    header = p[3];
    if (header & 0x80) {
        int label_len = header & 0x1F;
        int render_sub = (header >> 5) & 3;
        int payload_onwire;
        int payload_padded;
        int frame_size;
        int i;

        if (render_sub > 1)
            return -1;
        if (len < 6)
            return 0;
        if (p[4] > 7 || p[5] < label_len)
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

    {
        const uint8_t sub = (header >> 5) & 3;
        const uint8_t channel = header & 0x1F;
        if (sub > 2)
            return -1;
        if (channel > 23 && header != 0x1F)
            return -1;
    }
    return 4;
}

static int ev2_headerless_string_size(const uint8_t *p, int len)
{
    int label_len;
    int render_sub;
    int payload_onwire;
    int payload_padded;
    int frame_size;
    int i;

    if (len < 1)
        return 0;
    if (!(p[0] & 0x80))
        return -1;

    label_len = p[0] & 0x1F;
    render_sub = (p[0] >> 5) & 3;
    if (render_sub > 1)
        return -1;
    if (len < 3)
        return 0;
    if (p[1] > 7 || p[2] < label_len)
        return -1;

    payload_onwire = 2 + p[2];
    payload_padded = (payload_onwire + 3) & ~3;
    frame_size = 1 + payload_padded;
    if (frame_size > UART_VCD_MAX_EVENT_SIZE - 3)
        return -1;
    if (len < frame_size)
        return 0;

    for (i = 1 + payload_onwire; i < frame_size; i++) {
        if (p[i] != 0)
            return -1;
    }
    return frame_size;
}

static uint64_t ev2_resync_offset(const struct uart_vcd_context *ctx,
                                  const uint8_t *p, uint64_t len)
{
    uint64_t offset;

    for (offset = 1; offset < len; offset++) {
        int frame_size = ev2_frame_size(ctx, p + offset, (int)(len - offset));

        if (frame_size > 0 && (p[offset + 3] & 0x80))
            return offset;
    }

    for (offset = 1; offset < len; offset++) {
        int headerless_size = ev2_headerless_string_size(
            p + offset, (int)(len - offset));
        if (headerless_size > 0)
            return offset;
    }

    return len > UART_VCD_MAX_EVENT_SIZE ?
        len - UART_VCD_MAX_EVENT_SIZE : 0;
}

/* ─── Protocol v2 parser ─── */
static int ev2_blow_buf(struct uart_vcd_context *ctx, const struct sr_dev_inst *sdi,
                         const uint8_t *p, int len)
{
    int pos=0;
    int frame_size = ev2_frame_size(ctx, p, len);
    if (frame_size <= 0)
        return frame_size;

    uint32_t delta_raw = (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16);
    uint64_t delta_samples = delta_raw;
    if (ctx->first_event) { ctx->first_event = FALSE; delta_samples = 1; }
    else if (delta_samples > ctx->total_samples) delta_samples = 1;

    pos=3;
    uint8_t header = p[pos++];

    if (header & 0x80) {
        int label_len = header & 0x1F;
        int render_sub = (header>>5)&3;
        uint8_t ch_byte = p[pos++]; int channel = ch_byte;
        uint8_t total_len = p[pos++]; int data_len = (int)total_len - label_len;
        int payload_onwire = 2+total_len;
        int payload_padded = (payload_onwire+3)&~3;

        {
            uint64_t target = ctx->collected_samples + delta_samples;
            if (!ctx->is_loop && target > ctx->total_samples)
                target = ctx->total_samples;
            uart_advance(ctx, sdi, target);
        }
        ctx->output_state |= (1u<<(24+channel));
        ctx->uart_fifo_overflow = FALSE;
        {
            const uint8_t *src = p+pos; int d;
            for (d=0; d<label_len; d++) {
                uart_fifo_push(ctx, channel, src[d]);
            }
            src+=label_len;
            for (d=0; d<data_len; d++) {
                uint8_t b = src[d];
                if (render_sub==0) {
                    static const char hexc[]="0123456789ABCDEF";
                    uint8_t h=(uint8_t)hexc[(b>>4)&0xF], l=(uint8_t)hexc[b&0xF];
                    uart_fifo_push(ctx, channel, h);
                    uart_fifo_push(ctx, channel, l);
                } else {
                    uart_fifo_push(ctx, channel, b);
                }
            }
        }
        pos+=label_len+data_len;
        pos+=payload_padded-payload_onwire;
        if (ctx->uart_tx_bit[channel]<0 && ctx->uart_fifo_head[channel]!=ctx->uart_fifo_tail[channel])
            { uart_start_byte(ctx, channel); ctx->uart_tx_active++; }
        record_state(ctx, sdi);
        if (ctx->uart_fifo_overflow) {
            sr_err("RX%d UART FIFO overflow: bytes dropped, acquisition continues",
                   channel);
            ctx->uart_fifo_overflow = FALSE;
        }
    } else {
        uint8_t sub = (header>>5)&3; int channel = header&0x1F;
        {
            uint64_t target = ctx->collected_samples + delta_samples;
            if (!ctx->is_loop && target > ctx->total_samples)
                target = ctx->total_samples;
            uart_advance(ctx, sdi, target);
        }
        if (channel<=23) {
            uint32_t mask = 1u<<channel;
            if (sub==0) ctx->gpio_state &= ~mask;
            else if (sub==1) ctx->gpio_state |= mask;
            else ctx->gpio_state ^= mask;
            ctx->output_state =
                (ctx->output_state & 0xff000000u) | ctx->gpio_state;
            record_state(ctx, sdi);
        }
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
    assert(pos == frame_size);
    return frame_size;
}

static int ev2_recover_headerless_string(struct uart_vcd_context *ctx,
                                         const struct sr_dev_inst *sdi,
                                         const uint8_t *p, int len)
{
    uint8_t frame[UART_VCD_MAX_EVENT_SIZE];
    int headerless_size = ev2_headerless_string_size(p, len);
    int consumed;

    if (headerless_size <= 0)
        return headerless_size;

    frame[0] = 0;
    frame[1] = 0;
    frame[2] = 0;
    memcpy(frame + 3, p, (size_t)headerless_size);
    consumed = ev2_blow_buf(ctx, sdi, frame, headerless_size + 3);
    if (consumed <= 0)
        return -1;

    ctx->bad_packets++;
    ctx->recovered_packets++;
    sr_err("bad MCU packet: missing 3-byte delta before string header; "
           "recovered=%d; data=%02X %02X %02X",
           headerless_size, p[0], p[1], p[2]);
    return headerless_size;
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
    memset(ctx->uart_tx_bit,-1,sizeof(ctx->uart_tx_bit));
    memset(ctx->uart_fifo_head,0,sizeof(ctx->uart_fifo_head));
    memset(ctx->uart_fifo_tail,0,sizeof(ctx->uart_fifo_tail));
    ctx->uart_tx_active=0;
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
    ctx->protocol=UART_VCD_DEFAULT_PROTOCOL;
    ctx->samplerate=UART_VCD_EVENT_SAMPLERATE_DEFAULT;
    ctx->total_samples=UART_VCD_EVENT_DEFAULT_TOTAL_SAMPLES;
    ctx->num_probes=UART_VCD_NUM_PROBES;
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
    ctx->uart_fifo_overflow=FALSE;
    ctx->recorded_state=0xff000000u; ctx->activity_mask=0;
    ctx->activity_report_sample=0; ctx->parsed_events=0;
    ctx->bad_packets=0; ctx->recovered_packets=0;
    ctx->dropped_input_bytes=0; ctx->dropped_uart_bytes=0;
    ctx->gpio_state=0; ctx->output_state=0xff000000u;
    ctx->input_len=0; ctx->input_offset=0; ctx->event_count=0;

    ctx->uart_tx_samp_per_bit=(int)(ctx->samplerate/UART_VCD_UART_BAUD_RATE);
    memset(ctx->uart_tx_data,0,sizeof(ctx->uart_tx_data));
    memset(ctx->uart_tx_bit,-1,sizeof(ctx->uart_tx_bit));
    {
        int channel;
        for (channel = 0; channel < 8; channel++)
            ctx->uart_tx_next_sample[channel] = UINT64_MAX;
    }
    memset(ctx->uart_fifo_head,0,sizeof(ctx->uart_fifo_head));
    memset(ctx->uart_fifo_tail,0,sizeof(ctx->uart_fifo_tail));
    ctx->uart_tx_active=0;

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
                int c=ev2_blow_buf(ctx, sdi, ctx->input_buf + ctx->input_offset, (int)ctx->input_len);
                if (c==0) break;
                if (c < 0) {
                    c = ev2_recover_headerless_string(ctx, sdi,
                        ctx->input_buf + ctx->input_offset,
                        (int)ctx->input_len);
                    if (c > 0) {
                        ctx->input_offset += c;
                        ctx->input_len -= c;
                        ec++;
                        continue;
                    }
                    if (c == 0)
                        break;

                    uint64_t skip = ev2_resync_offset(ctx,
                        ctx->input_buf + ctx->input_offset, ctx->input_len);
                    if (!skip)
                        break;
                    protocol_error(ctx, "invalid framing",
                        ctx->input_buf + ctx->input_offset, skip);
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
