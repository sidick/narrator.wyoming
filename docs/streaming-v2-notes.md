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

## Recommended next move when we come back to this

Either:
- **Re-investigate hook timing**: add to `hook_probe.c` a second
  `AHI_Play` queued while the first is still playing, plus calls to
  `AHI_GetAudioAttrs(AHIDB_PlayingSound)` to confirm what AHI thinks is
  on the channel at hook-fire time. Decide whether SoundFunc is
  reliable enough for ping-pong.
- Or **prototype the ring-buffer approach** as a new probe
  (`dynamicsample_probe.c`?). If polling `AHIDB_PlayingOffset` works
  cleanly, this becomes the streaming primitive of choice.

Either way, audio_ahi.c and nw_engine.c stay on v1 buffered playback
until one of those probes proves a path forward. v1 has a real latency
regression but is correct and audible; the bar for swapping it out is
"the alternative is also correct AND audible AND lower-latency".
