/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Simon Dick
 */

/* stream_probe.c — incremental producer + AHIST_DYNAMICSAMPLE ring buffer.
 *
 * Step 4 of the streaming v2 plan. dynamic_probe.c proved the dynamic-
 * sample loop primitive: load a ring as AHIST_DYNAMICSAMPLE, AHI_Play it
 * with looping, modify the buffer contents during playback. This probe
 * does the realistic producer-side version: chunks of fresh audio are
 * written progressively (not bulk-swapped like dynamic_probe), each at the
 * next position in the ring, paced by Delay so the write head stays just
 * ahead of the play head.
 *
 * Test pattern: 30 chunks of audio (~2.7 s total) generated as
 *   chunks  0-9  -> 440 Hz sine
 *   chunks 10-19 -> 880 Hz sine
 *   chunks 20-29 -> 440 Hz sine
 *
 * Then silence chunks for a drain interval. Phase is tracked across
 * chunks so the sine remains continuous within each frequency band —
 * the 440-to-880 transitions will click (different frequencies don't
 * phase-match) but the 440-block-to-440-block joins shouldn't.
 *
 * Ring layout:
 *   - BUF_BYTES = 32 KB (~371 ms at 22050 Hz mono 16-bit)
 *   - CHUNK_BYTES = 8 KB (~93 ms each; 4 chunks per ring wrap)
 *   - Pre-fill ring with two chunks of 440 Hz so when AHI_Play starts there's
 *     immediately a head-start ahead of the read head.
 *
 * Pacing strategy: Delay(CHUNK_TICKS) where CHUNK_TICKS slightly UNDER the
 * chunk's playback duration, so the write head naturally walks ahead of
 * the read head over time. The 371 ms ring is forgiveness for any clock
 * drift between Delay and the AHI mixer.
 *
 * Pass criteria (verified via BlackHole capture):
 *   - Continuous audio for ~2.7 seconds.
 *   - Goertzel filter: dominant 440 Hz in first second, dominant 880 Hz
 *     in second second, dominant 440 Hz in third second.
 *   - No audible silence gaps in the middle (would mean play head caught
 *     up to write head -> underrun).
 *
 * If this passes, the streaming primitive AND the producer-side timing
 * are both validated and we can move on to porting it into audio_ahi.c.
 */

#include <exec/memory.h>
#include <exec/io.h>
#include <devices/ahi.h>
#include <dos/dos.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/ahi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MODE_ID      0x0002000fUL    /* paula HiFi 14-bit mono calibrated */
#define RATE         22050
#define BUF_BYTES    32768           /* ring size (~371 ms at 22050/16/mono) */
#define CHUNK_BYTES  8192            /* per write (~93 ms each)              */
#define CHUNK_SAMPS  (CHUNK_BYTES / 2)
#define CHUNK_TICKS  9               /* ~180 ms — matches chunk dur (chunk is ~185 ms) */
#define N_CHUNKS_440_A  10           /* 0-9                                   */
#define N_CHUNKS_880    10           /* 10-19                                 */
#define N_CHUNKS_440_B  10           /* 20-29                                 */
#define N_TOTAL_CHUNKS  (N_CHUNKS_440_A + N_CHUNKS_880 + N_CHUNKS_440_B)
#define N_DRAIN_CHUNKS  6

struct Library *AHIBase;

static const short LUT[16] = {
        0,  12539,  23169,  30273,  32767,  30273,  23169,  12539,
        0, -12539, -23169, -30273, -32767, -30273, -23169, -12539
};

/* Write CHUNK_SAMPS samples of a `freq`-Hz sine to `dst`, in AHI native
 * big-endian. `start_sample` is the absolute sample index for phase
 * continuity across calls. Returns the new global sample count. */
static long fill_sine_be(unsigned char *dst, long samples,
                         int freq, int rate, long start_sample)
{
    long i;
    for (i = 0; i < samples; i++) {
        long  abs_i = start_sample + i;
        long  idx   = (abs_i * freq * 16 / rate) & 15;
        short s     = LUT[idx];
        dst[2*i]   = (unsigned char)((s >> 8) & 0xff);
        dst[2*i+1] = (unsigned char)(s & 0xff);
    }
    return start_sample + samples;
}

int main(void)
{
    struct MsgPort       *port;
    struct AHIRequest    *req;
    struct AHIAudioCtrl  *ctrl;
    struct AHISampleInfo  si;
    unsigned char        *buf;
    ULONG                 rc;
    long                  wpos;
    long                  sample_pos;
    int                   chunk;

    printf("stream_probe: incremental write-ahead streaming via DYNAMICSAMPLE\n");
    fflush(stdout);

    port = CreateMsgPort();
    if (!port) { printf("FAIL: CreateMsgPort\n"); return 20; }
    req = (struct AHIRequest *)CreateIORequest(port, sizeof(struct AHIRequest));
    if (!req)  { printf("FAIL: CreateIORequest\n"); return 20; }
    req->ahir_Version = 4;

    if (OpenDevice((STRPTR)"ahi.device", AHI_NO_UNIT,
                   (struct IORequest *)req, 0) != 0) {
        printf("FAIL: OpenDevice ahi.device\n"); return 20;
    }
    AHIBase = (struct Library *)req->ahir_Std.io_Device;

    ctrl = AHI_AllocAudio(
        AHIA_AudioID,    MODE_ID,
        AHIA_MixFreq,    (ULONG)RATE,
        AHIA_Channels,   1UL,
        AHIA_Sounds,     1UL,
        TAG_END);
    if (!ctrl) { printf("FAIL: AHI_AllocAudio\n"); return 20; }

    buf = (unsigned char *)AllocMem(BUF_BYTES, MEMF_PUBLIC);
    if (!buf) { printf("FAIL: AllocMem\n"); return 20; }

    /* Pre-fill the WHOLE ring with the first frequency so AHI_Play has
     * something audible immediately and we get a head-start before our
     * incremental writes need to keep pace. We then overwrite progressively
     * as the play head consumes — that's the actual streaming test. */
    sample_pos = fill_sine_be(buf, BUF_BYTES / 2, 440, RATE, 0);
    wpos       = 0;          /* write head starts at 0; first chunk we write
                                replaces what's at 0..CHUNK_BYTES (the silence
                                or pre-fill there). */

    si.ahisi_Type    = AHIST_M16S;
    si.ahisi_Address = buf;
    si.ahisi_Length  = BUF_BYTES;
    rc = AHI_LoadSound(0, AHIST_DYNAMICSAMPLE, &si, ctrl);
    if (rc != 0) { printf("FAIL: AHI_LoadSound rc=%lu\n", (unsigned long)rc); return 20; }

    if (AHI_ControlAudio(ctrl, AHIC_Play, TRUE, TAG_END) != 0) {
        printf("FAIL: AHIC_Play TRUE\n"); return 20;
    }

    AHI_Play(ctrl,
             AHIP_BeginChannel, 0UL,
             AHIP_Freq,         (ULONG)RATE,
             AHIP_Vol,          0x10000UL,
             AHIP_Pan,          0x8000UL,
             AHIP_Sound,        0UL,
             AHIP_LoopFreq,     (ULONG)RATE,
             AHIP_LoopVol,      0x10000UL,
             AHIP_LoopPan,      0x8000UL,
             AHIP_LoopSound,    0UL,
             AHIP_EndChannel,   0UL,
             TAG_END);
    printf("  AHI_Play started; streaming %d chunks (~%d ms each)\n",
           N_TOTAL_CHUNKS, (int)((CHUNK_TICKS * 1000) / 50));
    fflush(stdout);

    /* Incremental streaming. Each chunk: pick a frequency based on which
     * "band" we're in, generate CHUNK_SAMPS samples at the current global
     * phase, write them to wpos, Delay, advance wpos with wrap. */
    for (chunk = 0; chunk < N_TOTAL_CHUNKS; chunk++) {
        int freq;
        if (chunk < N_CHUNKS_440_A)                        freq = 440;
        else if (chunk < N_CHUNKS_440_A + N_CHUNKS_880)    freq = 880;
        else                                               freq = 440;
        sample_pos = fill_sine_be(buf + wpos, CHUNK_SAMPS, freq, RATE,
                                  sample_pos);
        wpos = (wpos + CHUNK_BYTES) % BUF_BYTES;
        Delay(CHUNK_TICKS);
    }
    printf("  streamed %d chunks; writing silence to drain\n", N_TOTAL_CHUNKS);
    fflush(stdout);

    /* Drain: overwrite remaining ring content with silence so the loop has
     * something silent to play once it cycles past the last real audio. */
    for (chunk = 0; chunk < N_DRAIN_CHUNKS; chunk++) {
        memset(buf + wpos, 0, CHUNK_BYTES);
        wpos = (wpos + CHUNK_BYTES) % BUF_BYTES;
        Delay(CHUNK_TICKS);
    }

    AHI_ControlAudio(ctrl, AHIC_Play, FALSE, TAG_END);
    AHI_UnloadSound(0, ctrl);
    AHI_FreeAudio(ctrl);
    FreeMem(buf, BUF_BYTES);
    CloseDevice((struct IORequest *)req);
    DeleteIORequest((struct IORequest *)req);
    DeleteMsgPort(port);

    printf("done\n");
    return 0;
}
