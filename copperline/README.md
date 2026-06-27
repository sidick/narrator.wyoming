# Copperline audio-capture harness

Deterministic capture of the **AHI rendering path** to a WAV file — the layer
where this project's silent bugs live (the LE→BE byte-swap static, audio-mode
aliasing, wrong sample rate). Amiberry can't capture AHI output (hence the
`wall ≈ duration` proxy and the BlackHole rig in `docs/audio-capture-rig.md`);
[Copperline](https://github.com/LinuxJedi/Copperline) can, headless and in
emulated time, via `--audio-wav`.

This is **not** an end-to-end `Say` test: Copperline has no host networking
yet (only a loopback NIC backend; no bsdsocket emulation), so the device can't
reach Piper here. Amiberry remains the harness for the network / `Say`
integration path. This harness covers only AHI audio correctness — decoupled
from the network by playing a fixed PCM fixture.

## How it works

`pcmplay` (`src/pcmplay.c`) is a network-free program that reads a raw PCM
fixture and plays it through the **same** `audio.h` sink the device and
`saytest` use (`audio_ahi.c` on Amiga: `AHI_AllocAudio` + `AHI_LoadSound` +
`AHI_Play`, with the LE→BE `copy_swap16`). So a captured WAV reflects the real
production playback path.

`capture.sh` does the rest:
1. cross-builds `pcmplay` in the container;
2. generates a 440 Hz sine fixture (or takes one you pass);
3. rsyncs the Amiberry `narrator` install into a throwaway `bootvol/` and
   patches its `S/User-Startup` to run `SYS:pcmplay`;
4. boots it headless with `--audio-wav out.wav --screenshot-after N shot.png`.

## Run

```sh
copperline/capture.sh                            # sine fixture, ~16s emulated
copperline/capture.sh copperline/fixtures/speech.pcm 20   # committed real speech
copperline/compare.sh                            # pass/fail aliasing check
```

`compare.sh` is the gate: the 22050 Hz fixture cannot contain content above
11025 Hz, so any energy the 44100 Hz capture has above that is aliasing/imaging
from the playback chain. It measures `RMS(>11025 Hz) - RMS(total)` over the
active window and PASSes if that ratio is at/below a threshold (default
-20 dB). Clean HiFi-14-bit measures ~-27 dB (PASS); broadband aliasing
measures near -4 dB (FAIL). Exit code 0/1, so it drops into CI.

Verified working: the sine capture is a clean 440 Hz tone ~280× above any
harmonic; the committed speech capture passes at -24 dB; `shot.png` shows a
clean Workbench (no Guru). Confirms AHI mode `0x0002000f` (paula HiFi 14-bit
calibrated) renders **and** that Copperline captures the paula.audio mix
faithfully.

### Fixtures

- `fixtures/speech.pcm` — **committed** 4.85 s Piper/Wyoming clip ("Narrator
  wyoming audio capture test. The quick brown fox…"). A fixed, server-
  independent input: the *rendered PCM* is committed, so the test is consistent
  on any machine with no Piper server needed. Use this as the canonical
  realistic regression input.
- `make-fixture.sh sweep` → a 200 Hz→11 kHz log sweep — the **strongest**
  aliasing probe (speech and low tones have little HF to fold). Regenerate a
  fresh speech clip any time with:
  `./build/host/wyomingtest --out copperline/fixtures/speech.pcm <piper-host>`.

## Files

| File | Purpose |
|---|---|
| `copperline.toml` | machine config — single IDE-master boot volume (`bootvol/`) |
| `capture.sh` | full build → assemble → boot → capture → report flow |
| `compare.sh` | pass/fail aliasing check on a capture (exit 0/1) |
| `make-fixture.sh` | generate a sine / HF tone / sweep `fixture.pcm` |
| `pcmplay.cfg` | pcmplay settings (input file, rate, channels, AHI mode) |
| `fixtures/speech.pcm` | **committed** real Wyoming clip (consistent input) |
| `bootvol/`, `fixture.pcm`, `out.wav`, `shot.png`, `.active.wav` | generated (gitignored) |

## Gotchas found wiring this up (keep these)

- **Copperline has no bsdsocket emulation.** `src/net.rs` is an emulated
  Ethernet NIC (a2065 LANCE) with only a loopback backend today (NAT/TAP are
  planned). Even when host networking lands it's NIC-level — you'd need a real
  Amiga TCP/IP stack bound to the a2065, not Amiberry's `bsdsocket_emu` trap.
  So the full `Say`→Piper round-trip can't run under Copperline.
- **KS3.1 only probes the IDE master.** A host dir mounted as `[ide].slave`
  never mounts → the boot hook never fires → silent capture. Use a single boot
  volume (or the A2091 `[scsi]` controller, which probes every unit).
- **Host-dir volume name = directory basename, and Amiga names are
  case-insensitive.** A boot dir `narrator` collides with a staging dir
  `Narrator`, so `Narrator:boot` resolves to the wrong volume. (Under Amiberry
  the boot volume has a different label, so the same install works there.)
- **A second SCSI host-dir volume didn't reliably get the `Narrator` label**
  the install's `Execute Narrator:boot` hook needs ("Please insert volume
  Narrator"). The single-volume copy with a patched `User-Startup` sidesteps
  every cross-volume naming/timing issue — that's why `capture.sh` does that.
- **Capture is 32-bit float stereo @ 44.1 kHz** regardless of the mono 22050
  source; resample before comparing to the fixture.
