# Streaming v2 — investigation notes

Status: **deferred**. v1 buffered playback stays in production until we
understand `AHIA_SoundFunc` semantics on Amiberry's paula HiFi 14-bit mono
calibrated mode well enough to drive a 2-slot ping-pong, or until we
implement `AHIST_DYNAMICSAMPLE` + ring buffer (the AHI Developer Guide's
canonical streaming primitive).

## What the streaming-v2 plan was

Two PCM slots ping-ponging, `AHI_LoadSound` + `AHI_Play` per slot, hook
fires at end-of-sound → producer wakes → promotes the queued slot to
playing. Same shape as the old `ahir_Link` CMD_WRITE design but expressed
in the AHI library interface. First-audio latency ≈ time to recv one
slot's worth of bytes (~50 ms on a fast LAN at 22050/16/mono).

## Step 1 — `hook_probe.c` (passed)

Verified that:

1. Bebbo amigaos GCC's register-args convention compiles and links for
   AHI hooks: `struct Hook * __asm("a0")`, `AHIAudioCtrl * __asm("a2")`,
   message in `a1`. No special wrapper or assembly trampoline needed.
2. `AHIA_SoundFunc` is honoured by paula HiFi 14-bit mono cal under
   Amiberry — `AHI_AllocAudio` succeeded with the tag and the hook
   actually fired.
3. `Signal()` from the hook reaches the consumer task's `Wait` — no
   cross-task complications.
4. `__attribute__((saveds))` "is only usable with fbaserel" — needed for
   the eventual nw_engine.c port (which IS -fbaserel) but rejected on
   the clib2 hook_probe / audio_ahi.c builds.

Result: hook fires twice for two `AHI_Play` calls. Counter ends at 2.
Single-sound case is well-behaved.

## Step 2 — ping-pong in `audio_ahi.c` (failed; reverted)

Implemented `AHIA_Sounds = 2` + hook-driven slot promotion, instrumented
with submit / hook-fire / blocked-wait counters. Result on the Say
acceptance text (29 buffers of 8 KB each):

  audio_close diag: submits=29 hook_fires=29 blocked_waits=27 inflight=0
  wall 1459 ms for ~5108 ms of audio

Plumbing is *correct in shape* — submits and hook fires are balanced,
inflight reaches zero on close. **But the wall time is far too short**:
1.5 s real-time for 5 s of audio means the hook fires roughly every
55 ms, not every ~370 ms (each slot's natural duration at 22050/16/mono).
And the BlackHole capture was silent — meaning the audio that the
producer thinks is "in flight" is being either truncated or otherwise
not playing through paula.

**Hypothesis**: `AHIA_SoundFunc` does NOT fire only at end-of-sound on
this code path. It may fire per mix buffer (every ~50 ms of internal
mixing), per loop iteration, or whenever the channel state changes —
the AHI guide says "called when a sound is no longer playing" which is
broader than "called at end of sound" and accommodates being replaced
mid-play. Single-sound hook_probe didn't surface this because there
was no other sound competing.

Reverted. Streaming via SoundFunc + slot promotion needs more digging.

## Step 2b — single-slot Delay-paced streaming (failed; reverted)

Tried a simpler alternative: one sound slot, fill → `AHI_LoadSound` →
`AHI_Play` → `Delay(samples * 50 / rate)`, repeat. No hook involved.
saytest log:

  wall 6175 ms for ~5027 ms of audio   (∼5 s playback + ∼1.1 s receive
                                         overhead — correct streaming wall)

Wall time was right. BlackHole was silent. Hypothesis: repeatedly
calling `AHI_LoadSound(0, ...)` on the same slot while the mixer is
running and the channel may still be playing the previous Play doesn't
work cleanly — either `LoadSound` fails silently or `AHI_Play` on a
just-reloaded slot races with the previous play's tail. Did not chase
further; reverted.

## What AHI offers for "chain this sound after that one"

Three real options exist; we just haven't validated any of them on this
setup:

1. **`AHIP_LoopSound` / `AHIP_LoopVol` etc. tags on `AHI_Play`** —
   primary sound plays once, then the "loop" sound plays (and loops
   forever) until something else interrupts. Useful for a single
   primary + an infinite trailing pad, less so for arbitrary chaining
   unless we accept the loop-forever semantics and interrupt with the
   next `AHI_Play`.
2. **`AHIA_SoundFunc` + `AHI_SetSound`** — canonical pattern in the AHI
   guide. Hook fires at end-of-sound, hook (or task) calls
   `AHI_SetSound` to change channel's sound. This is what we tried;
   the failure mode above suggests hook timing is the unknown to
   resolve, not the mechanism itself. Worth instrumenting with
   `AHI_GetAudioAttrs(AHIDB_PlayingSound)` to see what AHI thinks is
   playing at hook-fire time.
3. **`AHIST_DYNAMICSAMPLE` + ring buffer** — the AHI guide's
   canonical streaming primitive. One large sample marked DYNAMIC,
   played with loop, write into the buffer ahead of the play head,
   poll `AHIDB_PlayingOffset` for read position. Most complex but
   truly gapless and the design AHI was built for.

## Step 3 — `AHIST_DYNAMICSAMPLE` + looping (passed)

`dynamic_probe.c` allocates a 32 KB ring, pre-fills it with a 440 Hz sine,
loads it with `AHI_LoadSound(0, AHIST_DYNAMICSAMPLE, ...)`, sets up looping
in `AHI_Play` (`AHIP_Sound = 0` + `AHIP_LoopSound = 0` + `AHIP_LoopVol =
full`), then rewrites the ring's contents twice during playback (440 →
880 → 440 at 1-second intervals).

Goertzel analysis of the BlackHole capture against 440 / 880 / 1320 Hz:

  window         440 Hz       880 Hz       1320 Hz    dominant
  7.0-7.5s     10,266,435      122,696      108,693    440 Hz
  7.5-8.0s      4,825,452      201,592       79,875    440 Hz
  8.0-8.5s         36,390    2,037,948      108,897    880 Hz   <- swap took
  8.5-9.0s      1,561,765    1,838,606      159,410    880 Hz
  9.0-9.5s              0            0            0    (transition)
  9.5-10.0s     2,112,135       79,522       33,043    440 Hz   <- swap back

The buffer-content swaps clearly take effect within the running loop
without breaking continuity (modulo a brief transition window around
the swap point at 9.0-9.5s where one cycle was mid-replay).

This is the streaming primitive. The full streaming design becomes:

  - Allocate a ring of, say, ~370 ms (16 KB) or ~750 ms (32 KB).
  - LoadSound it as DYNAMICSAMPLE with looping.
  - Producer maintains a write head; chunks of incoming PCM are written
    ahead of the play head.
  - For pacing: either poll `AHI_GetAudioAttrs(AHIDB_PlayingOffset)` to
    track the read head, or write at a rate paced by `Delay()` matched
    to the audio sample rate (simpler, less precise).
  - End-of-stream: write silence to drain, AHIC_Play FALSE when the play
    head clears the last real sample.

## Step 4 — `stream_probe.c`, small incremental writes (surprising failure mode)

`stream_probe.c` writes 30 × 8 KB (~185 ms each) chunks into a 32 KB
ring, frequency 440 → 880 → 440, paced by `Delay(9)` (~180 ms). Phase
continuous, ring pre-filled with 440 Hz so playback starts immediately
with the head start of one full ring lap. No polling — relies on
`Delay` being close enough to chunk duration that the write head walks
steadily ahead of the play head.

BlackHole capture (Goertzel at 440 / 880 Hz, 50 ms windows):

  - Correct frequency content per band (440, 880, 440 in the right order
    at roughly the right wall times — content of the stream is right).
  - **But ~50/50 duty cycle of audio bursts vs full silence**:
    audio for ~600 ms, dead silence for ~600 ms, repeating across the
    whole 6 s run. Each burst is at full amplitude with correct
    frequency; each gap is `peak == 0` (clean digital zero).

This is *not* underrun-style stutter (which would be noise / wrong
phase). It looks like AHI's mixer alternately respecting and ignoring
the buffer contents. The 1.2 s period doesn't align cleanly with ring
laps (~371 ms), chunk pacing (~180 ms), or anything else in the probe.

Same probe with bulk-fill (write entire 32 KB ring then Delay 1 s)
was `dynamic_probe.c` step 3 — clean continuous audio across frequency
swaps. So **small incremental writes provoke the gap pattern; large
atomic whole-ring writes do not.** The difference is the write
granularity relative to AHI's internal mix-buffer lifecycle.

## Working hypothesis on the gaps

AHI under Amiberry's paula HiFi 14-bit mono cal driver pre-fetches /
caches the dynamic sample in mix buffers. Small mid-stream writes that
land on positions AHI has already cached but not yet played produce
inconsistent state, and the mixer falls back to silence rather than
mixing inconsistent samples. Bulk writes that change the whole buffer
between mix-buffer fetches don't trip this because the buffer is
stable across each fetch.

This is a hypothesis — not verified. Verifying would need either
source dive into the driver or a separate probe that varies the write
granularity (e.g. write 16 KB chunks, 4 KB chunks) and looks for the
gap pattern threshold.

## What this means for the design

`AHIST_DYNAMICSAMPLE` + ring buffer + small incremental writes is **not
a clean streaming primitive on this driver** (at least at small write
sizes). Three remaining paths to streaming:

1. **Large-write ring** — buffer ~3-4 ring-laps of audio before each
   atomic whole-ring rewrite. ~700 ms of buffering = ~700 ms of added
   latency. Defeats most of the point of streaming v2 (v1's first-audio
   ≈ 450 ms already; adding 700 ms makes it 1.15 s).
2. **Multiple sound slots, hook-driven promotion** — step 2 design.
   Failed because the hook fires per-mix-buffer (~55 ms) not per-sound
   (~370 ms) on this mode. Could revisit with different
   `AHIA_AudioID` modes (uaesnd / UAE) and instrument
   `AHIDB_PlayingSound` to figure out hook semantics, or use the hook
   as a "go" signal and trust AHI to play sounds back-to-back.
3. **Single sample, fill incrementally as a one-shot (no loop)** —
   pre-allocate a max-size buffer (~1.3 MB for 30 s of audio), fill
   from the front as Wyoming chunks arrive, AHI plays it as a single
   non-looping sound. The play head is bounded by what we've written
   (silence at the front if we haven't filled enough yet); we just
   need to write faster than AHI reads. Avoids the loop-and-rewrite
   race because AHI plays each byte once.

Option 3 is the most appealing of the three: it sidesteps the
loop/race entirely, and the wasted memory is unimportant. The
remaining question is whether AHI plays a non-DYNAMIC sample whose
contents we mutate ahead of the play head, or whether we still need
DYNAMIC even for one-shot non-looping playback. Probably DYNAMIC since
without it AHI is allowed to read once and cache.

## Recommended next move when we come back to this

Implement option 3 as `oneshot_stream_probe.c`: allocate ~256 KB
(~5.8 s of audio, big enough for the test) as DYNAMICSAMPLE, AHI_Play
the whole thing as one non-looping sample, fill 8 KB chunks from the
front while it plays, see if the gap pattern persists or not. If
audio is clean, port it into `audio_ahi.c`. If the gap pattern still
appears, fall back to v1 buffered playback for production and revisit
when the AHI driver behavior is better understood.

Until then, `audio_ahi.c` and `nw_engine.c` stay on v1 buffered
playback. v1 is well within latency budget (~450 ms first audio) and
ships now.
