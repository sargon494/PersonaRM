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
