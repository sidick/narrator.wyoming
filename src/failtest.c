/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Simon Dick
 */

/* failtest.c -- exercise narrator.device's graceful-failure paths.
 *
 * Each scenario rewrites ENV:narrator.wyoming with a known-broken config,
 * opens the device once, submits a single CMD_WRITE, prints io_Error +
 * io_Actual, and closes.  The original prefs are saved and restored at
 * the end of the run.
 *
 *   Scenario 1 -- unreachable IP (192.168.99.99 by convention).  The
 *                 engine's non-blocking connect should hit
 *                 NW_CONNECT_TIMEOUT (5 s) and io_Error should come back
 *                 non-zero (io_Actual = NWERR_CONNECT magnitude = 103).
 *                 Bare minimum: no hang.
 *
 *   Scenario 2 -- DNS that won't resolve ("no.such.host.invalid").  The
 *                 engine's gethostbyname() should fail fast and return
 *                 NWERR_RESOLVE (io_Actual = 102).  io_Error non-zero,
 *                 finishes within a second or so.
 *
 * (A "bad voice on a real Piper server" scenario was tried and dropped
 * for failtest: wyoming-piper handles an unknown voice by trying to
 * fetch the model upstream, which keeps the socket alive in slow drips
 * indefinitely.  SO_RCVTIMEO doesn't fire on slow drips, only on no
 * data at all, so the engine waits forever.  That is Piper-side
 * behaviour with a misconfigured client, not a regression in the
 * device -- the engine's connect/resolve failure paths, which are
 * what callers actually hit, are covered by scenarios 1 and 2.)
 */

#include <exec/types.h>
#include <exec/io.h>
#include <exec/tasks.h>
#include <devices/narrator.h>
#include <proto/exec.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ORIG_PATH   "config/narrator.wyoming"
#define ENV_PATH    "ENV:narrator.wyoming"
#define MAX_PREFS   4096

/* Slurp a whole text file into a NUL-terminated heap buffer (up to MAX_PREFS).
 * Returns NULL if the file is missing or too large. */
static char *slurp(const char *path)
{
    FILE *f = fopen(path, "r");
    char *buf;
    long n;
    if (!f) return NULL;
    buf = (char *)malloc(MAX_PREFS);
    if (!buf) { fclose(f); return NULL; }
    n = (long)fread(buf, 1, MAX_PREFS - 1, f);
    fclose(f);
    if (n < 0) n = 0;
    buf[n] = '\0';
    return buf;
}

/* Overwrite ENV:narrator.wyoming with a fresh config block. */
static int write_env(const char *contents)
{
    FILE *f = fopen(ENV_PATH, "w");
    if (!f) return -1;
    fputs(contents, f);
    fclose(f);
    return 0;
}

/* Submit one CMD_WRITE and report the outcome. Times are reported only as
 * "io_Error / io_Actual" -- the goal of failtest isn't to measure latency,
 * it's to confirm the device doesn't hang or Guru when the world is broken. */
static void run_scenario(const char *label,
                         struct narrator_rb *io,
                         const char *text)
{
    BYTE err;

    printf("--- %s ---\n", label);
    fflush(stdout);

    err = OpenDevice((STRPTR)"narrator.device", 0,
                     (struct IORequest *)io, 0);
    if (err != 0) {
        printf("  OpenDevice -> %d (open itself failed)\n", (int)err);
        fflush(stdout);
        return;
    }

    io->volume              = MAXVOL;
    io->message.io_Command  = CMD_WRITE;
    io->message.io_Data     = (APTR)text;
    io->message.io_Length   = strlen(text);
    DoIO((struct IORequest *)io);

    printf("  io_Error=%d io_Actual=%lu  (expect non-zero io_Error or zero io_Actual)\n",
           (int)io->message.io_Error,
           (unsigned long)io->message.io_Actual);
    fflush(stdout);

    CloseDevice((struct IORequest *)io);
}

int main(void)
{
    struct MsgPort     *mp;
    struct narrator_rb *io;
    char *orig;
    char  cfg[512];

    printf("failtest: graceful-failure scenarios for narrator.device\n");
    fflush(stdout);

    orig = slurp(ORIG_PATH);
    if (!orig) {
        printf("FATAL: can't read %s; aborting\n", ORIG_PATH);
        return 1;
    }

    mp = CreateMsgPort();
    if (!mp) { printf("CreateMsgPort failed\n"); free(orig); return 1; }
    io = (struct narrator_rb *)CreateIORequest(mp, sizeof(struct narrator_rb));
    if (!io) { printf("CreateIORequest failed\n"); DeleteMsgPort(mp); free(orig); return 1; }
    io->flags = NDF_NEWIORB;

    /* --- Scenario 1: unreachable IP (private LAN, no host at that addr) --- */
    snprintf(cfg, sizeof(cfg),
             "host 192.168.99.99\n"
             "port 10200\n");
    if (write_env(cfg) != 0) {
        printf("scenario 1: write_env failed; skipping\n");
    } else {
        run_scenario("Scenario 1: unreachable IP (192.168.99.99)",
                     io, "Hello.");
    }

    /* --- Scenario 2: hostname that won't resolve --- */
    snprintf(cfg, sizeof(cfg),
             "host no.such.host.invalid\n"
             "port 10200\n");
    if (write_env(cfg) != 0) {
        printf("scenario 2: write_env failed; skipping\n");
    } else {
        run_scenario("Scenario 2: unresolvable hostname",
                     io, "Hello.");
    }

    /* --- Restore the user's original prefs --- */
    {
        FILE *f = fopen(ENV_PATH, "w");
        if (f) { fputs(orig, f); fclose(f); printf("restored %s\n", ENV_PATH); }
        else    { printf("FATAL: couldn't restore %s\n", ENV_PATH); }
    }
    free(orig);

    DeleteIORequest((struct IORequest *)io);
    DeleteMsgPort(mp);
    printf("failtest done\n");
    return 0;
}
