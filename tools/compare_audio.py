#!/usr/bin/env python3
"""compare_audio.py — host-side A/B of two PCM streams (WAV or raw 16-bit mono).

Reports length / amplitude / RMS / SNR / per-band energy ratios. The per-band
split is the signal you want to see the smooth filter at: a deepening cut as
you go up the bands means the high-cut is doing its job.

Auto-detects WAV by RIFF magic; raw PCM needs --ref-rate/--ref-channels/
--ref-bits. Aligns the two streams by trimming whichever has more leading
silence (the device's per-utterance pre-roll typically puts a ~50ms hump on
the capture side; without alignment that wrecks every sample-level metric).

GOTCHA — Piper is NOT deterministic.

  Two consecutive `wyomingtest --out` calls with identical text and voice
  produce uncorrelated audio (verified: SNR -3 dB between two host-side
  Piper runs, random per-band ratios). Piper's neural TTS samples stochastic
  prosody per inference, so a fresh server-side reference is NOT a valid
  ground truth for an on-target capture. The two streams will be similar
  prosody/words but different waveforms, swamping any genuine pipeline
  effect in the comparison.

  Use this tool for comparisons that share a single Piper response:
    - Before/after a code change, both runs captured under the same Piper
      session (will still differ — Piper varies — but reduces uncertainty).
    - Different code paths fed the SAME pre-recorded PCM (e.g. a synthetic
      test wave, or a captured PCM replayed locally).
    - Validating filter passes by running a synthetic signal (sine, sweep)
      through both sides with deterministic generation.

  To genuinely A/B the on-target pipeline against the raw Wyoming input
  for one utterance, you need TWO capture taps in nw_engine — one before
  smooth/byte-swap (raw Wyoming PCM) and one after — both written by the
  same Wyoming response. That keeps the source identical.

Stdlib only (no numpy). Mono only. 16-bit PCM only.
"""

import argparse
import array
import json
import math
import struct
import sys
import wave


def read_audio(path, ref_rate=None, ref_channels=None, ref_bits=None):
    """Return (samples: array('h'), rate, channels, bits). Stereo is downmixed."""
    with open(path, 'rb') as f:
        head = f.read(4)
    if head == b'RIFF':
        with wave.open(path, 'rb') as w:
            channels = w.getnchannels()
            bits     = w.getsampwidth() * 8
            rate     = w.getframerate()
            raw      = w.readframes(w.getnframes())
    else:
        if not (ref_rate and ref_channels and ref_bits):
            sys.exit(f"{path!r} isn't a WAV; pass --ref-rate/--ref-channels/--ref-bits")
        rate, channels, bits = ref_rate, ref_channels, ref_bits
        with open(path, 'rb') as f:
            raw = f.read()
    if bits != 16:
        sys.exit(f"{path!r}: only 16-bit PCM supported (got {bits})")
    samples = array.array('h')
    samples.frombytes(raw)
    if channels == 2:
        mono = array.array('h', (0,) * (len(samples) // 2))
        for i in range(len(mono)):
            mono[i] = (samples[2 * i] + samples[2 * i + 1]) // 2
        samples, channels = mono, 1
    return samples, rate, channels, bits


def peak(samples):
    return max((abs(s) for s in samples), default=0)


def rms(samples):
    if not samples:
        return 0.0
    s = sum(int(v) * int(v) for v in samples)
    return math.sqrt(s / len(samples))


def find_onset(samples, threshold_pct=1.0):
    """Index of the first sample whose |amplitude| > threshold_pct of peak."""
    p = peak(samples)
    if p == 0:
        return 0
    cutoff = int(p * threshold_pct / 100)
    for i, s in enumerate(samples):
        if abs(s) > cutoff:
            return i
    return 0


def iir_lowpass(samples, rate, fc):
    """Single-pole RC low-pass at fc Hz. Returns a new int16 array."""
    if fc <= 0 or fc >= rate / 2:
        return array.array('h', samples)
    dt    = 1.0 / rate
    rc    = 1.0 / (2.0 * math.pi * fc)
    alpha = dt / (rc + dt)
    out   = array.array('h', (0,) * len(samples))
    y     = 0.0
    for i, s in enumerate(samples):
        y = alpha * s + (1.0 - alpha) * y
        out[i] = int(y)
    return out


def band_rms(samples, rate, fc_low, fc_high):
    """RMS of the (fc_low..fc_high) Hz band via a difference-of-lowpasses split."""
    if fc_high < rate / 2:
        hi = iir_lowpass(samples, rate, fc_high)
    else:
        hi = array.array('h', samples)
    if fc_low > 0:
        lo   = iir_lowpass(samples, rate, fc_low)
        band = array.array('h', (hi[i] - lo[i] for i in range(len(samples))))
    else:
        band = hi
    return rms(band)


def db(ratio):
    if ratio <= 0:
        return float('-inf')
    return 20.0 * math.log10(ratio)


def clip16(x):
    if x >  32767: return  32767
    if x < -32768: return -32768
    return x


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('capture',   help='on-target WAV from prefs.capture')
    ap.add_argument('reference', help='reference WAV or raw PCM')
    ap.add_argument('--ref-rate',     type=int, default=22050)
    ap.add_argument('--ref-channels', type=int, default=1)
    ap.add_argument('--ref-bits',     type=int, default=16)
    ap.add_argument('--diff-out',  help='write aligned (capture - reference) as WAV')
    ap.add_argument('--json',      action='store_true', help='machine-readable output')
    args = ap.parse_args()

    cap, cap_rate, cap_ch, _ = read_audio(args.capture)
    ref, ref_rate, ref_ch, _ = read_audio(
        args.reference, args.ref_rate, args.ref_channels, args.ref_bits)

    if cap_rate != ref_rate:
        sys.exit(f"rate mismatch: capture {cap_rate} vs reference {ref_rate}")
    if cap_ch != ref_ch:
        sys.exit(f"channel mismatch: capture {cap_ch} vs reference {ref_ch}")
    rate = cap_rate

    cap_onset = find_onset(cap)
    ref_onset = find_onset(ref)
    if cap_onset > ref_onset:
        cap = cap[cap_onset - ref_onset:]
        pre_roll = cap_onset - ref_onset
    elif ref_onset > cap_onset:
        ref = ref[ref_onset - cap_onset:]
        pre_roll = -(ref_onset - cap_onset)
    else:
        pre_roll = 0

    n   = min(len(cap), len(ref))
    cap = cap[:n]
    ref = ref[:n]
    diff = array.array('h', (clip16(int(cap[i]) - int(ref[i])) for i in range(n)))

    cap_rms = rms(cap)
    ref_rms = rms(ref)
    dif_rms = rms(diff)
    snr_db  = db(ref_rms / dif_rms) if dif_rms > 0 else float('inf')

    bands = [(0, 2000), (2000, 5000), (5000, rate // 2)]
    band_report = []
    for lo, hi in bands:
        c_e = band_rms(cap, rate, lo, hi)
        r_e = band_rms(ref, rate, lo, hi)
        band_report.append({
            'band'    : f'{lo/1000:.1f}-{hi/1000:.1f}kHz',
            'cap_rms' : c_e,
            'ref_rms' : r_e,
            'ratio_db': db(c_e / r_e) if r_e > 0 else float('-inf'),
        })

    report = {
        'capture'           : args.capture,
        'reference'         : args.reference,
        'rate'              : rate,
        'channels'          : cap_ch,
        'aligned_samples'   : n,
        'aligned_duration_s': n / rate,
        'pre_roll_samples'  : pre_roll,
        'pre_roll_ms'       : 1000.0 * pre_roll / rate,
        'capture_peak'      : peak(cap),
        'reference_peak'    : peak(ref),
        'capture_rms'       : cap_rms,
        'reference_rms'     : ref_rms,
        'diff_rms'          : dif_rms,
        'snr_db'            : snr_db,
        'bands'             : band_report,
    }

    if args.diff_out:
        with wave.open(args.diff_out, 'wb') as w:
            w.setnchannels(cap_ch)
            w.setsampwidth(2)
            w.setframerate(rate)
            w.writeframes(diff.tobytes())
        report['diff_out'] = args.diff_out

    if args.json:
        print(json.dumps(report, indent=2, default=str))
        return

    print(f"capture     : {args.capture}")
    print(f"reference   : {args.reference}")
    print(f"rate        : {rate} Hz, {cap_ch} channel(s)")
    print(f"aligned     : {n} samples ({report['aligned_duration_s']:.2f}s)")
    if pre_roll > 0:
        print(f"pre-roll    : {pre_roll} samples ({report['pre_roll_ms']:.0f}ms)"
              f" of leading silence in capture (trimmed for alignment)")
    elif pre_roll < 0:
        print(f"pre-roll    : reference led by {-pre_roll} samples"
              f" ({-report['pre_roll_ms']:.0f}ms) — unusual; trimmed for alignment")
    print()
    print(f"peak (cap)  : {peak(cap)}")
    print(f"peak (ref)  : {peak(ref)}")
    print(f"rms  (cap)  : {cap_rms:.1f}")
    print(f"rms  (ref)  : {ref_rms:.1f}")
    print(f"rms  (diff) : {dif_rms:.1f}")
    snr_s = "(higher = more identical)"
    print(f"SNR         : {snr_db:+.1f} dB  {snr_s}")
    print()
    print("Per-band RMS  (cap / ref — negative ratio = capture attenuated):")
    for b in band_report:
        arrow = "down" if b['ratio_db'] < -0.5 else ("up" if b['ratio_db'] > 0.5 else "flat")
        ratio = b['ratio_db']
        ratio_s = "-inf" if ratio == float('-inf') else f"{ratio:+.2f} dB"
        print(f"  {b['band']:>14}  cap={b['cap_rms']:7.1f}  ref={b['ref_rms']:7.1f}"
              f"  ratio={ratio_s:>10}  {arrow}")
    if args.diff_out:
        print()
        print(f"diff WAV    : {args.diff_out}  (play to hear residual)")


if __name__ == '__main__':
    main()
