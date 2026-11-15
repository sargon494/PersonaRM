# Widescreen (prototype)

`--widescreen 16:9` (or `21:9`, or any `W:H` at least 4:3) shows more of the 3D scene at the left and
right instead of stretching the 4:3 picture. Settings file: `video.widescreen` (`off` by default, see
[SETTINGS.md](SETTINGS.md)). The boot harness takes the same flag: `personarm --boot <cue> --widescreen 16:9`.

Status: works well on the 3D field scenes of disc 1; 2D drawn on top of a 3D scene (dialogue boxes,
character portraits) is stretched horizontally. Details below.

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
- the GTE has projected nothing in the last 30 frames (`Gte::ProjectionCount`): 2D-only screens such as
  the title, the opening and the main menu keep their real aspect.

With the option off nothing changes: same GTE results, same window, same 4:3 rectangle.

## Validation (disc 1 JP, `docs/examples/disc1_new_game_to_field.txt`)

Runs: `personarm --boot ... --input-script docs/examples/disc1_new_game_to_field.txt --max-frames 19700
--dump-frames <dir> --every 50`, with and without `--widescreen 16:9`, plus one at `21:9` and
`personarm_sdl --hidden --unthrottled --screenshot` at frames 1500, 18950 and 19450. The `--boot`
frames stay 320x240 (anamorphic); for viewing they were resized to 853x480. Frames are not committed.

**Off = unchanged.** All 394 dumped frames (f50..f19700) of the new build without the flag are
byte-identical (SHA-256) to the build before this change.

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

1. **2D over 3D is stretched.** Dialogue boxes, text, character portraits (e.g. f10500, f13500,
   f16500-18000) and other sprites drawn on a 3D frame are 33% wider at 16:9 (75% at 21:9). The GPU
   only sees final coordinates, so it cannot tell a HUD sprite from a projected polygon. Fixing it
   needs either game-side patches to the 2D layout code, or a GPU-side heuristic (squeeze sprites /
   rectangles toward the centre), neither done here.
2. **Room edges.** Rooms are built for a 4:3 camera; at the sides the black void outside the set
   shows (office, f18950). Correct emulation of the wider view, but it looks unfinished on a TV.
3. **Aspect switch delay.** The 4:3 / W:H choice uses "any projection in the last 30 frames": opening
   the main menu shows about half a second of stretched menu before it snaps to 4:3, and a 3D scene
   right after a 2D screen starts stretched for one frame or so... it switches as soon as the GTE
   projects.
4. **Dumps are anamorphic.** `--dump-dir` / `--dump-frames` PNGs stay 320 px wide; resize to W:H to
   view them. `--screenshot` (SDL) is the presented window and is correct.
5. **Not tested:** battles, the world map, other discs, the US disc, `--recomp`, `--interp`, and
   `--resolution-scale` combined with widescreen (all go through the same GTE and presenter, so they
   should work).
