/*
 * This file is part of the DSView project.
 * Copyright (C) 2024
 *
 * Windows socket implementation for UART VCD driver.
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "socket.h"
#include "uart_vcd.h"
#include "../../log.h"

#undef LOG_PREFIX
#define LOG_PREFIX "uart_vcd: "

static int winsock_initialized = 0;

static int ensure_winsock(void)
{
    if (!winsock_initialized) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            sr_err("WSAStartup failed");
            return SR_ERR;
        }
        winsock_initialized = 1;
    }
    return SR_OK;
}

static SOCKET tcp_connect_socket(const char *host, int port)
{
    SOCKET fd;
    struct sockaddr_in addr;
    struct hostent *he;
    u_long nonblock = 1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET) {
        sr_err("TCP socket: %s", strerror(errno));
        return INVALID_SOCKET;
    }

    he = gethostbyname(host);
    if (!he) {
        sr_err("TCP resolve: %s", host);
        closesocket(fd);
        return INVALID_SOCKET;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    memcpy(&addr.sin_addr, he->h_addr, he->h_length);

    ioctlsocket(fd, FIONBIO, &nonblock);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) {
            sr_err("TCP connect %s:%d: %s", host, port, strerror(errno));
            closesocket(fd);
            return INVALID_SOCKET;
        }

        struct timeval tv;
        fd_set wset;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        FD_ZERO(&wset);
        FD_SET(fd, &wset);

        int ret = select(0, NULL, &wset, NULL, &tv);
        if (ret <= 0) {
            sr_err("TCP connect %s:%d timeout", host, port);
            closesocket(fd);
            return INVALID_SOCKET;
        }

        int err = 0;
        int len = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
        if (err) {
            sr_err("TCP connect %s:%d: %s", host, port, strerror(err));
            closesocket(fd);
            return INVALID_SOCKET;
        }
    }

    sr_info("TCP connected to %s:%d", host, port);
    return fd;
}

int uart_vcd_socket_connect(const char *host, int port)
{
    if (ensure_winsock() != SR_OK)
        return -1;

    SOCKET sock = tcp_connect_socket(host, port);
    if (sock == INVALID_SOCKET)
        return -1;

    return (int)sock;
}

int uart_vcd_socket_reconnect(struct uart_vcd_context *ctx)
{
    if (ensure_winsock() != SR_OK)
        return SR_ERR;

    if (ctx->tcp_fd >= 0) {
        if (ctx->wsa_event) {
            WSAEventSelect((SOCKET)ctx->tcp_fd, NULL, 0);
            WSACloseEvent(ctx->wsa_event);
            ctx->wsa_event = NULL;
        }
        closesocket((SOCKET)ctx->tcp_fd);
        ctx->tcp_fd = -1;
    }

    sr_info("TCP connecting to %s:%d...", ctx->tcp_host, ctx->tcp_port);
    ctx->tcp_fd = uart_vcd_socket_connect(ctx->tcp_host, ctx->tcp_port);
    if (ctx->tcp_fd < 0) {
        sr_err("TCP connect failed");
        return SR_ERR;
    }

    SOCKET sock = (SOCKET)ctx->tcp_fd;
    u_long nonblock = 1;
    ioctlsocket(sock, FIONBIO, &nonblock);

    {
        int rcvbuf = 524288;
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char*)&rcvbuf, sizeof(rcvbuf));
    }

    {
        uint8_t d[1024];
        while (recv(sock, (char*)d, (int)sizeof(d), 0) > 0);
    }

    ctx->wsa_event = WSACreateEvent();
    if (ctx->wsa_event == WSA_INVALID_EVENT) {
        sr_err("WSACreateEvent failed");
        closesocket(sock);
        ctx->tcp_fd = -1;
        return SR_ERR;
    }
    if (WSAEventSelect(sock, ctx->wsa_event, FD_READ | FD_CLOSE) == SOCKET_ERROR) {
        sr_err("WSAEventSelect failed");
        WSACloseEvent(ctx->wsa_event);
        ctx->wsa_event = NULL;
        closesocket(sock);
        ctx->tcp_fd = -1;
        return SR_ERR;
    }

    ctx->input_len = 0;
    ctx->input_offset = 0;
    ctx->gpio_state = 0;
    ctx->first_event = TRUE;
    ctx->sync_seen = FALSE;
    return SR_OK;
}

void uart_vcd_socket_close(int fd)
{
    if (fd >= 0) {
        SOCKET sock = (SOCKET)fd;
        closesocket(sock);
    }
}

ssize_t uart_vcd_socket_read(int fd, void *buf, size_t count)
{
    int ret = recv((SOCKET)fd, (char*)buf, (int)count, 0);
    if (ret == SOCKET_ERROR) {
        int wsa_err = WSAGetLastError();
        if (wsa_err == WSAEWOULDBLOCK) {
            errno = EAGAIN;
        } else {
            errno = EIO;
        }
        return -1;
    }
    return (ssize_t)ret;
}