# narrator.device — API reference

Authoritative source: **Amiga ROM Kernel Reference Manual: Devices, 3rd ed.,
Chapter 8 "Narrator Device" (pp. 131–158)**, supplemented by
<https://wiki.amigaos.net/wiki/Narrator_Device>. This is the interface
`narrator.wyoming` must emulate: existing software (e.g. `Say`) opens
`narrator.device` by name and drives it with these commands/structures, so our
device must accept them and behave plausibly while forwarding text to Piper.

> **Headline finding for our design:** narrator.device consumes **phonetic
> (ARPABET) input**, not English. English→ARPABET is done *by the caller* via
> `translator.library` before `CMD_WRITE`. Piper wants **plain English**. See
> "Input is phonetic — the core problem" below; it's the central design
> decision for the device.

## Device commands

| Command | Operation |
|---|---|
| `CMD_WRITE` | Write a stream of (phonetic) characters; generate mouth-movement data for reads. **Our core path:** send text to Piper over Wyoming, play returned PCM via AHI. |
| `CMD_READ` | Read mouth shapes / sync events for an active write (must be matched to a `CMD_WRITE`). Returns `mouth_rb` width/height/sync. |
| `CMD_FLUSH` | Purge all active and queued requests. |
| `CMD_RESET` | Reset the port to initialized state; abort all active/queued I/O. |
| `CMD_START` | Restart currently active speech; resume queued requests (un-stop). |
| `CMD_STOP` | Stop active speech and hold queued requests. |

Minimum to implement: `OpenDevice`/`CloseDevice` + `CMD_WRITE`. The rest
(READ/mouth shapes, FLUSH/STOP/START queue control) is hardening.

Device lifecycle (matches our persistent-TCP mapping): open initializes globals,
**opens the audio device + allocates audio channels** (we use AHI instead),
housekeeping; then it takes `CMD_WRITE`/`CMD_READ`; close frees buffers and lets
the device be expunged.

## `narrator_rb` write-request structure (authoritative field order)

```c
struct narrator_rb {
    struct IOStdReq message;   /* standard IORequest (io_Command/io_Data/io_Length/io_Error) */
    UWORD  rate;               /* speaking rate, words/minute        */
    UWORD  pitch;              /* baseline pitch in Hertz            */
    UWORD  mode;               /* pitch mode                         */
    UWORD  sex;                /* sex of voice                       */
    UBYTE *ch_masks;           /* pointer to audio allocation maps   */
    UWORD  nm_masks;           /* number of audio allocation maps    */
    UWORD  volume;             /* volume, 0 (off) .. 64              */
    UWORD  sampfreq;           /* audio sampling frequency           */
    UBYTE  mouths;             /* if non-zero, generate mouth shapes */
    UBYTE  chanmask;           /* internal - DO NOT MODIFY           */
    UBYTE  numchan;            /* internal - DO NOT MODIFY           */
    UBYTE  flags;              /* new-feature flags (was `pad` pre-V37) */
    /* --- V37 (2.0) additions below; only valid if NDF_NEWIORB set --- */
    UBYTE  F0enthusiasm;       /* F0 excursion factor       (0..255, dflt 32) */
    UBYTE  F0perturb;          /* F0 perturbation amount    (0..255)          */
    BYTE   F1adj, F2adj, F3adj;/* formant freq adjust, ±5% steps              */
    BYTE   A1adj, A2adj, A3adj;/* amplitude adjust, decibels                  */
    UBYTE  articulate;         /* transition-time multiplier % (dflt 100)     */
    UBYTE  centralize;         /* vowel centralization 0..100%                */
    char  *centphon;           /* central ASCII phoneme target                */
    BYTE   AVbias;             /* amplitude-of-voicing bias                   */
    BYTE   AFbias;             /* amplitude-of-frication bias                 */
    BYTE   priority;           /* task priority while speaking (dflt 100)     */
    BYTE   pad1;
};
```

Value ranges / constants (from the wiki + include file):
- `rate` 40–400 wpm · `pitch` 65–320 Hz · `volume` 0–64 · `sampfreq` default 22200 Hz.
- `mode`: `ROBOTICF0`, `NATURALF0`, `MANUALF0`.
- `sex`: `MALE`, `FEMALE`.
- `flags`: `NDF_WORDSYNC`, `NDF_SYLSYNC`, `NDF_NEWIORB` (bit `NDB_NEWIORB`).

**V37 compatibility mechanism (important):** the pre-V37 struct ended at a `pad`
byte that "no one should ever have touched"; V37 renamed it `flags`. On
`OpenDevice` the device checks `NDB_NEWIORB` in that byte to decide whether the
caller supplied an old (short) or new (extended) `narrator_rb`. **Our device must
do the same:** treat the request as the short layout unless `NDF_NEWIORB` is set,
and never read the V37 fields when it isn't.

**Mapping to Piper:** map `rate`/`pitch` (and maybe `sex`) to Piper voice params
where reasonable; **parse but ignore** the formant/amplitude/centralization knobs
(Paula-formant-synth specific, no Piper analogue). Don't crash on ignored fields.

## `mouth_rb` read-request structure

```c
struct mouth_rb {
    struct narrator_rb voice;  /* copy of the write rb, to match read<->write */
    UBYTE width;               /* returned: mouth width  (proportional)       */
    UBYTE height;              /* returned: mouth height (proportional)       */
    UBYTE shape;               /* internal - do not modify                    */
    UBYTE sync;                /* returned sync-event flags                   */
};
```
Set up a read by copying the opened `narrator_rb` into `voice`, pointing
`voice.message.io_Message.mn_ReplyPort` at a read port, and `io_Command =
CMD_READ`. Each `CMD_READ` must match an in-flight `CMD_WRITE`. Reads block until
an event occurs (mouth change / word / syllable) or speech completes.

`sync` flags: `0x01` mouth-shape change · `0x02` start-of-word · `0x04`
start-of-syllable (one or more may be set; events can be coalesced). Enable via
`mouths` (non-zero) and/or `flags` `NDF_WORDSYNC`/`NDF_SYLSYNC` on the write.

**Out of scope for narrator.wyoming (decided):** `CMD_READ`/mouth shapes and
word/syllable sync are **not** implemented — `CMD_READ` returns `ND_NoWrite`
(talking-head software gets no animation but doesn't hang). Piper returns PCM
with no phoneme timing, so mouth shapes could only be faked from audio amplitude,
and real lip-sync would need reads to run concurrently with the playing write.
Low value, not properly achievable — intentionally skipped.

## Error codes

| Code | Meaning |
|---|---|
| `0` | success (for reads: mouth shape changed) |
| `ND_NoWrite` | the associated write completed (read loop terminator) |

`io_Error` carries these; reads loop `while (io_Error != ND_NoWrite)`.

## Open / write / read example (condensed from RKM)

```c
/* open */
VoiceMP = CreatePort("speech write", 0);
VoiceIO = (struct narrator_rb *)CreateExtIO(VoiceMP, sizeof(struct narrator_rb));
VoiceIO->flags = NDF_NEWIORB;                         /* request V37 features */
OpenDevice("narrator.device", 0, (struct IORequest *)VoiceIO, 0);

/* write (async) */
SpeakIO->message.io_Command = CMD_WRITE;
SpeakIO->message.io_Data    = PhoneticText;           /* ARPABET, e.g. "KAET." */
SpeakIO->message.io_Length  = strlen(PhoneticText);
SendIO((struct IORequest *)SpeakIO);

/* read loop (mouth/sync), then WaitIO(SpeakIO) */
for (DoIO(MouthIO); MouthIO->voice.message.io_Error != ND_NoWrite; DoIO(MouthIO)) {
    if (MouthIO->sync & 0x01) DoMouthShape();
    if (MouthIO->sync & 0x02) DoWordSync();
    if (MouthIO->sync & 0x04) DoSyllableSync();
}
WaitIO((struct IORequest *)SpeakIO);
```

---

## Input is phonetic — the core problem

The narrator's `CMD_WRITE` text is **ARPABET phonetic**, not English. Callers
(including `Say` in English mode) run text through `translator.library`
`Translate()` (English → ARPABET) *first*, then write the phonemes. Piper wants
plain English. So our device receives phonemes and must do **one** of:

1. **Reverse-map ARPABET → English-ish** and send that to Piper (lossy, but the
   only fully-transparent option for arbitrary callers).
2. **Intercept English before translation** — e.g. provide a replacement
   `translator.library` (or detect `Say -x`/no-translate paths) so we get English
   directly. Investigate what `Say` actually sends; it has options around
   translation.
3. **Accept degraded quality** by feeding ARPABET to Piper as-is initially
   (a first PoC), and improve later.

Decide this early; it shapes the whole `CMD_WRITE` text path. See
[translator-library.md](translator-library.md).

### Decision (implemented): option 2 + silently discard stray phonemes

We took **option 2**: ship a pass-through `translator.library` whose `Translate()`
returns the English unchanged, so `Say` (which always calls it) and any standard
caller deliver **English** straight to our device, which forwards it to Piper.
This is the drop-in path and it works.

Option 1 (reverse-map ARPABET → English) was **rejected as not achievable**:
ARPABET → English is lossy/ambiguous, and — decisively — the Wyoming `synthesize`
event only carries **text**, so even a perfect ARPABET → IPA/eSpeak phoneme
conversion has no way to reach Piper. Precise per-phoneme control (the reason
phonetic input exists) simply can't be honoured through a neural TTS over Wyoming.

So a program that drives `narrator.device` **directly with ARPABET phonemes**
(bypassing `translator.library`) can't be served. Rather than feed Piper gibberish,
the device **detects phonetic input and silently discards it** — `CMD_WRITE`
returns success (`io_Error = 0`, `io_Actual = 0`) but plays nothing. Detection
(`looks_phonetic` in `nw_engine.c`) is biased hard toward speaking: input is
treated as phonemes only when it has **no lowercase letter anywhere** *and* a
**stress-digit pattern** (a digit straight after an A–Z letter, e.g. `EH1`).
English never embeds digits in words, so ordinary prose — and even all-caps words
like `WARNING` — are always spoken; the only inputs dropped are clearly-phonetic
ones (and the rare false positive like a bare all-caps `MP3`, considered
acceptable). Unmarked all-caps phonemes with no stress digits will slip through
and be spoken as-is.

### Phonetic alphabet (expanded ARPABET) the device accepts

Needed to reverse-map (option 1) or validate input. All **vowels are 2-letter**;
consonants are 1–2 letters. "Spell it like it sounds, not like it looks."

**Vowels:** `IY` (beet) · `IH` (bit) · `EH` (bet) · `AE` (bat) · `AA` (father/bottle)
· `AH` (but/fun) · `AO` (ball/awl) · `OH` (border) · `UH` (book) · `ER` (bird) ·
`AX`* (about) · `IX`* (solid). *`AX`/`IX` must never be used in stressed syllables.*

**Diphthongs:** `EY` (bay) · `AY` (bide/I) · `OY` (boy) · `OW` (boat/own) ·
`AW` (bound/owl) · `UW` (brew).

**Consonants:** `R` `L` `W` `Y` `M` `N` `NX`(sing) `SH` `S` `TH`(thin) `F` `ZH`(pleasure)
`Z` `DH`(then) `V` `WH`(when) `CH` `J`(judge) `H`/`/H` `/C`(loch) `B` `P` `D` `T`
`K` `G`. (Voiced/unvoiced pairs matter: `TH`/`DH`, `S`/`Z`.)

**Special:** `DX` (tongue flap, "pity") · `Q` (glottal stop) · `QX` (silent vowel).

**Contractions:** `UL`=AXL · `IL`=IXL · `UM`=AXM · `IM`=IXM · `UN`=AXN · `IN`=IXN.

**Stress / intonation:** a digit **1–9** immediately after a vowel marks stress
(value = intonation strength; 9 strongest). Rough guide: exclamations 9, adverbs/
quantifiers 7, nouns/adjectives 5, verbs 4, pronouns 3, secondary stress 1–2.

**Punctuation:** `.` sentence-final (falling pitch) · `?` yes/no question (rising)
· `-` phrase delimiter (short pause) · `,` clause delimiter · `()` noun-phrase
delimiters (intonation hint). Every utterance needs a final `.` or `?` (narrator
appends `-` if none).

Example (English → narrator phonetic):
> "cat" → `KAET` · "cent" → `SEHNT` · "Commodore" → `KAA1MAXDOHR` ·
> "computer" → `KUMPYUW1TER`
