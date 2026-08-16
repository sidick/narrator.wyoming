#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Simon Dick
"""Minimal Wyoming TTS server for host-side golden tests.

Speaks just enough of the Wyoming wire format (see ../src/wyoming.h) to drive
wyomingtest/saytest end to end without a real Piper install: one
`audio-start`, the fixture PCM split across a few `audio-chunk` events (using
both the inline-fields and the separate-`data_length`-block header shapes
Piper itself uses, so both wyo_read_event() paths in wyoming.c get
exercised), then `audio-stop`. Content is fixed -- this is not a TTS
implementation, it is a deterministic stand-in so the client-side framing/
audio-pipeline code can be regression-tested without network variance or a
live model server.

Serves exactly one connection then exits, so a test can `&`-background it,
run the client, and not worry about cleanup beyond the one process.

Usage: mock_wyoming_server.py PORT FIXTURE.pcm [RATE] [WIDTH] [CHANNELS]
"""
import socket
import sys


def chunk_header(rate, width, channels, payload_len, inline):
    if inline:
        return (
            '{"type": "audio-chunk", "payload_length": %d, '
            '"rate": %d, "width": %d, "channels": %d}\n'
            % (payload_len, rate, width, channels)
        ).encode()
    # Piper-style: fields live in a separate JSON block announced by
    # data_length, not inline in the header -- see wyoming.c's data_length
    # branch in wyo_read_event().
    data = (
        '{"rate": %d, "width": %d, "channels": %d}' % (rate, width, channels)
    ).encode()
    header = (
        '{"type": "audio-chunk", "data_length": %d, "payload_length": %d}\n'
        % (len(data), payload_len)
    ).encode()
    return header + data


def main():
    port = int(sys.argv[1])
    fixture_path = sys.argv[2]
    rate = int(sys.argv[3]) if len(sys.argv) > 3 else 22050
    width = int(sys.argv[4]) if len(sys.argv) > 4 else 2
    channels = int(sys.argv[5]) if len(sys.argv) > 5 else 1

    with open(fixture_path, "rb") as f:
        pcm = f.read()

    # Split into three chunks of uneven size so a chunk boundary lands
    # mid-buffer relative to wyoming.c's 16KiB rbuf -- exercises buf_fill()
    # refilling mid-payload, not just mid-header.
    n = len(pcm)
    a, b = n // 5, n // 5 + n // 2
    chunks = [pcm[:a], pcm[a:b], pcm[b:]]

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(1)
    conn, _ = srv.accept()
    try:
        # One line request in, ignored -- content doesn't affect the fixed
        # canned response; this server tests framing, not text handling.
        buf = b""
        while b"\n" not in buf:
            got = conn.recv(4096)
            if not got:
                break
            buf += got

        conn.sendall(
            (
                '{"type": "audio-start", "data": {"rate": %d, "width": %d, '
                '"channels": %d}}\n' % (rate, width, channels)
            ).encode()
        )
        for i, c in enumerate(chunks):
            if not c:
                continue
            # Alternate header shapes across chunks.
            conn.sendall(chunk_header(rate, width, channels, len(c), inline=(i % 2 == 0)))
            conn.sendall(c)
        conn.sendall(b'{"type": "audio-stop", "data": {}}\n')
    finally:
        conn.close()
        srv.close()


if __name__ == "__main__":
    main()
