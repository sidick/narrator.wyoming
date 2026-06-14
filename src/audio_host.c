/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Simon Dick
 */

/* audio_host.c — host backend for audio.h: writes a .wav file.
 *
 * Lets us verify on the host that the Wyoming->PCM path produces correct audio
 * (play say.wav, or analyse it) without needing AHI. */
#ifdef PLATFORM_POSIX

#include "audio.h"

#include <stdio.h>

static FILE       *g_f;
static const char *g_path = "say.wav";
static long        g_data;
static int         g_rate, g_width, g_channels;

static void wr16(FILE *f, unsigned v) { fputc(v & 0xff, f); fputc((v >> 8) & 0xff, f); }
static void wr32(FILE *f, unsigned long v)
{
    fputc(v & 0xff, f); fputc((v >> 8) & 0xff, f);
    fputc((v >> 16) & 0xff, f); fputc((v >> 24) & 0xff, f);
}

void audio_set_outfile(const char *path)
{
    if (path && *path) g_path = path;
}

void audio_set_mode(unsigned long mode_id) { (void)mode_id; }   /* AHI-only; ignored on host */

int audio_open(int rate, int width, int channels)
{
    g_rate = rate; g_width = width; g_channels = channels; g_data = 0;
    g_f = fopen(g_path, "wb");
    if (!g_f) return -1;

    /* 44-byte canonical WAV header; RIFF/data sizes patched in audio_close(). */
    fwrite("RIFF", 1, 4, g_f); wr32(g_f, 0); fwrite("WAVE", 1, 4, g_f);
    fwrite("fmt ", 1, 4, g_f); wr32(g_f, 16);
    wr16(g_f, 1);                                  /* PCM */
    wr16(g_f, (unsigned)channels);
    wr32(g_f, (unsigned long)rate);
    wr32(g_f, (unsigned long)rate * channels * width); /* byte rate */
    wr16(g_f, (unsigned)(channels * width));           /* block align */
    wr16(g_f, (unsigned)(width * 8));                  /* bits/sample */
    fwrite("data", 1, 4, g_f); wr32(g_f, 0);
    return 0;
}

long audio_write(const void *buf, long len)
{
    if (!g_f) return -1;
    if (len > 0) { fwrite(buf, 1, (size_t)len, g_f); g_data += len; }
    return len;
}

void audio_close(void)
{
    if (!g_f) return;
    fseek(g_f, 4, SEEK_SET);  wr32(g_f, (unsigned long)(36 + g_data));
    fseek(g_f, 40, SEEK_SET); wr32(g_f, (unsigned long)g_data);
    fclose(g_f);
    g_f = NULL;
    printf("wrote %s (%ld PCM bytes; %d Hz, %d-bit, %dch)\n",
           g_path, g_data, g_rate, g_width * 8, g_channels);
}

#endif /* PLATFORM_POSIX */
