# Widescreen (prototype)

`--widescreen 16:9` (or `21:9`, or any `W:H` at least 4:3) shows more of the 3D scene at the left and
right instead of stretching the 4:3 picture. Settings file: `video.widescreen` (`off` by default, see
[SETTINGS.md](SETTINGS.md)). The boot harness takes the same flag: `personarm --boot <cue> --widescreen 16:9`.

Status: works well on the 3D field and battle scenes of disc 1; dialogue boxes, text and character
portraits drawn on a 3D scene keep their 4:3 proportions and layout, centred (see "2D over 3D").
Details below.

## Approach: emulator-side GTE widescreen

How the game projects (disc 1 JP, `--boot` log line `gte H=... OFX=... OFY=...`, printed whenever the
values seen at the end of a frame change):

| frames | H | OFX | OFY | what |
|---|---|---|---|---|
| 19 .. 6180 | 1000 | 0 | 0 | library defaults; title, opening, name entry are 2D/FMV (no projections drawn) |
| 6181 .. 19700 | 1024 | 160 | 120 | every 3D scene of the run (shrine, ceremony, Kismet floors, editor's office) |

So the field camera is a plain `H=1024` projection centred on a 320x240 screen (OFX=160, OFY=120). The
3D code lives in overlays (GHIDRA_ANALYSIS GH-011/GH-014: ~700 GTE commands in 123 overlay files on
disc 1), so patching the game's projection would mean one patch per overlay, and the GTE has a single
`H` for both axes (changing it zooms, it does not widen).

Instead the emulator changes what RTPS/RTPT return, as DuckStation's "GTE widescreen hack" does:

```
SX = OFX + (H/SZ * IR1) * num/den      num/den = (4/3) / (W/H) = 4H / 3W   (16:9: 3/4, 21:9: 9/16)
SY = OFY +  H/SZ * IR2                  (unchanged)
```

The picture the game draws is still 320 px wide, but it now covers a W:H field of view; the host
shows it at W:H, so 3D objects keep their proportions and the sides show what was off screen.
Code: `Gte::SetWidescreen` (`src/cpu/gte.{h,cpp}`); the recompiled path calls the same `Gte`.

No per-game patch is needed and none was added. Widening the drawing area instead (rendering into a
wider buffer) was not attempted: the game draws into fixed 320x240 VRAM buffers next to its textures,
so it needs a renderer that rasterises outside VRAM, which the software path does not have.

### Host presentation

`personarm_sdl` opens the window at W:H and presents 3D frames at W:H. It presents 4:3 (pillarboxed)
when:

- the display is 24 bpp (FMV / MDEC pictures), or
- the GTE has projected nothing in the last 30 frames (`Widescreen2D::Wide`): 2D-only screens such as
  the title, the opening and the main menu keep their real aspect.

With the option off nothing changes: same GTE results, same GPU output, same window, same 4:3 rectangle.

The host GPU renderer (`--renderer gpu`, [GPU_RENDERER.md](GPU_RENDERER.md)) gets the same picture: the
software rasterizer reports each squeezed 2D primitive and line to it, so 2D over 3D keeps 4:3 there too
(checked at f6700, 16:9, 4x).

### 2D over 3D

The GPU only sees final screen coordinates, so 2D drawn on a W:H frame would be 33% too wide at 16:9.
The emulator tells 2D from 3D the way PGXP / DuckStation do: every screen XY that RTPS/RTPT produce is
noted (`Gte::SetProjectionTracker`, a 2048x2048 table of the frame each position was last projected in,
kept 2 frames), and the software rasterizer looks up the GP0 vertex words of each primitive. Then
(`src/rendering/widescreen_2d.h`, only while `Scene()`: the GTE projected in the last 30 frames):

| primitive | how it is recognised | what happens |
|---|---|---|
| 3D polygon / line | 2 or more vertices are recent GTE output (one match is treated as chance: a glyph on a stale projected point) | drawn as sent (already widened by the GTE) |
| background | 2D drawn before the frame's first 3D primitive (a frame starts at E3h-E5h, the game's PutDrawEnv); e.g. the battle panorama wall, built by the CPU | drawn as sent: stays full width |
| screen-anchored overlay | 2D that is not a scene sprite and reaches to within 4 px of the left or right edge of the drawing area, or covers 15/16 of its width (fades, flashes, tints, the school corridor's darkening vignette, a wipe's black rows, letterbox bars); then every untextured 2D primitive right after it, until a textured one, a 3D one or another GP0 command (the vignette's inner ring, a wipe's soft edge lines). A line starts one only if it spans the whole width | drawn as sent: the 4:3-wide layer is shown stretched over the whole wide picture |
| scene sprite | textured 2D after a 3D primitive in the stream (the ordering table interleaves it with the 3D): field and battle characters are flat quads placed at a projected point | X squeezed by 4H/3W (3/4 at 16:9, the GTE factor) around one anchor per character: consecutive pieces within 16 px of each other are one character; the anchor is the centre of the character's shadow (a GTE triangle fan drawn shortly before, under the pieces) or else the first piece's centre. Every piece keeps its offset from the anchor scaled by num/den, so arms and heads stay on the body |
| HUD | any other 2D: after a non-drawing GP0 command (texpage, upload, fill, copy), after a line or an untextured 2D primitive | X squeezed toward the centre of the drawing area: the 4:3 layout sits in the middle of the wide picture |

Textured 2D keeps its scene-sprite / HUD decision per texture (texpage + CLUT): it flips only after the
stream has disagreed for 8 frames in a row, so a portrait or a font never alternates between two places.

**4:3 safety net.** A semi-transparent 2D primitive at least 3/4 of the width wide that is neither
screen-anchored nor centred (its middle more than 1/16 of the width off the screen centre) cannot be
placed. The scene then falls back to 4:3: the GTE stops widening, nothing is squeezed and the host shows
4:3 with pillarbox bars, as for menus. It stays 4:3 until no such primitive has been drawn for 120
frames (2 s), so it cannot flicker. Each fallback is logged once:
`[WIDESCREEN] frame N: 2D layer x A..B y C is neither centred nor screen-anchored; showing 4:3`. The
frame that triggers it is still shown wide (the GTE had already projected it).

Rectangles (sprites) are never 3D; a squeezed textured rectangle samples its texture by nearest
neighbour. Lines and polylines use the same rules. Per primitive kind on disc 1:

- **dialogue box frame and text**: the gouraud box fill, its border lines and the glyph rectangles come
  after a VRAM copy / glyph uploads, so they are HUD: centred, 4:3 proportions.
- **portraits** (Maya at f13500, Mizuno at f16500-18000, the old man at f10500): textured quads drawn in
  the HUD part of the stream: centred with the box, 4:3 proportions.
- **field / battle characters**: scene sprites: right shape, right place.
- **battle message box** ("悪魔が話し掛けてきた！"): follows the battle sprites directly, but its border is a
  polyline, which ends the scene run: HUD.
- **menus, FMV, title**: no projection, so `Wide()` is false: nothing is squeezed and the host shows 4:3
  (unchanged fallback).

No per-game patch was needed.

## Validation (disc 1 JP, `docs/examples/disc1_new_game_to_field.txt`)

Runs: `personarm --boot ... --input-script docs/examples/disc1_new_game_to_field.txt --max-frames 19700
--dump-frames <dir> --every 50`, with and without `--widescreen 16:9`, plus one at `21:9` and
`personarm_sdl --hidden --unthrottled --screenshot` at frames 1500, 18950 and 19450. The `--boot`
frames stay 320x240 (anamorphic); for viewing they were resized to 853x480. Frames are not committed.

**Off = unchanged** (re-checked for fix/widescreen-2: the 394 frames f50..f19700 of
`disc1_new_game_to_field.txt` and 400 consecutive corridor frames are byte-identical to main). All 394 dumped frames (f50..f19700) of the new build without the flag are
byte-identical (SHA-256) to the build before this change. Re-checked after the 2D/3D split: the same
394 frames, plus 280 frames (f50..f14000) of `docs/examples/disc1_to_first_battle.txt` (on a copy of a
school 3F save), are byte-identical to the build before it.

**2D over 3D (16:9, second pass).** Side by side with the 4:3 frames, and `personarm_sdl --screenshot`
at f6700, f10500 and f17000:

| scene | frames | result |
|---|---|---|
| shrine dialogue | f6700 | box, border lines, name and text 4:3 and centred; the two characters keep their place next to their shadows |
| ceremony | f10500 | the old man's portrait and the box centred at 4:3; attendees at the sides are 3D-widened as before |
| Kismet floor, Maya's introduction | f13500 | portrait and box centred, 4:3 |
| editor's office, Mizuno | f16500-18000 | portrait and box centred, 4:3 |
| first battles | f4700, f12600-13800 (battle script) | message box centred at 4:3; party and demon sprites keep their shape and place; the panorama wall stays full width |

At 1x the squeezed text drops one pixel column in four, so glyphs look slightly rough; with
`--resolution-scale 2` or more the squeeze happens on the scaled raster and should keep the detail (not checked).

**16:9.** 253 of 394 frames differ from 4:3, all from f6250 on; the 141 identical ones are the 2D/FMV
part before f6181 plus black transitions (f9000-9100, f12100-12300, f16000-16100) and the main menu
(f19350-19600).

| scene | frames | result |
|---|---|---|
| shrine | f6700, f6950, f7300-8300 | good: more trees, the stone lantern and the shrine's right side come in at the edges; characters keep their shape |
| ceremony | f9500-11500 | good: banners and attendees at the sides are drawn; no pop-in seen |
| Kismet floors | f12500-15500 | good: boxes and a door at the left, a character at the right edge that the 4:3 view did not show |
| editor's office | f18800-19050 | good: filing cabinets and a plant at the left; at the right the room ends and the black void around the room set is visible |
| main menu | f19450 (SDL screenshot) | good: 2D-only, presented 4:3 with bars |
| title | f1500 (SDL, 21:9) | good: 4:3 with bars |
| 21:9 | f6700, f10500, f12500, f18950 | works the same; more void at the room edges, 2D stretched more |

**Culling / clipping.** No geometry popping at the new edges was seen in the sampled frames: objects
that were off screen in 4:3 are drawn at 16:9 and 21:9. The game appears to cull on the projected
screen coordinates (which the hack already compresses), not on a view-space frustum. This was checked
on stills every 50 frames, not frame by frame while walking.

## Known issues

1. **2D classification is a heuristic.** It relies on the game's stream order (backgrounds first, 3D
   with sprites interleaved, then the HUD after a state or upload command). A HUD texture first seen
   right after a 3D primitive is a scene sprite (right shape, wrong place) until the stream disagrees
   for 8 frames; a scene sprite drawn after a texpage command would be pulled toward the centre; a
   drawing environment (E3h-E5h) sent mid-frame would make the HUD after it count as background
   (stretched). Characters without a shadow fan (or whose fan is more than 16 px to the side) are
   anchored on their first piece: right shape, but a few px off their place if that piece is not
   central. Two characters closer than 16 px are squeezed as one. The squeezed text loses a pixel
   column in four at 1x.
   **Overlay rule (fix/widescreen-2).** Untextured 2D drawn right after a screen-anchored layer, with
   no state command in between, is stretched with it: a HUD box drawn directly after a fade would be
   stretched too (not seen). A HUD element that slides in from the screen edge is stretched while it
   touches the edge and squeezed once it leaves it (a jump of up to 40 px; not seen).
   **Checked (16:9, JP disc 1):** school 3F corridor walking and running (arms stay on the body, the
   darkening vignette now covers the sides), Kismet office NPCs and Maya's portrait (f13150-13400),
   Mizuno / Maya dialogue (f17350-17550, classification identical in every frame), the Kismet floor
   wipe (f12370-12420), the first battles, and with the map tour (docs/MAP_TOUR.md, branch
   feat/map-tour) the areas it flagged for the "square of shadows": 154 (white flash), 58 (blue tint),
   142 and 148 (fades), 66 and 125 (dark rooms) now have no seam at the 4:3 edges; 19, 22, 55, 84 and
   103 (dark side bands) are room geometry ending at the set's edge (item 2), unchanged. No 4:3
   fallback was logged on these runs. The classroom event animation (bug report "rooms break during
   animations") was not reached by any script and is not verified.
   **GPU renderer:** with `--renderer gpu` scattered pixels change every other frame, at 4:3 too (same
   on main); this is not a widescreen issue and is not fixed here.
2. **Room edges.** Rooms are built for a 4:3 camera; at the sides the black void outside the set
   shows (office, f18950). Correct emulation of the wider view, but it looks unfinished on a TV.
3. **Aspect switch delay.** The 4:3 / W:H choice uses "any projection in the last 30 frames": opening
   the main menu shows about half a second of stretched menu before it snaps to 4:3, and a 3D scene
   right after a 2D screen starts stretched for one frame or so... it switches as soon as the GTE
   projects.
4. **Dumps are anamorphic.** `--dump-dir` / `--dump-frames` PNGs stay 320 px wide; resize to W:H to
   view them. `--screenshot` (SDL) is the presented window and is correct.
5. **Not tested:** battle menus beyond the first two fights, the world map, other discs, the US disc,
   `--recomp`, `--interp` (now replays the logged squeezes: 2D stays 4:3 in interpolated pictures) and `--resolution-scale`
   combined with widescreen. 21:9 was not re-checked after the 2D/3D split.
6. **Game timing differs from 4:3.** In the battle script the 16:9 run's sprites are at other animation
   positions than the 4:3 run at the same frame (f4700): the wider projection changes what the game
   draws and so its timing. Not investigated.
