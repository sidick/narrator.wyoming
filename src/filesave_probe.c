/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Simon Dick
 */

/* filesave_probe.c — minimal AHI device-interface probe against filesave.
 *
 * Investigates whether the streaming hang we hit when pointing nw_engine.c at
 * a filesave-configured AHI unit is caused by:
 *   (a) filesave not honouring legacy ahi.device CMD_WRITE at all, or
 *   (b) something specific to our ahir_Link ping-pong pattern.
 *
 * This probe opens ahi.device on UNIT (hardcoded to 3 — see "Setup" below),
 * submits ONE 1-second CMD_WRITE of a synthetic sine wave (no link, no
 * streaming, no Wyoming), DoIO-blocks for it to return, and logs timing.
 *
 *   DoIO returns quickly  -> device interface works against filesave;
 *                            the hang is in our link chain.
 *   DoIO returns after ~1s -> filesave is rate-pacing like paula;
 *                             link chain probably an interaction bug.
 *   DoIO never returns     -> filesave doesn't reply to legacy CMD_WRITE
 *                             at all; see filesave_lib_probe.c.
 *
 * RESULT (recorded for posterity): DoIO never returned. Filesave does not
 * implement the ahi.device CMD_WRITE legacy device-interface; you must use
 * the AHI library interface (AHI_AllocAudio + AHI_LoadSound + AHI_Play),
 * which filesave_lib_probe.c demonstrates works.
 *
 * The ASL "Select a sound sample" requester fires when the filesave driver
 * loads — for the device interface, this is at the FIRST CMD_WRITE (NOT at
 * OpenDevice). Dismiss it externally (we drive Amiberry via MCP input).
 *
 * Setup
 * -----
 * The probe targets AHI unit 3 expecting it to be configured for filesave
 * mode 0x00010003 (HiFi 16 bit mono AIFF). AHI's prefs UI normally only
 * exposes filesave under the "music unit" slot (the library-interface user
 * mode), so to repurpose a numbered unit you have to edit ahi.prefs directly:
 *
 *   ENV-Archive/Sys/ahi.prefs offset 0xdc..0xdf  ->  00 01 00 03
 *                              offset 0xe0..0xe3  ->  00 00 56 22  (= 22050)
 *
 * Boot script also wants `Echo "ahi_unit 3" >>ENV:narrator.wyoming` so
 * downstream device-interface AHI consumers (if any) open unit 3 too.
 */

#include <exec/memory.h>
#include <exec/io.h>
#include <devices/ahi.h>
#include <proto/exec.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

#define UNIT          3
#define RATE          22050
#define SECONDS       1
#define SAMPLES       (RATE * SECONDS)
#define BUFBYTES      (SAMPLES * 2)         /* 16-bit mono */

/* AHI AHIST_M16S wants big-endian; we render the sine table little-endian and
 * swap before submit, mirroring exactly what audio_ahi.c / nw_engine.c do. */
static void swap16(unsigned char *b, long n)
{
    long i;
    for (i = 0; i + 1 < n; i += 2) {
        unsigned char t = b[i]; b[i] = b[i + 1]; b[i + 1] = t;
    }
}

/* 440Hz sine via a 16-entry lookup table (no FPU; -msoft-float build). */
static void sine_le(unsigned char *b, long samples, int freq, int rate)
{
    static const short table[16] = {
            0,  12539,  23169,  30273,  32767,  30273,  23169,  12539,
            0, -12539, -23169, -30273, -32767, -30273, -23169, -12539
    };
    long i;
    for (i = 0; i < samples; i++) {
        long idx = ((long)i * freq * 16 / rate) & 15;
        short s = table[idx];
        b[2*i]   = (unsigned char)(s & 0xff);
        b[2*i+1] = (unsigned char)((s >> 8) & 0xff);
    }
}

int main(void)
{
    struct MsgPort    *port;
    struct AHIRequest *req;
    unsigned char     *buf;
    time_t             t0, t1;
    int                rc;

    printf("filesave_probe: opening ahi.device unit %d (filesave expected)\n", UNIT);
    fflush(stdout);

    port = CreateMsgPort();
    if (!port) { printf("FAIL: CreateMsgPort\n"); fflush(stdout); return 20; }

    req = (struct AHIRequest *)CreateIORequest(port, sizeof(struct AHIRequest));
    if (!req) { printf("FAIL: CreateIORequest\n"); fflush(stdout); return 20; }
    req->ahir_Version = 4;

    rc = OpenDevice((STRPTR)"ahi.device", UNIT, (struct IORequest *)req, 0);
    if (rc != 0) {
        printf("FAIL: OpenDevice ahi.device unit %d -> %d\n", UNIT, rc);
        fflush(stdout);
        return 20;
    }
    printf("OpenDevice returned cleanly (no requester yet — filesave loads at CMD_WRITE)\n");
    fflush(stdout);

    buf = (unsigned char *)AllocMem(BUFBYTES, MEMF_PUBLIC);
    if (!buf) { printf("FAIL: AllocMem %d bytes\n", BUFBYTES); fflush(stdout); return 20; }
    sine_le(buf, SAMPLES, 440, RATE);
    swap16(buf, BUFBYTES);

    req->ahir_Std.io_Command = CMD_WRITE;
    req->ahir_Std.io_Data    = buf;
    req->ahir_Std.io_Length  = (ULONG)BUFBYTES;
    req->ahir_Std.io_Offset  = 0;
    req->ahir_Frequency      = (ULONG)RATE;
    req->ahir_Type           = AHIST_M16S;
    req->ahir_Volume         = 0x10000;
    req->ahir_Position       = 0x8000;
    req->ahir_Link           = NULL;

    printf("CMD_WRITE %d bytes (1s @ 22050Hz/16/mono) -> DoIO start\n", BUFBYTES);
    fflush(stdout);
    time(&t0);
    DoIO((struct IORequest *)req);
    time(&t1);
    printf("DoIO returned: io_Error=%d io_Actual=%lu wall=%lds\n",
           (int)req->ahir_Std.io_Error,
           (unsigned long)req->ahir_Std.io_Actual,
           (long)(t1 - t0));
    fflush(stdout);

    FreeMem(buf, BUFBYTES);
    CloseDevice((struct IORequest *)req);
    DeleteIORequest((struct IORequest *)req);
    DeleteMsgPort(port);
    printf("done\n");
    return 0;
}
