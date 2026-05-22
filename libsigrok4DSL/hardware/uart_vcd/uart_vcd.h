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

#ifndef LIBDSL_HARDWARE_UART_VCD_H
#define LIBDSL_HARDWARE_UART_VCD_H

#include <glib.h>
#include "../../libsigrok-internal.h"

#define UART_VCD_DEFAULT_SERIAL_PORT "/dev/ttyUSB0"
#define UART_VCD_DEFAULT_BAUD_RATE   1000000
#define UART_VCD_NUM_PROBES          32
#define UART_VCD_DEFAULT_SAMPLERATE  SR_KHZ(100)
#define UART_VCD_DEFAULT_TOTAL_SAMPLES SR_Mn(100)
#define UART_VCD_BUFSIZE             (1024 * 64)
#define UART_VCD_INPUT_WORD_BYTES    4
#define UART_VCD_SAMPLES_PER_CHUNK   (8 * UART_VCD_INPUT_WORD_BYTES)
#define UART_VCD_OUTPUT_CHUNK        (UART_VCD_NUM_PROBES * UART_VCD_INPUT_WORD_BYTES)

struct uart_vcd_context {
    int        serial_fd;
    char      *serial_port;
    int        baud_rate;
    uint64_t   samplerate;
    uint64_t   total_samples;
    uint64_t   collected_samples;
    int        num_probes;
    gboolean   collecting;
    gboolean   is_loop;
    uint8_t   *input_buf;
    uint64_t   input_len;
    uint8_t   *output_buf;
};

static const uint64_t uart_vcd_samplerates[] = {
    SR_MHZ(1),
};

static const char *uart_vcd_probe_names[] = {
    "D0",  "D1",  "D2",  "D3",  "D4",  "D5",  "D6",  "D7",
    "D8",  "D9",  "D10", "D11", "D12", "D13", "D14", "D15",
    "D16", "D17", "D18", "D19", "D20", "D21", "D22", "D23",
    "D24", "D25", "D26", "D27", "D28", "D29", "D30", "D31",
    NULL,
};

static const int32_t uart_vcd_hwoptions[] = {
    SR_CONF_LOOP_MODE,
};

static const int32_t uart_vcd_sessions[] = {
    SR_CONF_SAMPLERATE,
    SR_CONF_LIMIT_SAMPLES,
};

static int hw_init(struct sr_context *sr_ctx);
static int hw_clean_up(void);
static GSList *hw_scan(GSList *options);
static const GSList *hw_dev_mode_list(const struct sr_dev_inst *sdi);
static int hw_dev_open(struct sr_dev_inst *sdi);
static int hw_dev_close(struct sr_dev_inst *sdi);
static int dev_destroy(struct sr_dev_inst *sdi);
static int config_get(int id, GVariant **data, const struct sr_dev_inst *sdi,
                      const struct sr_channel *ch,
                      const struct sr_channel_group *cg);
static int config_set(int id, GVariant *data, struct sr_dev_inst *sdi,
                      struct sr_channel *ch,
                      struct sr_channel_group *cg);
static int config_list(int key, GVariant **data, const struct sr_dev_inst *sdi,
                       const struct sr_channel_group *cg);
static int hw_dev_acquisition_start(struct sr_dev_inst *sdi, void *cb_data);
static int hw_dev_acquisition_stop(const struct sr_dev_inst *sdi, void *cb_data);
static int hw_dev_status_get(const struct sr_dev_inst *sdi, struct sr_status *status, gboolean prg);
static int receive_data(int fd, int revents, const struct sr_dev_inst *sdi);

#endif
