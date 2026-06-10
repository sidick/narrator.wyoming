/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Simon Dick
 */

/* translator_library.c — narrator.wyoming's pass-through translator.library.
 *
 * The real translator.library converts English text into ARPABET phonemes for
 * the classic formant-synth narrator.device. Our narrator.device forwards text
 * to Piper, which wants plain ENGLISH — so this drop-in replacement makes
 * Translate() copy the English input straight to the output buffer (no phoneme
 * conversion). Effect: `Say` (which always calls translator.library, confirmed
 * on-target) delivers English to our narrator.device -> Piper.
 *
 * Built freestanding via libnix's libinit.o (see Makefile `translator` target):
 *   m68k-amigaos-gcc -nostdlib -fbaserel libinit.o translator_library.c -o translator.library
 *
 * Like the device, this is -fbaserel; the library hooks may not be entered with
 * A4 at our data segment, so use NO C globals/statics. Translate() is a pure
 * byte copy (no exec calls), so it needs nothing global anyway.
 */

#include <exec/types.h>
#include <proto/exec.h>

#include "stabs.h"

/* Version: NW_VERSION.NW_REVISION come from version.mk via the Makefile (-D), so
 * the number lives in one place and matches narrator.device. VERSION 44 sits
 * above every stock version (translator topped out at ~43) so the pair is easy
 * to tell apart and OpenLibrary("translator.library", N) succeeds for any
 * reasonable minimum N. The $VER string lets the Version command read it;
 * NW_BUILD_DATE is stamped at build time (two-digit day/month: "(dd.mm.yyyy)"). */
#ifndef NW_VERSION
#define NW_VERSION  44
#endif
#ifndef NW_REVISION
#define NW_REVISION 0
#endif
#ifndef NW_BUILD_DATE
#define NW_BUILD_DATE "(unknown date)"
#endif
#define NW_STR_(x) #x
#define NW_STR(x)  NW_STR_(x)
#define NW_VERSTR  NW_STR(NW_VERSION) "." NW_STR(NW_REVISION)   /* "44.0" */

const char  LibName[]     = "translator.library";
const char  LibIdString[] = "$VER: translator.library " NW_VERSTR " " NW_BUILD_DATE "\r\n";
const UWORD LibVersion    = NW_VERSION;
const UWORD LibRevision   = NW_REVISION;

/* libinit convention: __UserLibInit returns 0 for SUCCESS (non-zero -> fail).
 * (Opposite of the device's __UserDevInit.) No globals -> nothing to set up. */
int __UserLibInit(struct Library *lib)
{
    (void)lib;
    return 0;
}

void __UserLibCleanup(void)
{
}

/* LONG Translate(STRPTR english, LONG englishLen, STRPTR buffer, LONG bufferLen)
 *   registers: A0          D0           A1            D1            -> D0
 * Pass-through: copy up to bufferLen-1 bytes, NUL-terminate. Returns 0 if the
 * whole input fit, else the negative of the input position where it stopped
 * (the documented "buffer too small, resume here" contract). */
ADDTABL_4(__Translate, a0, d0, a1, d1);
LONG __Translate(STRPTR english, LONG englishLen, STRPTR buffer, LONG bufferLen)
{
    LONG i = 0;
    if (englishLen < 0) englishLen = 0;
    if (bufferLen <= 0) return 0;
    for (; i < englishLen && i < bufferLen - 1; i++)
        buffer[i] = english[i];
    buffer[i] = 0;
    if (i < englishLen)
        return -i;                 /* buffer full -> resume at input pos i */
    return 0;                      /* whole input translated */
}

ADDTABL_END();
