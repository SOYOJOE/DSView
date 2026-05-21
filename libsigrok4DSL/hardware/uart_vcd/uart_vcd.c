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

SR_PRIV struct sr_dev_driver uart_vcd_driver_info;
static struct sr_dev_driver *di = &uart_vcd_driver_info;

static int uart_configure(int fd, int baud_rate)
{
    struct termios tty;
    speed_t speed;

    if (tcgetattr(fd, &tty) != 0) {
        sr_err("tcgetattr failed: %s", strerror(errno));
        return SR_ERR;
    }

    cfmakeraw(&tty);

    tty.c_cflag |= (CLOCAL | CREAD);

    switch (baud_rate) {
    case 9600:    speed = B9600;    break;
    case 19200:   speed = B19200;   break;
    case 38400:   speed = B38400;   break;
    case 57600:   speed = B57600;   break;
    case 115200:  speed = B115200;  break;
    case 230400:  speed = B230400;  break;
    case 460800:  speed = B460800;  break;
    case 500000:  speed = B500000;  break;
    case 576000:  speed = B576000;  break;
    case 921600:  speed = B921600;  break;
    case 1000000: speed = B1000000; break;
    default:      speed = B115200;  break;
    }

    cfsetispeed(&tty, speed);
    cfsetospeed(&tty, speed);

    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 1;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        sr_err("tcsetattr failed: %s", strerror(errno));
        return SR_ERR;
    }

    return SR_OK;
}

static int uart_open(const char *port, int baud_rate)
{
    int fd;

    fd = open(port, O_RDWR | O_NOCTTY);
    if (fd < 0) {
        sr_err("Failed to open %s: %s", port, strerror(errno));
        return -1;
    }

    if (uart_configure(fd, baud_rate) != SR_OK) {
        close(fd);
        return -1;
    }

    return fd;
}

static int hw_init(struct sr_context *sr_ctx)
{
    return std_hw_init(sr_ctx, di, LOG_PREFIX);
}

static int hw_clean_up(void)
{
    return SR_OK;
}

static GSList *hw_scan(GSList *options)
{
    struct sr_dev_inst *sdi;
    struct uart_vcd_context *ctx;
    GSList *devices;
    struct stat st;

    (void)options;
    devices = NULL;

    if (stat(UART_VCD_DEFAULT_SERIAL_PORT, &st) < 0) {
        sr_info("Serial port %s not found, skipping.", UART_VCD_DEFAULT_SERIAL_PORT);
        return devices;
    }

    ctx = malloc(sizeof(struct uart_vcd_context));
    if (ctx == NULL) {
        sr_err("%s: ctx malloc failed", __func__);
        return devices;
    }
    memset(ctx, 0, sizeof(struct uart_vcd_context));

    sdi = sr_dev_inst_new(LOGIC, SR_ST_INACTIVE,
                          "FTDI", "FT232R USB UART", NULL);
    if (!sdi) {
        safe_free(ctx);
        sr_err("Device instance creation failed.");
        return NULL;
    }

    sdi->priv = ctx;
    sdi->driver = di;
    sdi->dev_type = DEV_TYPE_SERIAL;

    ctx->serial_port = g_strdup(UART_VCD_DEFAULT_SERIAL_PORT);
    ctx->baud_rate = UART_VCD_DEFAULT_BAUD_RATE;
    ctx->serial_fd = -1;
    ctx->samplerate = UART_VCD_DEFAULT_SAMPLERATE;
    ctx->total_samples = UART_VCD_DEFAULT_TOTAL_SAMPLES;
    ctx->collected_samples = 0;
    ctx->num_probes = UART_VCD_NUM_PROBES;
    ctx->collecting = FALSE;

    sdi->path = g_strdup(UART_VCD_DEFAULT_SERIAL_PORT);

    devices = g_slist_append(devices, sdi);

    return devices;
}

static const GSList *hw_dev_mode_list(const struct sr_dev_inst *sdi)
{
    (void)sdi;

    GSList *l = NULL;
    l = g_slist_append(l, (gpointer)&sr_mode_list[0]);
    return l;
}

static int hw_dev_open(struct sr_dev_inst *sdi)
{
    struct uart_vcd_context *ctx;
    struct sr_channel *probe;
    int i;

    assert(sdi);
    assert(sdi->priv);

    if (sdi->status == SR_ST_ACTIVE) {
        return SR_OK;
    }

    ctx = sdi->priv;

    ctx->serial_fd = uart_open(ctx->serial_port, ctx->baud_rate);
    if (ctx->serial_fd < 0) {
        sr_err("Failed to open serial port %s", ctx->serial_port);
        return SR_ERR;
    }

    sr_dev_probes_free(sdi);

    for (i = 0; i < ctx->num_probes; i++) {
        if (!(probe = sr_channel_new(i, SR_CHANNEL_LOGIC, TRUE, uart_vcd_probe_names[i]))) {
            sr_err("%s: create channel failed", __func__);
            sr_dev_inst_free(sdi);
            return SR_ERR;
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

        if (ctx->serial_fd >= 0) {
            close(ctx->serial_fd);
            ctx->serial_fd = -1;
        }

        sdi->status = SR_ST_INACTIVE;
        return SR_OK;
    }

    return SR_ERR_CALL_STATUS;
}

static int dev_destroy(struct sr_dev_inst *sdi)
{
    struct uart_vcd_context *ctx;

    assert(sdi);

    hw_dev_close(sdi);

    ctx = sdi->priv;
    if (ctx) {
        safe_free(ctx->serial_port);
        safe_free(sdi->path);
    }

    safe_free(ctx);
    sdi->priv = NULL;
    sr_dev_inst_free(sdi);

    return SR_OK;
}

static int config_get(int id, GVariant **data, const struct sr_dev_inst *sdi,
                      const struct sr_channel *ch,
                      const struct sr_channel_group *cg)
{
    (void)cg;

    struct uart_vcd_context *ctx;

    assert(sdi);
    assert(sdi->priv);

    ctx = sdi->priv;

    switch (id) {
    case SR_CONF_SAMPLERATE:
        *data = g_variant_new_uint64(ctx->samplerate);
        break;
    case SR_CONF_LIMIT_SAMPLES:
        *data = g_variant_new_uint64(ctx->total_samples);
        break;
    case SR_CONF_DEVICE_MODE:
        *data = g_variant_new_int16(sdi->mode);
        break;
    case SR_CONF_HW_DEPTH:
        *data = g_variant_new_uint64(ctx->total_samples);
        break;
    case SR_CONF_UNIT_BITS:
        *data = g_variant_new_byte(1);
        break;
    case SR_CONF_VLD_CH_NUM:
        *data = g_variant_new_int16(ctx->num_probes);
        break;
    case SR_CONF_PROBE_EN:
        if (ch)
            *data = g_variant_new_boolean(ch->enabled);
        else
            return SR_ERR;
        break;
    case SR_CONF_HAVE_ZERO:
        *data = g_variant_new_boolean(FALSE);
        break;
    case SR_CONF_LOAD_DECODER:
        *data = g_variant_new_boolean(FALSE);
        break;
    case SR_CONF_RLE:
        *data = g_variant_new_boolean(FALSE);
        break;
    case SR_CONF_INSTANT:
        *data = g_variant_new_boolean(FALSE);
        break;
    case SR_CONF_OPERATION_MODE:
        *data = g_variant_new_int16(LOGIC);
        break;
    default:
        return SR_ERR_NA;
    }

    return SR_OK;
}

static int config_set(int id, GVariant *data, struct sr_dev_inst *sdi,
                      struct sr_channel *ch,
                      struct sr_channel_group *cg)
{
    (void)cg;

    struct uart_vcd_context *ctx;

    assert(sdi);
    assert(sdi->priv);

    ctx = sdi->priv;

    switch (id) {
    case SR_CONF_SAMPLERATE:
        ctx->samplerate = g_variant_get_uint64(data);
        sr_dbg("Setting samplerate to %llu.", (unsigned long long)ctx->samplerate);
        break;
    case SR_CONF_LIMIT_SAMPLES:
        ctx->total_samples = g_variant_get_uint64(data);
        sr_dbg("Setting limit samples to %llu.", (unsigned long long)ctx->total_samples);
        break;
    case SR_CONF_PROBE_EN:
        if (ch)
            ch->enabled = g_variant_get_boolean(data);
        break;
    case SR_CONF_DEVICE_MODE:
        sdi->mode = g_variant_get_int16(data);
        break;
    case SR_CONF_TRIGGER_SOURCE:
    case SR_CONF_TRIGGER_SLOPE:
    case SR_CONF_TRIGGER_VALUE:
    case SR_CONF_INSTANT:
    case SR_CONF_OPERATION_MODE:
    case SR_CONF_RLE:
    case SR_CONF_TEST:
    case SR_CONF_USB_SPEED:
    case SR_CONF_USB30_SUPPORT:
    case SR_CONF_WAIT_UPLOAD:
    case SR_CONF_CLOCK_TYPE:
    case SR_CONF_CLOCK_EDGE:
    case SR_CONF_RLE_SUPPORT:
    case SR_CONF_STREAM:
        break;
    default:
        return SR_ERR_NA;
    }

    return SR_OK;
}

static int config_list(int key, GVariant **data, const struct sr_dev_inst *sdi,
                       const struct sr_channel_group *cg)
{
    (void)cg;
    (void)sdi;

    GVariant *gvar;
    GVariantBuilder gvb;

    switch (key) {
    case SR_CONF_DEVICE_OPTIONS:
        *data = g_variant_new_from_data(G_VARIANT_TYPE("ai"),
                                        uart_vcd_hwoptions, ARRAY_SIZE(uart_vcd_hwoptions) * sizeof(int32_t), TRUE, NULL, NULL);
        break;
    case SR_CONF_DEVICE_SESSIONS:
        *data = g_variant_new_from_data(G_VARIANT_TYPE("ai"),
                                        uart_vcd_sessions, ARRAY_SIZE(uart_vcd_sessions) * sizeof(int32_t), TRUE, NULL, NULL);
        break;
    case SR_CONF_SAMPLERATE:
        g_variant_builder_init(&gvb, G_VARIANT_TYPE("a{sv}"));
        gvar = g_variant_new_from_data(G_VARIANT_TYPE("at"),
                                       uart_vcd_samplerates, ARRAY_SIZE(uart_vcd_samplerates) * sizeof(uint64_t), TRUE, NULL, NULL);
        g_variant_builder_add(&gvb, "{sv}", "samplerates", gvar);
        *data = g_variant_builder_end(&gvb);
        break;
    default:
        return SR_ERR_ARG;
    }

    return SR_OK;
}

static int hw_dev_acquisition_start(struct sr_dev_inst *sdi, void *cb_data)
{
    (void)cb_data;

    struct uart_vcd_context *ctx;

    assert(sdi);
    assert(sdi->priv);

    ctx = sdi->priv;
    ctx->collected_samples = 0;
    ctx->collecting = TRUE;

    sr_info("Start UART VCD acquisition on %s, baud=%d, samplerate=%llu",
            ctx->serial_port, ctx->baud_rate,
            (unsigned long long)ctx->samplerate);

    sr_session_source_add(ctx->serial_fd, G_IO_IN, 100, receive_data, sdi);

    return SR_OK;
}

static int hw_dev_acquisition_stop(const struct sr_dev_inst *sdi, void *cb_data)
{
    (void)cb_data;

    struct uart_vcd_context *ctx;
    struct sr_datafeed_packet packet;

    assert(sdi);
    assert(sdi->priv);

    ctx = sdi->priv;
    ctx->collecting = FALSE;

    if (ctx->serial_fd >= 0) {
        close(ctx->serial_fd);
        ctx->serial_fd = -1;
    }

    packet.type = SR_DF_END;
    packet.status = SR_PKT_OK;
    ds_data_forward(sdi, &packet);

    return SR_OK;
}

static int hw_dev_status_get(const struct sr_dev_inst *sdi, struct sr_status *status, gboolean prg)
{
    (void)prg;

    if (sdi && status) {
        memset(status, 0, sizeof(struct sr_status));
        return SR_OK;
    }

    return SR_ERR;
}

static int receive_data(int fd, int revents, const struct sr_dev_inst *sdi)
{
    struct uart_vcd_context *ctx;
    struct sr_datafeed_packet packet;
    struct sr_datafeed_logic logic;
    uint8_t buf[UART_VCD_BUFSIZE];
    ssize_t n;

    (void)fd;

    assert(sdi);
    assert(sdi->priv);

    ctx = sdi->priv;

    if (!ctx->collecting) {
        return FALSE;
    }

    if (!(revents & G_IO_IN)) {
        return TRUE;
    }

    n = read(fd, buf, sizeof(buf));
    if (n < 0) {
        sr_err("Serial read error: %s", strerror(errno));
        return FALSE;
    }

    if (n == 0) {
        return TRUE;
    }

    packet.type = SR_DF_LOGIC;
    packet.status = SR_PKT_OK;
    packet.payload = &logic;
    logic.format = LA_CROSS_DATA;
    logic.index = 0;
    logic.order = 0;
    logic.length = (uint64_t)n;
    logic.unitsize = 1;
    logic.data_error = 0;
    logic.error_pattern = 0;
    logic.data = buf;

    ctx->collected_samples += (uint64_t)n;

    ds_data_forward(sdi, &packet);

    if (ctx->collected_samples >= ctx->total_samples) {
        packet.type = SR_DF_END;
        packet.status = SR_PKT_OK;
        ds_data_forward(sdi, &packet);
        ctx->collecting = FALSE;
        return FALSE;
    }

    return TRUE;
}

SR_PRIV struct sr_dev_driver uart_vcd_driver_info = {
    .name = "uart-vcd",
    .longname = "FT232R USB UART VCD capture",
    .api_version = 1,
    .driver_type = DRIVER_TYPE_HARDWARE,
    .init = hw_init,
    .cleanup = hw_clean_up,
    .scan = hw_scan,
    .dev_mode_list = hw_dev_mode_list,
    .config_get = config_get,
    .config_set = config_set,
    .config_list = config_list,
    .dev_open = hw_dev_open,
    .dev_close = hw_dev_close,
    .dev_destroy = dev_destroy,
    .dev_status_get = hw_dev_status_get,
    .dev_acquisition_start = hw_dev_acquisition_start,
    .dev_acquisition_stop = hw_dev_acquisition_stop,
    .priv = NULL,
};
