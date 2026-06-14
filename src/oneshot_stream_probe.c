/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Simon Dick
 */

/* oneshot_stream_probe.c — option 3 from docs/streaming-v2-notes.md.
 *
 * stream_probe.c (incremental writes into a small DYNAMICSAMPLE ring with
 * looping) produced ~50/50 audio/silence duty cycle under Amiberry's
 * paula HiFi 14-bit mono cal mode. Working hypothesis was that AHI's
 * mix-buffer cache + the loop + mid-stream small writes interact badly.
 *
 * This probe sidesteps the loop entirely:
 *
 *   - Allocate one ~5.8 s buffer (256 KB at 22050/16/mono).
 *   - Pre-fill with silence.
 *   - AHI_LoadSound it as AHIST_DYNAMICSAMPLE.
 *   - AHI_Play it as a single sound, NO loop (AHIP_LoopSound is omitted).
 *     AHI plays each byte exactly once and stops at the end.
 *   - Fill from the front, 8 KB at a time, paced by Delay so the write
 *     head stays ahead of the play head. Same 440 -> 880 -> 440 pattern
 *     as stream_probe, ~5.4 s total content.
 *
 * Pass criteria:
 *   - Continuous audio for ~5.4 s (no gap/silence pattern).
 *   - Goertzel sees 440 / 880 / 440 in the right time windows.
 *
 * If gaps go away: option 3 is the production design — port into
 * audio_ahi.c with a buffer sized to expected max Piper response (~1.3 MB
 * for 30 s of audio is fine).
 * If gaps persist: the mixer cache interacts with ANY mid-stream write,
 * loop or not. Fall back to v1 buffered playback indefinitely.
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

#define MODE_ID         0x0002000fUL    /* paula HiFi 14-bit mono cal */
#define RATE            22050
#define BUF_BYTES       (256 * 1024)    /* ~5.8 s of 22050/16/mono     */
#define CHUNK_BYTES     8192
#define CHUNK_SAMPS     (CHUNK_BYTES / 2)
#define CHUNK_TICKS     9               /* ~180 ms (chunk is ~185 ms)  */
#define N_CHUNKS_440_A  10
#define N_CHUNKS_880    10
#define N_CHUNKS_440_B  10
#define N_TOTAL_CHUNKS  (N_CHUNKS_440_A + N_CHUNKS_880 + N_CHUNKS_440_B)

struct Library *AHIBase;

static const short LUT[16] = {
        0,  12539,  23169,  30273,  32767,  30273,  23169,  12539,
        0, -12539, -23169, -30273, -32767, -30273, -23169, -12539
};

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

    printf("oneshot_stream_probe: one-shot DYNAMICSAMPLE, no loop, fill from front\n");
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

    buf = (unsigned char *)AllocMem(BUF_BYTES, MEMF_PUBLIC | MEMF_CLEAR);
    if (!buf) { printf("FAIL: AllocMem\n"); return 20; }

    /* Buffer is initialized to silence (MEMF_CLEAR). Pre-fill the first
     * couple of chunks so AHI_Play has audible content right out the
     * gate. */
    sample_pos = 0;
    sample_pos = fill_sine_be(buf, CHUNK_SAMPS, 440, RATE, sample_pos);
    sample_pos = fill_sine_be(buf + CHUNK_BYTES, CHUNK_SAMPS, 440, RATE,
                              sample_pos);
    wpos = 2 * CHUNK_BYTES;     /* next write goes here                */

    si.ahisi_Type    = AHIST_M16S;
    si.ahisi_Address = buf;
    si.ahisi_Length  = BUF_BYTES >> 1;  /* samples (not bytes), M16S */
    rc = AHI_LoadSound(0, AHIST_DYNAMICSAMPLE, &si, ctrl);
    if (rc != 0) { printf("FAIL: AHI_LoadSound rc=%lu\n", (unsigned long)rc); return 20; }

    if (AHI_ControlAudio(ctrl, AHIC_Play, TRUE, TAG_END) != 0) {
        printf("FAIL: AHIC_Play TRUE\n"); return 20;
    }

    /* Play sound 0 ONCE — no AHIP_LoopSound tag. AHI plays through the
     * entire buffer and stops. */
    AHI_Play(ctrl,
             AHIP_BeginChannel, 0UL,
             AHIP_Freq,         (ULONG)RATE,
             AHIP_Vol,          0x10000UL,
             AHIP_Pan,          0x8000UL,
             AHIP_Sound,        0UL,
             AHIP_EndChannel,   0UL,
             TAG_END);
    printf("  AHI_Play started; streaming %d more chunks (no loop)\n",
           N_TOTAL_CHUNKS - 2);
    fflush(stdout);

    /* The first 2 chunks already exist (pre-fill). Generate the remaining
     * chunks of the 440 / 880 / 440 pattern ahead of the play head. */
    for (chunk = 2; chunk < N_TOTAL_CHUNKS; chunk++) {
        int freq;
        if (chunk < N_CHUNKS_440_A)                      freq = 440;
        else if (chunk < N_CHUNKS_440_A + N_CHUNKS_880)  freq = 880;
        else                                             freq = 440;
        sample_pos = fill_sine_be(buf + wpos, CHUNK_SAMPS, freq, RATE,
                                  sample_pos);
        wpos += CHUNK_BYTES;
        Delay(CHUNK_TICKS);
    }
    printf("  filled %d chunks; waiting for play to finish\n",
           N_TOTAL_CHUNKS);
    fflush(stdout);

    /* Wait long enough for AHI to play through to the end. Total content
     * is N_TOTAL_CHUNKS * CHUNK_BYTES = 30 * 8192 = 240 KB = 5.44 s of
     * audio. We've already Delay'd 28 * 180 = 5040 ms while writing, so
     * the play head still has ~400 ms + the buffer's tail silence to
     * cover. Delay for ~600 ms to be safe. */
    Delay(30);

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
