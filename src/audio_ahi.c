/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Simon Dick
 */

/* audio_ahi.c — Amiga backend for audio.h: AHI v4 library-interface streaming.
 *
 * Uses AHI's library interface (AHI_AllocAudio + AHI_LoadSound + AHI_Play)
 * with an explicit AHIA_AudioID so the audio mode is OUR choice, not whatever
 * the user happens to have on ahi.device unit 0. Default mode is paula HiFi
 * 14-bit mono calibrated (0x0002000f); the caller may override via
 * audio_set_mode() before audio_open().
 *
 * Streaming v2: one large AHIST_DYNAMICSAMPLE buffer played NON-LOOPING.
 * audio_open allocates the buffer (silence-filled via MEMF_CLEAR) and
 * registers it with AHI_LoadSound. audio_write copies incoming PCM into
 * the buffer from the front, byteswapping LE -> BE atomically per 16-bit
 * sample (in-place swap would race AHI mid-play; copy-swap doesn't —
 * each destination short is written exactly once). The first real write
 * starts AHI_Play; subsequent writes just advance the write head. AHI's
 * play head follows naturally. audio_close Delays for the audio duration
 * then stops the mixer.
 *
 * Why one-shot non-looping: a small DYNAMICSAMPLE ring with looping +
 * frequent small mid-stream writes produces a ~50/50 audio/silence duty
 * cycle on Amiberry's paula HiFi mode (almost certainly a mix-buffer
 * cache + loop race). Non-looping playback reads each byte exactly once
 * so the race can't trigger. See docs/streaming-v2-notes.md for the
 * full investigation.
 *
 * First-audio latency: ~one Wyoming chunk's recv time + AHI startup
 * (vs v1 buffered's "whole utterance receive + then play" latency).
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

/* Sized for ~11.6 s of audio at 22050 Hz / 16-bit / mono — covers any
 * single split_words chunk and most full utterances. Beyond this we
 * truncate (audio_write returns -1 once full). */
#define BUF_BYTES (512L * 1024L)

/* Head start before AHI_Play kicks in. With less than this much audio
 * buffered, the write head can fall behind AHI's mix-buffer read-ahead
 * (~50 ms internally) on network-paced producers, and AHI reads the
 * MEMF_CLEAR silence at the gap → audible click at every ~chunk-write
 * boundary. ~370 ms covers AHI's read-ahead plus typical network
 * jitter on a real LAN. Trade-off: first-audio latency picks up
 * ~370 ms of "buffer first" instead of starting on chunk 1, but
 * Piper's wyoming-piper streams the first ~370 ms in well under
 * 100 ms on a fast LAN, so the effective added latency is the
 * network gap, not the wall time of 370 ms of audio. If audio_close
 * is reached with less buffered (very short utterance), we kick off
 * Play there with whatever we have. */
#define PLAY_HEADSTART_BYTES (32L * 1024L)

struct Library *AHIBase;

static struct MsgPort       *g_port;
static struct AHIRequest    *g_req;
static struct AHIAudioCtrl  *g_ctrl;
static int                   g_opened;
static int                   g_alloced;
static int                   g_loaded;
static int                   g_play_started;
static unsigned char        *g_buf;
static long                  g_buf_size;
static long                  g_write_pos;
static unsigned long         g_rate;
static int                   g_type;
static unsigned long         g_mode_id = 0x0002000fUL;  /* paula HiFi 14-bit mono cal */

/* Copy `len` bytes of LE 16-bit PCM from src to dst, byteswapping each
 * 16-bit sample to BE. Writes one word per sample so the 68k MOVE.W
 * makes the swap bus-atomic — AHI either sees the old word (silence)
 * or the new (correct sample), never a half-byte mix. Both pointers
 * must be 2-byte aligned; AllocMem gives 4-byte alignment and Wyoming
 * PCM chunks are 16-bit aligned naturally. */
static void copy_swap16(unsigned char *dst, const unsigned char *src, long len)
{
    /* Reading two adjacent bytes as a 68k word gives (byte0 << 8) | byte1.
     * Since src is LE (byte0 = low, byte1 = high), the read value is
     * actually the BYTE-SWAPPED sample. We swap once more — (v >> 8) |
     * (v << 8) — to get the correct sample value, then the MOVE.W to
     * dst[i] writes it in 68k-native BE order, which is what AHI wants. */
    short                 *d = (short *)dst;
    const unsigned short  *s = (const unsigned short *)src;
    long                   n = len >> 1;
    long                   i;
    for (i = 0; i < n; i++) {
        unsigned short v = s[i];
        d[i] = (short)((v >> 8) | (v << 8));
    }
}

static void start_play(void);

void audio_set_outfile(const char *path) { (void)path; }
void audio_set_mode(unsigned long mode_id) { g_mode_id = mode_id; }

int audio_open(int rate, int width, int channels)
{
    struct AHISampleInfo si;

    if (width != 2)
        return -1;
    g_type = (channels >= 2) ? AHIST_S16S : AHIST_M16S;
    g_rate = (unsigned long)rate;
    g_buf_size = BUF_BYTES;
    /* Skip a small silence pre-roll at the start of the buffer (the
     * MEMF_CLEAR-filled bytes are already zero) so AHI's startup
     * transient lands on silence — same trick v1 used, but the silence
     * comes for free instead of being explicitly written. */
    g_write_pos    = ((long)g_rate >> 4) * 2;
    g_play_started = 0;
    g_loaded       = 0;
    g_alloced      = 0;
    g_opened       = 0;

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

    g_buf = (unsigned char *)AllocMem(g_buf_size, MEMF_PUBLIC | MEMF_CLEAR);
    if (!g_buf) {
        fprintf(stderr, "AllocMem %ld failed\n", g_buf_size);
        return -1;
    }
    /* Register the silence-filled buffer with AHI. We register the WHOLE
     * buffer; bytes past the eventual end-of-audio are silence and AHI
     * plays them as silence until audio_close stops the mixer. */
    si.ahisi_Type    = g_type;
    si.ahisi_Address = g_buf;
    si.ahisi_Length  = g_buf_size;
    if (AHI_LoadSound(0, AHIST_DYNAMICSAMPLE, &si, g_ctrl) != 0) {
        fprintf(stderr, "AHI_LoadSound DYNAMICSAMPLE failed\n");
        return -1;
    }
    g_loaded = 1;
    return 0;
}

/* data == NULL feeds `len` bytes of silence (no-op since the buffer is
 * already zero, but advance write_pos so the silence is "accounted for"
 * in the duration math at audio_close). */
long audio_write(const void *data, long len)
{
    if (len <= 0) return 0;
    if (g_write_pos + len > g_buf_size) {
        /* Buffer full — caller has given us more audio than we can hold.
         * Truncate to what fits and return that, then refuse subsequent
         * writes. */
        len = g_buf_size - g_write_pos;
        if (len <= 0) return -1;
    }
    if (data) {
        copy_swap16(g_buf + g_write_pos, (const unsigned char *)data, len);
    }
    /* else: buffer is MEMF_CLEAR-zero, no need to memset                 */
    g_write_pos += len;

    /* Kick off playback once we've buffered enough head start. Subsequent
     * writes just advance the head; AHI follows. */
    if (!g_play_started && g_loaded && g_write_pos >= PLAY_HEADSTART_BYTES) {
        start_play();
    }
    return len;
}

static void start_play(void)
{
    if (g_play_started || !g_loaded) return;
    if (AHI_ControlAudio(g_ctrl, AHIC_Play, TRUE, TAG_END) != 0) return;
    AHI_Play(g_ctrl,
             AHIP_BeginChannel, 0UL,
             AHIP_Freq,         g_rate,
             AHIP_Vol,          0x10000UL,
             AHIP_Pan,          0x8000UL,
             AHIP_Sound,        0UL,
             AHIP_EndChannel,   0UL,
             TAG_END);
    g_play_started = 1;
}

void audio_close(void)
{
    /* Very short utterance — audio_close hit before headstart threshold;
     * kick off playback now with whatever we've got. */
    if (!g_play_started && g_loaded && g_write_pos > 0) {
        start_play();
    }
    if (g_play_started && g_write_pos > 0) {
        long duration_ticks;

        /* Wait for AHI to play out g_write_pos bytes' worth of audio.
         * Over-estimates by however long audio_write was active (because
         * playback was concurrent), but a safe upper bound — playback
         * has definitely finished by then. Tuning this would require
         * tracking AHI_Play wall time via timer.device; not worth the
         * complexity for first cut. */
        duration_ticks = (long)(((g_write_pos / 2L) * 50UL) / g_rate) + 5;
        Delay(duration_ticks);
        AHI_ControlAudio(g_ctrl, AHIC_Play, FALSE, TAG_END);
    }

    if (g_loaded)  { AHI_UnloadSound(0, g_ctrl); g_loaded = 0; }
    if (g_alloced) { AHI_FreeAudio(g_ctrl); g_alloced = 0; g_ctrl = NULL; }
    if (g_buf)     { FreeMem(g_buf, g_buf_size); g_buf = NULL; }
    if (g_opened)  { CloseDevice((struct IORequest *)g_req); g_opened = 0; }
    if (g_req)     { DeleteIORequest((struct IORequest *)g_req); g_req = NULL; }
    if (g_port)    { DeleteMsgPort(g_port); g_port = NULL; }
}

#endif /* PLATFORM_AMIGA */
