# Audio capture & comparison rig

How to measure exactly what each stage of the pipeline does to the audio,
on-target under Amiberry. Built during the harsh-sibilance investigation.

## Three taps along one pipeline run

Each tap captures the audio at a different point. Because they're all fed
from a **single** Wyoming response within one `Say` invocation, Piper's
stochastic prosody doesn't matter (see `tools/compare_audio.py` docstring
for that gotcha).

| Stream | Where | How |
|---|---|---|
| `capture_raw.wav` | Raw Wyoming PCM into our device, **before** smooth and pre-roll | `nw_engine.c` tap inside `ahi_write`, on real PCM only (silence pre-roll has `data == NULL`) |
| `capture.wav` | Post-smooth, pre-byte-swap — exactly what we hand AHI | `nw_engine.c` tap inside `ahi_submit`, after `smooth_buf` and before `swap16` |
| `blackhole.wav` | Speaker-out — what comes out of paula.audio after the whole chain | Amiberry routed to BlackHole 16ch; ffmpeg records from avfoundation |

The first two are configured via prefs (`capture` / `capture_raw`); the
third needs the host-side setup below.

## BlackHole loopback setup

One-time:

1. Install BlackHole (Homebrew: `brew install blackhole-16ch`, or grab the
   package from Existential Audio).
2. In Amiberry's GUI (Sound prefs): **uncheck** "Use system default audio
   device" (config knob: `amiberry.soundcard_default=false`) and pick
   **BlackHole 16ch** as the output device. Save the config.
3. Verify ffmpeg sees BlackHole: `ffmpeg -f avfoundation -list_devices true -i ""`
   and note its index (here: `[4]`).

Recording: start ffmpeg *before* launching Amiberry so the boot delay is
captured as leading silence (the analysis tools trim it):

```sh
ffmpeg -y -f avfoundation -i ":N" -ac 1 -ar 44100 -t 60 blackhole.wav 2>blackhole.ffmpeg.log
```

**Always look up `N` fresh** with `ffmpeg -f avfoundation -list_devices true -i ""` —
avfoundation device numbers are unstable across sessions (plugging in
a USB camera, restarting CoreAudio, etc. shifts the indices). Using a
stale index silently captures the wrong device (typically a microphone
that picks up the ambient room sound). The spectrum will look like
"noise" with no obvious silence between speech and background — that's
the pattern to recognise, and the fix is to re-run the device list.

The **44.1 kHz** rate is important — it preserves 11-22 kHz content,
the band the harsh sibilance lives in. Recording at 22050 Hz throws
away exactly the bin you want to see.

Per-utterance prefs:

```
capture     Narrator:capture.wav
capture_raw Narrator:capture_raw.wav
```

After the run: `tools/compare_audio.py capture.wav capture_raw.wav`
gives the smooth filter's effect (same-source A/B); the BlackHole capture
needs a custom per-band analysis to look above 11 kHz — see
`tools/spectrum_report.py`.

If ffmpeg is killed mid-write the WAV header trailer is missing; repair
with `ffmpeg -y -i blackhole.wav -c:a copy blackhole.fixed.wav` before
processing.

## Key findings (validated on the Say acceptance text)

After peak-normalising each WAV (so amplitude differences don't confuse
the per-band ratios):

```
            band   blackhole     capture         raw    bh vs raw    bh vs cap
    0.0- 2.0kHz       6171        6017        6123    +0.07 dB    +0.22 dB
    2.0- 5.0kHz        857         815         857    -0.00 dB    +0.44 dB
    5.0- 8.0kHz        257         233         259    -0.07 dB    +0.84 dB
    8.0-11.0kHz        130         112         129    +0.03 dB    +1.28 dB
   11.0-15.0kHz        100    (above 22050 Nyquist; source physically zero)
   15.0-20.0kHz         74    (above 22050 Nyquist; source physically zero)
```

1. **The smooth filter is being defeated downstream.** capture (post-smooth)
   has 1.28 dB less 8-11 kHz than blackhole (post-paula). The chain after
   our filter restores that energy.
2. **There is substantial energy above the source's Nyquist** on the
   speaker-out: 100 RMS in 11-15 kHz, 74 RMS in 15-20 kHz, where the
   source PCM physically cannot contain any signal (22050 Hz Nyquist =
   11.025 kHz). This is aliasing/quantisation noise injected by the
   playback chain.
3. **BlackHole noise floor is exactly zero** in the pre-Say silence
   region: every band reads 0.00 RMS. The 11-20 kHz energy during speech
   is therefore real pipeline artefact, not loopback noise.
4. **Speech/silence ratio** in those bands is effectively infinite (zero
   in silence), confirming the aliasing tracks the speech amplitude —
   the classic "hissy on consonants" signature.

The 11-15 kHz aliasing reaches 78% of the legitimate 8-11 kHz content,
which is exactly the harsh-sibilance perceptual effect.

## Counter-data: Play16 sounds clear through the same AHI/paula

The stock Amiga `Play16` CLI player plays the same 22050 Hz source PCM
through the same Amiberry + AHI + paula chain **without** the harshness
we measure on our path. That rules out "AHI/paula is inherently broken
for 22050 Hz speech": the aliasing is specific to **our** use of AHI.

Pipeline difference, almost certainly:

- `Play16` uses AHI v4's **library interface** (`AHI_AllocAudio` with
  explicit `AHIA_AudioID` and `AHIA_MixFreq`, `AHI_LoadSound`,
  `AHI_Play`) — the same family of calls `filesave_lib_probe.c`
  demonstrates.
- `nw_engine.c` / `audio_ahi.c` use the **device interface**
  (`OpenDevice` + `CMD_WRITE` + `ahir_Link` ping-pong), which inherits
  the audio mode from the AHI unit's prefs entry. The configured Paula
  mode for the user's unit 0 may not be a clean 22050 path — there's
  likely a mismatched mixer rate hidden behind that mode ID.

So before reaching for more aggressive smoothing, look at moving the
playback path onto `AHI_AllocAudio` with `AHIA_MixFreq = 22050`
explicitly, like `filesave_lib_probe.c` already does for capture. That
would let the AHI mixer skip the suspect resample entirely and hand a
clean stream to paula.

`Play16` is available on-target at `SYS:Utilities/Play16` — useful as
the reference for "this should be how AHI sounds." Pipe a WAV through it
and capture via BlackHole to baseline what a well-behaved AHI consumer
produces with the same input. The boot script can run it as:

```
SYS:Utilities/Play16 Narrator:reference.wav
```

## Why the smooth filter doesn't help (and what would)

`smooth 2` (the validated default) cuts 0-2k / 2-5k / 5-11k by
-0.05 / -0.33 / -1.31 dB respectively (verified via `capture` vs
`capture_raw`). That's the right shape for a Paula-rate anti-alias
pre-filter — *if* aliasing were happening at the input stage. It's not.
Aliasing is being injected *after* our processing, in the AHI -> paula
output path. Pre-filtering input can't suppress what gets added later.

### What we've ruled out

A 4-phase BlackHole-loopback test (all in one boot, split on silence
gaps) compared every plausible knob:

| Phase | Path | 11-15kHz | 15-20kHz |
|---|---|---|---|
| 1 | Say (CMD_WRITE -> AHI -> paula mode 0x0002000c) | 92.0 | 68.3 |
| 2 | paula_lib_probe (AHI library -> paula mode 0x00020018) | 94.7 | 69.2 |
| 3 | Play16 OUTPUT=AHI MODE=131096 (paula mode 0x00020018) | 95.8 | 69.9 |
| 4 | Play16 OUTPUT=PAULA14 (direct paula, AHI bypassed) | **76.8** | **53.8** |

The first three are within 4% of each other — **library-vs-device API
doesn't matter; choice of paula mode doesn't matter** (at least between
the two we tested). The aliasing is intrinsic to having AHI's mixer in
the path, regardless of which paula mode the mixer feeds.

Phase 4 (bypass AHI) is the only meaningfully cleaner path: -1.8 dB and
-2.2 dB in the two aliasing bands. It also has -1.07 dB at 8-11 kHz vs
source — paula 14-bit direct has an inherent low-pass at that band,
doing some of what `smooth_buf` tries to do but applied at the right
point in the chain. It's not zero aliasing (76.8 RMS at 11-15 kHz, vs
BlackHole's exact-zero noise floor), but it's the cleanest path
measured.

### Things that would actually help, in roughly increasing engineering cost

1. **Bypass AHI entirely** — write directly to `audio.device` (Paula
   8/14-bit) like Play16's PAULA14 output does. Loses AHI's portability
   (sound cards) but eliminates the AHI-mixer aliasing. Significant
   refactor of `audio_ahi.c` / `nw_engine.c`. Doesn't eliminate paula's
   own aliasing (still 77/54 RMS above source Nyquist) but it's the
   single biggest improvement we can measure.
2. **Pre-filter harder** — `smooth 3` or stronger. The smooth_buf
   filter's effect on the *source* survives the chain in inverse
   proportion to AHI's contribution (since AHI puts highs back), so
   pre-cutting the source more aggressively still helps if the input
   has less to be aliased up. Sample-rate-dependent: smooth that nulls
   at 22050/2 doesn't shape the band we care about (5-11 kHz). Worth
   trying anyway.
3. **Pick a hi-fi AHI mode explicitly** (instead of inheriting unit 0's
   "Fast 8 bit mono"). Run `ahi_modes_probe` to A/B alternatives. Modes
   measured so far (peak-normalised, 11-15 / 15-20 kHz aliasing RMS):

       paula:Fast 8 bit mono            0x0002000c    96 / 71  (default / worst)
       paula:DMA 8 bit stereo           0x00020018    92 / 67
       paula:HiFi 14 bit mono calibr.   0x0002000f    89 / 64  (-7 / -10%)
       uaesnd:HiFi Stereo               0x003b0002    83 / 61  (-14 / -15%)
       UAE 16 bit HIFI Stereo++         0x001a0000    83 / 60  (-14 / -15%)

   uaesnd / UAE are emulator-specific (won't exist on real hardware);
   paula HiFi 14 bit mono calibrated is the best real-hardware-portable
   choice and the right default for nw_engine.c on real targets.
   On Amiberry, uaesnd or UAE is cleaner and what user-facing AHI
   prefs typically steers toward (this is what other AHI consumers like
   Play16 sound clean through, when the user picks the AHI mode in
   Play16's requester). The right strategy is: switch nw_engine.c to
   the AHI library interface, expose the audio mode ID as a prefs key
   so the user can pick, default to 0x0002000f (paula HiFi 14 bit
   mono calibrated). This is the one actionable refactor with measured
   improvement.

### Confirmed not to help

- **Lowering source rate to 11025 Hz** — we tested this empirically by
  downsampling 22050 -> 11025 with anti-aliased 2:1 decimation
  (`paula_11025_probe.c`) and playing through the same AHI/paula
  chain. The result was DRAMATICALLY worse: the aliasing bands went
  from 91/67 RMS at 22050 to 277/211 at 11025, with 2-11 kHz energy
  +5 to +9 dB above the source's reference. The chain's upsample from
  11025 back to its internal rate is almost certainly zero-order hold
  (sample-and-hold), which mirrors the source spectrum around the
  source Nyquist (5512.5 Hz) and pushes all 0-5.5 kHz content into the
  5.5-11 kHz band as spectral images. Those then alias further into
  11-20 kHz. Lesson: the chain is bad at both downsample AND upsample.
  Asking Piper for 11025 output would have the same problem; do not
  pursue.

## Reproduction quick-script

```sh
# 1. Enable taps
echo "capture     Narrator:capture.wav"     >> config/narrator.wyoming
echo "capture_raw Narrator:capture_raw.wav" >> config/narrator.wyoming

# 2. Start BlackHole recording (60s, more than enough for one Say)
rm -f capture.wav capture_raw.wav blackhole.wav
ffmpeg -y -f avfoundation -i ":4" -ac 1 -ar 44100 -t 60 blackhole.wav \
    2>blackhole.ffmpeg.log &

# 3. Launch Amiberry (uses configured BlackHole output)
amiberry --config narrator.uae &

# 4. Wait for the two prefs.capture files to finalize (size in header > 0)
until [ -s capture.wav ] && [ -s capture_raw.wav ] && \
      python3 -c "import struct; \
                  exit(0 if struct.unpack('<I', open('capture.wav','rb').read()[40:44])[0] else 1)" && \
      python3 -c "import struct; \
                  exit(0 if struct.unpack('<I', open('capture_raw.wav','rb').read()[40:44])[0] else 1)"; do
    sleep 2
done

# 5. Stop ffmpeg + Amiberry, repair WAV (ffmpeg killed mid-write)
pkill -f "ffmpeg -y -f avfoundation"
pkill amiberry
ffmpeg -y -i blackhole.wav -c:a copy blackhole.fixed.wav
mv blackhole.fixed.wav blackhole.wav

# 6. Analyse
tools/compare_audio.py capture.wav capture_raw.wav       # smooth's effect
tools/spectrum_report.py blackhole.wav --reference capture_raw.wav
```
