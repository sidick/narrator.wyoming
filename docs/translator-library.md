# translator.library — API reference + dummy-replacement notes

Sources: <https://wiki.amigaos.net/wiki/Translator_Library>, the RKM Libraries
"Translator Library" chapter, and the standard Amiga `translator.i`/`.fd`
function descriptor. The RKM **Devices** manual (3rd ed., Ch. 8) corroborates the
role: the translator "derives the required phonetic spelling from English text"
that `narrator.device` then speaks.

> It's a **library** (`translator.library`), not a device. But the drop-in
> trick is identical to what we do for `narrator.device`: a replacement loaded
> from `LIBS:` by name overrides the original for every caller.

## What it is

A tiny shared library with a **single public function**, `Translate()`, that
converts English text → the expanded-ARPABET phonetic codes `narrator.device`
expects (full table in [narrator-device.md](narrator-device.md)).

- Open name: `"translator.library"`. Version: historically lax — callers often
  `OpenLibrary("translator.library", 0)`; modern code may ask v37+.
- Classic flow used by `Say` and friends: **English → `Translate()` → ARPABET →
  `narrator.device` `CMD_WRITE`**.

## `Translate()`

C prototype:
```c
LONG Translate(STRPTR english, LONG englishLen, STRPTR phonBuffer, LONG bufferLen);
```

Amiga register/vector convention (standard `.fd`; **re-verify against
`translator.i` before hand-rolling a vector table**):
```
##bias 30                      ; first function after Open/Close/Expunge/Reserved
Translate(string,length,buffer,bufferLength)(A0,D0,A1,D1)   ; result in D0
```
i.e. `_LVOTranslate = -30`, args in `A0,D0,A1,D1`, return in `D0`.

Return semantics:
- `0` — the entire input was translated into `phonBuffer`.
- **non-zero (negative)** — the output buffer filled up; the value is the
  *negative of the input position* where translation stopped. Resume by calling
  again from `-(rtnCode)` with a fresh/drained buffer (callers loop:
  translate → drain to device → repeat).

## Why this matters to narrator.wyoming

Our device will be handed **ARPABET phonemes** (because the caller already ran
`Translate()`), but **Piper wants English**. This is the "core problem"
in [narrator-device.md](narrator-device.md). A dummy `translator.library` is
option 2 (intercept English before translation):

### Dummy / pass-through `translator.library` (design sketch for later)

A replacement `LIBS:translator.library` whose `Translate()` **copies the English
input straight to the output buffer** (no phoneme conversion) and returns `0` (or
the negative-position protocol if the buffer is too small). Effect: every caller
that does English → `Translate()` → narrator now delivers **English** to our
`narrator.device`, which forwards it verbatim to Piper. Clean and fully
transparent for well-behaved callers.

What building it entails (non-trivial but well-trodden):
- A real (minimal) Amiga ROMTag/`InitResident` shared library: `Open`/`Close`/
  `Expunge`/(reserved) at `-6/-12/-18/-24`, then `Translate` at `-30`; a library
  base; built `-fbaserel`/resident as a library, installed in `LIBS:`.
- `Translate()` body: `len = min(englishLen, bufferLen-? )`, `CopyMem` input→
  output, honour the negative-position contract on overflow, return `0` on full
  copy. Possibly normalise/strip control chars.
- Caveats: some callers may bypass `translator.library` or expect *valid*
  phonetic output (e.g. they inspect it). `Say` is the primary target — check
  what it does. Also coexisting with the real library (only one
  `LIBS:translator.library` wins by name).

### Alternatives (if we don't replace the library)
1. Our `narrator.device` **reverse-maps ARPABET → English-ish** before calling
   Piper (lossy; handles callers that translate regardless of our library).
2. **Feed ARPABET to Piper as-is** for an initial PoC and accept degraded quality.

Decide alongside the `narrator.device` `CMD_WRITE` text path — the two interact
(if the dummy library guarantees English in, the device needs no reverse-map).

## Sources
- <https://wiki.amigaos.net/wiki/Translator_Library>
- RKM Libraries, "Translator Library" chapter (autodoc + `translator.i`).
- Amiga ROM Kernel Reference Manual: Devices, 3rd ed., Ch. 8 (the English→
  phonetic→narrator flow).
