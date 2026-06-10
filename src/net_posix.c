/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Simon Dick
 */

/* net_posix.c — host (BSD sockets) backend for net.h. */
#ifdef PLATFORM_POSIX

#include "net.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <arpa/inet.h>

int net_init(void) { return 0; }
void net_cleanup(void) {}

int net_connect(const char *host, int port)
{
    char portstr[16];
    struct addrinfo hints, *res, *ai;
    int sock = -1;

    snprintf(portstr, sizeof(portstr), "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;        /* Amiga side is IPv4-only; match it. */
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, portstr, &hints, &res) != 0)
        return -1;

    for (ai = res; ai; ai = ai->ai_next) {
        sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock < 0)
            continue;
        if (connect(sock, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        close(sock);
        sock = -1;
    }
    freeaddrinfo(res);

    if (sock >= 0) {
        int one = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
    return sock;
}

long net_read(int sock, void *buf, size_t len)
{
    return (long)recv(sock, buf, len, 0);
}

long net_write(int sock, const void *buf, size_t len)
{
    return (long)send(sock, buf, len, 0);
}

void net_close(int sock)
{
    if (sock >= 0)
        close(sock);
}

unsigned long net_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * 1000UL + (unsigned long)(ts.tv_nsec / 1000000L);
}

#endif /* PLATFORM_POSIX */
