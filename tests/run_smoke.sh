#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Simon Dick
#
# On-target smoke test: boots the cross-compiled build/amiga/wyomingtest
# under Copperline (bundled AROS ROM, no licensed Kickstart needed) and
# checks it produces the same PCM as the host-side golden test
# (tests/golden/wyomingtest.pcm), against the same mock Wyoming server
# (tests/mock_wyoming_server.py) reached through Copperline's [hostsocket]
# board in `net = "host"` mode. See tests/copperline/machine.toml.
#
# Requires: `copperline` on PATH, build/amiga/wyomingtest already built
# (`make docker` or `make amiga`).
#
# Usage: tests/run_smoke.sh (run from the repo root)
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

SYS="tests/copperline/sys"
BIN="build/amiga/wyomingtest"
PORT=48215
BENCH_SECS=75   # AROS boot is ~40 emulated seconds; leave headroom for the
                # request/response round trip on top of that.

if ! command -v copperline >/dev/null 2>&1; then
    echo "run_smoke.sh: 'copperline' not found on PATH" >&2
    exit 1
fi
if [ ! -f "$BIN" ]; then
    echo "run_smoke.sh: $BIN not found -- build it first (make docker)" >&2
    exit 1
fi

cleanup() {
    # .uaem sidecars carry the UAE-family protection-bits/comment metadata
    # Copperline's writable [[filesys]] mount writes alongside new files.
    rm -f "$SYS/C/wyomingtest" "$SYS/C/wyomingtest.uaem" \
          "$SYS/wyomingtest.pcm" "$SYS/wyomingtest.pcm.uaem" \
          "$SYS/wyomingtest.log" "$SYS/wyomingtest.log.uaem"
    [ -n "${srv_pid:-}" ] && kill "$srv_pid" 2>/dev/null || true
}
trap cleanup EXIT

cp "$BIN" "$SYS/C/wyomingtest"
chmod +x "$SYS/C/wyomingtest"

python3 tests/mock_wyoming_server.py "$PORT" tests/golden/fixture.pcm \
    >/tmp/copperline-smoke-srv.log 2>&1 &
srv_pid=$!

echo "booting under Copperline (benchmark-until ${BENCH_SECS}s emulated)..."
timeout 300 copperline --config tests/copperline/machine.toml \
    --noaudio --benchmark-until "$BENCH_SECS"

wait "$srv_pid" 2>/dev/null || true
srv_pid=

if [ -f "$SYS/wyomingtest.log" ]; then
    echo "--- wyomingtest.log ---"
    cat "$SYS/wyomingtest.log"
    echo "-----------------------"
fi

if [ ! -f "$SYS/wyomingtest.pcm" ]; then
    echo "FAIL: $SYS/wyomingtest.pcm was never written (device/network failure -- see log above)" >&2
    exit 1
fi

if cmp -s "$SYS/wyomingtest.pcm" tests/golden/wyomingtest.pcm; then
    echo "ok: on-target wyomingtest PCM output matches tests/golden/wyomingtest.pcm"
else
    echo "FAIL: $SYS/wyomingtest.pcm differs from tests/golden/wyomingtest.pcm" >&2
    exit 1
fi
