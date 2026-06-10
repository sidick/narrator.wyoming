/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Simon Dick
 */

/* main.c — standalone Wyoming latency probe.
 *
 * Connects to a Piper Wyoming server, sends one `synthesize` request, reads the
 * audio-start / audio-chunk* / audio-stop response, and reports:
 *   - time-to-first-audio  (request sent -> first audio-chunk): perceived start
 *   - total round trip      (request sent -> audio-stop)
 *   - audio format + total PCM bytes + decoded duration
 * Optionally dumps the raw PCM to a file (--out).
 *
 * Latency gate: under ~500ms is good; consistently >1s feels broken.
 *
 * Usage:
 *   wyomingtest [opts] <host> [port]
 *     --text "..."     text to speak (default: a short phrase)
 *     --voice NAME     piper voice name (e.g. en_US-lessac-medium)
 *     --language LANG  voice language
 *     --speaker N      speaker id (integer)
 *     --out FILE       write raw PCM to FILE
 *     --runs N         repeat the request N times (reports each + min/avg)
 *   port defaults to 10200 (Wyoming Piper default).
 */
#include "net.h"
#include "wyoming.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_PORT 10200
#define DEFAULT_TEXT "The quick brown fox jumps over the lazy dog."
#define CHUNK_BUF 8192

typedef struct {
    unsigned long first_audio_ms;  /* request -> first chunk */
    unsigned long total_ms;        /* request -> audio-stop  */
    long          pcm_bytes;
    int           rate, width, channels;
    int           ok;
} RunResult;

static int do_run(const char *host, int port,
                  const char *text, const char *voice,
                  const char *language, const char *speaker,
                  FILE *out, RunResult *r)
{
    int sock, got_first = 0, stopped = 0;
    unsigned long t0;
    char *buf;
    WyoEvent ev;

    memset(r, 0, sizeof(*r));

    sock = net_connect(host, port);
    if (sock < 0) {
        fprintf(stderr, "connect to %s:%d failed\n", host, port);
        return -1;
    }

    buf = (char *)malloc(CHUNK_BUF);
    if (!buf) { net_close(sock); return -1; }

    wyo_reset();
    t0 = net_now_ms();
    if (wyo_send_synthesize(sock, text, voice, language, speaker) != 0) {
        fprintf(stderr, "send synthesize failed\n");
        free(buf); net_close(sock);
        return -1;
    }

    for (;;) {
        int rc = wyo_read_event(sock, &ev);
        if (rc <= 0) break;     /* EOF or error */

        if (ev.rate)     r->rate = ev.rate;
        if (ev.width)    r->width = ev.width;
        if (ev.channels) r->channels = ev.channels;

        if (strcmp(ev.type, "audio-chunk") == 0) {
            long left = ev.payload_length;
            if (!got_first) {
                r->first_audio_ms = net_now_ms() - t0;
                got_first = 1;
            }
            while (left > 0) {
                long want = left < CHUNK_BUF ? left : CHUNK_BUF;
                if (wyo_read_payload(sock, buf, want) != 0) { left = -1; break; }
                if (out) fwrite(buf, 1, (size_t)want, out);
                r->pcm_bytes += want;
                left -= want;
            }
            if (left < 0) { fprintf(stderr, "short read on payload\n"); break; }
        } else if (strcmp(ev.type, "audio-stop") == 0) {
            r->total_ms = net_now_ms() - t0;
            stopped = 1;
            break;
        } else if (ev.payload_length > 0) {
            /* Unknown event with a payload: consume and ignore it. */
            long left = ev.payload_length;
            while (left > 0) {
                long want = left < CHUNK_BUF ? left : CHUNK_BUF;
                if (wyo_read_payload(sock, buf, want) != 0) break;
                left -= want;
            }
        }
    }

    free(buf);
    net_close(sock);

    if (!stopped) {
        fprintf(stderr, "no audio-stop received (got %ld PCM bytes)\n", r->pcm_bytes);
        return -1;
    }
    r->ok = 1;
    return 0;
}

static void report(const RunResult *r, int idx)
{
    /* Integer-only math: the target 68020 has no FPU, and floating point would
     * trap (Guru #8000000B, F-line). Duration in milliseconds = bytes * 1000 /
     * (rate * width * channels). */
    printf("run %d: first-audio %lums  round-trip %lums  | %ld PCM bytes",
           idx, r->first_audio_ms, r->total_ms, r->pcm_bytes);
    if (r->rate && r->width && r->channels) {
        long bytes_per_sec = (long)r->rate * r->width * r->channels;
        /* 32-bit: pcm_bytes*1000 stays < 2^31 for any realistic clip. */
        long dur_ms = (r->pcm_bytes * 1000L) / bytes_per_sec;
        printf("  (%dHz %d-bit %dch, %ld.%02lds audio)",
               r->rate, r->width * 8, r->channels,
               dur_ms / 1000, (dur_ms % 1000) / 10);
    }
    printf("\n");
}

/* The probe uses several KB of stack locals and bsdsocket needs headroom, so
 * it must run with more than the AmigaDOS 4KB default CLI stack or it corrupts
 * memory (manifests as a Guru). With the clib2 runtime there is no reliable
 * link-time stack symbol, so the launcher is responsible: the boot script runs
 * `Stack 131072` before invoking the binary. Run it the same way by hand. */

/* Settings live in globals so the config-file reader and (on host) argv can
 * both populate them. Empty string == "not set". */
static char g_host[128]    = "";
static int  g_port         = DEFAULT_PORT;
static char g_text[1024]   = DEFAULT_TEXT;
static char g_voice[128]   = "";
static char g_language[64] = "";
static char g_speaker[32]  = "";
static char g_out[256]     = "";
static int  g_runs         = 1;

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

/* Read "key value" lines from a config file. Lines starting with # or ; are
 * comments. Returns 1 if a host was set, 0 otherwise (e.g. file missing).
 *
 * On Amiga this is how the probe gets its parameters: libnix does not hand us
 * a usable argv when launched from the startup-sequence, so we read a fixed
 * file (config/narrator.wyoming in the current dir) instead of the command line. */
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
        memcpy(key, p, kl);
        key[kl] = '\0';
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
        else if (!strcmp(key, "out"))      set_str(g_out, sizeof g_out, val);
        else if (!strcmp(key, "runs"))     g_runs = atoi(val);
    }
    fclose(f);
    return got_host;
}

int main(int argc, char **argv)
{
    const char *host, *text, *voice, *language, *speaker, *outpath;
    int port, runs, i, rc = 0;
    FILE *out = NULL;

    /* Config file first (the only input path on Amiga); try the cwd then the
     * volume root so it works whether or not the boot script CDs in. */
    if (!read_config("config/narrator.wyoming"))
        read_config("Narrator:config/narrator.wyoming");

#ifndef PLATFORM_AMIGA
    /* On the host, argv is reliable and overrides the config file. */
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--text") && i + 1 < argc) set_str(g_text, sizeof g_text, argv[++i]);
        else if (!strcmp(argv[i], "--voice") && i + 1 < argc) set_str(g_voice, sizeof g_voice, argv[++i]);
        else if (!strcmp(argv[i], "--language") && i + 1 < argc) set_str(g_language, sizeof g_language, argv[++i]);
        else if (!strcmp(argv[i], "--speaker") && i + 1 < argc) set_str(g_speaker, sizeof g_speaker, argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) set_str(g_out, sizeof g_out, argv[++i]);
        else if (!strcmp(argv[i], "--runs") && i + 1 < argc) g_runs = atoi(argv[++i]);
        else if (argv[i][0] == '-') { fprintf(stderr, "unknown option: %s\n", argv[i]); return 2; }
        else if (!g_host[0]) set_str(g_host, sizeof g_host, argv[i]);
        else g_port = atoi(argv[i]);
    }
#else
    (void)argc; (void)argv;
#endif

    if (!g_host[0]) {
        fprintf(stderr,
            "no host set. Pass <host> [port] (host build) or provide a\n"
            "config/narrator.wyoming with a line like:  host 192.168.86.55\n");
        return 2;
    }
    if (g_runs < 1) g_runs = 1;

    host = g_host; port = g_port; text = g_text; runs = g_runs;
    voice = g_voice; language = g_language; speaker = g_speaker;
    outpath = g_out[0] ? g_out : NULL;

    if (net_init() != 0) {
        fprintf(stderr, "net_init failed (no TCP/IP stack?)\n");
        return 1;
    }

    if (outpath) {
        out = fopen(outpath, "wb");
        if (!out) { fprintf(stderr, "cannot open %s\n", outpath); net_cleanup(); return 1; }
    }

    printf("Wyoming latency probe -> %s:%d  text=\"%s\"\n", host, port, text);

    {
        unsigned long best_first = 0, sum_first = 0;
        int ok = 0;
        for (i = 0; i < runs; i++) {
            RunResult r;
            if (do_run(host, port, text, voice, language, speaker,
                       /* only dump PCM on the first run */ i == 0 ? out : NULL, &r) == 0) {
                report(&r, i + 1);
                if (!ok || r.first_audio_ms < best_first) best_first = r.first_audio_ms;
                sum_first += r.first_audio_ms;
                ok++;
            } else {
                rc = 1;
            }
        }
        if (ok > 1)
            printf("first-audio: best %lums  avg %lums over %d runs\n",
                   best_first, sum_first / (unsigned)ok, ok);
        if (ok) {
            unsigned long gate = best_first;
            printf("\nlatency gate (<~500ms good, >1000ms broken): %lums -> %s\n",
                   gate,
                   gate <= 500 ? "GOOD" : (gate <= 1000 ? "MARGINAL" : "TOO SLOW"));
        }
    }

    if (out) fclose(out);
    net_cleanup();
    return rc;
}
