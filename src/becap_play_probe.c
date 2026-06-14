/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Simon Dick
 */

/* becap_play_probe.c — read saytest.becap (BE PCM dumped by audio_ahi.c's
 * debug tap) and play it through AHI, no streaming, no mid-play writes.
 *
 * Isolates: is the click source AHI's playback of THIS PCM content, or
 * is it triggered by our streaming write pattern? Probe loads the whole
 * file into a static buffer, AHI_LoadSound + AHI_Play one-shot.
 *
 * If clicks present → AHI/Amiberry can't play this content cleanly. The
 * issue is downstream of our PCM (which host replay verified is clean).
 *
 * If clicks absent → something specifically in saytest's path introduces
 * them (despite host replay being clean, which would be very strange).
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

#define MODE_ID  0x0002000fUL    /* paula HiFi 14-bit mono cal */
#define RATE     22050

struct Library *AHIBase;

int main(void)
{
    struct MsgPort       *port;
    struct AHIRequest    *req;
    struct AHIAudioCtrl  *ctrl;
    struct AHISampleInfo  si;
    FILE                 *f;
    unsigned char        *buf;
    long                  flen;
    long                  duration_ticks;

    printf("becap_play_probe: play saytest.becap via AHI, no streaming\n");
    fflush(stdout);

    f = fopen("Narrator:saytest.becap", "rb");
    if (!f) { printf("FAIL: open saytest.becap\n"); return 20; }
    fseek(f, 0, SEEK_END);
    flen = ftell(f);
    fseek(f, 0, SEEK_SET);
    printf("  becap is %ld bytes (~%ld ms at 22050 Hz mono 16-bit)\n",
           flen, (flen / 2L * 1000L) / 22050L);
    fflush(stdout);

    buf = (unsigned char *)AllocMem(flen, MEMF_PUBLIC);
    if (!buf) { printf("FAIL: AllocMem\n"); fclose(f); return 20; }
    if ((long)fread(buf, 1, (size_t)flen, f) != flen) {
        printf("FAIL: short fread\n"); FreeMem(buf, flen); fclose(f); return 20;
    }
    fclose(f);

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
        AHIA_AudioID,  MODE_ID,
        AHIA_MixFreq,  (ULONG)RATE,
        AHIA_Channels, 1UL,
        AHIA_Sounds,   1UL,
        TAG_END);
    if (!ctrl) { printf("FAIL: AHI_AllocAudio\n"); return 20; }

    si.ahisi_Type    = AHIST_M16S;
    si.ahisi_Address = buf;
    si.ahisi_Length  = flen >> 1;  /* samples (devices/ahi.h), M16S */
    if (AHI_LoadSound(0, AHIST_SAMPLE, &si, ctrl) != 0) {
        printf("FAIL: AHI_LoadSound\n"); return 20;
    }
    if (AHI_ControlAudio(ctrl, AHIC_Play, TRUE, TAG_END) != 0) {
        printf("FAIL: AHIC_Play TRUE\n"); return 20;
    }
    AHI_Play(ctrl,
             AHIP_BeginChannel, 0UL,
             AHIP_Freq,         (ULONG)RATE,
             AHIP_Vol,          0x10000UL,
             AHIP_Pan,          0x8000UL,
             AHIP_Sound,        0UL,
             AHIP_EndChannel,   0UL,
             TAG_END);

    duration_ticks = (long)(((flen / 2L) * 50UL) / RATE) + 5;
    printf("  playing; Delay %ld ticks (~%ld ms)\n",
           duration_ticks, duration_ticks * 20);
    fflush(stdout);
    Delay(duration_ticks);

    AHI_ControlAudio(ctrl, AHIC_Play, FALSE, TAG_END);
    AHI_UnloadSound(0, ctrl);
    AHI_FreeAudio(ctrl);
    FreeMem(buf, flen);
    CloseDevice((struct IORequest *)req);
    DeleteIORequest((struct IORequest *)req);
    DeleteMsgPort(port);

    printf("done\n");
    return 0;
}
