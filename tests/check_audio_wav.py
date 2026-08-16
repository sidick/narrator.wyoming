#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Simon Dick
"""Check a Copperline --audio-wav capture for non-silent audio content.

Copperline's --audio-wav writes 32-bit float, stereo, 44.1kHz WAV -- as
WAVE_FORMAT_EXTENSIBLE (format tag 0xFFFE) with an IEEE-float SubFormat
GUID, not the plain WAVE_FORMAT_IEEE_FLOAT (tag 3) fmt chunk you might
expect. Either way, Python's stdlib `wave` module only understands integer
PCM (format 1) and raises on both, so this parses the RIFF/fmt/data chunks
directly instead.

This is a content check, not a perceptual one: it just confirms the
capture is long enough and loud enough somewhere to rule out "AHI opened
but nothing actually played" -- not that the speech is correct or
intelligible.

Usage: check_audio_wav.py WAV_FILE MIN_DURATION_SECONDS [MIN_PEAK]
  MIN_DURATION_SECONDS  fail if the capture is shorter than this
  MIN_PEAK              fail if the loudest sample's absolute value never
                         reaches this (default 0.01 -- catches silence/
                         near-silence, not a loudness/quality bar)
"""
import array
import struct
import sys

WAVE_FORMAT_IEEE_FLOAT = 3
WAVE_FORMAT_EXTENSIBLE = 0xFFFE
# First 4 bytes of the KSDATAFORMAT_SUBTYPE_IEEE_FLOAT GUID
# (00000003-0000-0010-8000-00AA00389B71), little-endian.
SUBFORMAT_IEEE_FLOAT_PREFIX = struct.pack("<I", 3)


def read_wav_float(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("not a RIFF/WAVE file")

    pos = 12
    fmt_body = None
    pcm = None
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        csize = struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + csize]
        if cid == b"fmt ":
            fmt_body = body
        elif cid == b"data":
            pcm = body
        pos += 8 + csize + (csize & 1)  # chunks are word-aligned

    if fmt_body is None or pcm is None:
        raise ValueError("missing fmt or data chunk")

    audio_format, channels, rate, _byte_rate, _block_align, bits = \
        struct.unpack("<HHIIHH", fmt_body[:16])

    is_float = audio_format == WAVE_FORMAT_IEEE_FLOAT
    if audio_format == WAVE_FORMAT_EXTENSIBLE and len(fmt_body) >= 40:
        # cbSize(2) validBitsPerSample(2) channelMask(4) SubFormat(16) after
        # the first 16 bytes -- see the WAVEFORMATEXTENSIBLE layout.
        subformat = fmt_body[24:40]
        is_float = subformat[:4] == SUBFORMAT_IEEE_FLOAT_PREFIX

    return is_float, channels, rate, bits, pcm


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)

    path = sys.argv[1]
    min_duration = float(sys.argv[2])
    min_peak = float(sys.argv[3]) if len(sys.argv) > 3 else 0.01

    is_float, channels, rate, bits, pcm = read_wav_float(path)
    if not is_float or bits != 32:
        print("FAIL: %s: expected 32-bit IEEE-float WAV, got float=%s bits=%d"
              % (path, is_float, bits))
        sys.exit(1)
    if channels < 1 or rate < 1:
        print("FAIL: %s: bad fmt chunk (channels=%d rate=%d)" % (path, channels, rate))
        sys.exit(1)

    samples = array.array("f")
    usable = len(pcm) - (len(pcm) % 4)
    samples.frombytes(pcm[:usable])
    if sys.byteorder != "little":
        samples.byteswap()  # WAV is always little-endian

    n_frames = len(samples) // channels
    duration = n_frames / rate
    peak = max((abs(s) for s in samples), default=0.0)

    print("%s: %.3fs, %dch, %dHz, peak=%.4f" % (path, duration, channels, rate, peak))

    ok = duration >= min_duration and peak >= min_peak
    if not ok:
        print("  (want duration >= %.3fs and peak >= %.4f)" % (min_duration, min_peak))
    print("%s: audio_content (%s)" % ("PASS" if ok else "FAIL", path))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
