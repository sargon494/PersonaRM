# Settings file

`personarm_sdl` reads one JSON settings file. The format is game-agnostic so a launcher (any language)
can read and write the same file. Implementation: `src/host/settings.{h,cpp}`.

## Location and precedence

- Default: `<per-user config dir>/PersonaRM/settings.json` (`SDL_GetPrefPath("PersonaRM", "PersonaRM")`:
  `%APPDATA%\PersonaRM\PersonaRM\` on Windows, `~/.local/share/PersonaRM/PersonaRM/` on Linux,
  `~/Library/Application Support/PersonaRM/PersonaRM/` on macOS).
- `--settings <path>` overrides the location. A missing file is not an error (defaults).
- Precedence: **defaults < settings file < command line**. `--save-settings` writes the effective result
  (file + command line, defaults filled in) back to the file.
- Writing is atomic (`<file>.tmp` then rename). **Unknown keys are preserved** at every level, so a
  launcher can keep its own sections (e.g. `"launcher": {...}`) and the host will not drop them. A launcher
  must do the same: read, change only its keys, write the whole document back.
- A bad file never stops the game: a malformed document or an invalid value logs `[settings] ...` to stderr
  and that value falls back to its default.

## Schema (version 1)

```json
{
  "version": 1,
  "video": {
    "window_mode": "windowed",
    "window_scale": 3,
    "integer_scaling": false,
    "aspect": "4:3",
    "vsync": false,
    "fps_target": 60,
    "renderer": "software",
    "resolution_scale": 1,
    "fast_forward_max": 4,
    "widescreen": "off"
  },
  "audio": { "master_volume": 100, "mute": false },
  "general": { "pause_on_focus_loss": true, "language": "ja", "save_dir": "" },
  "input": {
    "keyboard": { "cross": ["X"], "start": ["Return"], "select": ["Backspace", "Right Shift"] },
    "gamepad":  { "cross": ["a"], "l2": ["lefttrigger"] }
  },
  "game": {
    "SLPS-02192": {
      "memcard1": "C:/saves/p2ep-1.mcd", "memcard2": "",
      "text_speed": "fast", "mods": "C:/mods", "recomp": false
    }
  }
}
```

`version` is an integer. A file with a higher version than the host knows is still read (known keys
as-is, unknown ones preserved) with a warning.

### video

| key | values | default | status |
|---|---|---|---|
| `window_mode` | `windowed`, `fullscreen` (exclusive, desktop resolution), `borderless` (desktop-sized) | `windowed` | honoured; Alt+Enter toggles windowed and the configured mode (borderless if the configured one is windowed) |
| `window_scale` | integer 1..8; window = 320x240 x scale | 3 (960x720) | honoured |
| `integer_scaling` | bool; largest whole multiple of the picture that fits, else plain fit | false | honoured |
| `aspect` | `4:3`, `stretch` (fill the window) | `4:3` | honoured |
| `vsync` | bool | false | honoured; ignored while interpolation or `--unthrottled` is on (it would slow the game down) |
| `fps_target` | `30`, `60`, `90`, `120` | 60 | the pictures presented per second; the game always runs at 60 Hz (game speed and audio never change). `60` = native; `30` = every other frame is presented (low-end PCs, battery; the skipped frame's host conversion is skipped too); `90` = frame interpolation, 3 pictures per 2 game frames (interpolation phases 0, 2/3, 1/3); `120` = interpolation, 2 per frame. Same as `--interp N` (`--interp 0` = 60). Interpolation is display only and one game frame late |
| `renderer` | `software`, `gpu` | `software` | same as `--renderer`. `gpu` draws the scaled picture on the host GPU (SDL_gpu; D3D12 on Windows), see [GPU_RENDERER.md](GPU_RENDERER.md); where it cannot start, the software renderer runs with a log line |
| `resolution_scale` | integer 1..9, or `"auto"` | 1 | internal resolution (display only); same as `--resolution-scale`. Software renderer: 1..4 (larger values and `auto` fall back to 1..4 with a warning) and 1 while `fps_target` > 60 (warning). `gpu`: 1..9, also with interpolation; `auto` = enough lines for the desktop height (ceil(height / 240): 1080p 5, 1440p 6, 2160p 9) |
| `fast_forward_max` | integer 2..16 | 4 | fast-forward speed cap (x real time), see [Save states and fast-forward](#save-states-and-fast-forward) |
| `widescreen` | `off`, or `W:H` at least 4:3 (`16:9`, `21:9`, ...) | `off` | honoured; same as `--widescreen`. Wider 3D view (GTE hack) shown at W:H; 2D-only and FMV frames stay 4:3; dialogue boxes, text and portraits over 3D keep 4:3 proportions. See [WIDESCREEN.md](WIDESCREEN.md) |

### audio

`master_volume` integer 0..100 (default 100), `mute` bool. Both honoured (stream gain).

### general

| key | values | default | status |
|---|---|---|---|
| `pause_on_focus_loss` | bool | true | honoured: when the game window loses focus, emulation pauses and audio goes silent; both resume on focus. The window is never minimized and never steals focus |
| `save_dir` | directory path | `""` | honoured: memory card directory. With a recognised disc and no explicit `memcard1`, slot 1 is `<save_dir>/<serial>-1.mcd` (created if missing). Slot 2 and explicit `game.<serial>.memcard1/2` are unaffected / win. Save states go to `<save_dir>/<serial>.state<N>` (settings directory when empty) |
| `language` | string | `"ja"` | **reserved**: stored only, no effect yet |

### input

`keyboard` and `gamepad` map an **action** to a list of key/button names; several names per action are
fine and an empty list unbinds it. Actions (PS1 digital pad): `up down left right cross circle square
triangle start select l1 r1 l2 r2`. Names use SDL spelling: keyboard `SDL_GetScancodeName`
(`"Return"`, `"Space"`, `"Right Shift"`, `"F"`), gamepad `SDL_GetGamepadStringForButton/Axis` (`"a"`, `"b"`,
`"x"`, `"y"` = south/east/west/north, `"dpup"`, `"start"`, `"back"`, `"leftshoulder"`, `"lefttrigger"`...).
An action present in the file replaces that action's defaults; absent actions keep them. Unknown actions
and unknown key names log a warning and are ignored. Defaults: see `KeyboardMap()` / `GamepadMap()` in
`src/host/host_logic.cpp`.

### game

One object per game, keyed by the disc serial the engine recognises (e.g. `SLPS-02192`). Unset keys
(or `""`) mean "not set". The section is chosen after the disc boots.

| key | meaning | CLI equivalent |
|---|---|---|
| `memcard1`, `memcard2` | raw `.mcd` path per slot | `--memcard1/2` |
| `text_speed` | `normal`, `fast`, `instant` | `--text-speed` |
| `mods` | mods directory | `--mods` |
| `recomp` | run the recompiled code linked into the build | `--recomp` |

## Save states and fast-forward

Keys in `personarm_sdl` (fixed for now, not in `input`):

| key | action |
|---|---|
| F5 | save the whole machine to the current slot |
| F8 | load the current slot |
| F6 / F7 | previous / next slot (0-9, starts at 0) |
| Tab (held), gamepad right-stick click (held) | fast-forward |

- **Files**: `<save_dir>/<serial>.state<N>` (e.g. `SLPS-02785.state0`); with an empty `save_dir`, the
  settings directory. Written atomically (`.tmp` then rename). The result shows on stderr (`[state] ...`)
  and in the window title for 3 s.
- **What a state holds**: CPU (registers, delay slots, COP0), GTE, RAM, scratchpad, GPU (VRAM, registers,
  packet and transfer state), DMA, interrupt controller, timers, CD-ROM (controller, position, pending
  responses, sector buffers), XA decoder, MDEC, SPU (sound RAM, voices, reverb, queued CD audio), the
  SIO0 pad/memory-card protocol position, the HLE kernel (open files, card handles, IRQ chain position;
  its tables live in RAM), lazy device time, gameplay-patch counters and which recompiled modules were
  live (re-checked against the loaded RAM by their code hash).
- **What it does not hold**: memory card images. A state never rewrites your `.mcd` saves: loading an
  old state keeps the cards as they are now (as on real hardware with a card swapped in).
- **Format**: binary, little-endian: `PRMSTATE`, u32 format version, disc serial, build id (version and
  git commit), u64 frame, then tagged sections. A state from another disc or another format version is
  refused with a message and the game keeps running untouched; so is a truncated file. A state from
  another build with the same format version loads (warning on stderr), so states survive updates.
  A state loads the same with or without `recomp`. Different mods or `text_speed` than when it was saved:
  it loads, the current options stay active (warning on stderr).
- **Fast-forward**: while held, up to `video.fast_forward_max` emulated frames run per 1/60 s (less if the
  PC is slower), one picture in `fast_forward_max` is shown and the sound is dropped (no pitched or
  crackling audio). `--unthrottled` is already uncapped.
- **Scripted (headless)**: `personarm --boot <disc> ... --save-state-at <N> <file>` writes a state at the
  end of frame N; `--load-state <file>` starts from it and runs to `--max-frames`. With the same input
  script, the RAM and VRAM at the end are byte-identical to the uninterrupted run (checked on disc 1).

## Command line

Existing flags keep working and win over the file. New: `--settings F`, `--save-settings`,
`--window-mode M`, `--window-scale N`, `--volume N`, `--mute`.

### Recording your play: `--record-input <file>`

`personarm_sdl <disc.cue> --record-input session.txt` writes the pad presses of the session as an
`--input-script` file (`<frame> <button> <hold>` per line; several buttons may start on the same frame).
The header comment has the disc serial, the date and the memory cards used. The file is rewritten every
600 emulated frames and on quit, so a crash loses at most 10 s. Frames spent paused (`pause_on_focus_loss`)
do not count. It also records the presses of a `--input-script` given at the same time (the effective pad).
Replay with `personarm --boot <disc.cue> --input-script session.txt --max-frames N` (N is in the header).
Replay is exact only from the same starting state: same disc, same memory card contents, no mods.
Recordings can contain your saves' effects: do not commit them.
