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
#include <netinet/in.h>
#include <netdb.h>
#include <assert.h>
#include "../../log.h"

#undef LOG_PREFIX
#define LOG_PREFIX "uart_vcd: "

#define UART_VCD_SAMPLES_PER_OUTPUT     64
#define UART_VCD_OUTPUT_SIZE            (UART_VCD_NUM_PROBES * 8)
#define UART_VCD_BATCH_CHUNKS           256
#define UART_VCD_BATCH_OUTPUT_SIZE      (UART_VCD_OUTPUT_SIZE * UART_VCD_BATCH_CHUNKS)
#define UART_VCD_READ_BUF_SIZE          65536
#define UART_VCD_EVENT_LIMIT            2048
#define UART_VCD_SAMPLE_LIMIT           3145728
#define UART_VCD_DELTA_CLAMP            12000000
#define UART_VCD_LOOP_WINDOW_MAX       SR_Mn(2500)

SR_PRIV struct sr_dev_driver uart_vcd_driver_info;
static struct sr_dev_driver *di = &uart_vcd_driver_info;

/* ─── UART TX sim ─── */
static uint32_t uart_tx_process(struct uart_vcd_context *ctx, uint32_t state)
{
    int ch;
    if (!ctx->uart_tx_active) return state;
    for (ch = 0; ch < 8; ch++) {
        if (ctx->uart_tx_bit[ch] >= 0) {
            uint32_t ch_bit = 1u << (24 + ch);
            if (ctx->uart_tx_bit[ch] == 0) state &= ~ch_bit;
            else if (ctx->uart_tx_bit[ch] >= 9) state |= ch_bit;
            else if (ctx->uart_tx_data[ch] & (1u<<(ctx->uart_tx_bit[ch]-1))) state |= ch_bit;
            else state &= ~ch_bit;
            if (--ctx->uart_tx_samp_left[ch] <= 0) {
                ctx->uart_tx_bit[ch]++;
                if (ctx->uart_tx_bit[ch] >= 10) {
                    ctx->uart_tx_bit[ch] = -1;
                    ctx->uart_tx_active--;
                    if (ctx->uart_fifo_head[ch] != ctx->uart_fifo_tail[ch]) {
                        ctx->uart_tx_data[ch] = ctx->uart_fifo[ch][ctx->uart_fifo_tail[ch]];
                        ctx->uart_fifo_tail[ch] = (ctx->uart_fifo_tail[ch]+1)&0x3F;
                        ctx->uart_tx_bit[ch] = 0;
                        ctx->uart_tx_samp_left[ch] = ctx->uart_tx_samp_per_bit;
                        ctx->uart_tx_active++;
                    }
                } else ctx->uart_tx_samp_left[ch] = ctx->uart_tx_samp_per_bit;
            }
        }
    }
    return state;
}

static void flush_batch(struct uart_vcd_context *ctx, const struct sr_dev_inst *sdi)
{
    if (!ctx->batch_chunk) return;
    struct sr_datafeed_packet pkt; struct sr_datafeed_logic log;
    pkt.type=SR_DF_LOGIC; pkt.status=SR_PKT_OK; pkt.payload=&log;
    log.format=LA_CROSS_DATA; log.index=0; log.order=0;
    log.length=(uint64_t)ctx->batch_chunk * UART_VCD_OUTPUT_SIZE;
    log.unitsize=1; log.data_error=0; log.error_pattern=0; log.data=ctx->batch_buf;
    ds_data_forward(sdi, &pkt);
    ctx->batch_chunk=0;
}

static void ev2_push_chunk(struct uart_vcd_context *ctx, const struct sr_dev_inst *sdi,
                            const uint8_t *chunk)
{
    memcpy(ctx->batch_buf + ctx->batch_chunk * UART_VCD_OUTPUT_SIZE,
           chunk, UART_VCD_OUTPUT_SIZE);
    if (++ctx->batch_chunk == UART_VCD_BATCH_CHUNKS) flush_batch(ctx, sdi);
}

static void emit_event_sample(struct uart_vcd_context *ctx, const struct sr_dev_inst *sdi)
{
    uint32_t state = uart_tx_process(ctx, ctx->gpio_state);
    int byte_idx = ctx->event_sample_pos >> 3;
    int bit_idx  = ctx->event_sample_pos & 7;
    int ch;
    for (ch = 0; ch < UART_VCD_NUM_PROBES; ch++)
        if (state & (1u<<ch)) ctx->output_buf[(ch<<3)+byte_idx] |= (uint8_t)(1u<<bit_idx);
    if (++ctx->event_sample_pos == UART_VCD_SAMPLES_PER_OUTPUT) {
        ev2_push_chunk(ctx, sdi, ctx->output_buf);
        memset(ctx->output_buf, 0, UART_VCD_OUTPUT_SIZE);
        ctx->event_sample_pos = 0;
    }
}

static void ev2_emit_samples(struct uart_vcd_context *ctx,
                              const struct sr_dev_inst *sdi, uint64_t count)
{
    uint64_t emitted = 0;
    if (!ctx->uart_tx_active && !ctx->event_sample_pos && count >= UART_VCD_SAMPLES_PER_OUTPUT) {
        uint8_t chunk[UART_VCD_OUTPUT_SIZE];
        memset(chunk, 0, UART_VCD_OUTPUT_SIZE);
        {
            int ch; uint32_t s = ctx->gpio_state;
            for (ch = 0; ch < UART_VCD_NUM_PROBES; ch++)
                if (s & (1u<<ch)) memset(chunk + (ch<<3), 0xFF, 8);
        }
        uint64_t n_chunks = count / UART_VCD_SAMPLES_PER_OUTPUT;
        if (n_chunks > (uint64_t)(UART_VCD_BATCH_CHUNKS - ctx->batch_chunk))
            n_chunks = UART_VCD_BATCH_CHUNKS - ctx->batch_chunk;
        {
            uint64_t i;
            for (i = 0; i < n_chunks && ctx->collecting; i++)
                ev2_push_chunk(ctx, sdi, chunk);
        }
        emitted = n_chunks * UART_VCD_SAMPLES_PER_OUTPUT;
        count -= emitted;
    }
    {
        uint64_t i;
        for (i = 0; i < count && ctx->collecting; i++) emit_event_sample(ctx, sdi);
    }
    emitted += count;
    ctx->collected_samples += emitted;
}

static void flush_event_output(struct uart_vcd_context *ctx, const struct sr_dev_inst *sdi)
{
    if (!ctx->event_sample_pos) return;
    while (ctx->event_sample_pos > 0) emit_event_sample(ctx, sdi);
}

static void send_event_end(struct uart_vcd_context *ctx, const struct sr_dev_inst *sdi)
{
    flush_event_output(ctx, sdi);
    flush_batch(ctx, sdi);
    struct sr_datafeed_packet pkt;
    pkt.type=SR_DF_END; pkt.status=SR_PKT_OK;
    ds_data_forward(sdi, &pkt); ctx->collecting=FALSE;
}

/* ─── Protocol v2 parser ─── */
static int ev2_blow_buf(struct uart_vcd_context *ctx, const struct sr_dev_inst *sdi,
                         const uint8_t *p, int len)
{
    int pos=0;
    if (len<4) return 0;

    uint32_t delta_raw = (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16);
    uint64_t delta_samples = delta_raw;
    if (delta_samples > ctx->total_samples) delta_samples = 1;
    if (delta_samples > UART_VCD_DELTA_CLAMP) delta_samples = UART_VCD_DELTA_CLAMP;

    pos=3;
    uint8_t header = p[pos++];

    if (header & 0x80) {
        int label_len = header & 0x1F;
        int render_sub = (header>>5)&3; (void)render_sub;
        if (len < pos+2) return 0;
        uint8_t ch_byte = p[pos++]; int channel = ch_byte&7;
        uint8_t total_len = p[pos++]; int data_len = (int)total_len - label_len;
        if (data_len<0) data_len=0;
        int payload_onwire = 2+total_len;
        int payload_padded = (payload_onwire+3)&~3;
        if (len < pos+label_len+data_len+(payload_padded-payload_onwire)) return 0;

        ev2_emit_samples(ctx, sdi, delta_samples);
        ctx->gpio_state |= (1u<<(24+channel));
        {
            const uint8_t *src = p+pos; int d;
            for (d=0; d<label_len; d++) {
                uint8_t nxt = (ctx->uart_fifo_head[channel]+1)&0x3F;
                if (nxt!=ctx->uart_fifo_tail[channel])
                    { ctx->uart_fifo[channel][ctx->uart_fifo_head[channel]]=src[d];
                      ctx->uart_fifo_head[channel]=nxt; }
            }
            src+=label_len;
            for (d=0; d<data_len; d++) {
                uint8_t b = src[d];
                if (render_sub==0) {
                    static const char hexc[]="0123456789ABCDEF";
                    uint8_t h=(uint8_t)hexc[(b>>4)&0xF], l=(uint8_t)hexc[b&0xF], nxt;
                    nxt=(ctx->uart_fifo_head[channel]+1)&0x3F;
                    if (nxt!=ctx->uart_fifo_tail[channel])
                        {ctx->uart_fifo[channel][ctx->uart_fifo_head[channel]]=h;
                         ctx->uart_fifo_head[channel]=nxt;}
                    nxt=(ctx->uart_fifo_head[channel]+1)&0x3F;
                    if (nxt!=ctx->uart_fifo_tail[channel])
                        {ctx->uart_fifo[channel][ctx->uart_fifo_head[channel]]=l;
                         ctx->uart_fifo_head[channel]=nxt;}
                } else {
                    uint8_t nxt=(ctx->uart_fifo_head[channel]+1)&0x3F;
                    if (nxt!=ctx->uart_fifo_tail[channel])
                        {ctx->uart_fifo[channel][ctx->uart_fifo_head[channel]]=b;
                         ctx->uart_fifo_head[channel]=nxt;}
                }
            }
        }
        pos+=label_len+data_len;
        pos+=payload_padded-payload_onwire;
        if (ctx->uart_tx_bit[channel]<0 && ctx->uart_fifo_head[channel]!=ctx->uart_fifo_tail[channel])
            { ctx->uart_tx_data[channel]=ctx->uart_fifo[channel][ctx->uart_fifo_tail[channel]];
              ctx->uart_fifo_tail[channel]=(ctx->uart_fifo_tail[channel]+1)&0x3F;
              ctx->uart_tx_bit[channel]=0; ctx->uart_tx_samp_left[channel]=ctx->uart_tx_samp_per_bit;
              ctx->uart_tx_active++; }
    } else {
        uint8_t sub = (header>>5)&3; int channel = header&0x1F;
        ev2_emit_samples(ctx, sdi, delta_samples);
        if (channel<=23) {
            uint32_t mask = 1u<<channel;
            if (sub==0) ctx->gpio_state &= ~mask;
            else if (sub==1) ctx->gpio_state |= mask;
            else ctx->gpio_state ^= mask;
        }
    }

    if (!ctx->is_loop && ctx->collected_samples >= ctx->total_samples)
        send_event_end(ctx, sdi);
    return pos;
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
    if (connect(fd,(struct sockaddr*)&addr,sizeof(addr))<0)
        { sr_err("TCP connect %s:%d: %s",host,port,strerror(errno)); close(fd); return -1; }
    sr_info("TCP connected to %s:%d", host, port);
    return fd;
}

static int tcp_reconnect(struct uart_vcd_context *ctx)
{
    int retries=50;
    if (ctx->tcp_fd>=0) { close(ctx->tcp_fd); ctx->tcp_fd=-1; }
    sr_info("TCP connecting to %s:%d...", UART_VCD_DEFAULT_TCP_HOST, ctx->tcp_port);
    while (retries-->0) {
        ctx->tcp_fd=tcp_connect(UART_VCD_DEFAULT_TCP_HOST, ctx->tcp_port);
        if (ctx->tcp_fd>=0) break;
        usleep(100000);
    }
    if (ctx->tcp_fd<0) { sr_err("TCP connect failed"); return SR_ERR; }
    { int fl=fcntl(ctx->tcp_fd,F_GETFL,0); fcntl(ctx->tcp_fd,F_SETFL,fl|O_NONBLOCK); }
    { int rcvbuf=524288; setsockopt(ctx->tcp_fd,SOL_SOCKET,SO_RCVBUF,&rcvbuf,sizeof(rcvbuf)); }
    { uint8_t d[1024]; while (read(ctx->tcp_fd,d,sizeof(d))>0); }
    ctx->input_len=0; ctx->input_offset=0; ctx->gpio_state=0;
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
        free(ctx->input_buf); free(ctx->output_buf); free(ctx->batch_buf);
        ctx->input_buf=ctx->output_buf=ctx->batch_buf=NULL;
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
    case SR_CONF_HW_DEPTH: *data=g_variant_new_uint64(ctx->total_samples); break;
    case SR_CONF_UNIT_BITS: *data=g_variant_new_byte(1); break;
    case SR_CONF_VLD_CH_NUM: *data=g_variant_new_int16(ctx->num_probes); break;
    case SR_CONF_PROBE_EN: if (ch) *data=g_variant_new_boolean(TRUE); else return SR_ERR; break;
    case SR_CONF_HAVE_ZERO: *data=g_variant_new_boolean(FALSE); break;
    case SR_CONF_LOAD_DECODER: *data=g_variant_new_boolean(TRUE); break;
    case SR_CONF_RLE: *data=g_variant_new_boolean(FALSE); break;
    case SR_CONF_INSTANT: *data=g_variant_new_boolean(FALSE); break;
    case SR_CONF_OPERATION_MODE: *data=g_variant_new_int16(LO_OP_STREAM); break;
    case SR_CONF_LOOP_MODE: *data=g_variant_new_boolean(ctx->is_loop); break;
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
    case SR_CONF_LIMIT_SAMPLES:
        ctx->total_samples = g_variant_get_uint64(data);
        if (ctx->total_samples < UART_VCD_LOOP_WINDOW_MAX)
            ctx->total_samples = UART_VCD_LOOP_WINDOW_MAX;
        break;
    case SR_CONF_PROBE_EN: (void)ch; break; /* always enabled */
    case SR_CONF_DEVICE_MODE: sdi->mode=g_variant_get_int16(data); break;
    case SR_CONF_LOOP_MODE: ctx->is_loop=g_variant_get_boolean(data); break;
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

    ctx->collected_samples=0; ctx->collecting=TRUE;
    ctx->event_sample_pos=0; ctx->gpio_state=0; ctx->input_len=0; ctx->input_offset=0; ctx->batch_chunk=0;

    ctx->uart_tx_samp_per_bit=(int)(ctx->samplerate/UART_VCD_UART_BAUD_RATE);
    memset(ctx->uart_tx_data,0,sizeof(ctx->uart_tx_data));
    memset(ctx->uart_tx_bit,-1,sizeof(ctx->uart_tx_bit));
    memset(ctx->uart_tx_samp_left,0,sizeof(ctx->uart_tx_samp_left));
    memset(ctx->uart_fifo_head,0,sizeof(ctx->uart_fifo_head));
    memset(ctx->uart_fifo_tail,0,sizeof(ctx->uart_fifo_tail));
    ctx->uart_tx_active=0;

    if (tcp_reconnect(ctx)!=SR_OK) { ctx->collecting=FALSE; return SR_ERR; }

    free(ctx->input_buf); free(ctx->output_buf); free(ctx->batch_buf);
    ctx->input_buf=malloc(UART_VCD_BUFSIZE);
    ctx->output_buf=malloc(UART_VCD_OUTPUT_SIZE);
    ctx->batch_buf=malloc(UART_VCD_BATCH_OUTPUT_SIZE);
    if (!ctx->input_buf||!ctx->output_buf||!ctx->batch_buf)
        { sr_err("malloc failed"); return SR_ERR_MALLOC; }
    memset(ctx->output_buf,0,UART_VCD_OUTPUT_SIZE);

    sr_info("Start acquisition on TCP port %d, samplerate=%llu",
            ctx->tcp_port,(unsigned long long)ctx->samplerate);
    sr_session_source_add(ctx->tcp_fd, G_IO_IN, 100, receive_data_event, sdi);
    return SR_OK;
}

static int hw_dev_acquisition_stop(const struct sr_dev_inst *sdi, void *cb_data)
{
    (void)cb_data; struct uart_vcd_context *ctx; assert(sdi->priv); ctx=sdi->priv;
    ctx->collecting=FALSE;
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

    if (!ctx->collecting) { pkt.type=SR_DF_END; pkt.status=SR_PKT_OK;
                            ds_data_forward(sdi,&pkt); return FALSE; }
    if (!(revents & G_IO_IN)) return TRUE;

    {
        uint64_t es=ctx->collected_samples; int ec=0;

        while (ctx->collecting && ec < UART_VCD_EVENT_LIMIT &&
               ctx->collected_samples-es < UART_VCD_SAMPLE_LIMIT) {

            while (ctx->collecting && ctx->input_len >= 4 &&
                   ec < UART_VCD_EVENT_LIMIT &&
                   ctx->collected_samples-es < UART_VCD_SAMPLE_LIMIT) {
                int c=ev2_blow_buf(ctx, sdi, ctx->input_buf + ctx->input_offset, (int)ctx->input_len);
                if (c==0) break;
                ctx->input_offset += c;
                ctx->input_len   -= c;
                ec++;
            }

            if (!ctx->collecting) break;
            if (ec >= UART_VCD_EVENT_LIMIT) break;
            if (ctx->collected_samples-es >= UART_VCD_SAMPLE_LIMIT) break;

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
                    sr_info("input overflow, discarding buffered data");
                    ctx->input_len = 0;
                    {
                        ssize_t keep = n;
                        if ((uint64_t)keep > UART_VCD_BUFSIZE) keep = UART_VCD_BUFSIZE;
                        memcpy(ctx->input_buf, read_buf, keep);
                        ctx->input_len = (uint64_t)keep;
                    }
                    continue;
                }

                memcpy(ctx->input_buf + ctx->input_len, read_buf, n);
                ctx->input_len += (uint64_t)n;
            }
        }
    }
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
