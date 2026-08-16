# nondist/

Local scratch space for files this repo cannot redistribute: Kickstart ROM
images, a real AHI driver (`ahi.device` + `Devs/AHI/paula.audio`), a licensed
Workbench install, or anything else under a license that doesn't permit
committing it here.

Everything under this directory except this README and `.gitkeep` is
gitignored (see `.gitignore`'s `/nondist/*` rule) -- drop licensed content in
here and it stays local, never staged, never pushed. If you need one of
these for local testing (e.g. a real Kickstart for
`config/copperline-narrator.toml`'s `rom =`, or AHI for a full Say-acceptance
boot volume), put it here rather than loosely in the repo root where it's
easy to `git add -A` by accident.
