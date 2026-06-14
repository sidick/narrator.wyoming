/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Simon Dick
 */

/* ahi_modes_probe.c — play the same PCM through several AHI audio modes,
 * back-to-back with silence between, for one-shot per-mode A/B testing.
 *
 * Background: the BlackHole-loopback test rig (docs/audio-capture-rig.md)
 * showed dramatic aliasing in 11-15 / 15-20 kHz when our pipeline opens AHI
 * unit 0, which is paula mode 0x0002000c -- "Paula: Fast 8 bit mono". We
 * had been testing two 8-bit paula modes and missing the actual choices.
 * This probe sweeps the high-fidelity modes available in the AudioMode
 * files on the test drive so the next BlackHole capture can split per-mode
 * by silence and run spectrum_report.py against each.
 *
 * Hardcoded mode list (sample-rate-friendly mono speech candidates):
 *   1. paula 0x0002000f  Paula:HiFi 14 bit mono calibrated   (best paula mono)
 *   2. uaesnd 0x003b0002 uaesnd: HiFi Stereo                 (emulator-clean)
 *   3. uae    0x001a0000 UAE:16 bit HIFI Stereo++            (emulator-clean)
 *
 * Same usage pattern as paula_lib_probe.c: OpenDevice("ahi.device",
 * AHI_NO_UNIT, ...), AHI_AllocAudio with AHIA_AudioID + AHIA_MixFreq = source
 * rate, AHI_LoadSound, AHIC_Play TRUE, AHI_Play, Delay for the audio's
 * duration, AHIC_Play FALSE, AHI_FreeAudio, CloseDevice. Between modes:
 * delay ~2 seconds of silence so the host-side split-on-silence works.
 *
 * Note on the post-playback DC tail: paula_11025_probe showed paula's DAC
 * holding a constant value after AHIC_Play FALSE; this probe issues
 * AHI_FreeAudio and CloseDevice between modes which fully resets state.
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

#define INPUT_PATH      "Narrator:capture_raw.wav"
#define HEADROOM_TICKS  25     /* 0.5s on top of audio duration */
#define GAP_TICKS      100     /* ~2s silence between modes (for split-on-silence) */

struct Library *AHIBase;

struct ModeTest {
    unsigned long mode_id;
    const char   *name;
};

/* Add/remove modes here to sweep different candidates. */
static const struct ModeTest TESTS[] = {
    { 0x0002000fUL, "paula HiFi 14 bit mono calibrated" },
    { 0x003b0002UL, "uaesnd HiFi Stereo"                },
    { 0x001a0000UL, "UAE 16 bit HIFI Stereo++"          },
};
#define NTESTS  (sizeof(TESTS) / sizeof(TESTS[0]))

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

/* Play `buf` (BE samples, sample_bytes total) through AHI on `mode_id` at
 * `rate` Hz. Returns 0 on success, non-zero rc on failure (prints which). */
static int play_through(unsigned long mode_id, const char *name,
                        unsigned char *buf, long sample_bytes,
                        unsigned long rate)
{
    struct MsgPort       *port;
    struct AHIRequest    *req;
    struct AHIAudioCtrl  *ctrl;
    struct AHISampleInfo  si;
    ULONG                 rc;
    long                  wait_ticks;
    unsigned long         samples = (unsigned long)(sample_bytes / 2);

    printf("  mode 0x%08lx (%s)\n", mode_id, name);
    fflush(stdout);

    port = CreateMsgPort();
    if (!port) { printf("  FAIL: CreateMsgPort\n"); return 1; }
    req = (struct AHIRequest *)CreateIORequest(port, sizeof(struct AHIRequest));
    if (!req) { printf("  FAIL: CreateIORequest\n"); DeleteMsgPort(port); return 1; }
    req->ahir_Version = 4;
    if (OpenDevice((STRPTR)"ahi.device", AHI_NO_UNIT,
                   (struct IORequest *)req, 0) != 0) {
        printf("  FAIL: OpenDevice\n");
        DeleteIORequest((struct IORequest *)req); DeleteMsgPort(port); return 1;
    }
    AHIBase = (struct Library *)req->ahir_Std.io_Device;

    ctrl = AHI_AllocAudio(
        AHIA_AudioID,  mode_id,
        AHIA_MixFreq,  rate,
        AHIA_Channels, 1UL,
        AHIA_Sounds,   1UL,
        TAG_END);
    if (!ctrl) {
        printf("  FAIL: AHI_AllocAudio (mode unavailable)\n");
        CloseDevice((struct IORequest *)req);
        DeleteIORequest((struct IORequest *)req); DeleteMsgPort(port); return 1;
    }

    si.ahisi_Type    = AHIST_M16S;
    si.ahisi_Address = buf;
    si.ahisi_Length  = sample_bytes;
    rc = AHI_LoadSound(0, AHIST_SAMPLE, &si, ctrl);
    if (rc != 0) {
        printf("  FAIL: AHI_LoadSound rc=%lu\n", (unsigned long)rc);
        AHI_FreeAudio(ctrl);
        CloseDevice((struct IORequest *)req);
        DeleteIORequest((struct IORequest *)req); DeleteMsgPort(port); return 1;
    }

    rc = AHI_ControlAudio(ctrl, AHIC_Play, TRUE, TAG_END);
    if (rc != 0) {
        printf("  FAIL: AHIC_Play rc=%lu\n", (unsigned long)rc);
        AHI_UnloadSound(0, ctrl); AHI_FreeAudio(ctrl);
        CloseDevice((struct IORequest *)req);
        DeleteIORequest((struct IORequest *)req); DeleteMsgPort(port); return 1;
    }

    AHI_Play(ctrl,
             AHIP_BeginChannel, 0UL,
             AHIP_Freq,         rate,
             AHIP_Vol,          0x10000UL,
             AHIP_Pan,          0x8000UL,
             AHIP_Sound,        0UL,
             AHIP_EndChannel,   0UL,
             TAG_END);

    wait_ticks = (long)((samples * 50UL) / rate) + HEADROOM_TICKS;
    Delay(wait_ticks);

    AHI_ControlAudio(ctrl, AHIC_Play, FALSE, TAG_END);
    AHI_UnloadSound(0, ctrl);
    AHI_FreeAudio(ctrl);
    CloseDevice((struct IORequest *)req);
    DeleteIORequest((struct IORequest *)req);
    DeleteMsgPort(port);
    return 0;
}

int main(void)
{
    FILE *f;
    unsigned char  hdr[44];
    unsigned char *buf;
    long           buf_bytes, i;
    unsigned long  rate, data_size, samples;
    unsigned short channels, bits;
    int            k;

    printf("ahi_modes_probe: reading %s\n", INPUT_PATH);
    fflush(stdout);

    f = fopen(INPUT_PATH, "rb");
    if (!f)                          { printf("FAIL: fopen\n"); return 20; }
    if (fread(hdr, 1, 44, f) != 44)  { printf("FAIL: short header\n"); return 20; }
    if (memcmp(hdr,     "RIFF", 4)) { printf("FAIL: not RIFF\n"); return 20; }
    if (memcmp(hdr + 8, "WAVE", 4)) { printf("FAIL: not WAVE\n"); return 20; }
    channels  = u16_le(hdr + 22);
    rate      = u32_le(hdr + 24);
    bits      = u16_le(hdr + 34);
    data_size = u32_le(hdr + 40);
    samples   = data_size / (channels * (bits / 8));
    {
        unsigned long secs = samples / rate;
        unsigned long ms   = ((samples - secs * rate) * 1000UL) / rate;
        printf("  format: %ldHz %d-bit %d ch (%ld samples, %ld.%03lds)\n",
               (long)rate, (int)bits, (int)channels, (long)samples, secs, ms);
    }
    fflush(stdout);
    if (channels != 1 || bits != 16) {
        printf("FAIL: need mono 16-bit\n"); return 20;
    }

    buf_bytes = (long)samples * 2;
    buf = (unsigned char *)malloc(buf_bytes);
    if (!buf)                                            { printf("FAIL: malloc\n"); return 20; }
    if ((long)fread(buf, 1, buf_bytes, f) != buf_bytes) { printf("FAIL: short read\n"); return 20; }
    fclose(f);

    /* LE -> BE for AHIST_M16S. Same as paula_lib_probe. */
    for (i = 0; i + 1 < buf_bytes; i += 2) {
        unsigned char t = buf[i]; buf[i] = buf[i + 1]; buf[i + 1] = t;
    }

    /* Sweep all modes back-to-back, with silence between for split-on-silence. */
    for (k = 0; k < (int)NTESTS; k++) {
        printf("--- test %d/%d ---\n", k + 1, (int)NTESTS);
        fflush(stdout);
        (void)play_through(TESTS[k].mode_id, TESTS[k].name, buf, buf_bytes, rate);
        if (k + 1 < (int)NTESTS) {
            printf("  (silence gap)\n"); fflush(stdout);
            Delay(GAP_TICKS);
        }
    }

    free(buf);
    printf("done\n");
    return 0;
}
