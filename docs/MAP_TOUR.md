# Map tour: every field area without playing

Widescreen, interpolation, high resolution and the GPU renderer have to look right in every area of the
game, and nobody can play the whole game to check. The map tour loads one save state in a field,
triggers the game's own map change to each area in turn (as a door does), captures a 4:3 and a 16:9
frame, flags suspicious ones and writes a contact sheet a human can scan in minutes.

No game data is in this document or in the repository: only addresses and how to run the tour.
Screenshots, states and cards stay in a folder outside the repo.

## How the game changes map (JP disc 1, SLPS-02785)

| What | Address | Confidence | Evidence |
|---|---|---|---|
| Field area id (field bank) | `0x8007E33E` (s16, `0x8007D1E8+0x1156`) | HIGH | the map-change code `0x8002A5D8` reads it and loads `FILEPOS[0x39 + area]` (table `0x80083948`, so area 0 = F0057) with `0x80027438(lba, 0, sectors, 85, cb)`; watched: `--warp-map 11` reads LBA `0xAA31` = F0068. Writing it and requesting a change loads that area (every capture of the tour) |
| Entry / room word | `0x8007E340` (s16) | MEDIUM | set by an event-script opcode of F0042 (`0x800C1B2C`: `lhu v0,0(args); sh v0,E340`); the scene table at `0x80072C70` and the F0042 table at `0x800D9288` (299 rows) are indexed by it; it changes on every real room change (`0x68 → 0x96 → 0x97 → 0x98` in the opening). With only the area changed the area's script picks its own start; what the word selects exactly (door, room, story step) is not traced |
| Previous room | `0x8008227A` (s16) | MEDIUM | holds the last value of `0x8007E340` after each change |
| Field request byte | `0x800DE798` (u8, F0042 data) | HIGH | F0042's per-frame handler `0x800BEBF8` switches on it (jump table `0x800AB10C`, 16 cases); 1..5 store the scene request `{0x2D, 0x800932A0}` (F0045, or `0x80093558` when byte `+5` is set) in `0x8007E2AC/0x8007E2B0` |
| Scene request (id, entry) | `0x8007E2AC` / `0x8007E2B0` | CONFIRMED | OVERLAY_MAP OV-003; the scene loop loads the id in slot A and calls the entry |
| Map-change overlay | F0045 at `0x80093000`, entry `0x800932A0` | HIGH | fades out, calls `0x8002A5D8` (load the area bank), returns; the scene loop then reloads F0042 (scene mode 1) |

A real door, as seen in the harness log of the opening (`docs/examples/disc1_new_game_to_field.txt`):
`scene request id 0xFFFFFFFF -> 0x2D entry=0x800932A0`, `load_file_by_id(F45)`, request back to -1,
then `load_file_by_id(F42)`, with `0x8007E340` and `0x8007E33E` already holding the new values when
F0045 runs (RAM diffs with `--dump-ram-at-loads`).

Two field overlays exist: F0042 (scene mode 1: towns, buildings, story rooms) and F0736 (scene mode 0,
loaded when continuing a save made in a dungeon such as the school 3F). The warp drives F0042 only; an
area that the real game shows with F0736 still loads, under F0042, with its event script.

## US disc (SLUS-01158)

| What | JP | US | Confidence | Evidence |
|---|---|---|---|---|
| Field area id | `0x8007E33E` | `0x8007FA5E` | HIGH | the US map-change code `0x8002A88C` is the JP one instruction for instruction with base `0x8007E908` (JP `0x8007D1E8`), `lh v1,0x1156(s0)`; in the US office state it holds 3 like the JP one; `--warp-map 11` loads F0045 then F0042 and shows the area-11 scene in English |
| Entry / room word | `0x8007E340` | `0x8007FA60` | MEDIUM | same base `+0x1158`; holds `0x98` in the US office state, as in JP |
| Field request byte | `0x800DE798` | `0x800E2F90` | HIGH | F0042's per-frame handler found by its prologue at `0x800C2C18` (JP `0x800BEBF8` + `0x4020`): `lbu v1,0x2F90(s1)` with `s1 = 0x800E0000`, `sltiu 16`, jump table `0x800AF10C`; the overlay data moved by `+0x47F8`, not the `+0x4000` of the code |

The resident words moved by `+0x1720`, like the scene mode word (`0x8007E298` / `0x8007F9B8`). The US
state for the tour: `docs/examples/us_new_game_to_field.txt` cut before f17550 (free movement in the
Kismet office), `--max-frames 17550 --save-state-at 17550 <out>/us_office.state`.

## Harness options

```
personarm --boot <disc1.cue> --load-state <field.state> --warp-map <area>[:<entry>] [--warp-at <N>] ...   (JP or US)
personarm --boot <disc.cue> ... --poke-at <N> <hex addr>=<hex value>[:<bytes 1|2|4>]   (repeatable)
```

- `--warp-map` writes the area word (and the entry word when given), then the request byte, at the
  first frame from `--warp-at` (default: the first frame run) in which the scene mode is 1 (F0042 field).
  The log shows `warp area N entry ...`, then F0045 and F0042 loads. Area and entry take decimal or `0x`.
- `--poke-at` is the generic form (`--poke-at 18860 8007E33E=B:2 --poke-at 18860 800DE798=1:1` is the
  same warp by hand).

## Tour

1. A state in the F0042 field (copy cards, never use the originals):

   ```
   personarm --boot <disc1.cue> --input-script <copy of disc1_new_game_to_field.txt cut before f18800> \
       --max-frames 18850 --save-state-at 18850 <out>/office.state
   ```

   US: `us_new_game_to_field.txt` cut before f17550, `--max-frames 17550 --save-state-at 17550`.

2. The tour (Python 3 + Pillow; developer tool, not shipped):

   ```
   python tools/map_tour/map_tour.py --exe build/Release/Release/personarm.exe --disc <disc1.cue> \
       --state <out>/office.state --out <out>/tour_jp --areas 0-299 --jobs 12
   ```

   Per area it runs the harness twice (4:3 and `--widescreen 16:9`) for 600 frames after the state,
   keeps the last non-black frame of each, and writes `<out>/tour_jp/index.html` (one row per area:
   4:3 | 16:9 | flags) and `summary.md`. Several `--areas` runs into one folder share the sheet.
   `python tools/map_tour/map_tour.py --self-test` checks the edge detector.

Flags (16:9 frames are anamorphic, 320 wide; the 4:3 picture covers x = 40..280):

| Flag | Meaning |
|---|---|
| HIGH edge step x=40/280 | a sharp vertical luminance step at the old 4:3 edges on most rows: a 2D layer (fade, tint, shadow) drawn 4:3 only over a widened 3D scene, the "square of shadows" |
| HIGH hang / crash / engine stopped / BIOS unimplemented / field not reloaded | the harness did not get through the change |
| MED dark side bands | side bands much darker than the centre |
| MED 2D bottom mismatch | the message-window area differs between 4:3 and 16:9 (squeezed): 2D pieces may have moved apart; coarse, 3D motion also raises it |
| LOW black in side band | the side bands show no geometry where the 4:3 frame does: usually a small room with nothing beyond the 4:3 camera |
| LOW mostly black 4:3 | empty bank, fade or dark room |

## Player-config pass (`--sdl`)

The headless harness has no GPU renderer and no interpolation. With `--sdl <personarm_sdl.exe>` the tour
also saves a state at the end of the 4:3 harness run of each area (`--save-state-at`, after the 600
frames) and runs `personarm_sdl` twice from it (`--hidden --unthrottled --mute --no-recomp --load-state 0`,
a per-area settings file whose `general.save_dir` holds the state as `<serial>.state0`):

- `--renderer gpu --resolution-scale 4 --interp 120`, 4:3: every present of the next 90 frames (still for
  60 frames, then right held 30 frames: the player walks where the area allows it);
- `--renderer software --resolution-scale 1 --interp 60`: the reference. Emulation does not depend on the
  renderer, so software frame f is what the game drew at f.

```
python tools/map_tour/map_tour.py --exe build/Release/Release/personarm.exe --disc <disc1.cue> \
    --state <out>/office.state --out <out>/tour_jp_gpu --areas 0-299 --jobs 12 \
    --sdl build/Release/Release/personarm_sdl.exe
```

The presents (about 850 MB per area) are deleted after the analysis (`--keep-presents` keeps them); each
area keeps `gpu/shot.png` (a still present) and the worst crop of each check (`gpu/crop_*.png`), the
metrics are stored in `result.json`, and `--reflag` re-applies changed thresholds to them. The first 8
frames after the load are not judged (the GPU upscaled VRAM and the interpolation history refill).
The software frames f-4..f+3 are the *window* of GPU present f: interpolation shows the game one or two
frames late, and the GPU picture of the double-buffered game can be up to two frames ahead of the software
display. A pixel is *still* when the window's software frames agree there (grown by one pixel).

| Flag | Check | Threshold (`map_tour.py`) |
|---|---|---|
| HIGH GPU present went back in time | a present matches only software frames older than one an earlier present already showed (more than 0.5 mean luma closer to them): the picture steps back, a visible flicker | any |
| HIGH / MED GPU jitter in a still scene | consecutive GPU presents, block-averaged to 1x, change by more than 8 on still pixels | MED > 20 px, HIGH > 300 px (1x pixels) |
| MED GPU thin stray lines | hi-res pixels more than 48 from the software frame upscaled (nearest), on still pixels away from software edges (3x3 grown), that a 3x3 opening removes (1-2 px wide: seams, outlines) | > 400 hi-res px in one present |
| MED GPU differs from software | median over presents of the mean difference between the GPU present (box-downscaled) and the best software frame of the window | > 16 |
| MED GPU spike | one present's difference above the median of the 8 presents around it (a pop or flash) | > 8 |
| HIGH GPU pass: ... | the SDL run hung, crashed, could not load the state or fell back to software | |

Crops in the sheet: back in time = earlier present, this present, change x4; jitter = present, next
present, change x4; lines = GPU, software upscaled; spike = previous present, this present, software;
differs = GPU downscaled, software. The GPU pass runs at 4:3 only (comparison with the 4:3 software frame).
