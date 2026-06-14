/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Simon Dick
 */

/* ahi_capture.c — replay a WAV through AHI library interface to filesave.
 *
 * Reads INPUT_PATH (a 22050Hz/16-bit/mono LE WAV — the format the device's
 * prefs.capture_raw produces). Plays the samples through AHI v4's library
 * interface in filesave mode (0x00010003, HiFi 16-bit mono AIFF). Filesave
 * writes an AIFF that captures the AHI MIXER's output for that input —
 * i.e. what the mixer does to our PCM before handing it to whichever output
 * driver is active. With prefs.capture_raw on hand as ground truth, the
 * diff (after trimming filesave's trailing silence) is the AHI mixer's
 * contribution to the pipeline.
 *
 * What this DOES NOT capture: paula.audio's effect. The CLAUDE.md sibilance
 * suspect "AHI's resample to Paula's rate plus 8-bit Paula output" is split
 * across the mixer (the resample, caught here) and paula (the 8-bit
 * reduction + actual DAC, NOT caught here). Catching the latter requires
 * recording Amiberry's host audio output (BlackHole loopback or similar).
 *
 * Caveats:
 *   - Filesave does NOT real-time pace. The mixer consumes input as fast
 *     as the CPU allows, so wall_time(playback) << audio_duration. We
 *     Delay for an estimated 5x speedup; trailing silence in the AIFF is
 *     trimmed on the host. If the filesave AIFF ends up TRUNCATED, raise
 *     the speedup estimate.
 *   - The ASL "Select a sound sample" requester fires at AHIC_Play TRUE,
 *     and a "Rendering finished" advisory dialog fires at AHI_FreeAudio.
 *     Both must be dismissed externally (Amiberry MCP input).
 *
 * Hardcoded paths (boot-script-launched programs can't trust argv). Edit
 * the macros to retarget. Mono 16-bit WAV input only.
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

#define INPUT_PATH  "Narrator:capture_raw.wav"
#define MODE_ID     0x00010003UL          /* FILESAVE: HiFi 16-bit mono AIFF */
#define SPEEDUP_EST 5                     /* conservative — overshoot wait    */

struct Library *AHIBase;

static unsigned long u32_le(const unsigned char *b)
{
    return (unsigned long)b[0]
         | ((unsigned long)b[1] << 8)
         | ((unsigned long)b[2] << 16)
         | ((unsigned long)b[3] << 24);
}

static unsigned short u16_le(const unsigned char *b)
{
    return (unsigned short)b[0] | ((unsigned short)b[1] << 8);
}

int main(void)
{
    FILE *f;
    unsigned char  hdr[44];
    unsigned char *buf;
    long           buf_bytes;
    unsigned long  rate, data_size, samples;
    unsigned short channels, bits;
    long           i;
    struct MsgPort       *port;
    struct AHIRequest    *req;
    struct AHIAudioCtrl  *ctrl;
    struct AHISampleInfo  si;
    ULONG                 rc;
    long                  wait_ticks;

    printf("ahi_capture: reading %s\n", INPUT_PATH);
    fflush(stdout);

    f = fopen(INPUT_PATH, "rb");
    if (!f)                          { printf("FAIL: fopen\n");        return 20; }
    if (fread(hdr, 1, 44, f) != 44)  { printf("FAIL: short header\n"); return 20; }
    if (memcmp(hdr,     "RIFF", 4)) { printf("FAIL: not RIFF\n");     return 20; }
    if (memcmp(hdr + 8, "WAVE", 4)) { printf("FAIL: not WAVE\n");     return 20; }
    channels  = u16_le(hdr + 22);
    rate      = u32_le(hdr + 24);
    bits      = u16_le(hdr + 34);
    data_size = u32_le(hdr + 40);
    samples   = data_size / (channels * (bits / 8));
    {
        unsigned long secs = samples / rate;
        unsigned long ms   = ((samples - secs * rate) * 1000UL) / rate;
        printf("  format: %ldHz %d-bit %d ch  (%ld samples, %ld.%03lds)\n",
               (long)rate, (int)bits, (int)channels, (long)samples, secs, ms);
    }
    fflush(stdout);
    if (channels != 1 || bits != 16) {
        printf("FAIL: need mono 16-bit (got %d ch, %d bit)\n", (int)channels, (int)bits);
        return 20;
    }

    buf_bytes = (long)samples * 2;
    buf       = (unsigned char *)malloc(buf_bytes);
    if (!buf)                                            { printf("FAIL: malloc %ld\n", buf_bytes); return 20; }
    if ((long)fread(buf, 1, buf_bytes, f) != buf_bytes) { printf("FAIL: short PCM read\n");        return 20; }
    fclose(f);

    /* WAV is LE; AHIST_M16S is BE. Swap each 16-bit sample in place. */
    for (i = 0; i + 1 < buf_bytes; i += 2) {
        unsigned char t = buf[i]; buf[i] = buf[i + 1]; buf[i + 1] = t;
    }

    /* AHI library interface (AHI_NO_UNIT + explicit mode). */
    port = CreateMsgPort();
    if (!port) { printf("FAIL: CreateMsgPort\n"); return 20; }
    req = (struct AHIRequest *)CreateIORequest(port, sizeof(struct AHIRequest));
    if (!req) { printf("FAIL: CreateIORequest\n"); return 20; }
    req->ahir_Version = 4;
    if (OpenDevice((STRPTR)"ahi.device", AHI_NO_UNIT,
                   (struct IORequest *)req, 0) != 0) {
        printf("FAIL: OpenDevice ahi.device\n"); return 20;
    }
    AHIBase = (struct Library *)req->ahir_Std.io_Device;
    printf("  OpenDevice OK\n"); fflush(stdout);

    printf("  AHI_AllocAudio (filesave %ldHz mono); ASL requester pops at AHIC_Play\n",
           (long)rate);
    fflush(stdout);
    ctrl = AHI_AllocAudio(
        AHIA_AudioID,  MODE_ID,
        AHIA_MixFreq,  rate,
        AHIA_Channels, 1UL,
        AHIA_Sounds,   1UL,
        TAG_END);
    if (!ctrl) { printf("FAIL: AHI_AllocAudio\n"); return 20; }
    printf("  AHI_AllocAudio OK\n"); fflush(stdout);

    si.ahisi_Type    = AHIST_M16S;
    si.ahisi_Address = buf;
    si.ahisi_Length  = buf_bytes;
    rc = AHI_LoadSound(0, AHIST_SAMPLE, &si, ctrl);
    if (rc != 0) { printf("FAIL: AHI_LoadSound rc=%lu\n", (unsigned long)rc); return 20; }
    printf("  AHI_LoadSound OK\n"); fflush(stdout);

    /* AHIC_Play TRUE triggers filesave's ASL requester. */
    rc = AHI_ControlAudio(ctrl, AHIC_Play, TRUE, TAG_END);
    if (rc != 0) { printf("FAIL: AHIC_Play TRUE rc=%lu\n", (unsigned long)rc); return 20; }
    printf("  AHIC_Play TRUE\n"); fflush(stdout);

    AHI_Play(ctrl,
             AHIP_BeginChannel, 0UL,
             AHIP_Freq,         rate,
             AHIP_Vol,          0x10000UL,
             AHIP_Pan,          0x8000UL,
             AHIP_Sound,        0UL,
             AHIP_EndChannel,   0UL,
             TAG_END);

    /* Wait long enough for filesave to consume the whole sound. The mixer
     * runs at ~SPEEDUP_EST x real-time, so wait = duration / SPEEDUP_EST,
     * clamped to a sensible minimum. Trailing silence in the AIFF is fine
     * — we trim it on the host. */
    wait_ticks = (long)((samples * 50UL) / (rate * (unsigned long)SPEEDUP_EST));
    if (wait_ticks < 50) wait_ticks = 50;           /* >= 1 wall second */
    printf("  AHI_Play submitted; Delay(%ld ticks ~= %ld.%lds wall)\n",
           wait_ticks, wait_ticks / 50, ((wait_ticks % 50) * 10) / 50);
    fflush(stdout);
    Delay(wait_ticks);

    AHI_ControlAudio(ctrl, AHIC_Play, FALSE, TAG_END);
    AHI_UnloadSound(0, ctrl);
    printf("  AHIC_Play FALSE; AHI_FreeAudio (rendering-finished dialog next)\n");
    fflush(stdout);
    AHI_FreeAudio(ctrl);
    free(buf);
    CloseDevice((struct IORequest *)req);
    DeleteIORequest((struct IORequest *)req);
    DeleteMsgPort(port);
    printf("done\n");
    return 0;
}
