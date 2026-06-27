#!/bin/sh
# capture.sh — deterministic AHI audio capture under Copperline.
#
# Builds pcmplay (cross-compiler container), assembles a throwaway bootable
# volume from the Amiberry "narrator" install with pcmplay wired into its
# User-Startup, boots it headless, and dumps the mixed Paula output to a WAV.
# No network, no BlackHole rig, no listening by ear. The real install is never
# modified (rsynced into copperline/bootvol; guest writes are discarded too).
#
# Why a single copied volume and not a second mounted dir: KS3.1 scsi.device
# only probes the IDE master (an IDE slave never mounts), and a second SCSI
# host-dir volume did not reliably get the "Narrator" label the install's
# `Execute Narrator:boot` hook needs. Patching User-Startup in a throwaway
# copy sidesteps all of that.
#
# Usage:
#   copperline/capture.sh [fixture.pcm] [seconds]
#     fixture.pcm  raw s16le/22050/mono PCM to play (default: generate a sine)
#     seconds      emulated run length (default 16: boot + ~2s play + drain)
#
# Swap in real speech:  wyomingtest --out /tmp/speech.pcm <host>
#                       copperline/capture.sh /tmp/speech.pcm
set -e

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/.." && pwd)
install="/Users/simond/Documents/Amiberry/HardDrives/narrator"
bootvol="$here/bootvol"
secs="${2:-16}"

# 1. Build pcmplay in the cross-compiler container.
echo "[capture] building pcmplay..."
docker run --rm -v "$repo":/work -w /work \
  stefanreinauer/amiga-gcc:latest make -s build/amiga/pcmplay >/dev/null

# 2. Fixture: explicit arg, or a generated sine if none present. (Guard the
#    self-copy when the arg already IS copperline/fixture.pcm.)
if [ -n "$1" ]; then
  [ "$(cd "$(dirname "$1")" && pwd)/$(basename "$1")" = "$here/fixture.pcm" ] \
    || cp "$1" "$here/fixture.pcm"
elif [ ! -f "$here/fixture.pcm" ]; then
  "$here/make-fixture.sh"
fi
echo "[capture] fixture: $(ls -l "$here/fixture.pcm" | awk '{print $5}') bytes"

# 3. Assemble the throwaway boot volume: rsync the install, patch its
#    User-Startup to run pcmplay, stage the binary + fixture + cfg.
echo "[capture] assembling bootvol (rsync $install)..."
rsync -a --delete "$install/" "$bootvol/"
python3 - "$bootvol/S/user-startup" <<'PY'
import sys, re
p = sys.argv[1]
s = open(p, encoding="latin-1").read()
repl = ("; --- narrator.wyoming Copperline audio-capture hook (throwaway copy) ---\n"
        "Stack 131072\n"
        "SYS:pcmplay\n")
if "SYS:pcmplay" not in s:
    s2 = re.sub(r"If EXISTS Narrator:boot.*?EndIf\s*", repl, s, flags=re.S | re.I)
    open(p, "w", encoding="latin-1").write(s2 if s2 != s else s + "\n" + repl)
PY
cp "$repo/build/amiga/pcmplay" "$bootvol/pcmplay"
cp "$here/fixture.pcm"         "$bootvol/fixture.pcm"
cp "$here/pcmplay.cfg"         "$bootvol/pcmplay.cfg"

# 4. Boot headless: capture WAV + a screenshot (the screenshot is the Guru
#    check — clean Workbench == no crash; it also bounds the run and exits).
out="$here/out.wav"; shot="$here/shot.png"
rm -f "$out" "$shot"
echo "[capture] running copperline headless (${secs}s emulated)..."
copperline --config "$here/copperline.toml" \
  --audio-wav "$out" --screenshot-after "$secs" "$shot" 2>&1 \
  | grep -iE 'guru|panic|error|screenshot saved' || true

# 5. Report. Silent capture (-inf/very low) => pcmplay didn't run or the AHI
#    mode/byte-swap is wrong; inspect shot.png.
echo "[capture] WAV:  $out"
echo "[capture] shot: $shot  (open to verify clean Workbench / no Guru)"
if [ -f "$out" ]; then
  ffmpeg -hide_banner -i "$out" -af volumedetect -f null - 2>&1 \
    | grep -iE 'max_volume|mean_volume' || true
fi
