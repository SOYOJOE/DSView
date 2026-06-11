/*
 * This file is part of the DSView project.
 * Copyright (C) 2024
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef LIBDSL_HARDWARE_UART_VCD_H
#define LIBDSL_HARDWARE_UART_VCD_H

#include <glib.h>
#include "../../libsigrok-internal.h"

#define UART_VCD_DEFAULT_TCP_PORT    12345
#define UART_VCD_DEFAULT_TCP_HOST    "192.168.100.135"
#define UART_VCD_NUM_PROBES          32
#define UART_VCD_BUFSIZE             (1024 * 1024)
#define UART_VCD_EVENT_SAMPLERATE_DEFAULT  24000000
#define UART_VCD_EVENT_DEFAULT_TOTAL_SAMPLES  SR_Mn(10)
#define UART_VCD_UART_BAUD_RATE      (UART_VCD_EVENT_SAMPLERATE_DEFAULT/4)
#define UART_VCD_PROTOCOL_RAW        0
#define UART_VCD_PROTOCOL_EVENT      1
#define UART_VCD_DEFAULT_PROTOCOL    UART_VCD_PROTOCOL_EVENT

struct uart_vcd_context {
    int        tcp_fd;
    int        tcp_port;
    uint64_t   samplerate;
    uint64_t   total_samples;
    uint64_t   collected_samples;
    int        num_probes;
    gboolean   collecting;
    gboolean   is_loop;
    uint8_t   *input_buf;
    uint64_t   input_len;
    uint8_t   *output_buf;
    uint8_t   *batch_buf;
    int        batch_chunk;
    int        protocol;
    uint32_t   gpio_state;
    int        event_sample_pos;
    uint8_t    uart_tx_data[8];
    int8_t     uart_tx_bit[8];
    int8_t     uart_tx_samp_left[8];
    int8_t     uart_tx_samp_per_bit;
    uint8_t    uart_fifo[8][64];
    uint8_t    uart_fifo_head[8];
    uint8_t    uart_fifo_tail[8];
    int        uart_tx_active;
};

static const uint64_t uart_vcd_samplerates[] = { 24000000 };

static const char *uart_vcd_probe_names[] = {
    "D0",  "D1",  "D2",  "D3",  "D4",  "D5",  "D6",  "D7",
    "D8",  "D9",  "D10", "D11", "D12", "D13", "D14", "D15",
    "D16", "D17", "D18", "D19", "D20", "D21", "D22", "D23",
    "RX0", "RX1", "RX2", "RX3", "RX4", "RX5", "RX6", "RX7",
    NULL,
};

static const int32_t uart_vcd_hwoptions[] = { SR_CONF_LOOP_MODE };
static const int32_t uart_vcd_sessions[]  = { SR_CONF_SAMPLERATE, SR_CONF_LIMIT_SAMPLES };

#endif
