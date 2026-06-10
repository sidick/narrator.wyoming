/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Simon Dick
 */

/* wyoming.h — minimal Wyoming-protocol client framing.
 *
 * Wire format (per the Wyoming spec):
 *   1. one UTF-8 JSON header line terminated by '\n'
 *   2. optional `data_length` bytes of additional UTF-8 JSON
 *   3. optional `payload_length` bytes of raw binary
 *
 * Header fields used here: "type", "data_length", "payload_length", and
 * (inside the header's inline "data" object, or the separate data block)
 * "rate"/"width"/"channels" for audio chunks.
 *
 * This is a deliberately small, single-line-header reader tailored to Piper's
 * TTS responses — NOT a general JSON parser. See json_find_* in wyoming.c.
 */
#ifndef NARRATOR_WYOMING_H
#define NARRATOR_WYOMING_H

#include <stddef.h>

#define WYO_TYPE_MAX 48

typedef struct {
    char          type[WYO_TYPE_MAX]; /* event type, e.g. "audio-chunk" */
    long          payload_length;     /* binary bytes following; -1 if none */
    int           rate;               /* audio params, 0 if absent         */
    int           width;
    int           channels;
} WyoEvent;

/* Reset the internal read buffer. Call once at the start of each connection
 * before the first wyo_read_event(). */
void wyo_reset(void);

/* Send a `synthesize` request. voice/language/speaker may be NULL/empty to
 * let the server pick defaults. Returns 0 on success, <0 on write error. */
int wyo_send_synthesize(int sock, const char *text,
                        const char *voice, const char *language,
                        const char *speaker);

/* Read the next event header (and consume any separate data block).
 * On return, *ev is filled and ev->payload_length tells the caller how many
 * raw bytes to read next via wyo_read_payload().
 * Returns 1 on an event, 0 on clean EOF/peer close, <0 on error. */
int wyo_read_event(int sock, WyoEvent *ev);

/* Read exactly `len` payload bytes into buf (len == ev->payload_length).
 * Returns 0 on success, <0 on error/short read. */
int wyo_read_payload(int sock, void *buf, long len);

#endif /* NARRATOR_WYOMING_H */
