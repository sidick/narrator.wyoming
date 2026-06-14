/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Simon Dick
 */

/* filesave_lib_probe.c — AHI LIBRARY-interface probe against filesave.
 *
 * Companion to filesave_probe.c (which uses the ahi.device CMD_WRITE device
 * interface and HANGS on filesave). This probe uses AHI v4's library API:
 *   OpenDevice("ahi.device", AHI_NO_UNIT, ...)
 *   AHIBase = io_Device
 *   AHI_AllocAudio(AHIA_AudioID = 0x00010003 = filesave 16-bit mono AIFF, ...)
 *   AHI_LoadSound(slot=0, AHIST_SAMPLE, &sampleInfo, ctrl)
 *   AHI_ControlAudio(AHIC_Play, TRUE, ...)
 *   AHI_Play(ctrl, channel 0 -> sound 0 at full vol center)
 *   Delay enough for the sound to render
 *   AHI_FreeAudio + CloseDevice
 *
 * If filesave produces a real AIFF (not just the 54-byte skeleton we saw with
 * CMD_WRITE), the library interface IS the supported consumer for filesave —
 * confirming that rewriting our streaming path onto AHI_Play (option 1 in
 * task 5) is the route to filesave-based capture.
 *
 * As with the device-interface probe, the ASL file requester fires when AHI
 * loads the filesave driver (at AHI_AllocAudio time for the library API);
 * dismiss it externally via Amiberry MCP input.
 */

#include <exec/memory.h>
#include <exec/io.h>
#include <devices/ahi.h>
#include <dos/dos.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/ahi.h>

#include <stdio.h>
#include <time.h>

#define MODE_ID  0x00010003UL   /* FILESAVE: HiFi 16-bit mono AIFF */
#define RATE     22050
#define SECONDS  1
#define SAMPLES  (RATE * SECONDS)
#define BUFBYTES (SAMPLES * 2)

/* proto/ahi.h's inlines call AHI_***() macros through AHIBase. */
struct Library *AHIBase;

/* 440Hz sine, rendered DIRECTLY in AHI's native big-endian (AHIST_M16S). */
static void sine_be(unsigned char *b, long samples, int freq, int rate)
{
    static const short table[16] = {
            0,  12539,  23169,  30273,  32767,  30273,  23169,  12539,
            0, -12539, -23169, -30273, -32767, -30273, -23169, -12539
    };
    long i;
    for (i = 0; i < samples; i++) {
        long idx = ((long)i * freq * 16 / rate) & 15;
        short s = table[idx];
        b[2*i]   = (unsigned char)((s >> 8) & 0xff);  /* BE: high byte first */
        b[2*i+1] = (unsigned char)(s & 0xff);
    }
}

int main(void)
{
    struct MsgPort       *port;
    struct AHIRequest    *req;
    struct AHIAudioCtrl  *ctrl = 0;
    struct AHISampleInfo  si;
    unsigned char        *buf;
    ULONG                 rc;
    time_t                t0, t1;

    printf("filesave_lib_probe: AHI library interface against filesave (mode 0x%lx)\n",
           MODE_ID);
    fflush(stdout);

    port = CreateMsgPort();
    if (!port) { printf("FAIL: CreateMsgPort\n"); fflush(stdout); return 20; }

    req = (struct AHIRequest *)CreateIORequest(port, sizeof(struct AHIRequest));
    if (!req) { printf("FAIL: CreateIORequest\n"); fflush(stdout); return 20; }
    req->ahir_Version = 4;

    /* AHI_NO_UNIT (-1) opens the ahi.device for library-interface use; the
     * audio mode is then picked explicitly via AHIA_AudioID at AHI_AllocAudio. */
    if (OpenDevice((STRPTR)"ahi.device", AHI_NO_UNIT,
                   (struct IORequest *)req, 0) != 0) {
        printf("FAIL: OpenDevice ahi.device AHI_NO_UNIT\n");
        fflush(stdout);
        return 20;
    }
    AHIBase = (struct Library *)req->ahir_Std.io_Device;
    printf("OpenDevice OK; AHIBase=%p\n", AHIBase);
    fflush(stdout);

    /* AHI_AllocAudio: this is where filesave's driver gets loaded and the ASL
     * file requester pops up. Modal — dismiss externally. */
    printf("AHI_AllocAudio (filesave mode, 22050Hz mono) -> requester will appear\n");
    fflush(stdout);
    ctrl = AHI_AllocAudio(
        AHIA_AudioID,  MODE_ID,
        AHIA_MixFreq,  (ULONG)RATE,
        AHIA_Channels, 1UL,
        AHIA_Sounds,   1UL,
        TAG_END);
    if (!ctrl) {
        printf("FAIL: AHI_AllocAudio (mode unavailable or rejected)\n");
        fflush(stdout);
        return 20;
    }
    printf("AHI_AllocAudio OK; ctrl=%p\n", ctrl);
    fflush(stdout);

    buf = (unsigned char *)AllocMem(BUFBYTES, MEMF_PUBLIC);
    if (!buf) { printf("FAIL: AllocMem %d bytes\n", BUFBYTES); fflush(stdout); return 20; }
    sine_be(buf, SAMPLES, 440, RATE);

    si.ahisi_Type    = AHIST_M16S;
    si.ahisi_Address = buf;
    si.ahisi_Length  = BUFBYTES;

    rc = AHI_LoadSound(0, AHIST_SAMPLE, &si, ctrl);
    if (rc != 0) {
        printf("FAIL: AHI_LoadSound -> %lu\n", (unsigned long)rc);
        fflush(stdout);
        return 20;
    }
    printf("AHI_LoadSound OK\n");
    fflush(stdout);

    rc = AHI_ControlAudio(ctrl,
                          AHIC_Play, TRUE,
                          TAG_END);
    if (rc != 0) {
        printf("FAIL: AHIC_Play TRUE -> %lu\n", (unsigned long)rc);
        fflush(stdout);
        return 20;
    }
    printf("AHIC_Play TRUE (mixer running)\n");
    fflush(stdout);

    AHI_Play(ctrl,
             AHIP_BeginChannel, 0UL,
             AHIP_Freq,         (ULONG)RATE,
             AHIP_Vol,          0x10000UL,
             AHIP_Pan,          0x8000UL,
             AHIP_Sound,        0UL,
             AHIP_EndChannel,   0UL,
             TAG_END);
    printf("AHI_Play submitted; waiting 2s for filesave to render\n");
    fflush(stdout);

    time(&t0);
    Delay(50 * 2);                       /* ~2 seconds at 50 ticks/s */
    time(&t1);
    printf("waited %lds; tearing down\n", (long)(t1 - t0));
    fflush(stdout);

    AHI_ControlAudio(ctrl, AHIC_Play, FALSE, TAG_END);
    AHI_UnloadSound(0, ctrl);
    AHI_FreeAudio(ctrl);
    FreeMem(buf, BUFBYTES);
    CloseDevice((struct IORequest *)req);
    DeleteIORequest((struct IORequest *)req);
    DeleteMsgPort(port);
    printf("done\n");
    return 0;
}
