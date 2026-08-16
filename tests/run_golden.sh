#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Simon Dick
#
# Golden-file regression tests for the host build: run wyomingtest/saytest
# against tests/mock_wyoming_server.py (a canned, deterministic stand-in for
# a real Piper server -- content is fixed, so these test the client-side
# Wyoming framing and audio-sink code in wyoming.c/audio_host.c, not TTS
# quality) and byte-compare the output against tests/golden/*.
#
# Ports are chosen well away from Piper's default 10200 so this never talks
# to a real local Piper instance by accident.
#
# Usage: tests/run_golden.sh (run from the repo root, after `make host`)
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

WT="build/host/wyomingtest"
ST="build/host/saytest"
FIXTURE="tests/golden/fixture.pcm"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fail=0

run_case() {
    name=$1; bin=$2; port=$3; text=$4; out=$5; golden=$6

    python3 tests/mock_wyoming_server.py "$port" "$FIXTURE" >"$WORK/$name.srv.log" 2>&1 &
    srv=$!
    # Give the server a moment to bind before the client connects. Can't
    # probe-connect to check readiness instead: the server's listen()
    # backlog only holds one connection and accept()s whichever arrives
    # first, so a readiness probe would itself get accepted and starve
    # the real client's connection.
    sleep 0.3

    # run from a dir with no config/narrator.wyoming, so a real Piper host
    # in this repo's local config can never leak into the positional host
    # argument via read_config()'s fallback.
    (cd "$WORK" && "$ROOT/$bin" --text "$text" --out "$out" 127.0.0.1 "$port") \
        >"$WORK/$name.client.log" 2>&1 || true
    wait "$srv" 2>/dev/null || true

    if ! cmp -s "$WORK/$out" "$golden"; then
        echo "FAIL: $name: $WORK/$out differs from $golden"
        cat "$WORK/$name.client.log"
        fail=1
    else
        echo "ok: $name"
    fi
}

run_case wyomingtest "$WT" 48213 \
    "The quick brown fox jumps over the lazy dog." \
    out.pcm tests/golden/wyomingtest.pcm

run_case saytest "$ST" 48214 \
    "Hello from the Amiga. This is neural speech over Wyoming." \
    out.wav tests/golden/saytest.wav

exit $fail
