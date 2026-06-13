/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Simon Dick
 */

/* net.h — minimal blocking-socket + monotonic-clock abstraction.
 *
 * Two backends implement this interface:
 *   net_posix.c  (PLATFORM_POSIX) — host build, for protocol validation
 *   net_amiga.c  (PLATFORM_AMIGA) — bsdsocket.library, the on-target build
 *
 * All functions are blocking. Return values follow the POSIX convention:
 * >=0 on success, <0 on error.
 */
#ifndef NARRATOR_NET_H
#define NARRATOR_NET_H

#include <stddef.h>

/* One-time TCP stack init/teardown (opens bsdsocket.library on Amiga). */
int  net_init(void);
void net_cleanup(void);

/* Resolve host (name or dotted IP) and open a blocking TCP connection.
 * Returns a socket handle (>=0) or <0 on failure. */
int  net_connect(const char *host, int port);

/* Blocking read/write. net_read returns bytes read (0 = peer closed),
 * net_write returns bytes written, or <0 on error. */
long net_read(int sock, void *buf, size_t len);
long net_write(int sock, const void *buf, size_t len);

void net_close(int sock);

/* Monotonic-ish wall-clock milliseconds, for latency timing. */
unsigned long net_now_ms(void);

#endif /* NARRATOR_NET_H */
