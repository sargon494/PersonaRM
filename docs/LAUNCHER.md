# Launcher

`launcher/` is a small Rust (eframe/egui) program: a game library that edits the game settings and starts `personarm_sdl`.
Dark theme, resizable window (minimum 480x520), no repaint loop while idle (idle CPU is 0).

## Pages

- **Library** (home): a grid of 250x348 px cards (it reflows with the window width), one per entry of `games`, with
  a summary next to the heading ("1 game · 1 ready"). A card shows the title, a platform badge (`PS1`) and a status
  with an icon (shape, not only colour): check **Ready**, ring **Set up** (no valid disc yet), cross **Game exe
  missing**. Ready cards have a **PLAY** button that launches directly; **Set up** opens the game page. With a cover the
  card shows it; without one it is the red-bar title card. No box art is shipped.
- **Game page**: a one-line explanation (pick the `.cue` of your own disc image or let **Find my discs** do it, Japanese and/or US release; the
  Language option chooses which disc boots), the discs, language, text speed, cover picker and PLAY. Every path
  field shows its state, validated on each edit: check found, cross file missing, dash not set.
- **Discs, zero setup**: the launcher reads the disc itself (`launcher/src/disc.rs`), nobody has to understand `.cue`
  files. It reads the data track of a `.cue`, `.bin` or `.iso` (ISO 9660 volume, `SYSTEM.CNF`, `BOOT` line: a few
  sectors, never a whole-disc hash) and shows the serial and region: `SLPS-02785` Japan disc 1, `SLUS-01158` US.
  `SLPS-02786` (Japan disc 2, the Sumaru TV promo) is labelled and never put in the game slot.
  **Find my discs** (game page) scans Downloads, Documents and Desktop (and the same under OneDrive) four folders
  deep, plus any folder added with **Search a folder...** (kept in `scan_dirs`), on a background thread (20 s cap;
  Downloads with several discs takes well under a second), lists "Persona 2: Eternal Punishment - US (SLUS-01158)"
  and fills the empty (or missing) Japanese / English fields. The same search runs once automatically on the
  first launch when no disc is set (`scanned` in `launcher.json`). Browsing to a file shows the verdict where
  "found" used to be: "US release (SLUS-01158)", "Not Persona 2 (found SLUS-00xxx)", "Disc image incomplete
  (missing X.bin)" (also: file not a whole number of sectors, fewer sectors than the volume or the cue say).
  Results are cached by path in `disc_cache` of `launcher.json`, valid while the size and mtime of the cue and its
  files are unchanged. Hidden `--find` forces the search (used for the screenshot).
- **Settings** (gear + "Settings" in the header): shared options in two columns (one when narrow), each technical
  option with a one-line hint (renderer, resolution, frame rate; widescreen is tagged EXPERIMENTAL), plus
  **Reduce motion**. All fit at the default window size; scroll bars are thin and always visible.

Keyboard: Tab / arrows move focus (red focus ring), Enter plays (or opens Set up) on a focused card, Esc goes back
from the game and settings pages. The header shows `Release vX.Y.Z` (`PRM_VERSION` or the crate version).

## Playtime

The windowless watcher (`PersonaRM --run`) that waits for the game records every session in `playtime.json`
(`launcher/src/playtime.rs`): start, duration, whether the game exited with status 0, and the serial of the disc
that was launched (read from the disc, so Japan and US are told apart). The game is the library entry whose
`serials` contain it. Per game: total time, sessions, longest, first/last played, the same per disc serial, and the
last 50 sessions. Sessions under 10 s (crash on start, quick quit) add no time and are only counted as "quick quits";
a crash or non-zero exit still records its session. The file is written atomically (temp file + rename).
A missing file (old launcher) is empty; an unreadable one is kept as `playtime.json.corrupt` and the stats restart.
It is **wall time while the game process is open**: time paused by "Pause when unfocused" still counts (the host has
no channel to the launcher; adding one was not worth it). If the watcher itself is killed, that session is lost.

UI: each library card has a muted line ("12 h 30 min · last played yesterday", or "Not played yet"). The game page
ends with a **Playtime** section (totals, average, per-version split, recent sessions with relative dates, **Show all**,
and **Reset playtime...** which asks for confirmation). Relative dates use the local time zone (today, yesterday,
N days ago, then the date after 30 days). Nothing repaints while idle.

## Motion

Snappy, 70-180 ms, ease-out, drawn with egui's `animate_bool_with_time_and_easing` and a time tween: page change
(28 px slide + fade), card hover/focus (lift, red bar grows), PLAY hover/press, update banner slide-in. Repaints are
requested only while something moves, so idle CPU stays 0. `"reduce_motion": true` in `launcher.json` (or the
Settings checkbox) makes every transition instant.

Hidden arguments for screenshots: `--page game|settings`, `--focus` (focus the first card) and `--shot <png>`
(saves the window's own pixels and quits).

## Adding a game

Only a data change in `launcher.json` (`games`); all fields except `name` and `serials` are optional:

```json
{ "name": "...", "platform": "PS1", "serials": ["SLPS-00000"], "discs": {"ja": "D:/discs/p2.cue"},
  "cover": "D:/covers/p2.png", "exe": "path/to/runner", "args": ["--extra"] }
```

`exe` empty means the shared `personarm_sdl`; `args` are appended after `<cue> --settings ... --renderer ...`.
Old files without `platform`, `cover`, `exe` or `args` still load (`platform` defaults to `PS1`). Only
Persona 2: Eternal Punishment ships in the default list.

## Build

```
cd launcher
cargo build --release        # launcher/target/release/PersonaRM(.exe)
cargo test                   # settings merge and command-line tests
```

Put `launcher` next to `personarm_sdl`, or use the CMake target (needs `cargo`, `-DPRM_WITH_SDL3=ON`):

```
cmake --build build --config Release --target launcher
```

If `personarm_sdl` is not next to the launcher, pick it under Settings > **Game executable**.

## Files

| file | content |
|---|---|
| `%APPDATA%\PersonaRM\launcher.json` | launcher config: game library (name, platform, serials, disc `.cue` per language, cover, exe, args), `personarm_sdl` path, renderer, internal resolution |
| `%APPDATA%\PersonaRM\playtime.json` | playtime per game (see Playtime); separate from `launcher.json`; a damaged file is moved to `playtime.json.corrupt` and the stats restart |
| `%APPDATA%\PersonaRM\PersonaRM\settings.json` | the game's settings ([SETTINGS.md](SETTINGS.md)); the launcher changes only its keys, every other key is preserved |
| `%APPDATA%\PersonaRM\logs\personarm_sdl.log` | stderr of the last game run |

Linux uses `$XDG_DATA_HOME` (default `~/.local/share`) instead of `%APPDATA%`. Disc paths are set by the user
in the UI; nothing is hard-coded.

## Play

The language picks the disc (Japanese or English). Play writes the settings, then runs
`personarm_sdl <cue> --settings <settings.json> --renderer <r> [--resolution-scale <n|auto>]`. Renderer and
resolutions above 4x (or `auto`) exist only on the command line. `--resolution-scale` is omitted above 60 FPS
(the host rejects it with frame interpolation).

While the game runs the launcher window is hidden (no taskbar button, not in Alt-Tab, never takes focus) and
returns when the game exits. A non-zero exit shows the last stderr lines.

## Look

The window uses the colours and fonts of the project site (`site/styles.css`): ink black `#07070b`, paper
`#ece8e1`, muted `#9b98a3`, red `#d7172a` / `#ff3347`, square corners, the logo in the header and as the window
icon. Fonts are Barlow and Barlow Condensed (SIL OFL 1.1, subset to Latin, 21 KB each; licence in
`launcher/assets/OFL-Barlow.txt`). The site's Bodoni Moda display face is not embedded (about 300 KB). The
logo is a 256 px copy of `site/assets/logo.png`.

## Updates

On start (setting **Check for updates on start**, default on, stored in `launcher.json` as `check_updates`) a
background thread makes one request to `https://api.github.com/repos/sargon494/PersonaRM/releases/latest`
(User-Agent `PersonaRM-launcher`, 5 s timeout, silent when offline). If the release tag is newer than the
launcher's version, a banner "Update available vA to vB" with an **Update** button appears. The UI never
polls: nothing runs until the request returns or you click.

**Update** downloads `PersonaRM-<tag>-windows-x64.zip` (Linux: `PersonaRM-<tag>-linux-x64.tar.gz`) and its `.sha256`, only from `https://github.com` or
GitHub's release CDN (`objects.githubusercontent.com`, `release-assets.githubusercontent.com`), using the URLs
from the API answer; every redirect hop is checked against the same list. The zip is refused unless its SHA-256
matches the `.sha256` file. It is extracted to `.prm-update/` next to the launcher (entries that would leave
that folder are refused, and the zip must contain the launcher itself), then each file is swapped in: the old
file is renamed to `<name>.old` (Windows lets a running exe be renamed, not overwritten), the new one is moved
in, and if any step fails everything is rolled back. The launcher then restarts itself; on the next start the
`.old` files are deleted (the list is in `.prm-update-old.txt`, so no other file is ever deleted). Only files
that are inside the zip are replaced: `launcher.json`, settings, saves and memory cards are never touched.
On Linux the `.tar.gz` is unpacked the same way (plain files and folders only, links refused) and keeps the
executable bits. The check is skipped when the release has no archive for the platform.

Test it without GitHub (debug builds only; release builds ignore these variables):
`PRM_UPDATE_API=http://127.0.0.1:<port>/release.json` points the check at a local server whose JSON uses
`http://127.0.0.1:<port>/...` download URLs, and `PRM_UPDATE_AUTO=1` applies the update without the click.

## Releasing

1. Make sure `main` is green and the version you want is not already released.
2. Tag and push: `git tag v0.2.0 && git push origin v0.2.0` (tags must look like `vX.Y.Z`).
3. `.github/workflows/release.yml` builds on Windows and on Ubuntu 22.04 (`personarm_sdl` with SDL3,
   `personarm_native` with TinyCC, the launcher with the tag baked in as its version), runs the launcher tests,
   packages with `scripts/package-release.ps1` / `scripts/package-release.sh` and creates the GitHub Release with
   both archives and their `.sha256`. A manual run (Actions, Release, Run workflow, with a
   tag name) creates a **draft** release instead: review it and publish it by hand.

The zip `PersonaRM-vX.Y.Z-windows-x64.zip` contains `PersonaRM.exe` (and `launcher.exe`, see below), `personarm_sdl.exe`, `SDL3.dll`,
`personarm_native.exe`, `libtcc.dll` (TinyCC, LGPL; licence and source offer in `licenses/`),
`README.md`, `LICENSE`, `docs/LAUNCHER.md`, `docs/SETTINGS.md` and `docs/NATIVE_CODE.md`. It never contains game data,
BIOS files or recompiled game code (the release build does not set `PRM_RECOMP_GENERATED_DIR`; `scripts/check-release-files.ps1`
fails the release on any file outside that list).

The Linux archive `PersonaRM-vX.Y.Z-linux-x64.tar.gz` (flat, no top folder: unpack it into a folder of its
own) contains `PersonaRM`, `personarm_sdl`, `personarm_native`, `libSDL3.so.0`, `libtcc.so`, the same docs and
`licenses/`, and is checked by `scripts/check-release-files.sh`. The programs find the two libraries next to
themselves (RUNPATH `$ORIGIN`). Built on Ubuntu 22.04, so it needs glibc 2.35 or newer. Only a
published (non-draft, non-pre-release) release counts as "latest" for the updater.

### Rename: `launcher.exe` -> `PersonaRM.exe`

Installs from v0.1.x-v0.2.2 run `launcher.exe`, and their updater refuses a zip that lacks that exact name. So the
zip ships the same binary twice, as `PersonaRM.exe` and `launcher.exe`, and the update swaps both in. When the new
binary is started as `launcher.exe` and `PersonaRM.exe` is next to it, it starts `PersonaRM.exe` with the same
arguments and exits, so users land on the new name (old shortcuts keep working). The game watcher (`--run`) and the
restart after an update use `PersonaRM.exe` whenever it exists.

The zip still carries `launcher.exe` so that installs from v0.2.2 and earlier can update. On start (Windows) `PersonaRM.exe` points
Desktop, Public Desktop (if writable) and Start Menu shortcuts that target that `launcher.exe` at itself, then deletes it (retried on the next start if it is in use).

`launcher.exe` can be dropped from the zip (and from `scripts/check-release-files.ps1`) once nobody is still on a
pre-rename version: ship at least two releases with both names, so every install gets one update that carries
`PersonaRM.exe` and runs under it from then on, and drop it in a later one. An old install that skips straight over
that window can no longer self-update and must download the zip by hand.
