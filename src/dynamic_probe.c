/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Simon Dick
 */

/* dynamic_probe.c — verify AHIST_DYNAMICSAMPLE + ring buffer mechanics.
 *
 * Step 1 of the streaming v2 plan (option 3 in docs/streaming-v2-notes.md).
 * AHIST_DYNAMICSAMPLE is the AHI Developer Guide's canonical streaming
 * primitive: tell AHI the sample data may change while playing, write into
 * the buffer ahead of the play head, AHI reads what we last wrote. Combined
 * with looping (AHIP_LoopSound = same slot, AHIP_LoopVol = max) the sample
 * is conceptually a circular buffer that plays forever until we stop.
 *
 * This probe tests the simplest possible version of that mechanism:
 *
 *   1. Allocate a 32 KB ring (~371 ms of 22050 Hz mono 16-bit).
 *   2. Pre-fill with a 440 Hz sine wave (so playback is audibly verifiable
 *      from the moment AHI_Play kicks in).
 *   3. AHI_LoadSound with loading method AHIST_DYNAMICSAMPLE, sample format
 *      AHIST_M16S.
 *   4. AHIC_Play TRUE; AHI_Play with AHIP_Sound = 0, AHIP_LoopSound = 0,
 *      and AHIP_LoopVol = full so the ring loops forever.
 *   5. Switch the buffer contents to a 880 Hz sine after 1 second of wall
 *      time. If DYNAMICSAMPLE works, the audible pitch should shift up
 *      one octave without breaking continuity.
 *   6. Switch back to 440 Hz after another second.
 *   7. AHIC_Play FALSE; teardown.
 *
 * Pass criteria (verified via BlackHole capture):
 *   - The recording contains continuous audio for ~3 seconds.
 *   - Spectrum at second 0-1 has energy around 440 Hz.
 *   - Spectrum at second 1-2 has energy around 880 Hz.
 *   - Spectrum at second 2-3 has energy around 440 Hz again.
 *   - No clicks / silences across the transitions.
 *
 * If this passes, we know the mechanism is sound and can move on to the
 * "write ahead of the play head, poll/track read position" half of the
 * real streaming design.
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

#define MODE_ID    0x0002000fUL    /* paula HiFi 14-bit mono calibrated */
#define RATE       22050
#define BUF_BYTES  32768           /* ring size; ~371 ms at 22050/16/mono */
#define BUF_SAMPS  (BUF_BYTES / 2) /* sample count (16-bit mono)          */

struct Library *AHIBase;

/* 16-entry sine LUT in 16-bit signed. */
static const short LUT[16] = {
        0,  12539,  23169,  30273,  32767,  30273,  23169,  12539,
        0, -12539, -23169, -30273, -32767, -30273, -23169, -12539
};

/* Fill `dst_samples` with a freq-Hz sine in AHI native big-endian (AHIST_M16S). */
static void fill_sine_be(unsigned char *dst_bytes, long samples,
                         int freq, int rate)
{
    long i;
    for (i = 0; i < samples; i++) {
        long  idx = ((long)i * freq * 16 / rate) & 15;
        short s   = LUT[idx];
        dst_bytes[2*i]   = (unsigned char)((s >> 8) & 0xff);
        dst_bytes[2*i+1] = (unsigned char)(s & 0xff);
    }
}

int main(void)
{
    struct MsgPort       *port;
    struct AHIRequest    *req;
    struct AHIAudioCtrl  *ctrl;
    struct AHISampleInfo  si;
    unsigned char        *buf;
    ULONG                 rc;

    printf("dynamic_probe: AHIST_DYNAMICSAMPLE ring-buffer mechanics\n");
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
    printf("  AHIBase = %p\n", AHIBase); fflush(stdout);

    ctrl = AHI_AllocAudio(
        AHIA_AudioID,    MODE_ID,
        AHIA_MixFreq,    (ULONG)RATE,
        AHIA_Channels,   1UL,
        AHIA_Sounds,     1UL,
        TAG_END);
    if (!ctrl) { printf("FAIL: AHI_AllocAudio\n"); return 20; }
    printf("  AHI_AllocAudio OK; ctrl=%p\n", ctrl); fflush(stdout);

    buf = (unsigned char *)AllocMem(BUF_BYTES, MEMF_PUBLIC);
    if (!buf) { printf("FAIL: AllocMem\n"); return 20; }

    /* Pre-fill ring with 440 Hz sine so playback is audible from the start. */
    fill_sine_be(buf, BUF_SAMPS, 440, RATE);

    si.ahisi_Type    = AHIST_M16S;      /* sample format                       */
    si.ahisi_Address = buf;
    si.ahisi_Length  = BUF_BYTES >> 1;  /* samples (devices/ahi.h), M16S */

    /* AHI_LoadSound with type AHIST_DYNAMICSAMPLE — the loading-method type
     * (not the sample format), which signals "this sample's data may change
     * while it's playing; re-read the buffer instead of caching". */
    rc = AHI_LoadSound(0, AHIST_DYNAMICSAMPLE, &si, ctrl);
    if (rc != 0) {
        printf("FAIL: AHI_LoadSound(DYNAMICSAMPLE) rc=%lu\n", (unsigned long)rc);
        return 20;
    }
    printf("  AHI_LoadSound(DYNAMICSAMPLE) OK\n"); fflush(stdout);

    if (AHI_ControlAudio(ctrl, AHIC_Play, TRUE, TAG_END) != 0) {
        printf("FAIL: AHIC_Play TRUE\n"); return 20;
    }

    /* Play sound 0 once, then loop sound 0 forever. AHIP_LoopVol at full
     * volume + AHIP_LoopSound = same slot is the canonical "loop the ring
     * forever" idiom. */
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
    printf("  AHI_Play submitted (sound 0 + loop sound 0); 1 s of 440 Hz\n");
    fflush(stdout);

    Delay(50);                              /* 1 s of 440 Hz */

    /* Switch the ring's contents to an 880 Hz sine. If DYNAMICSAMPLE works,
     * the next time AHI reads the buffer (next loop iteration, or the next
     * portion of the current loop) the new content takes effect. */
    fill_sine_be(buf, BUF_SAMPS, 880, RATE);
    printf("  switched ring contents to 880 Hz; 1 s of 880 Hz\n");
    fflush(stdout);

    Delay(50);                              /* 1 s of 880 Hz */

    /* Back to 440. */
    fill_sine_be(buf, BUF_SAMPS, 440, RATE);
    printf("  switched ring contents back to 440 Hz; 1 s of 440 Hz\n");
    fflush(stdout);

    Delay(50);                              /* 1 s of 440 Hz */

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
