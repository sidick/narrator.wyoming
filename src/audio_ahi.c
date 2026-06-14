/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Simon Dick
 */

/* audio_ahi.c — Amiga backend for audio.h: AHI v4 library-interface playback.
 *
 * Uses AHI's library interface (AHI_AllocAudio + AHI_LoadSound + AHI_Play)
 * with an explicit AHIA_AudioID so the audio mode is OUR choice, not whatever
 * the user happens to have on ahi.device unit 0 (which is typically "Paula:
 * Fast 8 bit mono" — the worst-quality option). Default mode is paula HiFi
 * 14-bit mono calibrated (0x0002000f); the caller may override via
 * audio_set_mode() before audio_open() (see audio.h).
 *
 * The previous CMD_WRITE-based version supported gapless streaming via
 * ahir_Link. This version is BUFFERED: audio_write() accumulates into a
 * growing PCM buffer; audio_close() submits the whole accumulated buffer
 * via AHI_LoadSound + AHI_Play + Delay(duration). The trade-off: first-audio
 * latency goes up by ~the receive time of the whole utterance, but the
 * audio mode is now well-defined and chosen for quality. See
 * docs/audio-capture-rig.md for the measurements that motivated this.
 */
#ifdef PLATFORM_AMIGA

#include "audio.h"

#include <exec/memory.h>
#include <exec/io.h>
#include <devices/ahi.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/ahi.h>

#include <string.h>
#include <stdio.h>

/* Initial buffer size; we grow geometrically as audio_write() appends. */
#define INITIAL_CAP   (32 * 1024)

struct Library *AHIBase;

static struct MsgPort       *g_port;
static struct AHIRequest    *g_req;
static struct AHIAudioCtrl  *g_ctrl;
static int                   g_opened;       /* OpenDevice succeeded         */
static int                   g_alloced;      /* AHI_AllocAudio succeeded     */
static unsigned char        *g_buf;          /* accumulating LE PCM          */
static long                  g_cap;          /* allocated bytes              */
static long                  g_len;          /* written bytes                */
static unsigned long         g_rate;
static int                   g_type;
static unsigned long         g_mode_id = 0x0002000fUL;  /* paula HiFi 14 bit mono calibrated */

/* Piper/Wyoming PCM is 16-bit signed LITTLE-endian; AHI (AHIST_M*) wants the
 * Amiga's native BIG-endian order. We accumulate in LE and swap once at
 * audio_close before handing the buffer to AHI_LoadSound. */
static void swap16(unsigned char *b, long n)
{
    long i;
    for (i = 0; i + 1 < n; i += 2) {
        unsigned char t = b[i]; b[i] = b[i + 1]; b[i + 1] = t;
    }
}

/* Grow g_buf so it can hold need_bytes total. Returns 0 on success. */
static int ensure_cap(long need_bytes)
{
    long new_cap;
    unsigned char *new_buf;
    if (need_bytes <= g_cap) return 0;
    new_cap = g_cap ? g_cap : INITIAL_CAP;
    while (new_cap < need_bytes) new_cap *= 2;
    new_buf = (unsigned char *)AllocMem(new_cap, MEMF_PUBLIC);
    if (!new_buf) return -1;
    if (g_buf) {
        if (g_len > 0) CopyMem(g_buf, new_buf, (ULONG)g_len);
        FreeMem(g_buf, g_cap);
    }
    g_buf = new_buf;
    g_cap = new_cap;
    return 0;
}

void audio_set_outfile(const char *path) { (void)path; }
void audio_set_mode(unsigned long mode_id) { g_mode_id = mode_id; }

int audio_open(int rate, int width, int channels)
{
    if (width != 2)
        return -1;
    g_type = (channels >= 2) ? AHIST_S16S : AHIST_M16S;
    g_rate = (unsigned long)rate;
    g_len  = 0;

    if (ensure_cap(INITIAL_CAP) != 0) return -1;

    g_port = CreateMsgPort();
    if (!g_port) return -1;

    g_req = (struct AHIRequest *)CreateIORequest(g_port, sizeof(struct AHIRequest));
    if (!g_req) return -1;
    g_req->ahir_Version = 4;

    if (OpenDevice((STRPTR)"ahi.device", AHI_NO_UNIT,
                   (struct IORequest *)g_req, 0) != 0) {
        fprintf(stderr, "OpenDevice ahi.device AHI_NO_UNIT failed\n");
        return -1;
    }
    g_opened = 1;
    AHIBase = (struct Library *)g_req->ahir_Std.io_Device;

    g_ctrl = AHI_AllocAudio(
        AHIA_AudioID,  g_mode_id,
        AHIA_MixFreq,  g_rate,
        AHIA_Channels, 1UL,
        AHIA_Sounds,   1UL,
        TAG_END);
    if (!g_ctrl) {
        fprintf(stderr, "AHI_AllocAudio failed (mode 0x%08lx)\n", g_mode_id);
        return -1;
    }
    g_alloced = 1;

    /* Pre-roll the start with a brief silence — absorbs the cold-start DAC
     * transient so the utterance's onset isn't clipped. */
    return audio_write(NULL, ((long)g_rate >> 4) * 2) >= 0 ? 0 : -1;
}

/* data == NULL feeds `len` bytes of silence. */
long audio_write(const void *data, long len)
{
    if (len <= 0) return 0;
    if (ensure_cap(g_len + len) != 0) return -1;
    if (data) memcpy(g_buf + g_len, data, (size_t)len);
    else      memset(g_buf + g_len, 0,    (size_t)len);
    g_len += len;
    return len;
}

void audio_close(void)
{
    /* Brief post-roll silence so the playback's stop-click lands on silence. */
    audio_write(NULL, ((long)g_rate >> 4) * 2);

    if (g_alloced && g_len > 0) {
        struct AHISampleInfo si;
        long  duration_ticks;

        swap16(g_buf, g_len);
        si.ahisi_Type    = g_type;
        si.ahisi_Address = g_buf;
        si.ahisi_Length  = g_len;

        if (AHI_LoadSound(0, AHIST_SAMPLE, &si, g_ctrl) == 0
         && AHI_ControlAudio(g_ctrl, AHIC_Play, TRUE, TAG_END) == 0) {
            AHI_Play(g_ctrl,
                     AHIP_BeginChannel, 0UL,
                     AHIP_Freq,         g_rate,
                     AHIP_Vol,          0x10000UL,
                     AHIP_Pan,          0x8000UL,
                     AHIP_Sound,        0UL,
                     AHIP_EndChannel,   0UL,
                     TAG_END);

            /* Wait for playback to finish: samples / rate seconds, +25 ticks
             * (0.5s) of safety margin. Paula plays at real-time, so this is
             * accurate to within a frame. */
            duration_ticks = (long)(((g_len / 2) * 50UL) / g_rate) + 25;
            Delay(duration_ticks);

            AHI_ControlAudio(g_ctrl, AHIC_Play, FALSE, TAG_END);
            AHI_UnloadSound(0, g_ctrl);
        }
    }

    if (g_alloced) { AHI_FreeAudio(g_ctrl); g_alloced = 0; g_ctrl = NULL; }
    if (g_buf)     { FreeMem(g_buf, g_cap); g_buf = NULL; g_cap = 0; g_len = 0; }
    if (g_opened)  { CloseDevice((struct IORequest *)g_req); g_opened = 0; }
    if (g_req)     { DeleteIORequest((struct IORequest *)g_req); g_req = NULL; }
    if (g_port)    { DeleteMsgPort(g_port); g_port = NULL; }
}

#endif /* PLATFORM_AMIGA */
