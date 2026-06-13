/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Simon Dick
 */

/* saytest.c — full pipeline: string -> Wyoming -> audio playback.
 *
 * Connects to Piper, sends one `synthesize`, and streams the returned PCM into
 * the audio sink as it arrives (audio.h): on the host that writes a .wav, on
 * the Amiga it plays through AHI. Playback is started as soon as the first
 * audio-chunk arrives, before the full response is in, to cut perceived
 * latency (audio_write() back-pressures to playback speed).
 *
 * Settings: argv on the host; on Amiga (no usable argv from the
 * startup-sequence) from config/narrator.wyoming in the current dir / Narrator:.
 *
 * Host:  ./saytest [--text "..."] [--voice NAME] [--out FILE.wav] <host> [port]
 */
#include "net.h"
#include "wyoming.h"
#include "audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_PORT 10200
#define DEFAULT_TEXT "Hello from the Amiga. This is neural speech over Wyoming."
#define CHUNK_BUF    4096

static char g_host[128]    = "";
static int  g_port         = DEFAULT_PORT;
static char g_text[1024]   = DEFAULT_TEXT;
static char g_voice[128]   = "";
static char g_language[64] = "";
static char g_speaker[32]  = "";
static char g_out[256]     = "say.wav";
static int  g_unit         = 0;        /* ahi.device unit (Amiga backend) */

static void set_str(char *dst, size_t n, const char *v)
{
    strncpy(dst, v, n - 1);
    dst[n - 1] = '\0';
}

static char *trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
        *--e = '\0';
    return s;
}

static int read_config(const char *path)
{
    FILE *f = fopen(path, "r");
    char ln[1100];
    int got_host = 0;
    if (!f) return 0;
    while (fgets(ln, sizeof(ln), f)) {
        char *p = trim(ln), *sp, *val;
        char key[32];
        size_t kl;
        if (!*p || *p == '#' || *p == ';') continue;
        sp = p;
        while (*sp && *sp != ' ' && *sp != '\t') sp++;
        kl = (size_t)(sp - p);
        if (kl >= sizeof(key)) kl = sizeof(key) - 1;
        memcpy(key, p, kl); key[kl] = '\0';
        val = trim(sp);
        {   /* strip inline comment: whitespace followed by # or ; */
            char *c;
            for (c = val; *c; c++)
                if ((*c == '#' || *c == ';') && c > val &&
                    (c[-1] == ' ' || c[-1] == '\t')) { *c = '\0'; break; }
            val = trim(val);
        }
        if      (!strcmp(key, "host"))     { set_str(g_host, sizeof g_host, val); got_host = 1; }
        else if (!strcmp(key, "port"))     g_port = atoi(val);
        else if (!strcmp(key, "text"))     set_str(g_text, sizeof g_text, val);
        else if (!strcmp(key, "voice"))    set_str(g_voice, sizeof g_voice, val);
        else if (!strcmp(key, "language")) set_str(g_language, sizeof g_language, val);
        else if (!strcmp(key, "speaker"))  set_str(g_speaker, sizeof g_speaker, val);
        else if (!strcmp(key, "wav") ||
                 !strcmp(key, "out"))      set_str(g_out, sizeof g_out, val);
        else if (!strcmp(key, "ahi_unit")) g_unit = atoi(val);
    }
    fclose(f);
    return got_host;
}

int main(int argc, char **argv)
{
    int sock, opened = 0, stopped = 0, rc = 0;
    long total = 0;
    unsigned long t_send = 0, t_done;
    int rate = 0, width = 0, channels = 0;
    char *buf;
    WyoEvent ev;

    if (!read_config("config/narrator.wyoming"))
        read_config("Narrator:config/narrator.wyoming");

#ifndef PLATFORM_AMIGA
    {
        int i;
        for (i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "--text") && i + 1 < argc) set_str(g_text, sizeof g_text, argv[++i]);
            else if (!strcmp(argv[i], "--voice") && i + 1 < argc) set_str(g_voice, sizeof g_voice, argv[++i]);
            else if (!strcmp(argv[i], "--out") && i + 1 < argc) set_str(g_out, sizeof g_out, argv[++i]);
            else if (argv[i][0] == '-') { fprintf(stderr, "unknown option: %s\n", argv[i]); return 2; }
            else if (!g_host[0]) set_str(g_host, sizeof g_host, argv[i]);
            else g_port = atoi(argv[i]);
        }
    }
#else
    (void)argc; (void)argv;
#endif

    if (!g_host[0]) {
        fprintf(stderr, "no host set (argv or config/narrator.wyoming 'host <ip>')\n");
        return 2;
    }

    if (net_init() != 0) { fprintf(stderr, "net_init failed\n"); return 1; }

    buf = (char *)malloc(CHUNK_BUF);
    if (!buf) { net_cleanup(); return 1; }

    audio_set_outfile(g_out);
    audio_set_unit(g_unit);

    printf("say -> %s:%d  text=\"%s\"\n", g_host, g_port, g_text);

    sock = net_connect(g_host, g_port);
    if (sock < 0) { fprintf(stderr, "connect failed\n"); free(buf); net_cleanup(); return 1; }

    wyo_reset();
    t_send = net_now_ms();
    if (wyo_send_synthesize(sock, g_text, g_voice, g_language, g_speaker) != 0) {
        fprintf(stderr, "send synthesize failed\n");
        free(buf); net_close(sock); net_cleanup(); return 1;
    }

    for (;;) {
        int r = wyo_read_event(sock, &ev);
        if (r <= 0) break;

        if (strcmp(ev.type, "audio-chunk") == 0) {
            long left = ev.payload_length;
            if (!opened && ev.rate && ev.width && ev.channels) {
                if (audio_open(ev.rate, ev.width, ev.channels) != 0) {
                    fprintf(stderr, "audio_open failed (%dHz %d-bit %dch)\n",
                            ev.rate, ev.width * 8, ev.channels);
                    rc = 1;
                    break;
                }
                printf("playing: %dHz %d-bit %dch\n", ev.rate, ev.width * 8, ev.channels);
                rate = ev.rate; width = ev.width; channels = ev.channels;
                opened = 1;
            }
            while (left > 0) {
                long want = left < CHUNK_BUF ? left : CHUNK_BUF;
                if (wyo_read_payload(sock, buf, want) != 0) { left = -1; break; }
                if (opened) audio_write(buf, want);
                total += want;
                left -= want;
            }
            if (left < 0) { fprintf(stderr, "short read on payload\n"); rc = 1; break; }
        } else if (strcmp(ev.type, "audio-stop") == 0) {
            stopped = 1;
            break;
        } else if (ev.payload_length > 0) {
            long left = ev.payload_length;
            while (left > 0) {
                long want = left < CHUNK_BUF ? left : CHUNK_BUF;
                if (wyo_read_payload(sock, buf, want) != 0) break;
                left -= want;
            }
        }
    }

    if (opened) audio_close();
    t_done = net_now_ms();
    free(buf);
    net_close(sock);
    net_cleanup();

    {
        long audio_ms = (rate && width && channels)
                      ? (total * 1000L) / ((long)rate * width * channels) : 0;
        printf("done: %ld PCM bytes%s\n", total, stopped ? "" : " (no audio-stop!)");
        /* If AHI is really pacing playback, wall time ~ audio duration. If it
         * returned instantly (e.g. no audible output), wall << audio. */
        printf("wall %lums for ~%ldms of audio\n",
               (unsigned long)(t_done - t_send), audio_ms);
    }
    return rc;
}
