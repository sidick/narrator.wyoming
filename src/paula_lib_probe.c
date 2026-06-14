/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Simon Dick
 */

/* paula_lib_probe.c — replay a WAV through paula.audio via AHI LIBRARY API.
 *
 * Companion to ahi_capture.c (which targets the filesave driver and writes an
 * AIFF). This one targets the SAME output driver our normal nw_engine.c uses
 * (paula.audio) so the host-side BlackHole capture is directly comparable to a
 * Say acceptance-test recording — the ONLY difference between the two is HOW
 * AHI is consumed:
 *
 *   nw_engine.c / audio_ahi.c  =  device interface
 *     OpenDevice("ahi.device", unit, ...) then CMD_WRITE with AHIRequest, mode
 *     inherited from the unit's prefs entry. This is the path that currently
 *     produces the harsh 11-15 kHz aliasing measured in docs/audio-capture-rig.md
 *
 *   THIS PROBE  =  library interface (the path Play16 almost certainly uses)
 *     OpenDevice("ahi.device", AHI_NO_UNIT, ...) then AHI_AllocAudio with an
 *     explicit AHIA_AudioID (paula mode) and AHIA_MixFreq=source rate. Then
 *     AHI_LoadSound + AHI_Play. The unit prefs entry is bypassed entirely.
 *
 * Decision tree after running this against the BlackHole rig:
 *   no >11 kHz aliasing  -> library interface IS the fix. Refactor audio_ahi.c
 *                           and nw_engine.c onto AHI_AllocAudio + AHI_Play and
 *                           the smooth filter can be removed.
 *   same aliasing        -> library interface ISN'T the cause. Look elsewhere
 *                           (likely the paula mode itself; try a different mode
 *                           ID or mix freq before any refactor).
 *   different bands hot  -> the chain is more nuanced than the binary; informs
 *                           further probing without committing to a rewrite.
 *
 * Hardcoded paula audio mode: 0x0002000c. This is the mode for unit 0 in the
 * test setup's ahi.prefs (Paula 8-bit something). If you've reconfigured your
 * paula prefs, edit MODE_ID below to whichever mode unit 0 actually uses, so
 * the probe stays faithful to "same mode, different consumer API".
 *
 * Unlike filesave, paula real-time-paces playback: it plays at the audio's
 * actual duration, so we Delay for that + a small headroom. No ASL requester
 * or rendering-finished dialog (those are filesave-only).
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
#define MODE_ID         0x00020018UL    /* paula candidate mode — empirically NOT better
                                         * than unit 0's 0x0002000c (see commit log).
                                         * Edit this to test other paula modes; the
                                         * docs/audio-capture-rig.md rig will report
                                         * per-band RMS so you can compare. */
#define HEADROOM_TICKS  25              /* 0.5 s on top of the audio's duration */

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
    long           buf_bytes, i;
    unsigned long  rate, data_size, samples;
    unsigned short channels, bits;
    struct MsgPort       *port;
    struct AHIRequest    *req;
    struct AHIAudioCtrl  *ctrl;
    struct AHISampleInfo  si;
    ULONG                 rc;
    long                  wait_ticks;

    printf("paula_lib_probe: reading %s\n", INPUT_PATH);
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

    /* LE WAV -> BE for AHIST_M16S (same conversion audio_ahi.c does). */
    for (i = 0; i + 1 < buf_bytes; i += 2) {
        unsigned char t = buf[i]; buf[i] = buf[i + 1]; buf[i + 1] = t;
    }

    /* AHI library interface: AHI_NO_UNIT means "I'll pick the mode myself". */
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

    printf("  AHI_AllocAudio (paula mode 0x%lx, MixFreq=%ldHz, mono)\n",
           MODE_ID, (long)rate);
    fflush(stdout);
    ctrl = AHI_AllocAudio(
        AHIA_AudioID,  MODE_ID,
        AHIA_MixFreq,  rate,
        AHIA_Channels, 1UL,
        AHIA_Sounds,   1UL,
        TAG_END);
    if (!ctrl) { printf("FAIL: AHI_AllocAudio (mode unavailable?)\n"); return 20; }
    printf("  AHI_AllocAudio OK\n"); fflush(stdout);

    si.ahisi_Type    = AHIST_M16S;
    si.ahisi_Address = buf;
    si.ahisi_Length  = buf_bytes;
    rc = AHI_LoadSound(0, AHIST_SAMPLE, &si, ctrl);
    if (rc != 0) { printf("FAIL: AHI_LoadSound rc=%lu\n", (unsigned long)rc); return 20; }
    printf("  AHI_LoadSound OK\n"); fflush(stdout);

    rc = AHI_ControlAudio(ctrl, AHIC_Play, TRUE, TAG_END);
    if (rc != 0) { printf("FAIL: AHIC_Play TRUE rc=%lu\n", (unsigned long)rc); return 20; }
    printf("  AHIC_Play TRUE (mixer running, paula will start)\n"); fflush(stdout);

    AHI_Play(ctrl,
             AHIP_BeginChannel, 0UL,
             AHIP_Freq,         rate,
             AHIP_Vol,          0x10000UL,
             AHIP_Pan,          0x8000UL,
             AHIP_Sound,        0UL,
             AHIP_EndChannel,   0UL,
             TAG_END);

    /* Paula real-time-paces, so wait the audio's actual duration plus a
     * small headroom — unlike filesave's flat-out CPU-bound rendering. */
    wait_ticks = (long)((samples * 50UL) / rate) + HEADROOM_TICKS;
    printf("  AHI_Play submitted; Delay(%ld ticks = %ld.%lds wall)\n",
           wait_ticks, wait_ticks / 50, ((wait_ticks % 50) * 10) / 50);
    fflush(stdout);
    Delay(wait_ticks);

    AHI_ControlAudio(ctrl, AHIC_Play, FALSE, TAG_END);
    AHI_UnloadSound(0, ctrl);
    AHI_FreeAudio(ctrl);
    free(buf);
    CloseDevice((struct IORequest *)req);
    DeleteIORequest((struct IORequest *)req);
    DeleteMsgPort(port);
    printf("done\n");
    return 0;
}
