/*
 * This file is part of the DSView project.
 * Copyright (C) 2024
 *
 * Linux/POSIX socket implementation for UART VCD driver.
 */

#define _GNU_SOURCE
#include "socket.h"
#include "uart_vcd.h"
#include "../../log.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netdb.h>

#undef LOG_PREFIX
#define LOG_PREFIX "uart_vcd: "

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

int uart_vcd_socket_connect(const char *host, int port)
{
    return tcp_connect(host, port);
}

int uart_vcd_socket_reconnect(struct uart_vcd_context *ctx)
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
    ctx->sync_seen=FALSE;
    return SR_OK;
}

void uart_vcd_socket_close(int fd)
{
    if (fd >= 0)
        close(fd);
}

ssize_t uart_vcd_socket_read(int fd, void *buf, size_t count)
{
    return read(fd, buf, count);
}