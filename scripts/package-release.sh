#!/usr/bin/env bash
# Linux counterpart of package-release.ps1: builds PersonaRM-<tag>-linux-x64.tar.gz + .sha256 from
# already-built binaries. Only program files and docs go in: no game data, BIOS or recompiled code
# (the player's PC builds that with personarm_native + libtcc.so). Used by .github/workflows/release.yml.
#
#   scripts/package-release.sh <tag> <bin dir> <launcher binary> [out dir]
#   bin dir: personarm_sdl, personarm_native, libSDL3.so.0, libtcc.so, TinyCC-*.txt (build/Release)
set -euo pipefail
[ $# -ge 3 ] || { echo "usage: $0 <tag> <bin dir> <launcher> [out]" >&2; exit 2; }
tag=$1 bin=$2 launcher=$3 out=${4:-dist}
here=$(cd "$(dirname "$0")" && pwd)
stage=$out/stage
rm -rf "$stage"
mkdir -p "$stage/docs" "$stage/licenses"
install -m 755 "$launcher" "$stage/PersonaRM"
install -m 755 "$bin/personarm_sdl" "$bin/personarm_native" "$stage/"
# libSDL3.so.0 is the soname personarm_sdl asks for (-L: the real file, not the symlink).
install -m 644 -T "$(readlink -f "$bin/libSDL3.so.0")" "$stage/libSDL3.so.0"
install -m 644 "$bin/libtcc.so" "$stage/"
install -m 644 "$bin/TinyCC-LICENSE.txt" "$bin/TinyCC-SOURCE.txt" "$stage/licenses/"
install -m 644 README.md LICENSE "$stage/"
install -m 644 docs/LAUNCHER.md docs/SETTINGS.md docs/NATIVE_CODE.md "$stage/docs/"
"$here/check-release-files.sh" --dir "$stage"
name=PersonaRM-$tag-linux-x64.tar.gz
rm -f "$out/$name"
# Flat archive (like the Windows zip): the launcher's updater swaps these paths in place.
(cd "$stage" && tar --owner=0 --group=0 --sort=name -czf "../$name" *)
(cd "$out" && sha256sum "$name" > "$name.sha256")
rm -rf "$stage"
cat "$out/$name.sha256"
