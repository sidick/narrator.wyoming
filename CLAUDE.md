# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this project is

`narrator.wyoming` is a drop-in replacement for the Amiga **`narrator.device`**. Instead of running local formant synthesis, it forwards text to a [Piper](https://github.com/rhasspy/piper) neural TTS server over the [Wyoming protocol](https://github.com/rhasspy/wyoming) (plain TCP) and plays the returned PCM audio through **AHI**. Because it replaces `narrator.device` by name in `DEVS:`, existing Amiga software (e.g. the `Say` command) gets modern neural speech transparently, with no modification.

It is a proof-of-concept / curiosity project targeting **emu68k PiStorm-equipped Amigas** (and other well-accelerated 68k machines, ~68030/40MHz+) where emulated CPU speed makes the network round-trip practical.

Requirements on the Amiga side: **AHI v4 or later** (the final version targets AHI version 4 as a hard minimum — code against the `ahi.device` v4 API and don't rely on older fallbacks), a **TCP/IP stack** (Roadshow or AmiTCP), and a **Piper** TTS server with the Wyoming protocol enabled reachable on the LAN. No TLS / AmiSSL (Wyoming is plain TCP).

> Note: a local `docs/PLAN.md` may hold fuller design notes but is gitignored (not committed) — do not rely on it being present. This file is self-contained.
>
> Committed reference material in `docs/`, distilled from the **Amiga RKM: Devices, 3rd ed., Ch. 8** + the AmigaOS wiki: **[narrator-device.md](docs/narrator-device.md)** (the device API we must emulate — commands, full `narrator_rb`/IOSpeech struct, `mouth_rb`, error codes, the pre-V37/V37 `NDF_NEWIORB` compatibility rule, and the **complete ARPABET phoneme table**) and **[translator-library.md](docs/translator-library.md)** (`Translate()`, English↔ARPABET). **Key finding:** narrator.device takes *phonetic* (ARPABET) input, but Piper wants *English* — see the "core problem" in narrator-device.md. One clean fix carried in translator-library.md: ship a **dummy `LIBS:translator.library`** whose `Translate()` passes English through unchanged, so callers like `Say` deliver English straight to our device (same drop-in-replacement trick we use for narrator.device).

## Current state

**The full drop-in pair plus the standalone test programs are complete and validated on-target under the Amiberry emulator.** Source lives in `src/`:

- `net.h` / `net_posix.c` / `net_amiga.c` — a tiny socket + monotonic-clock abstraction with two backends, selected by `-DPLATFORM_POSIX` (host) or `-DPLATFORM_AMIGA` (bsdsocket.library + timer.device). The seam that lets the same protocol code run on the host for fast iteration and on Amiga for the real thing.
- `wyoming.c` / `wyoming.h` — Wyoming framing: writes a `synthesize` event, reads `audio-start`/`audio-chunk`/`audio-stop`. A **buffered reader** (`rbuf`, `wyo_reset()`) backs both the line reader and the payload reader — one `recv` fills the buffer and both drain it. Header parsing uses small targeted `json_find_*` helpers, **not** a general JSON parser. Piper's chunks carry a separate `data_length` JSON block before the `payload_length` PCM, which the reader consumes.
- `audio.h` / `audio_host.c` / `audio_ahi.c` — streaming PCM sink. Host backend writes a `.wav` (verify audio without AHI); **Amiga backend is AHI v4 double-buffered streaming** (two ping-ponging `AHIRequest`s linked via `ahir_Link` for gapless playback; `audio_write()` blocks on a busy buffer, which paces the network read to playback speed = streaming).
- `main.c` (`wyomingtest`) — latency probe.
- `saytest.c` (`saytest`) — full pipeline: text → Wyoming → audio sink, streaming playback started as soon as the first chunk arrives.

Settings come from argv on the host; on Amiga argv is unusable from the startup-sequence, so both programs read `config/narrator.wyoming` instead (see below).

Results (Piper, host LAN, warm): audio is **22050 Hz / 16-bit / mono** as predicted.
- **Latency, on emulated Amiga (68020): ~424ms best / ~443ms avg first-audio → GOOD** (host ~440ms — the emulated 68k + bsdsocket adds almost no overhead). Round trip for ~2.5s of audio is ~550ms.
- **AHI streaming playback works on emulated Amiga.** `saytest` reports wall-time ≈ audio duration (e.g. `wall 5399ms for ~4841ms of audio`), proving AHI is actually pacing real-time output rather than accepting-and-dropping. Perceived start latency ≈ first-audio (~450ms) + one buffer (~370ms at 16KB/buffer).
- First request to a cold server is ~1.8s (one-time voice-model load); warm requests are the meaningful number. Warming the model at `OpenDevice` could hide this in the device.

## Architecture (target design)

The Amiga device lifecycle maps directly onto the persistent TCP connection — this mapping is the core design idea and the reason Wyoming was chosen over Piper's HTTP API:

- **`OpenDevice`** → establish and hold the TCP connection to the Piper Wyoming server (connection cost paid once)
- **`CMD_WRITE`** (carries an `IOSpeech` request: text + voice params) → send text via Wyoming, receive raw PCM, play via AHI
- **`CloseDevice`** → tear down the connection

Server address is configurable via a prefs file, not hardcoded.

### Implementation notes that require cross-file / external context

- **Wyoming protocol** — line-delimited JSON control messages followed by raw binary PCM. Returns raw PCM (no WAV container, no HTTP headers). **Feed the actual Wyoming spec to Claude Code when implementing the network layer** rather than relying on training-data recall.
- **AHI playback** — Piper emits 16-bit mono PCM, typically 22050Hz (confirmed on-target); AHI routes this to hardware (no direct Paula handling). **Target AHI v4 minimum** (`OpenDevice("ahi.device", ...)` requiring version >= 4); use the v4 API and fail cleanly if an older AHI is present rather than working around it. Streaming playback (feeding AHI before the full response arrives) is an explicit goal to cut perceived latency.
- **IOSpeech / `narrator_rb`** — `CMD_WRITE` carries text (in the embedded `IOStdReq` `io_Data`/`io_Length`) plus voice parameters (`rate`, `pitch`, `sex`, `mode`, `volume`, …). Parse the structure correctly but ignore the Paula-formant-specific knobs we can't map to Piper (don't crash on them); honour `flags & NDF_NEWIORB` for the extended V37+ layout. Full field reference: **[docs/narrator-device.md](docs/narrator-device.md)**. Open question carried there: classic callers may hand the device *ARPABET phonemes* (via `translator.library`), but Piper wants plain English — see **[docs/translator-library.md](docs/translator-library.md)**.
- **Exec task / message loop** — the device runs in its own Exec task; the message-handling loop conventions are the area most likely to need close review.
- **Latency gate** — <~500ms round trip on local network is good; consistently >1s means the experience will feel broken.
- **No TLS** — Wyoming is plain TCP, so AmiSSL is not needed. Requires AHI + a TCP/IP stack (Roadshow or AmiTCP) on the Amiga side, and a Piper server on the LAN.

### What's implemented

Everything below is built and validated on-target under Amiberry.

- **Standalone probe** (`wyomingtest`): connect to Piper via Wyoming, receive PCM, measure latency. On-target GOOD.
- **Standalone full pipeline** (`saytest`): text → Wyoming → AHI streaming playback. On-target, real-time playback confirmed.
- **The drop-in pair** — real `narrator.device` (`src/narrator_device.c` + freestanding `src/nw_engine.c`) **plus a pass-through `translator.library`** (`src/translator_library.c`). **Stock unmodified `Say` produces neural speech through the pair, on-target** — `Say` → our `translator.library` (English passed through; `Say` always calls it) → our `narrator.device` → Piper → AHI, no Guru, clean exit. `nw_engine.c` is a globals-free, libc-free, exec-only engine (bsdsocket + ahi.device). Voice params mapped: `volume`→AHI `ahir_Volume`, `sex`→Piper voice name; `rate`/`pitch` parsed-but-ignored (Wyoming has no per-request equivalent); `OpenDevice` inits the rb defaults. Runtime prefs: the device reads `ENV:narrator.wyoming` (`host`/`port`/`voice`/`voice_male`/`voice_female`/`ahi_unit`/`split_words`) via dos.library at `CMD_WRITE` time — nothing baked in but a localhost fallback; `sex` stays a runtime `narrator_rb` field, prefs just hold the per-sex voice-name mapping. Graceful failure: non-blocking connect + `WaitSelect` timeout (5s) so an unreachable/silent server fails fast instead of hanging on the OS TCP timeout, `SO_RCVTIMEO` bounds a mid-stream stall, and AHI/socket cleanup runs on every error path; `CMD_WRITE` returns a non-zero `io_Error` on failure (no hang/Guru).

**Persistent connection + own task.** The device runs its **own task** (created at `OpenDevice` — the context is passed via `tc_UserData`, set under an explicit `Forbid()/Permit()` around `CreateNewProcTags` so libnix's open-vector Forbid status doesn't matter; no globals). `CMD_WRITE` is **asynchronous**: `BeginIO` forwards the IORequest to the task's message port and the task `ReplyMsg()`s when done, so `SendIO` works and `BeginIO` doesn't block. The task holds **one Piper connection + AHI across all writes** (the `nw_say` per-call logic became `session_*` in `nw_engine.c`), reconnecting only if the endpoint changes or the link drops (one retry on a stale send). Verified on-target: 2 writes → 1 `socket()`/`connect()`. **Lifecycle handshake (signals).** `nw_dev_open` allocates two signal bits on the calling task and `Wait`s on `ready` before returning — the task `Signal`s it after publishing `cmdPort` (or after a startup failure), so by the time `OpenDevice` returns the port is guaranteed to be either valid or NULL (the submit path handles both). `nw_dev_close` sets `shutting`, sends the shutdown `Message`, and `Wait`s on `dead` — the task runs all its cleanup (socket, AHI, port) and `Signal`s `dead` as its absolute last segment act; the parent then `FreeMem`s `ctx` and `FreeSignal`s the bits. A small window remains between the task's `Signal(dead)` and its implicit RTS where the task is still executing segment code; expunge isn't synchronous with `CloseDevice` in practice, so an out-of-segment finalizer isn't justified. Shutdown also drains any messages still queued on the port (replied with `IOERR_ABORTED`) so a `shutMsg` arriving FIFO-ahead of queued `CMD_WRITE`s doesn't strand them. Open and close must come from the same task — `AllocSignal`/`FreeSignal` are per-task. Request queue + async are effectively done (the task's message port IS the queue: `SendIO` returns immediately, the task processes FIFO over the held connection — validated on-target). **`CMD_READ`/mouth-shape + word/syllable sync are out of scope (decided):** no phoneme timing from Piper, so it could only be faked from amplitude and would need reads concurrent with the playing write — `CMD_READ` returns `ND_NoWrite` (no hang). **Direct ARPABET phoneme input (the old contract, bypassing translator.library) is detected and silently discarded** (`looks_phonetic` in `nw_engine.c`: no lowercase + a stress digit like `EH1` → accept the write, play nothing) — Piper takes text not phonemes over Wyoming, so there's nothing useful to do with phonemes; English (incl. all-caps) is always spoken. AHI playback primes both buffers before starting (gapless first transition; see `ahi_write`).

Remaining hardening ideas: reconnect-on-drop mid-session, real `CMD_FLUSH`/`AbortIO` semantics, better IOSpeech mapping.

## Build / test / run

The Amiga build uses the **Bebbo m68k-amigaos GCC** cross-toolchain (GCC 6.5.0b), shipped in the `stefanreinauer/amiga-gcc:latest` Docker image (compiler at `/opt/amiga/bin/m68k-amigaos-gcc`, already on PATH inside the container).

- `make host` (default) — native binary `build/host/wyomingtest` via system `cc`. Use this to validate the protocol against a real server quickly.
- `make amiga` — cross-compile `build/amiga/wyomingtest` (needs `m68k-amigaos-gcc` on PATH; this is what runs inside the container).
- `make docker` — **recommended for the Amiga build**: runs `make amiga` inside the container with the repo mounted at `/work`; no local toolchain needed. The image is amd64, so on Apple Silicon it runs under emulation (works, just slower).
- `make dist` — pack the Aminet upload archive `build/narrator-wyoming.lha` (device + translator + readme + example + `Install`); upload it alongside `NarratorWyomingDev.readme`.
- `make clean` — remove `build/`.

**Versioning (single source of truth).** `version.mk` holds `VERSION`/`REVISION` — the Amiga `version.revision` shared by **both** the device and the library (currently **44.0**). The Makefile feeds them as `-DNW_VERSION/-DNW_REVISION` and the two sources derive `lib_Version`/`lib_Revision` **and** the `$VER` string from those macros (plus `-DNW_BUILD_DATE`, the build-time `(dd.mm.yyyy)`), so the number lives in exactly one place. `VERSION` 44 sits above every stock narrator (37) / translator (~43) so the pair is recognisable and passes any minimum-version check; bump it only on an incompatible change. The Aminet readme's `Version:` field tracks the same number: `make bump` syncs `NarratorWyomingDev.readme`, and `dist-pack` re-stamps the packed copy from `version.mk` regardless. **Release-then-bump flow:** the working tree always holds the revision you're *building toward*, and `make release` ships **exactly that number** — it does **not** bump. `make release` = `readme-version` sync + `clean dist` + (commit readme only if it drifted) + `git tag v<VERSION>.<REVISION>` at HEAD. After releasing, run `make bump` (`REVISION += 1` + readme sync) to **open the next test cycle**. So a cycle is: develop/test at `44.N` → `make release` (tags `v44.N`, ships the tested `44.N`) → `git push --follow-tags` → `make bump` (tree → `44.(N+1)` for the next cycle). **Never bump as part of an ordinary commit** — `version.mk` moves only via `make bump` right after a release. `make version` prints the current number; `make readme-version` re-syncs the readme (run it after a manual `VERSION` edit). Verify on-target with `Version narrator.device` / `Version translator.library`.

Run the probe (host):

```
./build/host/wyomingtest [--text "..."] [--voice NAME] [--out FILE] [--runs N] <host> [port=10200]
```

Use `--runs 3+` because the first request includes a one-time server-side voice-model load; the warm runs are the meaningful latency. `--out file.pcm` dumps raw PCM (headerless 22050/16/mono — wrap or play accordingly; `saytest` handles actual playback).

### Running on-target under Amiberry

The Amiga binaries are driven through the **Amiberry** emulator (`amiberry` MCP tools, or launch by hand) using the **`narrator`** config:

- `narrator.uae` sets `bsdsocket_emu=true` (bsdsocket.library mapped to the host network — no Roadshow/AmiTCP needed), boots DH0 from the **`HardDrives/narrator`** drive (which has **AHI installed**: `Devs/ahi.device` + `Devs/AHI/paula.audio` + `Devs/AudioModes/PAULA`), has sound enabled at full volume (`sound_volume=0` is Amiberry's "no attenuation"), and mounts this repo as **`NR0:` / volume `Narrator:`**. The system drive's `S:User-Startup` runs `Execute Narrator:boot` near the end of startup.
- `boot` (repo root, AmigaDOS script): `CD Narrator:`, **`Stack 131072`** (mandatory — see lessons), copies the device/translator/prefs into place, then runs the **Say acceptance test** (`SYS:Utilities/Say "..."`). Commented-out lines run a standalone program instead with stdout redirected to a log in the repo (`saytest` → `Narrator:saytest.log`, `wyomingtest` → `wyomingtest.log`, `devtest` → `devtest.log`).
- `config/narrator.wyoming` (**gitignored** — local/machine-specific; copy from the committed `config/narrator.wyoming.example`): `host <ip>` / `port 10200` / `runs 3` / `text ...`. Named to match the on-target `ENV:narrator.wyoming` the boot script copies it to.

Workflow: edit/rebuild (`make docker`) → **kill and relaunch Amiberry** (don't just reset — Amiberry caches mounted-file contents across a reset, so a soft reset re-runs the *old* binary) → check the result. **Only the `wyomingtest`/`saytest`/`devtest` boot variants write a log** (`./wyomingtest.log` / `./saytest.log` / `./devtest.log`) — the **Say acceptance test (the default `boot`) writes NO log** (it prints to a console that auto-closes after `Wait 30`), so don't poll for a log on a Say run; verify it with `runtime_screenshot_view` (clean Workbench = no Guru) + `get_crash_info` instead. Use `runtime_screenshot_view` to see Guru/console state. AHI audio can't be captured here — `saytest` prints `wall <ms> for ~<ms> of audio` as proof of real-time playback; listen on the emulator to confirm it's audible/correct.

### Amiga runtime gotchas (each one cost real debugging — keep them)

- **Use the clib2 C runtime (`-mcrt=clib2`).** The toolchain default is newlib, whose startup mishandles this launch context (garbage `argv`) and whose path faults. clib2 is the proven-good runtime (it's what the working MicroPython Amiga port at `/Users/simond/src/micropython/ports/amiga/` uses — a useful reference for bsdsocket code).
- **Stack.** The AmigaDOS default 4KB CLI stack is too small; the probe's locals + bsdsocket overflow it and corrupt memory, surfacing as a wild **Guru #8000000B (F-line)** far from the real cause. The launcher must raise it (`Stack 131072` in `boot`); there's no reliable link-time stack symbol under clib2.
- **errno pointer.** After `OpenLibrary("bsdsocket.library")`, call `SocketBaseTags(SBTM_SETVAL(SBTC_ERRNOPTR(sizeof(errno))), (long)&errno, TAG_END)`. Without it, the first socket call that sets errno (e.g. `recv` hitting EWOULDBLOCK) writes through an unset pointer.
- **Buffer your reads.** One `recv` per byte is murder on emulated bsdsocket (each call is a library trap) — it inflated round trip ~5×. `wyoming.c` reads into `rbuf` and drains it for both lines and payloads; call `wyo_reset()` per connection.
- **No FPU / no 64-bit libgcc in hot paths.** Build `-msoft-float` and keep latency/duration math 32-bit; hardware float opcodes (and some Bebbo 64-bit helpers) trap as F-line on this config.
- **argv is unusable** when launched from the startup-sequence — read settings from `config/narrator.wyoming` instead (the `#ifdef PLATFORM_AMIGA` path skips argv entirely).
- **AHI playback** (`audio_ahi.c`, `nw_engine.c`): open `ahi.device` unit `AHI_DEFAULT_UNIT` (0) with `ahir_Version = 4`; play 16-bit mono as `AHIST_M16S` via `CMD_WRITE`. Double-buffer with two `AHIRequest`s — the second is a `CopyMem` clone of the OpenDevice'd one sharing its reply port (so `FreeMem` it, don't `DeleteIORequest` it). `ahir_Link` chains the next buffer for gapless output; `WaitIO` the finished buffer before refilling. Requires AHI installed on the Amiga **and** an AHI unit-0 mode configured (the `paula.audio` driver works under emulation).
- **AHI sequencing needs `ahir_Link`; a deep ring of links hangs.** Two hard-won facts: (1) AHI plays *linked* `CMD_WRITE`s sequentially but plays *unlinked* concurrently-submitted ones **on top of each other** (sounds sped-up/garbled) — so you must link, you can't just `SendIO` many and rely on FIFO. (2) Tried to widen read-ahead by linking a deep N-buffer ring; reusing buffers makes the links form a **cycle**, and AHI then never replies a request so `WaitIO` hangs forever (no Guru, just a hang). Stick with the 2-buffer ping-pong. Consequence: read-ahead is bounded by AHI back-pressure (~2 buffers), so the playback lead can't be deep without a proper async AHI link-chain or a second reader task — not worth it (see `split_words` below).
- **`split_words` latency knob.** Piper renders one *sentence* before emitting audio, so a single long sentence has a long first-audio wait (~2.3s for ~40 words) while multi-sentence text already starts fast. `split_words N` (prefs; 0 = off) breaks input into ~N-word chunks sent **pipelined** on the one connection (wyoming-piper answers them back-to-back; verified) and played as one continuous AHI stream — first chunk starts in ~¼s. Splits happen **only at punctuation** (full stop always; comma/semicolon/colon once past N words) so seams land in Piper's natural pauses and stay inaudible; an unpunctuated run-on simply doesn't split (one request, slower start, no mid-phrase gap). Engine: `nw_split` + `session_send_one` (per-chunk) + `session_send_recv` (sends all up front, then reads until it has seen one `audio-stop` per chunk). A bigger `SO_RCVBUF` (set **before** connect) helps the server pile chunks ahead, but Amiberry's bsdsocket appears to cap it, so on emulation the lookahead is still AHI-bounded.
- **PCM endianness — byte-swap before AHI.** Piper/Wyoming PCM is 16-bit signed **little-endian**; AHI `AHIST_M16S` wants the Amiga's **big-endian** order. Feed it as-is and it plays as loud static (sounds like it's "working" — right duration, real-time pacing — but it's noise). Swap each 16-bit sample in place before `CMD_WRITE` (`swap16()`). Buffer sizes are even so samples never straddle a submitted buffer. The host `.wav` is unaffected (WAV is little-endian), so listen on-target — timing alone won't catch this.

### Device build (`narrator.device`) — libnix devinit framework

- `make device` (container) builds `src/narrator_device.c` into a real loadable `narrator.device`: `m68k-amigaos-gcc -nostdlib -fbaserel devinit.o narrator_device.c -o narrator.device`. libnix's `devinit.o` supplies the ROMTag + Open/Close/Expunge vectors; we supply `__UserDevInit/Open/Close/Cleanup` + `__BeginIO`/`__AbortIO` (registered via `ADDTABL_1(...,a1)`, terminated by `ADDTABL_END()`). Reference: the libnix repo `examples/simpledev.c` + `sources/startup/devinit.c`.
- The boot script `Copy`s it to `DEVS:narrator.device`; `src/devtest.c` (a normal clib2 program) opens and drives it. Test via the boot→`devtest.log` loop.
- **`__UserDevInit` must return NON-ZERO for success** (devinit.c: `if (!__UserDevInit(...)) { free; fail; }`). The stock `simpledev.c` returns 0 — which silently fails to open. (`__UserDevOpen` is the opposite: 0 = success.)
- **⚠️ NO C globals/statics in the device.** It's `-fbaserel` (A4-relative small data), but the libnix device hooks are **not** entered with A4 pointing at the device's data segment, so reading/writing any C global hits random memory and corrupts the exec free list → **Guru #8100 0005 (AN_MemCorrupt)**, surfacing *after* the device call (e.g. at the caller's exit). Confirmed the hard way. Instead: get `SysBase` locally from `*(struct ExecBase **)4UL`, keep `const` identity strings (those land in code/rodata, fine), and hang all per-open state off allocated structs reached via the IORequest — never a file-scope variable. This also means `wyoming.c`/`net_amiga.c`/`audio_ahi.c` need a globals-free pass before reuse inside the device.

### Build toolchain / NDK notes

- Amiga build flags: `-m68020 -msoft-float -mcrt=clib2` (see the runtime gotchas above for why each matters).
- The `Makefile` sets `CC := m68k-amigaos-gcc`, not `?=`: make pre-defines `CC` to `cc` (origin `default`), which `?=` will **not** override. Command-line `make CC=...` still wins.
- Bebbo's GCC does **not** accept `-noixemul`. Use `-m68020`.
- In `net_amiga.c`, include the C network headers (`sys/types.h` first, then `sys/socket.h`/`netinet/in.h`/`arpa/inet.h`) **before** `proto/bsdsocket.h`. `proto/bsdsocket.h` defines statement-expression macros for `inet_addr`/`inet_aton`/etc. and re-includes `arpa/inet.h`; if the prototypes aren't parsed first, the macros mangle them.
- bsdsocket.library has no linkable `gettimeofday`; Amiga timing uses `timer.device` (UNIT_ECLOCK / `ReadEClock`). `proto/timer.h` declares `extern struct Device *TimerBase;` — define it in exactly one TU.
