/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Simon Dick
 */

/* net_amiga.c — Amiga bsdsocket.library backend for net.h.
 *
 * Sockets come from the TCP/IP stack's bsdsocket.library (Roadshow / AmiTCP);
 * they are closed with CloseSocket() (NOT exec/dos Close). Monotonic timing
 * for the latency measurement comes from timer.device's E-Clock (ReadEClock),
 * which is high-resolution and avoids the struct timeval clash between the
 * NDK's sys/time.h and devices/timer.h.
 */
#ifdef PLATFORM_AMIGA

#include "net.h"

/* Header order matters on the NDK:
 *  - sys/types.h first: the socket headers use ssize_t/in_addr_t/socklen_t
 *    without pulling it in themselves.
 *  - The C network headers (sys/socket.h, arpa/inet.h, ...) must precede
 *    proto/bsdsocket.h. proto/bsdsocket.h defines statement-expression macros
 *    for inet_addr()/inet_aton()/etc. and then re-includes arpa/inet.h; if the
 *    prototype hasn't already been parsed (and its include guard set), the
 *    macro mangles the re-declaration. Include them first so the guard wins.
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>

#include <exec/io.h>
#include <devices/timer.h>
#include <bsdsocket/socketbasetags.h>

#include <proto/exec.h>
#include <proto/bsdsocket.h>
#include <proto/timer.h>

#include <errno.h>
#include <string.h>

struct Library *SocketBase = NULL;

/* timer.device (UNIT_ECLOCK) for monotonic timing. proto/timer.h declares
 * `extern struct Device *TimerBase;` and its ReadEClock inline references it,
 * so we provide the definition here. */
struct Device       *TimerBase    = NULL;
static struct MsgPort     *g_timerPort = NULL;
static struct timerequest *g_timerReq  = NULL;
static unsigned long       g_eclockHz  = 0;

int net_init(void)
{
    SocketBase = OpenLibrary((STRPTR)"bsdsocket.library", 4);
    if (!SocketBase)
        return -1;

    /* Tell bsdsocket where this task's errno lives. Mandatory: without it, any
     * socket call that sets errno (e.g. recv hitting EWOULDBLOCK) writes through
     * an unset pointer and corrupts memory -> Guru. connect()/send() happened to
     * succeed without setting errno, which is why only recv() crashed. */
    SocketBaseTags(
        SBTM_SETVAL(SBTC_ERRNOPTR(sizeof(errno))), (long)&errno,
        TAG_END);

    /* Best-effort timer setup; if it fails, net_now_ms() returns 0 and the
     * latency figures read as 0 rather than crashing. */
    g_timerPort = CreateMsgPort();
    if (g_timerPort) {
        g_timerReq = (struct timerequest *)
            CreateIORequest(g_timerPort, sizeof(struct timerequest));
        if (g_timerReq &&
            OpenDevice((unsigned char *)"timer.device", UNIT_ECLOCK,
                       (struct IORequest *)g_timerReq, 0) == 0) {
            struct EClockVal ev;
            TimerBase  = g_timerReq->tr_node.io_Device;
            g_eclockHz = ReadEClock(&ev);
        }
    }
    return 0;
}

void net_cleanup(void)
{
    if (TimerBase) {
        CloseDevice((struct IORequest *)g_timerReq);
        TimerBase = NULL;
    }
    if (g_timerReq) {
        DeleteIORequest((struct IORequest *)g_timerReq);
        g_timerReq = NULL;
    }
    if (g_timerPort) {
        DeleteMsgPort(g_timerPort);
        g_timerPort = NULL;
    }
    if (SocketBase) {
        CloseLibrary(SocketBase);
        SocketBase = NULL;
    }
}

int net_connect(const char *host, int port)
{
    struct sockaddr_in sa;
    int sock;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);

    /* Try dotted-quad first; fall back to a name lookup. */
    sa.sin_addr.s_addr = inet_addr((STRPTR)host);
    if (sa.sin_addr.s_addr == (unsigned long)-1) {
        struct hostent *he = gethostbyname((STRPTR)host);
        if (!he || !he->h_addr_list[0])
            return -1;
        memcpy(&sa.sin_addr, he->h_addr_list[0], sizeof(sa.sin_addr));
    }

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return -1;

    if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        CloseSocket(sock);
        return -1;
    }
    return sock;
}

long net_read(int sock, void *buf, size_t len)
{
    return (long)recv(sock, buf, len, 0);
}

long net_write(int sock, const void *buf, size_t len)
{
    return (long)send(sock, (void *)buf, len, 0);
}

void net_close(int sock)
{
    if (sock >= 0)
        CloseSocket(sock);
}

unsigned long net_now_ms(void)
{
    /* 32-bit math only. Bebbo's 64-bit libgcc helpers (__udivdi3 etc.) can pull
     * in FP-tainted code that traps (Guru #8000000B) on a strict/FPU-less
     * config, so avoid 64-bit entirely. The EClock low word alone wraps roughly
     * every 2^32 / eclockHz seconds (~6000s on PAL) -- fine for the short
     * deltas we measure; a wrap mid-measurement would just produce one bad
     * sample. */
    struct EClockVal ev;
    unsigned long lo, ticks_per_ms;

    if (!TimerBase || !g_eclockHz)
        return 0;

    ReadEClock(&ev);
    lo = (unsigned long)ev.ev_lo;
    ticks_per_ms = g_eclockHz / 1000UL;
    if (!ticks_per_ms)
        return 0;
    return lo / ticks_per_ms;
}

#endif /* PLATFORM_AMIGA */
