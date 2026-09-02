#!/bin/bash
# Sync our forked clockface sources between the tracked snapshot and the copy
# PlatformIO actually builds.
#
# Why this exists: firmware/clockfaces/* are upstream git submodules (jnthas), so
# our edits cannot be committed there, and firmware/lib/cw-cf-* is gitignored
# (it's the LDF symlink/copy target the build uses). That left our only modified
# clockface living in exactly one untracked place on one laptop. On 2026-09-02
# the tracked snapshot had already drifted out of sync with the built source
# (clockwise#11). This directory is the tracked home; lib/ is the build copy.
#
#   ./sync.sh push   # tracked snapshot -> lib/  (after a pull, before a build)
#   ./sync.sh pull   # lib/ -> tracked snapshot  (after editing, before commit)
#   ./sync.sh check  # exit 1 if they differ
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
LIB="$HERE/../lib"
rc=0
for cf in "$HERE"/cw-cf-*; do
  name=$(basename "$cf")
  for f in "$cf"/*; do
    b=$(basename "$f")
    case "${1:-check}" in
      push)  cp "$f" "$LIB/$name/$b"; echo "push $name/$b" ;;
      pull)  cp "$LIB/$name/$b" "$f"; echo "pull $name/$b" ;;
      check) diff -q "$f" "$LIB/$name/$b" >/dev/null 2>&1 || { echo "DRIFT: $name/$b"; rc=1; } ;;
      *)     echo "usage: $0 {push|pull|check}" >&2; exit 2 ;;
    esac
  done
done
[ "${1:-check}" = check ] && [ $rc -eq 0 ] && echo "clockface sources in sync"
exit $rc
