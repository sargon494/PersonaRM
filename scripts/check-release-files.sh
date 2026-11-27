#!/usr/bin/env bash
# Linux counterpart of check-release-files.ps1: fails unless a release folder (or .tar.gz) holds
# exactly the expected program files: never recompiled game code (modules.so, generated .c,
# manifest.json from a native cache), game data or BIOS. For a folder it also checks that the
# programs find their libraries next to themselves (RUNPATH $ORIGIN).
#
#   scripts/check-release-files.sh --dir <folder> | --archive <file.tar.gz>
set -euo pipefail
expected='LICENSE
PersonaRM
README.md
docs/LAUNCHER.md
docs/NATIVE_CODE.md
docs/SETTINGS.md
libSDL3.so.0
libtcc.so
licenses/TinyCC-LICENSE.txt
licenses/TinyCC-SOURCE.txt
personarm_native
personarm_sdl'
case "${1:-}" in
    --dir) files=$(cd "$2" && find . -type f -o -type l | sed 's|^\./||' | LC_ALL=C sort) ;;
    --archive) files=$(tar -tzf "$2" | sed 's|^\./||' | grep -v '/$' | grep . | LC_ALL=C sort) ;;
    *) echo "usage: $0 --dir <folder> | --archive <file.tar.gz>" >&2; exit 2 ;;
esac
if [ "$files" != "$expected" ]; then
    echo "release files differ from the allowed list (no generated game code may ship):" >&2
    diff <(echo "$expected") <(echo "$files") >&2 || true
    exit 1
fi
if [ "$1" = --dir ]; then
    for f in personarm_sdl personarm_native; do
        readelf -d "$2/$f" | grep -q 'R.*PATH.*\$ORIGIN' || { echo "$f: no RUNPATH \$ORIGIN (would not find its libraries)" >&2; exit 1; }
    done
fi
echo "release files OK ($(echo "$files" | wc -l) files, no generated game code)"
