/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Simon Dick
 */

/* audio_ahi.c — Amiga backend for audio.h: AHI v4 double-buffered streaming.
 *
 * Uses the low-level ahi.device CMD_WRITE interface with two ping-ponging
 * AHIRequests. ahir_Link chains buffer N+1 onto buffer N so playback is
 * gapless; we WaitIO the buffer that just finished, refill it, and re-queue.
 * audio_write() blocks when both buffers are busy, which paces the network
 * read to playback speed — i.e. streaming.
 *
 * Opened on AHI unit 0 (the user's AHI prefs "Unit 0" mode); AHI does the
 * mixing / rate-conversion from our 22050Hz/16-bit/mono samples to the mode.
 */
#ifdef PLATFORM_AMIGA

#include "audio.h"

#include <exec/memory.h>
#include <exec/io.h>
#include <devices/ahi.h>

#include <proto/exec.h>

#include <string.h>
#include <stdio.h>

#define NBUF    2
#define BUFSIZE 8192       /* per buffer; ~0.19s at 44100 bytes/s */

static struct MsgPort    *g_port;
static struct AHIRequest *g_req[NBUF];
static unsigned char     *g_buf[NBUF];
static int                g_opened;     /* OpenDevice succeeded            */
static int                g_haveReq1;   /* g_req[1] allocated              */
static int                g_queued[NBUF];
static int                g_fill;       /* buffer currently being filled   */
static long               g_fillpos;
static int                g_primed;     /* both buffers queued before start*/
static unsigned long      g_rate;
static int                g_type;       /* AHIST_M16S / AHIST_S16S         */
static int                g_unit;       /* ahi.device unit (default 0)     */

/* Piper/Wyoming PCM is 16-bit signed LITTLE-endian; AHI (AHIST_M16S) wants the
 * Amiga's native BIG-endian order, or it plays as static. Swap each sample in
 * place before playback. Buffer boundaries are even, so samples never straddle
 * a submitted buffer. */
static void swap16(unsigned char *b, long n)
{
    long i;
    for (i = 0; i + 1 < n; i += 2) {
        unsigned char t = b[i]; b[i] = b[i + 1]; b[i + 1] = t;
    }
}

static void submit(int i, long bytes)
{
    struct AHIRequest *r = g_req[i];
    swap16(g_buf[i], bytes);            /* little-endian PCM -> AHI big-endian */
    r->ahir_Std.io_Command = CMD_WRITE;
    r->ahir_Std.io_Data    = g_buf[i];
    r->ahir_Std.io_Length  = (ULONG)bytes;
    r->ahir_Std.io_Offset  = 0;
    r->ahir_Frequency      = g_rate;
    r->ahir_Type           = g_type;
    r->ahir_Volume         = 0x10000;   /* 1.0  (Fixed 16.16), full        */
    r->ahir_Position       = 0x8000;    /* 0.5  centre                     */
    r->ahir_Link           = g_queued[1 - i] ? g_req[1 - i] : NULL;
    SendIO((struct IORequest *)r);
    g_queued[i] = 1;
}

static void wait_free(int i)
{
    if (g_queued[i]) {
        WaitIO((struct IORequest *)g_req[i]);
        g_queued[i] = 0;
    }
}

void audio_set_outfile(const char *path) { (void)path; }
void audio_set_unit(int unit) { g_unit = unit; }

int audio_open(int rate, int width, int channels)
{
    if (width != 2)
        return -1;                       /* AHI path assumes 16-bit        */
    g_type    = (channels >= 2) ? AHIST_S16S : AHIST_M16S;
    g_rate    = (unsigned long)rate;
    g_fill    = 0;
    g_fillpos = 0;
    g_primed  = 0;
    g_queued[0] = g_queued[1] = 0;

    g_port = CreateMsgPort();
    if (!g_port) return -1;

    g_req[0] = (struct AHIRequest *)CreateIORequest(g_port, sizeof(struct AHIRequest));
    if (!g_req[0]) return -1;
    g_req[0]->ahir_Version = 4;          /* AHI v4 minimum                 */

    if (OpenDevice((STRPTR)AHINAME, (ULONG)g_unit,
                   (struct IORequest *)g_req[0], 0) != 0) {
        fprintf(stderr, "OpenDevice ahi.device unit %ld failed\n", (long)g_unit);
        return -1;
    }
    g_opened = 1;

    /* Second request is a clone sharing the same reply port + device. */
    g_req[1] = (struct AHIRequest *)AllocMem(sizeof(struct AHIRequest),
                                             MEMF_PUBLIC | MEMF_CLEAR);
    if (!g_req[1]) return -1;
    CopyMem(g_req[0], g_req[1], sizeof(struct AHIRequest));
    g_haveReq1 = 1;

    g_buf[0] = (unsigned char *)AllocMem(BUFSIZE, MEMF_PUBLIC);
    g_buf[1] = (unsigned char *)AllocMem(BUFSIZE, MEMF_PUBLIC);
    if (!g_buf[0] || !g_buf[1]) return -1;

    /* Silence pre-roll (~64ms): absorbs the cold-start DAC transient so the
     * utterance's onset isn't clipped. See nw_engine.c for the rationale. */
    audio_write(NULL, ((long)g_rate >> 4) * 2);

    return 0;
}

/* data == NULL feeds `len` bytes of silence (used for the start-up pre-roll). */
long audio_write(const void *data, long len)
{
    const unsigned char *p = (const unsigned char *)data;
    long left = len;

    while (left > 0) {
        long space = BUFSIZE - g_fillpos;
        long take  = left < space ? left : space;
        if (p) { memcpy(g_buf[g_fill] + g_fillpos, p, (size_t)take); p += take; }
        else   { memset(g_buf[g_fill] + g_fillpos, 0, (size_t)take); }  /* silence */
        g_fillpos += take;
        left      -= take;

        if (g_fillpos == BUFSIZE) {
            if (!g_primed) {
                /* Prime: hold buffer 0 until buffer 1 is also full, then
                 * submit BOTH back-to-back (0 unlinked, 1 linked onto 0) so
                 * AHI always has the next buffer queued before the current
                 * one drains. Submitting buffer 0 alone and only filling 1
                 * afterwards races the network and underruns mid-word. */
                if (g_fill == 0) {
                    g_fill = 1; g_fillpos = 0;        /* hold 0, fill 1     */
                } else {
                    submit(0, BUFSIZE);               /* link NULL          */
                    submit(1, BUFSIZE);               /* linked onto 0      */
                    g_primed = 1;
                    g_fill = 0;
                    wait_free(0);                     /* reclaim 0 to refill*/
                    g_fillpos = 0;
                }
            } else {
                int other = 1 - g_fill;
                submit(g_fill, BUFSIZE); /* play it, linked to the other   */
                wait_free(other);        /* reclaim the other to refill     */
                g_fill    = other;
                g_fillpos = 0;
            }
        }
    }
    return len;
}

void audio_close(void)
{
    if (g_opened) {
        /* Small silence post-roll so the channel-stop click lands on silence
         * (see nw_engine.c). ~64ms. */
        audio_write(NULL, ((long)g_rate >> 4) * 2);
        if (!g_primed) {
            /* Never reached the prime point: flush whatever is held. If a
             * full buffer 0 is being held (g_fill == 1), submit it first,
             * then the partial buffer 1 linked onto it. */
            if (g_fill == 1) {
                submit(0, BUFSIZE);                  /* held full buf, link NULL */
                if (g_fillpos > 0) submit(1, g_fillpos);
            } else if (g_fillpos > 0) {
                submit(0, g_fillpos);                /* only a partial buf 0 */
            }
        } else if (g_fillpos > 0) {
            submit(g_fill, g_fillpos);   /* flush the partial tail buffer  */
        }
        wait_free(0);
        wait_free(1);
    }

    if (g_buf[0]) { FreeMem(g_buf[0], BUFSIZE); g_buf[0] = NULL; }
    if (g_buf[1]) { FreeMem(g_buf[1], BUFSIZE); g_buf[1] = NULL; }
    if (g_haveReq1 && g_req[1]) { FreeMem(g_req[1], sizeof(struct AHIRequest)); g_req[1] = NULL; }
    if (g_opened)  { CloseDevice((struct IORequest *)g_req[0]); g_opened = 0; }
    if (g_req[0])  { DeleteIORequest((struct IORequest *)g_req[0]); g_req[0] = NULL; }
    if (g_port)    { DeleteMsgPort(g_port); g_port = NULL; }
}

#endif /* PLATFORM_AMIGA */
