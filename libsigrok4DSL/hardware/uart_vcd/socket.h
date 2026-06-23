/*
 * This file is part of the DSView project.
 * Copyright (C) 2024
 *
 * Socket abstraction layer for UART VCD driver.
 * Platform-specific implementations: socket_linux.c, etc.
 */

#ifndef UART_VCD_SOCKET_H
#define UART_VCD_SOCKET_H

#include <sys/types.h>

struct uart_vcd_context;

int uart_vcd_socket_connect(const char *host, int port);
int uart_vcd_socket_reconnect(struct uart_vcd_context *ctx);
void uart_vcd_socket_close(int fd);
ssize_t uart_vcd_socket_read(int fd, void *buf, size_t count);

#endif