# narrator.wyoming

A drop-in replacement for the Amiga **`narrator.device`** that speaks with a
modern **neural voice** instead of the old Paula formant synthesizer. It forwards
text to a [Piper](https://github.com/rhasspy/piper) text-to-speech server over the
[Wyoming protocol](https://github.com/OHF-Voice/wyoming) (plain TCP) and plays the
returned PCM through **AHI**.

Because it replaces `narrator.device` by name in `DEVS:`, existing Amiga software
gets neural speech transparently — **the stock `Say` command works with no
modification**.

> Aimed at **PiStorm / emu68k**-accelerated and other fast 68k Amigas, where
> the emulated CPU makes the network round-trip practical. Developed and
> tested under the Amiberry emulator; see CLAUDE.md and docs/ for the gory
> details.

## Status

Works end-to-end on-target:

- `Say "hello"` → neural speech out of AHI. ✅
- Programs that open `narrator.device` directly (`CMD_WRITE`) get neural speech. ✅
- Warm round-trip latency ~440 ms first-audio on an emulated 68020 (GOOD). ✅
- `volume` and `sex`→voice mapping; graceful failure if the server is down. ✅
- Persistent connection held by the device's own task; `CMD_WRITE`s are async
  (`SendIO` returns immediately) and queued FIFO over the one socket. ✅

Not done by design (see "Limitations"): `CMD_READ` mouth-shape/word sync, and
per-request rate/pitch (a Wyoming limitation).

## How it works

```
Amiga app ──IOSpeech/CMD_WRITE──▶ narrator.device (this)
   (Say) ──English──▶ translator.library (this, pass-through)──┘
                                          │  Wyoming over TCP (bsdsocket)
                                          ▼
                                   Piper TTS server (your LAN)
                                          │  raw 16-bit PCM
                                          ▼
                                   narrator.device ──byte-swap──▶ AHI playback
```

`Say` translates English→phonemes via `translator.library` before writing to
narrator. Piper wants **English**, so narrator.wyoming ships a **pass-through
`translator.library`** whose `Translate()` returns the English unchanged — so
English reaches our device, which forwards it to Piper.

## Requirements

**Amiga side**
- **AHI v4+** installed, with a Unit 0 audio mode configured (the `paula.audio`
  driver works under emulation).
- A **TCP/IP stack** (Roadshow / AmiTCP) providing `bsdsocket.library` — or
  Amiberry's built-in `bsdsocket.library` emulation.
- Fast 68k (PiStorm/emu68k or ~68030/40MHz+). No FPU required. No TLS/AmiSSL.
- **Optional**: `codesets.library` (Aminet: `util/libs/codesets`) for native-locale
  text input. ASCII and pre-encoded UTF-8 work without it; ISO-8859-1 (accents,
  umlauts) needs it or Piper may reject the request.

**Server side**
- A **Piper** TTS server reachable on your LAN with the **Wyoming** protocol
  enabled (default port **10200**), e.g. the `wyoming-piper` add-on / container
  (see below).

## Running a Piper/Wyoming server (Docker)

narrator.wyoming needs a Piper TTS server speaking the Wyoming protocol on your
LAN. The easiest way to get one is the official **`rhasspy/wyoming-piper`**
container. Run it on any always-on machine (a NAS, a mini-PC, or the same
host you build on):

```
docker run -d --name wyoming-piper \
  -p 10200:10200 \
  -v wyoming-piper-data:/data \
  rhasspy/wyoming-piper \
  --voice en_US-lessac-medium
```

- `-p 10200:10200` — exposes the Wyoming TCP port the device connects to.
- `-v wyoming-piper-data:/data` — persists downloaded voice models across
  restarts (so they aren't re-fetched each time).
- `--voice` — the startup voice; Piper auto-downloads it on first run. The
  server listens on `tcp://0.0.0.0:10200` by default.

Then point the device at that machine's LAN IP in `ENV:narrator.wyoming`:

```
host 192.168.1.50
port 10200
```

### Multiple voices

A single wyoming-piper instance serves **more than one voice** — it loads any
voice named in a request on demand (downloading it the first time). So `voice`,
`voice_male`, and `voice_female` can each point at a different voice against the
*same* server, and `Say`'s male/female option just selects between them. Browse
voices in the [Piper voice samples](https://rhasspy.github.io/piper-samples/) and
use the exact model name (e.g. `en_US-ryan-high`).

### Docker Compose

The same thing as a `docker-compose.yml`:

```yaml
services:
  wyoming-piper:
    image: rhasspy/wyoming-piper
    command: --voice en_US-lessac-medium
    ports:
      - "10200:10200"
    volumes:
      - wyoming-piper-data:/data
    restart: unless-stopped
volumes:
  wyoming-piper-data:
```

### Verify it before involving the Amiga

From your build host, point a native test build at the server — no Amiga needed:

```
make host
./build/host/saytest --voice en_US-lessac-medium --out say.wav 192.168.1.50 10200
```

A playable `say.wav` confirms the server works. (The first request is slow while
Piper loads the voice model; warm requests are fast — use `wyomingtest --runs 3`
to see the warm latency.)

## Install

Copy the two built artifacts into place and create the prefs file:

```
Copy narrator.device     DEVS:narrator.device
Copy translator.library  LIBS:translator.library
```

Create the prefs file **`ENV:narrator.wyoming`** (and `ENVARC:` to persist it):

```
host 192.168.1.50      ; your Piper/Wyoming server
port 10200
voice         en_US-lessac-medium   ; optional; must exist on your server
voice_male    en_US-ryan-high       ; used when the caller sets sex = MALE
voice_female  en_US-amy-medium      ; used when the caller sets sex = FEMALE
```

All keys are optional except `host`. Omit the `voice*` keys to use the server's
default voice. Built-in fallbacks apply if a key (or the whole file) is missing
(host defaults to `127.0.0.1`). See `config/narrator.wyoming.example`.

> **⚠️ Persisting across reboots.** `ENV:` lives in RAM and is rebuilt from
> `ENVARC:` at every boot, so editing **only** `ENV:narrator.wyoming` lasts until
> the next reboot. To make your settings permanent, also save the file to
> `ENVARC:`:
>
> ```
> Copy ENV:narrator.wyoming ENVARC:narrator.wyoming
> ```
>
> (The device reads `ENV:` at runtime; `ENVARC:` is just the on-disk copy the
> system restores `ENV:` from at startup.)

Then just use speech as normal:

```
Say "Hello from the Amiga."
```

## Works with other narrator.device software

Anything that speaks through `narrator.device` (+ `translator.library`) gets the
neural voice for free — that's the whole point of replacing them by name. A nice
companion is **[speak-handler](https://aminet.net/package/util/sys/speak-handler)**
(Alexander Fritsch, v39.1), a from-scratch native replacement for the Commodore
`SPEAK:` handler that drives `narrator.device`/`translator.library`. Mount its
`SPEAK:` and you can pipe text straight to neural speech, e.g.:

```
Type README.txt TO SPEAK:
echo "Hello from the Amiga" >SPEAK:
```

It pairs well with narrator.wyoming as an up-to-date, native speech handler.

## Build

The Amiga binaries cross-compile with **Bebbo's m68k-amigaos GCC**, shipped in the
`stefanreinauer/amiga-gcc:latest` Docker image — no local toolchain needed:

```
make docker      # build build/amiga/{narrator.device, translator.library, ...}
make host        # native build/host/{wyomingtest, saytest} for protocol testing
make clean
```

Build outputs:
- `narrator.device` — the device (libnix `devinit.o`, freestanding).
- `translator.library` — the pass-through translator (libnix `libinit.o`).
- `wyomingtest` — latency probe.
- `saytest` — standalone text→Wyoming→audio pipeline (`.wav` on host).
- `devtest` — opens and drives the device directly.

`make host` reads `config/narrator.wyoming` (copy from
`config/narrator.wyoming.example`) for the server
address; on Amiga the device reads `ENV:narrator.wyoming` at runtime instead.

## Configuration reference (`ENV:narrator.wyoming`)

`key value` per line; `#` or `;` start comments (inline comments after a value
are allowed). The device reads `ENV:` at runtime — remember to also copy the
file to `ENVARC:` so edits survive a reboot (see the persistence note under
[Install](#install)).

| key | meaning | default |
|---|---|---|
| `host` | Piper/Wyoming server address | `127.0.0.1` |
| `port` | server port | `10200` |
| `voice` | default Piper voice name | server default |
| `voice_male` | voice for `narrator_rb.sex == MALE` | `voice` |
| `voice_female` | voice for `narrator_rb.sex == FEMALE` | `voice` |
| `ahi_unit` | `ahi.device` unit to play through (for multiple AHI units) | `0` |
| `gain` | AHI output level as percent of full scale (1–100). Lower = more headroom against Piper's hot peaks | `80` |
| `smooth` | high-cut strength (cascade of 1-tap averagers) to tame AHI/Paula sibilance. 0 = off | `2` |
| `split_words` | split long input into ~N-word pipelined chunks for faster start (0 = off) | `0` |

`sex` is set at runtime by the caller (e.g. `Say`'s male/female option); the
prefs only say which Piper voice each maps to. `volume` (0–64) maps to AHI
output level. `rate`/`pitch` are accepted but ignored — the Wyoming `synthesize`
request has no per-request rate/pitch knob (those are properties of the Piper
voice, set server-side).

> **Prefs are snapshotted at `OpenDevice`.** The device reads `ENV:narrator.wyoming`
> once when its task starts (so edits don't take effect mid-open). Interactive
> `Say` opens a fresh device for each invocation, so live edits keep working
> in practice; a long-running consumer that holds the device open across edits
> will keep using the values it loaded at `OpenDevice`. Close and reopen the
> device to pick up new prefs.

> **Text codeset.** The Wyoming JSON requires UTF-8. The device auto-detects
> the caller's text: valid UTF-8 (including pure ASCII) is passed through,
> and anything else is transcoded from ISO-8859-1 to UTF-8 via the optional
> `codesets.library` (Aminet) if it is installed. If `codesets.library` is
> not present, non-UTF-8 input is passed through unchanged and Piper may
> reject it — install codesets.library to handle native-locale text.

### Faster start on long input (`split_words`)

Piper synthesizes one **sentence** at a time and only emits audio once the whole
sentence is rendered, so a *single long sentence* has a long wait before any
sound (≈2.3s for a 40-word run-on); multi-sentence text already starts fast
because the device plays sentence 1 while the rest render. Set `split_words` to a
word count (e.g. `12`) to break long input into smaller chunks that are sent
**pipelined** on the one connection — playback of the first chunk starts in
~¼s while the server renders the rest. Splits happen **only at punctuation**
(full stops always; commas/semicolons once a chunk passes the word target), so
the joins land in the natural pauses Piper already inserts and stay inaudible. A
genuinely unpunctuated run-on won't split (it stays one request — slower start,
but never a mid-phrase gap). Leave it at `0` for the most faithful prosody.

## Limitations / future work

- **`CMD_READ` (mouth shapes) and word/syllable sync are intentionally out of
  scope.** `CMD_READ` returns `ND_NoWrite` (so talking-head software gets no
  lip-sync data but doesn't hang). Real sync isn't meaningfully achievable here:
  we forward English to Piper and get back PCM with no phoneme timing, so mouth
  shapes could only be faked from audio amplitude, and accurate lip-sync would
  need playback to run concurrently with each write. Niche value, can't be done
  properly — deliberately skipped.
- **Direct ARPABET phoneme input is silently discarded.** Classic
  `narrator.device` takes phonemes; our drop-in expects English (via the
  pass-through `translator.library`, which `Say` and standard callers use). A
  program that writes raw phonemes straight to the device can't be served —
  Piper takes text, not phonemes, over Wyoming — so the device accepts the write
  but plays nothing rather than emit gibberish. Detection is conservative (no
  lowercase + a stress digit like `EH1`), so ordinary English, including all-caps
  words, is always spoken. See `docs/narrator-device.md`.
- **No `rate`/`pitch` control** (Wyoming limitation, above).
- **First request after the server is idle is slow** (~1.8 s) while Piper loads
  the voice model; subsequent (warm) requests are fast.
- Validated under **Amiberry** only; real PiStorm hardware untested.

## Repository layout

- `src/` — C sources (device, translator, engine, standalone test programs).
- `Makefile` — host + container (Amiga) builds.
- `CLAUDE.md` — guidance + the hard-won Amiga/toolchain lessons (worth reading
  before hacking on this).
- `docs/narrator-device.md`, `docs/translator-library.md` — API references
  distilled from the Amiga RKM and the AmigaOS wiki.

## Development

This project was developed with the assistance of AI tooling (Anthropic's
Claude, via Claude Code), under human direction and validated on-target under
the Amiberry emulator.

## License

MIT — see `LICENSE`.
