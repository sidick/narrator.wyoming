#!/usr/bin/env python3
"""spectrum_report.py — multi-band RMS for one WAV, with noise-floor check.

Designed for the BlackHole-loopback capture in `docs/audio-capture-rig.md`.
Splits a recording into per-band RMS using single-pole IIR low-passes
(no FFT / no numpy), with extended bins that go above the source's
22050 Hz Nyquist so aliasing in the on-target playback chain becomes
visible.

The default six bands cover everything we care about for the harsh-
sibilance investigation:

  0-2k    speech body, all the energy
  2-5k    speech mid
  5-8k    upper formant
  8-11k   sibilance (legitimate s/sh/f content)
  11-15k  aliasing band 1 — anything here on a 22050 Hz source is artefact
  15-20k  aliasing band 2 — same; closer to the host audio Nyquist

A WAV that reads non-trivial RMS in the 11-15k / 15-20k bands while the
source PCM is 22050 Hz proves something downstream of our pipeline is
folding energy upward (paula 8-bit reduction + the AHI mixer's resample
to paula's rate are the suspects in our setup).

Auto-detects silence vs speech regions via amplitude threshold so it can
report both the noise floor and the active-signal spectrum — a non-zero
floor in the high bands would mean the input device itself is noisy
(BlackHole loopback is exactly zero, so any high-band content during
speech is genuine pipeline artefact).

If --reference is given, also reports per-band ratios (capture/reference,
in dB) only across bands the reference's sample rate covers. Designed to
compare a 44.1 kHz BlackHole recording against the 22.05 kHz Wyoming
source (capture_raw.wav).
"""

import argparse
import array
import math
import struct
import sys
import wave


BANDS = [
    (0,     2000),
    (2000,  5000),
    (5000,  8000),
    (8000,  11000),
    (11000, 15000),
    (15000, 20000),
]


def read_wav(path):
    with wave.open(path, 'rb') as w:
        ch    = w.getnchannels()
        bits  = w.getsampwidth() * 8
        rate  = w.getframerate()
        raw   = w.readframes(w.getnframes())
    if bits != 16:
        sys.exit(f"{path!r}: only 16-bit PCM supported (got {bits})")
    samples = array.array('h')
    samples.frombytes(raw)
    if ch == 2:
        mono = array.array('h', (0,) * (len(samples) // 2))
        for i in range(len(mono)):
            mono[i] = (samples[2*i] + samples[2*i+1]) // 2
        samples = mono
    return samples, rate


def rms(s):
    if not s:
        return 0.0
    return math.sqrt(sum(int(v)*int(v) for v in s) / len(s))


def peak(s):
    return max((abs(v) for v in s), default=0)


def iir_lp(samples, rate, fc):
    if fc <= 0 or fc >= rate / 2:
        return list(samples)
    dt    = 1.0 / rate
    rc    = 1.0 / (2.0 * math.pi * fc)
    alpha = dt / (rc + dt)
    out, y = [0] * len(samples), 0.0
    for i, s in enumerate(samples):
        y = alpha * s + (1.0 - alpha) * y
        out[i] = int(y)
    return out


def band_rms(samples, rate, lo, hi):
    if hi < rate / 2:
        hi_pass = iir_lp(samples, rate, hi)
    else:
        hi_pass = list(samples)
    if lo > 0:
        lo_pass = iir_lp(samples, rate, lo)
        band    = [hi_pass[i] - lo_pass[i] for i in range(len(samples))]
    else:
        band = hi_pass
    return rms(band)


def split_silence_speech(samples, rate, threshold_pct=1.0, head_seconds=0.2):
    """Return (silence_region, speech_region). Silence is the longest
    leading run where every 100-ms window stays below threshold_pct of the
    file peak. Speech is everything from the first non-silent window to
    the last."""
    pk = peak(samples)
    if pk == 0:
        return samples, []
    thr = int(pk * threshold_pct / 100)
    win = rate // 10                                # 100 ms
    if win < 1:
        win = 1

    def active(i):
        end = min(len(samples), i + win)
        return any(abs(samples[k]) > thr for k in range(i, end))

    n = len(samples)
    head_end = 0
    for i in range(0, n, win):
        if active(i):
            head_end = i
            break
    else:
        head_end = n

    tail_start = n
    for i in range(n - win, head_end, -win):
        if active(i):
            tail_start = min(n, i + win)
            break

    silence = list(samples[:head_end])
    speech  = list(samples[head_end:tail_start])
    if head_seconds > 0:
        keep = min(len(silence), int(rate * head_seconds))
        silence = silence[-keep:] if keep else []
    return silence, speech


def db(ratio):
    if ratio <= 0:
        return float('-inf')
    return 20 * math.log10(ratio)


def fmt_db(x):
    if x == float('-inf'):
        return '   -inf'
    if x == float('inf'):
        return '   +inf'
    return f'{x:+7.2f}'


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument('input',     help='WAV to analyse (e.g. blackhole.wav)')
    ap.add_argument('--reference',
                    help='optional reference WAV; report per-band ratios over '
                         'bands within its Nyquist')
    ap.add_argument('--no-normalise', action='store_true',
                    help='disable peak normalisation (default: scale to peak 32767)')
    args = ap.parse_args()

    samples, rate = read_wav(args.input)
    silence, speech = split_silence_speech(samples, rate)
    print(f'{args.input}')
    print(f'  {len(samples)}f @ {rate} Hz  ({len(samples)/rate:.2f} s total)')
    print(f'  pre-speech silence: {len(silence)} samples '
          f'({len(silence)/rate*1000:.0f} ms)')
    print(f'  active region:      {len(speech)} samples '
          f'({len(speech)/rate:.2f} s)')

    # Noise floor (per-band RMS of the silence region)
    print(f'\n  noise floor (per-band RMS of pre-speech silence):')
    for lo, hi in BANDS:
        if hi > rate / 2:
            continue
        nf = band_rms(silence, rate, lo, hi) if silence else 0.0
        print(f'    {lo/1000:5.1f}-{hi/1000:5.1f}kHz  {nf:8.2f}')

    # Speech-region per-band RMS, normalised so amplitude doesn't dominate
    if speech:
        pk_sp = peak(speech)
        if pk_sp > 0 and not args.no_normalise:
            scale = 32767 / pk_sp
            clip  = lambda x: 32767 if x > 32767 else (-32768 if x < -32768 else x)
            speech_n = [clip(int(round(v * scale))) for v in speech]
            print(f'\n  active region (peak-normalised, ×{scale:.2f}):')
        else:
            speech_n = speech
            print(f'\n  active region (raw):')
        for lo, hi in BANDS:
            if hi > rate / 2:
                continue
            print(f'    {lo/1000:5.1f}-{hi/1000:5.1f}kHz  {band_rms(speech_n, rate, lo, hi):8.2f}')

    # Reference comparison: only bands within the reference's Nyquist
    if args.reference and speech:
        ref_samples, ref_rate = read_wav(args.reference)
        _, ref_speech = split_silence_speech(ref_samples, ref_rate)
        if ref_speech:
            pk_r = peak(ref_speech)
            if pk_r > 0 and not args.no_normalise:
                scale_r = 32767 / pk_r
                clip = lambda x: 32767 if x > 32767 else (-32768 if x < -32768 else x)
                ref_n = [clip(int(round(v * scale_r))) for v in ref_speech]
            else:
                ref_n = ref_speech

            print(f'\n  vs reference: {args.reference} ({ref_rate} Hz, '
                  f'Nyquist {ref_rate/2/1000:.1f} kHz)')
            print(f'    {"band":>13}  {"input":>8}  {"reference":>10}  {"ratio":>8}')
            for lo, hi in BANDS:
                if hi > rate / 2:
                    continue
                ib = band_rms(speech_n, rate, lo, hi)
                if hi <= ref_rate / 2:
                    rb  = band_rms(ref_n, ref_rate, lo, hi)
                    rat = db(ib / rb) if rb > 0 else float('inf')
                    print(f'    {lo/1000:5.1f}-{hi/1000:5.1f}kHz  '
                          f'{ib:8.1f}  {rb:10.1f}  {fmt_db(rat)} dB')
                else:
                    print(f'    {lo/1000:5.1f}-{hi/1000:5.1f}kHz  '
                          f'{ib:8.1f}  {"(above ref Nyquist)":>10}  {"":>10}')


if __name__ == '__main__':
    main()
