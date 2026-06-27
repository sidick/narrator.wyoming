/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Simon Dick
 */

/* pcmplay.c — network-free AHI playback harness for deterministic audio
 * capture under Copperline (`copperline --audio-wav out.wav`).
 *
 * The full Say -> Wyoming -> AHI pipeline can only run under Amiberry
 * (bsdsocket.library emulation); Copperline has no host networking yet.
 * But Copperline CAN do what Amiberry can't: dump the mixed Paula output
 * to a WAV in emulated time, headless and deterministically. That makes it
 * the right harness for the AHI-rendering layer — exactly where this
 * project's silent bugs live (the LE->BE byte-swap static, audio-mode
 * aliasing, wrong sample rate).
 *
 * This program decouples that layer from the network: it reads a fixed
 * raw-PCM fixture from disk and plays it through the SAME audio.h sink the
 * device/saytest use (audio_ahi.c on Amiga), so a captured WAV reflects the
 * real production playback path. Capture a fixture once on the host
 * (`wyomingtest --out fixture.pcm`, or a synthetic sine — see
 * copperline/make-fixture.sh) and diff the WAV across builds to catch AHI
 * regressions with no BlackHole rig and no listening by ear.
 *
 * Input PCM: raw, headerless, signed 16-bit LITTLE-ENDIAN, mono (or stereo
 * with channels=2) — i.e. exactly what Piper/Wyoming emits. audio_ahi.c
 * byteswaps to big-endian on the way into AHI, so the fixture must be LE,
 * NOT pre-swapped.
 *
 * Settings (argv is unusable when launched from the startup-sequence, same
 * as saytest) come from `pcmplay.cfg` in the current dir, all optional:
 *   file 22khz.pcm        input PCM path (default "fixture.pcm")
 *   rate 22050            sample rate
 *   channels 1            1 = mono, 2 = stereo
 *   mode 0x0002000f       AHI audio mode (AHIA_AudioID); default paula
 *                         HiFi 14-bit mono calibrated. For a safer first
 *                         bring-up try a plain Paula mode (e.g. 0x00020004
 *                         "Paula:8 bit mono") to confirm capture works
 *                         before trusting the HiFi 14-bit volume trick.
 */
#include "audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_FILE "fixture.pcm"
#define CHUNK_BUF    4096

static char          g_file[256]  = DEFAULT_FILE;
static int           g_rate       = 22050;
static int           g_channels   = 1;
static unsigned long g_mode       = 0x0002000fUL;

static char *trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
        *--e = '\0';
    return s;
}

/* Tiny key/value config, same shape as saytest's read_config. Lines are
 * "key value"; blank lines and '#'/';' comments ignored. Absent file is
 * fine — defaults stand. */
static void read_config(const char *path)
{
    FILE *f = fopen(path, "r");
    char  ln[512];
    if (!f) return;
    while (fgets(ln, sizeof ln, f)) {
        char *p = trim(ln);
        char *v;
        if (!*p || *p == '#' || *p == ';') continue;
        v = p;
        while (*v && *v != ' ' && *v != '\t') v++;
        if (*v) *v++ = '\0';
        v = trim(v);
        if      (!strcmp(p, "file"))     { strncpy(g_file, v, sizeof g_file - 1);
                                           g_file[sizeof g_file - 1] = '\0'; }
        else if (!strcmp(p, "rate"))     g_rate     = atoi(v);
        else if (!strcmp(p, "channels")) g_channels = atoi(v);
        else if (!strcmp(p, "mode"))     g_mode     = strtoul(v, NULL, 0);
    }
    fclose(f);
}

int main(void)
{
    FILE         *f;
    char          buf[CHUNK_BUF];
    long          total = 0;
    size_t        n;

    read_config("pcmplay.cfg");

    f = fopen(g_file, "rb");
    if (!f) {
        fprintf(stderr, "pcmplay: cannot open %s\n", g_file);
        return 20;
    }

    audio_set_mode(g_mode);
    if (audio_open(g_rate, 2, g_channels) < 0) {
        fprintf(stderr, "pcmplay: audio_open failed (mode 0x%08lx)\n", g_mode);
        fclose(f);
        return 20;
    }

    fprintf(stderr, "pcmplay: %s -> AHI %d Hz %dch mode 0x%08lx\n",
            g_file, g_rate, g_channels, g_mode);

    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        if (audio_write(buf, (long)n) < 0) {
            fprintf(stderr, "pcmplay: audio_write rejected (buffer full at %ld)\n",
                    total);
            break;
        }
        total += (long)n;
    }
    fclose(f);

    audio_close();
    fprintf(stderr, "pcmplay: done, %ld bytes (~%ld ms)\n",
            total, (total / 2L) * 1000L / (g_rate * g_channels));
    return 0;
}
