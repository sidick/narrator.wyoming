#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Simon Dick
#
# refresh.sh — pull the latest jens-maus/libcodesets release headers
# into third_party/codesets/include/, then patch the README's pinned
# revision block.
#
# Usage:
#   ./refresh.sh              # latest GitHub release
#   ./refresh.sh --version 6.22
#   ./refresh.sh --version master  # tip of master (unreleased)
#
# Needs: bash, curl, tar, jq.

set -euo pipefail

REPO="jens-maus/libcodesets"
VENDOR_DIR="$(cd "$(dirname "$0")" && pwd)"
INCLUDE_DIR="$VENDOR_DIR/include"
README="$VENDOR_DIR/README.md"

VERSION=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --version) VERSION="${2:?--version needs a tag}"; shift 2 ;;
    -h|--help)
      sed -n '/^# refresh.sh/,/^# Needs:/p' "$0" | sed 's/^# \{0,1\}//'
      exit 0 ;;
    *) echo "refresh.sh: unknown arg: $1" >&2; exit 2 ;;
  esac
done

need() { command -v "$1" >/dev/null 2>&1 || { echo "refresh.sh: need $1 in PATH" >&2; exit 1; }; }
for tool in curl tar jq; do need "$tool"; done

api() { curl -fsSL --max-time 30 "$@"; }

# --- Resolve the release we're refreshing to ----------------------------------
# Three modes:
#   --version master  -> tip of master (no release), source tarball from default
#                        branch (helpful when upstream has unreleased fixes).
#   --version <tag>   -> that named release.
#   (no arg)          -> latest GitHub release.

if [[ "$VERSION" == "master" ]]; then
  echo "resolving master..."
  ref=$(api "https://api.github.com/repos/$REPO/commits/master")
  TAG="master"
  TAG_SHA=$(jq -r '.sha' <<<"$ref")
  PUBLISHED_DATE=$(jq -r '.commit.committer.date' <<<"$ref" | cut -d'T' -f1)
  TARBALL="https://api.github.com/repos/$REPO/tarball/$TAG_SHA"
  TAG_URL="https://github.com/$REPO/tree/$TAG_SHA"
  COMMIT_URL="https://github.com/$REPO/commit/$TAG_SHA"
  REV_LINE_DESC="commit"
else
  if [[ -z "$VERSION" ]]; then
    echo "querying latest release..."
    meta=$(api "https://api.github.com/repos/$REPO/releases/latest")
  else
    echo "querying release $VERSION..."
    meta=$(api "https://api.github.com/repos/$REPO/releases/tags/$VERSION")
  fi
  TAG=$(jq -r '.tag_name'     <<<"$meta")
  PUBLISHED=$(jq -r '.published_at' <<<"$meta")
  PUBLISHED_DATE=${PUBLISHED%T*}

  # Tarball URL points at the tag, follows annotated-tag indirection if any.
  TARBALL="https://api.github.com/repos/$REPO/tarball/refs/tags/$TAG"

  # Resolve the underlying commit SHA so the README pins to a hash too.
  tagref=$(api "https://api.github.com/repos/$REPO/git/refs/tags/$TAG")
  ttype=$(jq -r '.object.type' <<<"$tagref")
  tsha=$( jq -r '.object.sha'  <<<"$tagref")
  if [[ "$ttype" == "tag" ]]; then
    tagobj=$(api "https://api.github.com/repos/$REPO/git/tags/$tsha")
    tsha=$(jq -r '.object.sha' <<<"$tagobj")
  fi
  TAG_SHA="$tsha"

  TAG_URL="https://github.com/$REPO/releases/tag/$TAG"
  COMMIT_URL="https://github.com/$REPO/commit/$TAG_SHA"
  REV_LINE_DESC="release"
fi

echo "  version       : $TAG"
echo "  commit        : $TAG_SHA"
echo "  published date: $PUBLISHED_DATE"

# --- Fetch + extract ----------------------------------------------------------
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "downloading tarball..."
curl -fsSL --max-time 90 -o "$TMP/source.tar.gz" "$TARBALL"
tar -xzf "$TMP/source.tar.gz" -C "$TMP"

# Tarball top-level dir looks like jens-maus-libcodesets-<sha>/
SRC=$(find "$TMP" -mindepth 1 -maxdepth 1 -type d ! -name '.' | head -n 1)
[[ -n "$SRC" ]] || { echo "refresh.sh: couldn't find extracted source dir" >&2; exit 1; }

for f in libraries/codesets.h clib/codesets_protos.h inline/codesets.h proto/codesets.h; do
  [[ -f "$SRC/include/$f" ]] || { echo "refresh.sh: tarball missing include/$f" >&2; exit 1; }
done

# --- Copy headers in place ----------------------------------------------------
echo "copying headers..."
mkdir -p "$INCLUDE_DIR/libraries" "$INCLUDE_DIR/clib" "$INCLUDE_DIR/inline" "$INCLUDE_DIR/proto"
cp "$SRC/include/libraries/codesets.h"        "$INCLUDE_DIR/libraries/codesets.h"
cp "$SRC/include/clib/codesets_protos.h"      "$INCLUDE_DIR/clib/codesets_protos.h"
cp "$SRC/include/inline/codesets.h"           "$INCLUDE_DIR/inline/codesets.h"
cp "$SRC/include/proto/codesets.h"            "$INCLUDE_DIR/proto/codesets.h"

# --- Patch the README's managed revision block --------------------------------
# Stage the new body in a tempfile so awk can splice it as multi-line content;
# passing it via -v body=... loses newlines on classic awk implementations.
echo "updating README..."
body_file=$(mktemp)
if [[ "$REV_LINE_DESC" == "release" ]]; then
  cat > "$body_file" <<EOF
Vendored from upstream release [\`$TAG\`]($TAG_URL) (published $PUBLISHED_DATE,
commit [\`$TAG_SHA\`]($COMMIT_URL)).
EOF
else
  cat > "$body_file" <<EOF
Vendored from upstream master at commit [\`$TAG_SHA\`]($COMMIT_URL)
($PUBLISHED_DATE).
EOF
fi

OPEN_MARK="<!-- BEGIN: source-revision (managed by refresh.sh) -->"
CLOSE_MARK="<!-- END: source-revision -->"
tmp_readme=$(mktemp)
# `open` and `close` are awk builtins -- name the markers something else.
awk -v om="$OPEN_MARK" -v cm="$CLOSE_MARK" -v body_file="$body_file" '
  BEGIN { keep = 1 }
  $0 == om {
    print om
    while ((getline line < body_file) > 0) print line
    close(body_file)
    print cm
    keep = 0
    next
  }
  $0 == cm { keep = 1; next }
  keep     { print }
' "$README" > "$tmp_readme"
mv "$tmp_readme" "$README"
rm -f "$body_file"

echo
echo "done. inspect the change with:"
echo "  git diff --stat third_party/codesets"
echo "  git diff third_party/codesets/README.md"
echo
echo "then rebuild + run on-target devtest:"
echo "  make docker"
echo "  # (boot Amiberry with the devtest variant of boot)"
