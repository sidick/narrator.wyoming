#!/bin/sh
# make-fixture.sh — generate a clean 440 Hz sine test fixture for pcmplay.
#
# Raw, headerless, signed 16-bit LITTLE-ENDIAN, mono, 22050 Hz — exactly the
# format Piper/Wyoming emits (audio_ahi.c byteswaps to BE on the way into AHI,
# so the fixture must be LE, NOT pre-swapped).
#
# A clean sine is the right FIRST signal: aliasing or a dead byte-swap shows
# up obviously in the captured WAV's spectrum (the live capture is a 440 Hz
# tone ~280x above any harmonic). Swap in real Piper PCM later with:
#   wyomingtest --out copperline/fixture.pcm <host>
#
# Usage: make-fixture.sh [freq=440 | sweep] [seconds=2]
#   make-fixture.sh            -> 440 Hz sine (basic liveness)
#   make-fixture.sh 8000       -> 8 kHz sine (HF aliasing probe)
#   make-fixture.sh sweep      -> 200 Hz..11 kHz log sweep (STRONGEST aliasing
#                                 probe: a bad mode folds the top of the sweep
#                                 back down as audible junk; compare.sh catches it)
set -e
here=$(cd "$(dirname "$0")" && pwd)
freq="${1:-440}"
dur="${2:-2}"
out="$here/fixture.pcm"
if [ "$freq" = "sweep" ]; then
  src="sine=frequency=200:beep_factor=0:duration=${dur}:sample_rate=22050"  # placeholder; replaced below
  ffmpeg -y -f lavfi -i "aevalsrc=0.7*sin(2*PI*(200*exp(t/${dur}*log(11000/200)))*t):d=${dur}:s=22050" \
    -ac 1 -f s16le "$out" 2>/dev/null
  echo "fixture: $out ($(ls -l "$out" | awk '{print $5}') bytes, 200Hz->11kHz sweep ${dur}s)"
else
  ffmpeg -y -f lavfi -i "sine=frequency=${freq}:duration=${dur}:sample_rate=22050" \
    -ac 1 -f s16le "$out" 2>/dev/null
  echo "fixture: $out ($(ls -l "$out" | awk '{print $5}') bytes, ${freq}Hz ${dur}s)"
fi
