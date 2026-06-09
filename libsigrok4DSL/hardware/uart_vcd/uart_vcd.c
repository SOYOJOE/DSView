/*
 * This file is part of the DSView project.
 *
 * Copyright (C) 2024
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#define _GNU_SOURCE
#include "uart_vcd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <assert.h>
#include "../../log.h"

#undef LOG_PREFIX
#define LOG_PREFIX "uart_vcd: "

#define UART_VCD_UART_BYTES_PER_SAMPLE  4
#define UART_VCD_SAMPLES_PER_OUTPUT     64
#define UART_VCD_UART_BUF_SIZE          (UART_VCD_SAMPLES_PER_OUTPUT * UART_VCD_UART_BYTES_PER_SAMPLE)
#define UART_VCD_OUTPUT_SIZE            (UART_VCD_NUM_PROBES * 8)
#define UART_VCD_BATCH_CHUNKS           16
#define UART_VCD_BATCH_OUTPUT_SIZE      (UART_VCD_OUTPUT_SIZE * UART_VCD_BATCH_CHUNKS)
#define UART_VCD_READ_BUF_SIZE          4096

SR_PRIV struct sr_dev_driver uart_vcd_driver_info;
static struct sr_dev_driver *di = &uart_vcd_driver_info;

static uint32_t uart_tx_process(struct uart_vcd_context *ctx, uint32_t state)
{
    int ch;
    for (ch = 0; ch < 8; ch++) {
        if (ctx->uart_tx_bit[ch] >= 0) {
            uint32_t ch_bit = 1u << (24 + ch);
            if (ctx->uart_tx_bit[ch] == 0)
                state &= ~ch_bit;
            else if (ctx->uart_tx_bit[ch] >= 9)
                state |= ch_bit;
            else if (ctx->uart_tx_data[ch] & (1u << (ctx->uart_tx_bit[ch] - 1)))
                state |= ch_bit;
            else
                state &= ~ch_bit;
            if (--ctx->uart_tx_samp_left[ch] <= 0) {
                ctx->uart_tx_bit[ch]++;
                if (ctx->uart_tx_bit[ch] >= 10) {
                    ctx->uart_tx_bit[ch] = -1;
                    if (ctx->uart_fifo_head[ch] != ctx->uart_fifo_tail[ch]) {
                        ctx->uart_tx_data[ch] = ctx->uart_fifo[ch][ctx->uart_fifo_tail[ch]];
                        ctx->uart_fifo_tail[ch] = (ctx->uart_fifo_tail[ch] + 1) & 0x0F;
                        ctx->uart_tx_bit[ch] = 0;
                        ctx->uart_tx_samp_left[ch] = ctx->uart_tx_samp_per_bit;
                    }
                } else {
                    ctx->uart_tx_samp_left[ch] = ctx->uart_tx_samp_per_bit;
                }
            }
        }
    }
    return state;
}

static void emit_event_sample(struct uart_vcd_context *ctx,
                               const struct sr_dev_inst *sdi)
{
    uint32_t state = uart_tx_process(ctx, ctx->gpio_state);
    int byte_idx = ctx->event_sample_pos / 8;
    int bit_idx  = ctx->event_sample_pos % 8;
    int ch;

    for (ch = 0; ch < UART_VCD_NUM_PROBES; ch++) {
        if (state & (1u << ch))
            ctx->output_buf[ch * 8 + byte_idx] |= (1u << bit_idx);
    }
    ctx->event_sample_pos++;

    if (ctx->event_sample_pos == UART_VCD_SAMPLES_PER_OUTPUT) {
        memcpy(ctx->batch_buf + ctx->batch_chunk * UART_VCD_OUTPUT_SIZE,
               ctx->output_buf, UART_VCD_OUTPUT_SIZE);
        ctx->batch_chunk++;
        memset(ctx->output_buf, 0, UART_VCD_OUTPUT_SIZE);
        ctx->event_sample_pos = 0;

        if (ctx->batch_chunk == UART_VCD_BATCH_CHUNKS) {
            struct sr_datafeed_packet packet;
            struct sr_datafeed_logic logic;
            packet.type    = SR_DF_LOGIC;
            packet.status  = SR_PKT_OK;
            packet.payload = &logic;
            logic.format        = LA_CROSS_DATA;
            logic.index         = 0;
            logic.order         = 0;
            logic.length        = UART_VCD_BATCH_OUTPUT_SIZE;
            logic.unitsize      = 1;
            logic.data_error    = 0;
            logic.error_pattern = 0;
            logic.data          = ctx->batch_buf;
            ds_data_forward(sdi, &packet);
            ctx->batch_chunk = 0;
        }
    }
}

static void ev2_emit_samples(struct uart_vcd_context *ctx,
                              const struct sr_dev_inst *sdi,
                              uint64_t count)
{
    uint64_t i;
    for (i = 0; i < count && ctx->collecting; i++)
        emit_event_sample(ctx, sdi);
    ctx->collected_samples += count;
}

static void flush_event_output(struct uart_vcd_context *ctx,
                                const struct sr_dev_inst *sdi)
{
    int remain;
    if (ctx->event_sample_pos == 0) return;
    remain = UART_VCD_SAMPLES_PER_OUTPUT - ctx->event_sample_pos;
    while (remain-- > 0)
        emit_event_sample(ctx, sdi);
}

static void send_event_end(struct uart_vcd_context *ctx,
                            const struct sr_dev_inst *sdi)
{
    struct sr_datafeed_packet packet;
    struct sr_datafeed_logic logic;
    flush_event_output(ctx, sdi);
    if (ctx->batch_chunk > 0) {
        packet.type = SR_DF_LOGIC; packet.status = SR_PKT_OK; packet.payload = &logic;
        logic.format = LA_CROSS_DATA; logic.index = 0; logic.order = 0;
        logic.length = (uint64_t)ctx->batch_chunk * UART_VCD_OUTPUT_SIZE;
        logic.unitsize = 1; logic.data_error = 0; logic.error_pattern = 0;
        logic.data = ctx->batch_buf;
        ds_data_forward(sdi, &packet);
        ctx->batch_chunk = 0;
    }
    packet.type   = SR_DF_END;
    packet.status = SR_PKT_OK;
    ds_data_forward(sdi, &packet);
    ctx->collecting = FALSE;
}

/* ─── Protocol v2 byte parser — buffer full event before emitting ─── */

static void ev2_blow(struct uart_vcd_context *ctx, const struct sr_dev_inst *sdi)
{
    uint8_t *p   = ctx->input_buf;
    int      pos = 0;
    int      len = (int)ctx->input_len;

    if (len < 4) return; /* need at least 3B delta + 1B header */

    /* delta_ticks: 3-byte LE, 1 tick = 1 sample (both 24MHz) */
    uint32_t delta_raw = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    uint64_t delta_samples = delta_raw;
    if (delta_samples > ctx->total_samples) delta_samples = 1;
    if (delta_samples > 12000000) delta_samples = 12000000;

    pos = 3;

    /* header */
    uint8_t header = p[pos++];

    if (header & 0x80) {
        /* mode=1: string */
        int label_len = header & 0x1F;

        if (len < pos + 2) return;

        uint8_t ch_byte   = p[pos++];
        int     channel   = ch_byte & 0x07;
        uint8_t total_len = p[pos++];
        int     data_len  = (int)total_len - label_len;
        if (data_len < 0) data_len = 0;

        int payload_onwire = 2 + total_len;
        int payload_padded = (payload_onwire + 3) & ~3;

        if (len < pos + label_len + data_len + (payload_padded - payload_onwire))
            return;

        /* emit delta idle BEFORE string frame */
        ev2_emit_samples(ctx, sdi, delta_samples);

        /* feed label + data bytes to UART FIFO, set idle HIGH */
        ctx->gpio_state |= (1u << (24 + channel));
        for (int d = 0; d < label_len + data_len; d++) {
            uint8_t next = (ctx->uart_fifo_head[channel] + 1) & 0x0F;
            if (next != ctx->uart_fifo_tail[channel]) {
                ctx->uart_fifo[channel][ctx->uart_fifo_head[channel]] = p[pos + d];
                ctx->uart_fifo_head[channel] = next;
            }
        }
        pos += label_len + data_len;

        /* skip pad bytes */
        pos += payload_padded - payload_onwire;

        /* kick-start UART TX if idle */
        if (ctx->uart_tx_bit[channel] < 0 &&
            ctx->uart_fifo_head[channel] != ctx->uart_fifo_tail[channel]) {
            ctx->uart_tx_data[channel] = ctx->uart_fifo[channel][ctx->uart_fifo_tail[channel]];
            ctx->uart_fifo_tail[channel] = (ctx->uart_fifo_tail[channel] + 1) & 0x0F;
            ctx->uart_tx_bit[channel] = 0;
            ctx->uart_tx_samp_left[channel] = ctx->uart_tx_samp_per_bit;
        }

    } else {
        /* mode=0: GPIO */
        uint8_t sub = (header >> 5) & 0x03;
        int channel = header & 0x1F;

        /* emit delta IDLE before state change */
        ev2_emit_samples(ctx, sdi, delta_samples);

        if (channel <= 23) {
            uint32_t mask = 1u << channel;
            sr_dbg("ev2: GPIO sub=%u ch=%u state=0x%08x->", sub, channel, ctx->gpio_state);
            if (sub == 0)      ctx->gpio_state &= ~mask;
            else if (sub == 1) ctx->gpio_state |= mask;
            else if (sub == 2) ctx->gpio_state ^= mask;
            sr_dbg("ev2:       0x%08x delta_samples=%llu", ctx->gpio_state, (unsigned long long)delta_samples);
        }
    }

    /* remove consumed bytes from input buffer */
    ctx->input_len -= pos;
    if (ctx->input_len > 0)
        memmove(ctx->input_buf, ctx->input_buf + pos, ctx->input_len);

    if (!ctx->is_loop && ctx->collected_samples >= ctx->total_samples)
        send_event_end(ctx, sdi);
}

/* ─── Serial port ─── */

static int uart_configure(int fd, int baud_rate)
{
    struct termios tty; speed_t speed;
    if (tcgetattr(fd, &tty) != 0) { sr_err("tcgetattr failed: %s", strerror(errno)); return SR_ERR; }
    cfmakeraw(&tty); tty.c_cflag |= (CLOCAL | CREAD);
    switch (baud_rate) {
    case 9600: speed=B9600; break; case 19200: speed=B19200; break;
    case 38400: speed=B38400; break; case 57600: speed=B57600; break;
    case 115200: speed=B115200; break; case 230400: speed=B230400; break;
    case 460800: speed=B460800; break; case 500000: speed=B500000; break;
    case 576000: speed=B576000; break; case 921600: speed=B921600; break;
    case 1000000: speed=B1000000; break; default: speed=B115200; break;
    }
    cfsetispeed(&tty, speed); cfsetospeed(&tty, speed);
    tty.c_cc[VMIN] = 0; tty.c_cc[VTIME] = 1;
    if (tcsetattr(fd, TCSANOW, &tty) != 0) { sr_err("tcsetattr failed: %s", strerror(errno)); return SR_ERR; }
    return SR_OK;
}

static int uart_open(const char *port, int baud_rate)
{
    int fd = open(port, O_RDWR | O_NOCTTY);
    if (fd < 0) { sr_err("Failed to open %s: %s", port, strerror(errno)); return -1; }
    if (uart_configure(fd, baud_rate) != SR_OK) { close(fd); return -1; }
    return fd;
}

/* ─── Driver callbacks ─── */

static int hw_init(struct sr_context *sr_ctx) { return std_hw_init(sr_ctx, di, LOG_PREFIX); }
static int hw_clean_up(void) { return SR_OK; }

static GSList *hw_scan(GSList *options)
{
    struct sr_dev_inst *sdi; struct uart_vcd_context *ctx; struct drv_context *drvc;
    GSList *devices = NULL; struct stat st; (void)options;
    drvc = di->priv;
    if (drvc->instances) return devices;
    if (stat(UART_VCD_DEFAULT_SERIAL_PORT, &st) < 0) {
        sr_info("Serial port %s not found, skipping.", UART_VCD_DEFAULT_SERIAL_PORT);
        return devices;
    }
    ctx = malloc(sizeof(*ctx));
    if (!ctx) { sr_err("%s: ctx malloc failed", __func__); return devices; }
    memset(ctx, 0, sizeof(*ctx));
    sdi = sr_dev_inst_new(LOGIC, SR_ST_INACTIVE, "FTDI", "FT232R USB UART", NULL);
    if (!sdi) { safe_free(ctx); return NULL; }
    sdi->priv = ctx; sdi->driver = di; sdi->dev_type = DEV_TYPE_USB;
    ctx->serial_port = g_strdup(UART_VCD_DEFAULT_SERIAL_PORT);
    ctx->baud_rate   = UART_VCD_DEFAULT_BAUD_RATE;
    ctx->serial_fd   = -1;
    ctx->protocol    = UART_VCD_DEFAULT_PROTOCOL;
    if (ctx->protocol == UART_VCD_PROTOCOL_EVENT) {
        ctx->samplerate    = UART_VCD_EVENT_SAMPLERATE_DEFAULT;
        ctx->total_samples = UART_VCD_EVENT_DEFAULT_TOTAL_SAMPLES;
    } else {
        ctx->samplerate    = UART_VCD_DEFAULT_SAMPLERATE;
        ctx->total_samples = UART_VCD_DEFAULT_TOTAL_SAMPLES;
    }
    ctx->num_probes = UART_VCD_NUM_PROBES;
    ctx->gpio_state = 0;
    ctx->input_len  = 0;
    sdi->path = g_strdup(UART_VCD_DEFAULT_SERIAL_PORT);
    drvc->instances = g_slist_append(drvc->instances, sdi);
    devices = g_slist_append(devices, sdi);
    sr_info("uart_vcd device created: protocol=%s, samplerate=%llu, total_samples=%llu",
            ctx->protocol == UART_VCD_PROTOCOL_EVENT ? "event" : "raw",
            (unsigned long long)ctx->samplerate, (unsigned long long)ctx->total_samples);
    return devices;
}

static const GSList *hw_dev_mode_list(const struct sr_dev_inst *sdi) {
    (void)sdi; GSList *l = NULL; l = g_slist_append(l, (gpointer)&sr_mode_list[0]); return l;
}

static int hw_dev_open(struct sr_dev_inst *sdi)
{
    struct uart_vcd_context *ctx; struct sr_channel *probe; int i;
    assert(sdi); assert(sdi->priv); ctx = sdi->priv;
    if (sdi->status == SR_ST_ACTIVE) return SR_OK;
    ctx->serial_fd = uart_open(ctx->serial_port, ctx->baud_rate);
    if (ctx->serial_fd < 0) { sr_err("Failed to open serial port %s", ctx->serial_port); return SR_ERR; }
    sr_dev_probes_free(sdi);
    for (i = 0; i < ctx->num_probes; i++) {
        if (!(probe = sr_channel_new(i, SR_CHANNEL_LOGIC, TRUE, uart_vcd_probe_names[i]))) {
            sr_err("%s: create channel failed", __func__); sr_dev_inst_free(sdi); return SR_ERR;
        }
        sdi->channels = g_slist_append(sdi->channels, probe);
    }
    sdi->status = SR_ST_ACTIVE;
    return SR_OK;
}

static int hw_dev_close(struct sr_dev_inst *sdi)
{
    struct uart_vcd_context *ctx;
    if (sdi && sdi->priv) {
        ctx = sdi->priv;
        if (ctx->serial_fd >= 0) { close(ctx->serial_fd); ctx->serial_fd = -1; }
        safe_free(ctx->input_buf); safe_free(ctx->output_buf); safe_free(ctx->batch_buf);
        sdi->status = SR_ST_INACTIVE; return SR_OK;
    }
    return SR_ERR_CALL_STATUS;
}

static int dev_destroy(struct sr_dev_inst *sdi)
{
    struct uart_vcd_context *ctx; struct drv_context *drvc;
    assert(sdi); hw_dev_close(sdi);
    ctx = sdi->priv;
    if (ctx) { safe_free(ctx->serial_port); safe_free(sdi->path); }
    safe_free(ctx); sdi->priv = NULL;
    drvc = di->priv;
    if (drvc) drvc->instances = g_slist_remove(drvc->instances, sdi);
    sr_dev_inst_free(sdi);
    return SR_OK;
}

static int config_get(int id, GVariant **data, const struct sr_dev_inst *sdi,
                      const struct sr_channel *ch, const struct sr_channel_group *cg)
{
    (void)cg; struct uart_vcd_context *ctx;
    assert(sdi); assert(sdi->priv); ctx = sdi->priv;
    switch (id) {
    case SR_CONF_SAMPLERATE: *data = g_variant_new_uint64(ctx->samplerate); break;
    case SR_CONF_LIMIT_SAMPLES: *data = g_variant_new_uint64(ctx->total_samples); break;
    case SR_CONF_DEVICE_MODE: *data = g_variant_new_int16(sdi->mode); break;
    case SR_CONF_HW_DEPTH: *data = g_variant_new_uint64(ctx->total_samples); break;
    case SR_CONF_UNIT_BITS: *data = g_variant_new_byte(1); break;
    case SR_CONF_VLD_CH_NUM: *data = g_variant_new_int16(ctx->num_probes); break;
    case SR_CONF_PROBE_EN: if (ch) *data = g_variant_new_boolean(ch->enabled); else return SR_ERR; break;
    case SR_CONF_HAVE_ZERO: *data = g_variant_new_boolean(FALSE); break;
    case SR_CONF_LOAD_DECODER: *data = g_variant_new_boolean(TRUE); break;
    case SR_CONF_RLE: *data = g_variant_new_boolean(FALSE); break;
    case SR_CONF_INSTANT: *data = g_variant_new_boolean(FALSE); break;
    case SR_CONF_OPERATION_MODE: *data = g_variant_new_int16(LO_OP_STREAM); break;
    case SR_CONF_LOOP_MODE: *data = g_variant_new_boolean(ctx->is_loop); break;
    case SR_CONF_USB_SPEED: *data = g_variant_new_int32(LIBUSB_SPEED_HIGH); break;
    case SR_CONF_USB30_SUPPORT: *data = g_variant_new_boolean(FALSE); break;
    default: return SR_ERR_NA;
    }
    return SR_OK;
}

static int config_set(int id, GVariant *data, struct sr_dev_inst *sdi,
                      struct sr_channel *ch, struct sr_channel_group *cg)
{
    (void)cg; struct uart_vcd_context *ctx;
    assert(sdi); assert(sdi->priv); ctx = sdi->priv;
    switch (id) {
    case SR_CONF_SAMPLERATE: ctx->samplerate = g_variant_get_uint64(data); break;
    case SR_CONF_LIMIT_SAMPLES: ctx->total_samples = g_variant_get_uint64(data); break;
    case SR_CONF_PROBE_EN: if (ch) ch->enabled = g_variant_get_boolean(data); break;
    case SR_CONF_DEVICE_MODE: sdi->mode = g_variant_get_int16(data); break;
    case SR_CONF_LOOP_MODE: ctx->is_loop = g_variant_get_boolean(data); break;
    default: break;
    }
    return SR_OK;
}

static int config_list(int key, GVariant **data, const struct sr_dev_inst *sdi,
                       const struct sr_channel_group *cg)
{
    (void)cg; (void)sdi; GVariant *gvar; GVariantBuilder gvb;
    switch (key) {
    case SR_CONF_DEVICE_OPTIONS:
        *data = g_variant_new_from_data(G_VARIANT_TYPE("ai"), uart_vcd_hwoptions,
                ARRAY_SIZE(uart_vcd_hwoptions)*sizeof(int32_t), TRUE, NULL, NULL); break;
    case SR_CONF_DEVICE_SESSIONS:
        *data = g_variant_new_from_data(G_VARIANT_TYPE("ai"), uart_vcd_sessions,
                ARRAY_SIZE(uart_vcd_sessions)*sizeof(int32_t), TRUE, NULL, NULL); break;
    case SR_CONF_SAMPLERATE:
        g_variant_builder_init(&gvb, G_VARIANT_TYPE("a{sv}"));
        gvar = g_variant_new_from_data(G_VARIANT_TYPE("at"), uart_vcd_samplerates,
                ARRAY_SIZE(uart_vcd_samplerates)*sizeof(uint64_t), TRUE, NULL, NULL);
        g_variant_builder_add(&gvb, "{sv}", "samplerates", gvar);
        *data = g_variant_builder_end(&gvb); break;
    default: return SR_ERR_ARG;
    }
    return SR_OK;
}

static void pack_output_block(struct uart_vcd_context *ctx)
{
    const uint8_t *in = ctx->input_buf; uint8_t *out = ctx->output_buf; int ch, s, b;
    memset(out, 0, UART_VCD_OUTPUT_SIZE);
    for (ch = 0; ch < UART_VCD_NUM_PROBES; ch++) {
        for (b = 0; b < 8; b++) {
            uint8_t byte_val = 0;
            for (s = 0; s < 8; s++) {
                unsigned int sample_idx = b * 8 + s;
                const uint8_t *sample_ptr = in + sample_idx * UART_VCD_UART_BYTES_PER_SAMPLE;
                uint32_t sample = (uint32_t)sample_ptr[0] | ((uint32_t)sample_ptr[1]<<8)
                                | ((uint32_t)sample_ptr[2]<<16) | ((uint32_t)sample_ptr[3]<<24);
                if (sample & (1u<<ch)) byte_val |= (1u<<s);
            }
            out[ch*8+b] = byte_val;
        }
    }
}

static int hw_dev_acquisition_start(struct sr_dev_inst *sdi, void *cb_data)
{
    (void)cb_data; struct uart_vcd_context *ctx;
    assert(sdi); assert(sdi->priv); ctx = sdi->priv;

    ctx->collected_samples = 0;
    ctx->collecting        = TRUE;
    ctx->event_sample_pos  = 0;
    ctx->gpio_state        = 0;
    ctx->input_len         = 0;
    ctx->batch_chunk       = 0;

    ctx->uart_tx_samp_per_bit = (int)(ctx->samplerate / UART_VCD_UART_BAUD_RATE);
    memset(ctx->uart_tx_data, 0, sizeof(ctx->uart_tx_data));
    memset(ctx->uart_tx_bit, -1, sizeof(ctx->uart_tx_bit));
    memset(ctx->uart_tx_samp_left, 0, sizeof(ctx->uart_tx_samp_left));
    memset(ctx->uart_fifo_head, 0, sizeof(ctx->uart_fifo_head));
    memset(ctx->uart_fifo_tail, 0, sizeof(ctx->uart_fifo_tail));

    if (ctx->serial_fd >= 0) {
        close(ctx->serial_fd); ctx->serial_fd = -1;
        usleep(100000);
    }
    ctx->serial_fd = uart_open(ctx->serial_port, ctx->baud_rate);
    if (ctx->serial_fd < 0) {
        sr_err("Failed to open serial port %s", ctx->serial_port);
        ctx->collecting = FALSE; return SR_ERR;
    }
    tcflush(ctx->serial_fd, TCIOFLUSH);
    tcflush(ctx->serial_fd, TCIOFLUSH);
    usleep(100000);
    tcflush(ctx->serial_fd, TCIOFLUSH);

    safe_free(ctx->input_buf); safe_free(ctx->output_buf); safe_free(ctx->batch_buf);
    ctx->input_buf   = malloc(UART_VCD_BUFSIZE);
    ctx->output_buf  = malloc(UART_VCD_OUTPUT_SIZE);
    ctx->batch_buf   = malloc(UART_VCD_BATCH_OUTPUT_SIZE);
    if (!ctx->input_buf || !ctx->output_buf || !ctx->batch_buf) {
        sr_err("Failed to allocate buffers");
        safe_free(ctx->input_buf); safe_free(ctx->output_buf); safe_free(ctx->batch_buf);
        return SR_ERR_MALLOC;
    }
    memset(ctx->output_buf, 0, UART_VCD_OUTPUT_SIZE);

    sr_info("Start UART VCD acquisition on %s, baud=%d, protocol=%s, samplerate=%llu",
            ctx->serial_port, ctx->baud_rate,
            ctx->protocol == UART_VCD_PROTOCOL_EVENT ? "event" : "raw",
            (unsigned long long)ctx->samplerate);

    if (ctx->protocol == UART_VCD_PROTOCOL_EVENT)
        sr_session_source_add(ctx->serial_fd, G_IO_IN, 100, receive_data_event, sdi);
    else
        sr_session_source_add(ctx->serial_fd, G_IO_IN, 100, receive_data_raw, sdi);
    return SR_OK;
}

static int hw_dev_acquisition_stop(const struct sr_dev_inst *sdi, void *cb_data)
{
    (void)cb_data; struct uart_vcd_context *ctx;
    assert(sdi); assert(sdi->priv); ctx = sdi->priv;
    ctx->collecting = FALSE;
    if (ctx->serial_fd >= 0) { tcflush(ctx->serial_fd, TCIOFLUSH); tcflush(ctx->serial_fd, TCIOFLUSH); }
    return SR_OK;
}

static int hw_dev_status_get(const struct sr_dev_inst *sdi, struct sr_status *status, gboolean prg)
{
    (void)prg;
    if (sdi && status) { memset(status, 0, sizeof(struct sr_status)); return SR_OK; }
    return SR_ERR;
}

static int receive_data_raw(int fd, int revents, const struct sr_dev_inst *sdi)
{
    struct uart_vcd_context *ctx; struct sr_datafeed_packet packet; struct sr_datafeed_logic logic;
    uint8_t read_buf[UART_VCD_READ_BUF_SIZE]; ssize_t n; (void)fd;
    assert(sdi); assert(sdi->priv); ctx = sdi->priv;

    if (!ctx->collecting) { packet.type=SR_DF_END; packet.status=SR_PKT_OK; ds_data_forward(sdi,&packet); return FALSE; }
    if (!(revents & G_IO_IN)) return TRUE;
    n = read(fd, read_buf, sizeof(read_buf));
    if (n < 0) { sr_err("Serial read error: %s", strerror(errno)); return FALSE; }
    if (n == 0) return TRUE;
    if (ctx->input_len + (uint64_t)n > UART_VCD_BUFSIZE) ctx->input_len = 0;
    memcpy(ctx->input_buf + ctx->input_len, read_buf, n);
    ctx->input_len += (uint64_t)n;

    while (ctx->input_len >= UART_VCD_UART_BUF_SIZE) {
        pack_output_block(ctx);
        packet.type=SR_DF_LOGIC; packet.status=SR_PKT_OK; packet.payload=&logic;
        logic.format=LA_CROSS_DATA; logic.index=0; logic.order=0;
        logic.length=UART_VCD_OUTPUT_SIZE; logic.unitsize=1;
        logic.data_error=0; logic.error_pattern=0; logic.data=ctx->output_buf;
        ctx->collected_samples += UART_VCD_SAMPLES_PER_OUTPUT;
        ds_data_forward(sdi, &packet);
        if (ctx->input_len > UART_VCD_UART_BUF_SIZE) {
            uint64_t rem = ctx->input_len - UART_VCD_UART_BUF_SIZE;
            memmove(ctx->input_buf, ctx->input_buf+UART_VCD_UART_BUF_SIZE, rem);
            ctx->input_len = rem;
        } else ctx->input_len = 0;
        if (!ctx->is_loop && ctx->collected_samples >= ctx->total_samples) {
            packet.type=SR_DF_END; packet.status=SR_PKT_OK;
            ds_data_forward(sdi,&packet); ctx->collecting=FALSE; return FALSE;
        }
    }
    return TRUE;
}

static int receive_data_event(int fd, int revents, const struct sr_dev_inst *sdi)
{
    struct uart_vcd_context *ctx; struct sr_datafeed_packet packet;
    uint8_t read_buf[UART_VCD_BUFSIZE]; ssize_t n;
    (void)fd;
    assert(sdi); assert(sdi->priv); ctx = sdi->priv;

    if (!ctx->collecting) { packet.type=SR_DF_END; packet.status=SR_PKT_OK; ds_data_forward(sdi,&packet); return FALSE; }
    if (!(revents & G_IO_IN)) return TRUE;

    n = read(fd, read_buf, sizeof(read_buf));
    if (n < 0) { sr_err("Serial read error: %s", strerror(errno)); return FALSE; }
    if (n == 0) return TRUE;

    if (ctx->input_len + (uint64_t)n > UART_VCD_BUFSIZE) {
        sr_dbg("ev2: input buffer overflow, draining tail");
        ctx->input_len = 0;
        if (n > UART_VCD_BUFSIZE) return TRUE;
    }
    memcpy(ctx->input_buf + ctx->input_len, read_buf, n);
    ctx->input_len += (uint64_t)n;

    uint64_t ev_start = ctx->collected_samples;
    int ev_count = 0;
    while (ctx->input_len >= 4 && ctx->collecting &&
           ctx->collected_samples - ev_start < 786432 && ++ev_count < 512)
        ev2_blow(ctx, sdi);

    return ctx->collecting ? TRUE : FALSE;
}

SR_PRIV struct sr_dev_driver uart_vcd_driver_info = {
    .name = "uart-vcd", .longname = "FT232R USB UART VCD capture",
    .api_version = 1, .driver_type = DRIVER_TYPE_HARDWARE,
    .init = hw_init, .cleanup = hw_clean_up, .scan = hw_scan,
    .dev_mode_list = hw_dev_mode_list,
    .config_get = config_get, .config_set = config_set, .config_list = config_list,
    .dev_open = hw_dev_open, .dev_close = hw_dev_close, .dev_destroy = dev_destroy,
    .dev_status_get = hw_dev_status_get,
    .dev_acquisition_start = hw_dev_acquisition_start,
    .dev_acquisition_stop = hw_dev_acquisition_stop,
    .priv = NULL,
};
