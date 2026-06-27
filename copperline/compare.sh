#!/bin/sh
# compare.sh — pass/fail aliasing / artifact check on a captured WAV.
#
# The fixture is 22050 Hz, so it physically cannot contain any content above
# 11025 Hz (its Nyquist). The Copperline capture is 44100 Hz, so ANY energy it
# has above 11025 Hz was manufactured by the playback chain — imaging from a
# poor AHI resample, or the harsh >11 kHz aliasing a low-quality Paula mode
# (e.g. "Fast 8 bit mono") produces (see docs/audio-capture-rig.md). So the
# metric is the ratio of above-Nyquist energy to total energy: low = clean,
# high = aliasing.
#
#   artifact_ratio_dB = RMS(>11025 Hz) - RMS(total)      (both over the active
#                                                          playback window)
#
# A clean HiFi-14-bit capture measures about -27 dB; a deliberately aliased
# mode pushes it far higher. PASS if the ratio is at/below THRESH_DB.
#
# NOTE: a pure low tone (e.g. 440 Hz) is a WEAK aliasing probe — it has no high
# harmonics to fold. For the strongest test use a sweep or HF tone fixture:
#   copperline/make-fixture.sh sweep   &&  copperline/capture.sh copperline/fixture.pcm
#
# Usage:  compare.sh [capture.wav] [THRESH_DB]
#           capture.wav  default copperline/out.wav
#           THRESH_DB    default -20  (artifact band must be >=20 dB below total)
set -e
here=$(cd "$(dirname "$0")" && pwd)
cap="${1:-$here/out.wav}"
thresh="${2:--20}"
nyq=11025

[ -f "$cap" ] || { echo "compare: no capture at $cap (run capture.sh first)" >&2; exit 2; }

# Isolate the active playback window so boot silence/transients don't skew the
# ratio, and normalise to mono.
act="$(dirname "$cap")/.active.wav"
ffmpeg -hide_banner -y -i "$cap" \
  -af "aformat=channel_layouts=mono,silenceremove=start_periods=1:start_threshold=-50dB:stop_periods=-1:stop_threshold=-50dB" \
  -ar 44100 "$act" 2>/dev/null

rms() { # mean_volume (dBFS RMS) of stdin filter chain; "-inf" -> -120
  v=$(ffmpeg -hide_banner -i "$act" -af "$1volumedetect" -f null - 2>&1 \
        | sed -n 's/.*mean_volume: \(-*[0-9.inf]*\) dB.*/\1/p' | head -1)
  [ "$v" = "-inf" ] || [ -z "$v" ] && echo "-120" || echo "$v"
}

total=$(rms "")
high=$(rms "highpass=f=${nyq}:p=2,highpass=f=${nyq}:p=2,")

python3 - "$total" "$high" "$thresh" "$cap" <<'PY'
import sys
total, high, thresh = float(sys.argv[1]), float(sys.argv[2]), float(sys.argv[3])
cap = sys.argv[4]
ratio = high - total                       # dB, negative = artifacts below signal
print(f"capture        : {cap}")
print(f"total RMS      : {total:6.1f} dBFS")
print(f">{11025} Hz RMS  : {high:6.1f} dBFS   (above-Nyquist = artifacts)")
print(f"artifact ratio : {ratio:6.1f} dB     (threshold {thresh:.0f} dB)")
ok = ratio <= thresh
print(f"\nRESULT: {'PASS' if ok else 'FAIL'}  — "
      + ("artifacts well below signal" if ok
         else "excessive >Nyquist energy (aliasing/imaging)"))
sys.exit(0 if ok else 1)
PY
