/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Simon Dick
 */

/* nw_engine.h — freestanding Wyoming(/AHI) engine for narrator.device.
 *
 * Self-contained: exec + bsdsocket (+ AHI, later) only. NO C runtime, NO stdio,
 * NO malloc, and — critically for the -fbaserel device — NO C globals/statics.
 * SysBase is passed in; every other library base lives in the heap-allocated
 * context. See CLAUDE.md "Device build" for why globals are forbidden here.
 */
#ifndef NARRATOR_NW_ENGINE_H
#define NARRATOR_NW_ENGINE_H

struct ExecBase;

/* Runtime device config, read from the prefs file ENV:narrator.wyoming
 * ("key value" lines: host, port, voice, voice_male, voice_female). Nothing is
 * baked into the binary except a localhost fallback. */
struct nwprefs {
    char host[128];
    int  port;
    char voice[64];          /* default voice (used if no sex-specific match) */
    char voice_male[64];
    char voice_female[64];
    unsigned long audio_mode;/* AHI audio mode ID (AHIA_AudioID) to play through.
                              * Default 0x0002000f (paula HiFi 14 bit mono
                              * calibrated — highest-quality paula mode that
                              * works on real hardware). For Amiberry, 0x003b0002
                              * (uaesnd HiFi Stereo) or 0x001a0000 (UAE 16 bit
                              * HIFI Stereo++) measure cleaner. See
                              * docs/audio-capture-rig.md. */
    int  split_words;        /* 0 = off (whole text in one request); >0 = split
                              * the text into pipelined chunks of ~this many
                              * words for faster time-to-first-audio on long
                              * input (small prosody cost at the split points) */
    int  gain;               /* AHI output level, percent of full scale (1..100;
                              * default 80). Piper peaks touch 0 dBFS and AHI's
                              * resample to Paula's rate overshoots full-scale
                              * peaks -> harsh clipping; <100 leaves headroom so
                              * peaks no longer hit the ceiling. Lower it further
                              * if loud peaks still distort. */
    char capture[128];       /* if non-empty, also write the raw little-endian PCM
                              * Wyoming sent us to this AmigaDOS path as a WAV
                              * file (no pre-roll silence; only actual data).
                              * Spans one device-task lifetime; one CMD_WRITE may
                              * concat many synthesis chunks. Empty = capture
                              * off. Useful for the audio-quality investigation
                              * rig in docs/audio-capture-rig.md. */
};

/* Fill *p with defaults, then overlay ENV:narrator.wyoming if present. */
void nw_read_prefs(struct ExecBase *sysbase, struct nwprefs *p);

/* ---- device task (M5) ----
 * A dedicated task owns the connection so it can persist across CMD_WRITEs
 * (bsdsocket state is per-task). The device hooks use these:
 *   nw_dev_open  at OpenDevice  -> create the context + task (returns NULL on fail)
 *   nw_dev_close at CloseDevice -> ask the task to shut down (frees the context)
 *   nw_dev_submit in BeginIO    -> hand a CMD_WRITE IORequest to the task (async;
 *                                  the task ReplyMsg()s it when done)
 * struct IORequest is forward-declared; callers pass the narrator_rb as one. */
struct nwctx;
struct IORequest;

struct nwctx *nw_dev_open(struct ExecBase *sysbase);
void          nw_dev_close(struct nwctx *ctx);
void          nw_dev_submit(struct nwctx *ctx, struct IORequest *io);

/* nw_dev_abort: pull `io` off the device task's request queue if it's still
 * waiting there (CMD_WRITE not yet picked up by the task). 0 = aborted (io
 * has been replied with io_Error = IOERR_ABORTED). -1 = io is not on the
 * queue (in-flight in the task, or already replied) — there's nothing to
 * abort; the caller's WaitIO will see natural completion. */
long          nw_dev_abort(struct nwctx *ctx, struct IORequest *io);

/* nw_dev_flush: abort every queued request (each replied with IOERR_ABORTED).
 * The one currently being processed (if any) continues to natural completion. */
void          nw_dev_flush(struct nwctx *ctx);

/* Negative error codes (internal; reported via io_Error / io_Actual). */
#define NWERR_NOSOCKLIB  -100   /* can't open bsdsocket.library            */
#define NWERR_NOMEM      -101   /* AllocMem failed                         */
#define NWERR_RESOLVE    -102   /* host lookup failed                      */
#define NWERR_CONNECT    -103   /* connect() failed                        */
#define NWERR_SEND       -104   /* sending the synthesize request failed   */
#define NWERR_PROTO      -105   /* malformed/short Wyoming response         */

#endif /* NARRATOR_NW_ENGINE_H */
