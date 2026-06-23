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
#define UART_VCD_NUM_PROBES          28
#define UART_VCD_GPIO_PROBES         28
#define UART_VCD_TEXT_CHANNELS       4
#define UART_VCD_GPIO_MASK           0x0fffffffu
#define UART_VCD_NON_GPIO_MASK       0xf0000000u
#define UART_VCD_BUFSIZE             (1024 * 1024)
#define UART_VCD_EVENT_SAMPLERATE_DEFAULT  24000000
#define UART_VCD_EVENT_DEFAULT_TOTAL_SAMPLES  SR_Mn(10)
#define UART_VCD_MAX_HW_DEPTH             SR_Mn(25000)
#define UART_VCD_EVENT_BATCH_SIZE    4096

struct uart_vcd_context {
    int        tcp_fd;
    int        tcp_port;
    char       tcp_host[256];
    uint64_t   samplerate;
    uint64_t   total_samples;
    uint64_t   collected_samples;
    int        num_probes;
    gboolean   collecting;
    gboolean   end_sent;
    gboolean   is_loop;
    gboolean   first_event;
    uint8_t   *input_buf;
    uint64_t   input_len;
    uint64_t   input_offset;
    struct sr_logic_sparse_event *event_buf;
    uint32_t   event_count;
    uint32_t   gpio_state;
    uint32_t   output_state;
    uint32_t   recorded_state;
    uint32_t   activity_mask;
    uint64_t   activity_report_sample;
    uint64_t   parsed_events;
    uint64_t   bad_packets;
    uint64_t   recovered_packets;
    uint64_t   dropped_input_bytes;
    gboolean   sync_seen;
#ifdef _WIN32
    void      *wsa_event;
#endif
};

static const uint64_t uart_vcd_samplerates[] = { 24000000 };

static const char *uart_vcd_probe_names[] = {
    "D0",  "D1",  "D2",  "D3",  "D4",  "D5",  "D6",  "D7",
    "D8",  "D9",  "D10", "D11", "D12", "D13", "D14", "D15",
    "D16", "D17", "D18", "D19", "D20", "D21", "D22", "D23",
    "D24", "D25", "D26", "D27",
    NULL,
};

static const int32_t uart_vcd_hwoptions[] = { SR_CONF_LOOP_MODE, SR_CONF_TCP_HOST };
static const int32_t uart_vcd_sessions[]  = { SR_CONF_SAMPLERATE, SR_CONF_LIMIT_SAMPLES, SR_CONF_TCP_HOST };

#endif
