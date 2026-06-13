/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Simon Dick
 */

/* audio.h — streaming PCM playback sink.
 *
 * Two backends:
 *   audio_host.c  (PLATFORM_POSIX) — writes a .wav file (verify on the host)
 *   audio_ahi.c   (PLATFORM_AMIGA) — AHI v4 double-buffered streaming playback
 *
 * Lifecycle: audio_set_outfile() (optional) -> audio_open() -> audio_write()*
 * -> audio_close(). audio_write() provides natural back-pressure: it blocks
 * until a play buffer is free, which is what makes streaming work (the network
 * delivers faster than real time; playback paces consumption).
 */
#ifndef NARRATOR_AUDIO_H
#define NARRATOR_AUDIO_H

/* Host backend only: set the .wav output path before audio_open().
 * The AHI backend ignores this. */
void audio_set_outfile(const char *path);

/* AHI backend only: choose the ahi.device unit before audio_open() (default 0).
 * The host backend ignores this. */
void audio_set_unit(int unit);

/* Open the sink for the given PCM format (width in bytes, e.g. 2 for 16-bit).
 * Returns 0 on success, <0 on error. */
int  audio_open(int rate, int width, int channels);

/* Queue PCM for playback. May block for flow control. Returns len, or <0. */
long audio_write(const void *buf, long len);

/* Flush remaining audio, wait for playback to finish, release resources. */
void audio_close(void);

#endif /* NARRATOR_AUDIO_H */
