# Launcher

`launcher/` is a small Rust (eframe/egui) program that edits the game settings and starts `personarm_sdl`.
One window, dark theme, no repaint loop while idle (idle CPU is 0).

## Build

```
cd launcher
cargo build --release        # launcher/target/release/launcher(.exe)
cargo test                   # settings merge and command-line tests
```

Put `launcher` next to `personarm_sdl`, or use the CMake target (needs `cargo`, `-DPRM_WITH_SDL3=ON`):

```
cmake --build build --config Release --target launcher
```

If `personarm_sdl` is not next to the launcher, pick it under **Game executable**.

## Files

| file | content |
|---|---|
| `%APPDATA%\PersonaRM\launcher.json` | launcher config: game library (name, serials, disc `.cue` per language), `personarm_sdl` path, renderer, internal resolution |
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

**Update** downloads `PersonaRM-<tag>-windows-x64.zip` and its `.sha256`, only from `https://github.com` or
GitHub's release CDN (`objects.githubusercontent.com`, `release-assets.githubusercontent.com`), using the URLs
from the API answer; every redirect hop is checked against the same list. The zip is refused unless its SHA-256
matches the `.sha256` file. It is extracted to `.prm-update/` next to the launcher (entries that would leave
that folder are refused, and the zip must contain the launcher itself), then each file is swapped in: the old
file is renamed to `<name>.old` (Windows lets a running exe be renamed, not overwritten), the new one is moved
in, and if any step fails everything is rolled back. The launcher then restarts itself; on the next start the
`.old` files are deleted (the list is in `.prm-update-old.txt`, so no other file is ever deleted). Only files
that are inside the zip are replaced: `launcher.json`, settings, saves and memory cards are never touched.
Self-update is Windows only; the check is skipped when the zip asset of the platform does not exist.

Test it without GitHub (debug builds only; release builds ignore these variables):
`PRM_UPDATE_API=http://127.0.0.1:<port>/release.json` points the check at a local server whose JSON uses
`http://127.0.0.1:<port>/...` download URLs, and `PRM_UPDATE_AUTO=1` applies the update without the click.

## Releasing

1. Make sure `main` is green and the version you want is not already released.
2. Tag and push: `git tag v0.2.0 && git push origin v0.2.0` (tags must look like `vX.Y.Z`).
3. `.github/workflows/release.yml` builds on Windows (`personarm`, `personarm_sdl` with SDL3, the launcher with
   the tag baked in as its version), runs the launcher tests, packages with `scripts/package-release.ps1` and
   creates the GitHub Release with the zip and its `.sha256`. A manual run (Actions, Release, Run workflow, with a
   tag name) creates a **draft** release instead: review it and publish it by hand.

The zip `PersonaRM-vX.Y.Z-windows-x64.zip` contains `launcher.exe`, `personarm_sdl.exe`, `SDL3.dll`,
`personarm.exe`, `README.md`, `LICENSE`, `docs/LAUNCHER.md` and `docs/SETTINGS.md`. It never contains game data,
BIOS files or recompiled game code (the release build does not set `PRM_RECOMP_GENERATED_DIR`). Only a
published (non-draft, non-pre-release) release counts as "latest" for the updater.
