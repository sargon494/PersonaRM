# GPU renderer (`--renderer gpu`)

Status: ready to be the players' default on Windows (D3D12) and Linux (Vulkan); the build default is
still `software`, so a launcher or settings file picks it (`video.renderer`, [SETTINGS.md](SETTINGS.md)). The software GPU
(`src/rendering/ps1_gpu.*`) stays the emulation's source of truth; without the GPU renderer nothing
changes (byte-identical output). The GPU renderer is display only: it redraws the game's primitives at
s× on the host GPU, so high internal resolutions run at the cost of 1x. Code: `src/rendering/gpu_renderer.*`
(linked into `personarm_sdl` only).

```
personarm_sdl disc.cue --renderer gpu --resolution-scale 4      # 1-9, or auto
personarm_sdl disc.cue --renderer gpu --resolution-scale 4 --interp 120 --widescreen 16:9 --mods D
```

Settings file: `"video": {"renderer": "gpu", "resolution_scale": "auto"}`.

## API: SDL_gpu

SDL3's GPU API (D3D12 / Vulkan / Metal) over OpenGL 3.3:

- SDL3 is already the host dependency; SDL_gpu needs no GL loader and no second windowing path.
- Modern backends on every target (D3D12 on Windows, Metal on macOS, where GL is deprecated and stuck at 4.1).
- Explicit copy passes and command buffers fit the work: interleaved texture uploads, VRAM→VRAM copies
  and draws, all in order in one command buffer per picture.
- SDL 3.4's GPU renderer (`SDL_CreateGPURenderer`) shares the device and wraps an `SDL_GPUTexture` as an
  `SDL_Texture` (`SDL_PROP_TEXTURE_CREATE_GPU_TEXTURE_POINTER`): the zero-copy present path.

Shaders: SDL_gpu takes backend bytecode (DXBC/DXIL, SPIR-V, MSL), not source. One HLSL source per shader
(`kShaderVs`, `kShaderPs`, `kShaderDisplay` in `gpu_renderer.cpp`):

- Windows (D3D12): compiled at run time with `d3dcompiler_47.dll` (shipped with Windows) to DXBC SM5.1.
- Linux (Vulkan): compiled at build time to SPIR-V by `glslangValidator` (apt `glslang-tools`):
  `cmake/gpu_spirv.cmake` extracts the same strings and writes `build/generated/gpu_spirv/gpu_spirv.h`
  (never committed). Without the tool CMake says so and the GPU renderer is unavailable.

Without shaders or a GPU device the host logs `[gpu] renderer unavailable (...): software renderer` and
uses the software renderer. macOS (Metal) needs MSL: see [Metal](#metal).

## Present

With `--renderer gpu` the window gets SDL's GPU renderer (`SDL_CreateGPURenderer`) and `GpuRenderer`
creates its resources on that renderer's device. Each picture: the recorded work and the display pass
(display area of `hi` → `out`, 1x colour conversion) are submitted in one command buffer without a fence
wait; `out` is wrapped once as an `SDL_Texture` and drawn by `SDL_RenderTexture` like the software
picture (same 4:3 / widescreen / integer-scaling rect, linear filter). The renderer's own command buffer
is submitted later on the same queue, so it sees the finished picture.

Reads back to the CPU only when a picture is needed there: `--dump-dir` frames (fence wait + copy).
`--screenshot` uses `SDL_RenderReadPixels` (the presented window, as before). Texture dumps
(`--texture-dump`) come from the 1x VRAM and need nothing. If the window renderer is not a GPU renderer
(device creation failed there) the renderer makes its own device and reads every picture back, as the
prototype did (`[gpu] renderer: ... readback present`).

## Resources

| Resource | Format | Size | Role |
|---|---|---|---|
| `vram1x` | R16_UNORM | 1024×512 | copy of the 1x VRAM: every texel and CLUT lookup reads it (`Load`, exact 16-bit) |
| `hi` | RGBA8 | 1024s×512s | the scaled VRAM: render target; 5-bit channels as c5 << 3, mask bit in alpha |
| `tmp` | RGBA8 | 1024s×512s | staging for overlapping 80h copies; destination copy for mask-checked B/2+F/2 |
| `out` | RGBA8 | (w·s)×(h·s) | display area converted with the 1x formula (c5 << 3); presented directly |
| `hi2`, `snap0/1`, `vram1x2` | RGBA8, R16 | as `hi`, `vram1x` | `--interp` only: replay target, the scaled pictures of the last two game frames |
| HD images | RGBA8 | as the PNG | `--mods` only: one per replaced upload, created on first use |

Memory at 4x: 64 MB (interp: +100 MB); at 9x: 340 MB (interp: +510 MB). 9216×4608 is under the 16384
texture limit of D3D12/Vulkan.

## Primitive mapping

`Ps1Gpu` calls `GpuDrawListener::OnGp0` with every complete GP0 packet before it executes, `OnVramWrite`
after every non-raster write, `OnLine` for every line segment it draws and `OnSqueeze` when widescreen
squeezes a 2D primitive. The renderer parses packets exactly as `Ps1Gpu` does; a polygon or rectangle is
kept pending until the next callback (so a squeeze can still move it), then appended; consecutive
primitives with the same pipeline, scissor and HD image become one draw call.

- **Vertices**: signed 11-bit + drawing offset (E5h), times s. Polygons are shifted by +0.5 target pixel:
  the software samples at integer coordinates, the GPU at pixel centres. With the D3D top-left rule this
  reproduces `DrawTriangle`'s coverage. Quads are (v0,v1,v2)+(v1,v2,v3); triangles over 1023×511 are dropped.
- **Rectangles**: a quad from (x·s, y·s) to ((x+w)·s, (y+h)·s), no shift. UV corners u0 and u0+w
  (flipped: u0+1 and u0+1−w), so fine pixel dx samples u0 + (dx+0.5)/s, as `DrawRectangle`. Never dithered.
- **Lines and polylines**: each segment (`OnLine`, after the drawing offset and the widescreen squeeze) is a
  parallelogram one 1x pixel thick across its minor axis, through the end pixels' centres and extended half
  a pixel past both ends along the major axis: the pixels the 1x DDA plots, with fine edges at s× (the
  software scaled raster draws s×s blocks). Gouraud colours interpolate along it; dithering, semi-transparency
  and the mask bits as polygons.
- **Polygon UV and gouraud colour**: not the fp32 interpolation. Each vertex carries the triangle's three
  fine vertices, colours and UVs; the shader computes the barycentric weights of its fine pixel in integers
  and `DrawTriangle`'s nearest-rounded lerp exactly (a float estimate corrected by the remainder, which fits
  32 bits when the products do not), UV in 1/(256 s) texels with the half-texel shift as the scaled pass.
  Fine samples land on, or within fp32 noise of, texel and colour-step boundaries; the fp32 interpolation
  rounded them either way depending on where the primitive sits, so single pixels differed between the two
  display buffers and toggled every other frame (a 1/512 tie bias fixed the exact ties only).
- **UV** (rectangles, lines): interpolated as float texel coordinates and floored in the shader (their fine
  samples never land on a boundary). Then, for all primitives, texture window, page, 4/8/15-bit decode and
  CLUT lookup on `vram1x`, as `FetchTexel`.
  Texel 0000h discards. Modulation `(t5 * c8) >> 4`, raw `t5 << 3`.
- **UV limits** (scale > 1, polygons, both renderers): the floored texel is clamped to the range the 1x
  raster can sample (`Ps1Gpu::UvLimits`: the vertex UV range less one pixel's worth of texels at the end
  the UV grows toward). Without it the fine samples of a sprite's or glyph's last 1x row reached its far UV
  and drew the next sprite cell's row: a thin line under feet and letters (the compass `N`), cell outlines,
  dark seams between floor tiles. 1x is unchanged.
- **Dithering**: the 4×4 table indexed by (pixel / s) & 3, applied in the 8-bit domain before `>> 3`.
- **Semi-transparency**: fixed-function blending on c5 << 3 values (exact in 8 bits): B/2+F/2 = `ONE, CONSTANT(0.5)` with
  the shader halving F; B+F = `ONE, ONE`; B−F = reverse subtract; B+F/4 = `ONE, ONE` with the shader
  writing `c5 >> 2`. Untextured semi primitives blend all pixels. Textured ones are drawn twice: opaque
  pipeline keeping texels with bit 15 clear, then the blend pipeline keeping texels with bit 15 set.
  Channels are stored as c5 << 3, so every blend is exact in 8 bits and B/2+F/2 floors as the software
  (stored as c5/31 it rounded odd sums up, depending on the destination).
- **Mask bit**: the set bit (E6h.0) and texel bit 15 go to alpha. The check bit (E6h.1) is a test on the
  destination's alpha, done by blending instead of a stencil plane (no second copy of the mask bit to keep
  in sync through uploads, copies and blits): five more pipelines with source factor
  `ONE_MINUS_DST_ALPHA` and destination factor `DST_ALPHA` (opaque) or `ONE` (B+F, B−F, B+F/4), alpha the
  same way, so a pixel whose mask bit is set keeps colour and mask. B/2+F/2 cannot be written that way
  (it needs 0.5 + 0.5·dst_alpha); a mask-checked B/2+F/2 primitive first copies its rect of `hi` to `tmp`
  and blends in the shader with the exact software formula `(B + F) >> 1`.
- **Draw area**: scissor rect (x1·s, y1·s)–((x2+1)·s, (y2+1)·s).

Nothing falls back to the software rasterizer any more. 24bpp display (FMV) is presented by the software
path at 1x; the GPU work is still flushed.

## VRAM coherence

Same semantics as the software scaled mirror (`Ps1Gpu::SetResolutionScale`):

- **CPU uploads, fills, TIM loads, FMV** (`OnVramWrite`, i.e. wherever `UpscaleRect` runs): the rect is
  copied from the 1x VRAM into a staging buffer, uploaded to `vram1x`, then nearest-upscaled into `hi`.
- **Textures sampled after a draw** (render-to-texture, the game drawing into a page it samples): a
  32×32-block dirty grid tracks 1x VRAM written by the 1x rasterizer and by 80h copies. Before a textured
  primitive, the dirty blocks under its texture rect (UV bounding box, whole page with a texture window)
  and its CLUT row are re-uploaded to `vram1x` from the 1x VRAM at that moment. Textures therefore always
  come from the 1x VRAM, exactly as the scaled mirror.
- **80h VRAM→VRAM**: copies the scaled pixels (`hi` → `tmp` → `hi`), keeping render-to-texture detail.
  Copies crossing the VRAM edge are clipped there (the CPU path wraps).
- **GPUREAD, 1x reads**: untouched, the CPU VRAM answers them.

All operations are recorded in order (upload, copy pass, draw) and run in one command buffer per
presented picture. Upload and vertex buffers are cycled (`cycle = true`), so a new picture never waits for
the GPU to finish the previous one.

## Scale up to 9x (4K TVs)

`--resolution-scale 1-9` (240 × 9 = 2160 lines) and `auto`, which picks ceil(desktop height / 240)
(2160p → 9, 1440p → 6, 1080p → 5); the settings file takes the same (`"resolution_scale": 9` or
`"auto"`). The software renderer keeps 1-4 (larger values fall back with a warning). The CPU cost does not
depend on s; the GPU cost is fill rate (about 81× the 1x pixels at 9x).

## Widescreen

`--widescreen 16:9` ([WIDESCREEN.md](WIDESCREEN.md)) works the same on the GPU path. The GTE hack is
emulation-side, so 3D needs nothing; the 2D/3D classification (`Widescreen2D`) runs in the software
rasterizer, which squeezes 2D primitives drawn over 3D: it reports the squeezed X coordinates through
`OnSqueeze` (polygons: per vertex; rectangles: left/right edge, UVs unchanged so the texture is squeezed
with it) and the line endpoints through `OnLine`, so 2D over 3D keeps 4:3 on the GPU too. The presenter
uses the same 4:3 / W:H rect.

## Frame interpolation (`--interp 120`, `fps_target` 120)

The interpolator (`rendering/frame_interp.h`) matches the GP0 packets of the last two game frames and
replays the latest segment (the packets drawn between the two flips) with lerped vertices on a scratch
`Ps1Gpu` that starts from the 1x VRAM of the previous flip, the state that segment was drawn over. Both
frames are taken at their flip (`Ps1Gpu::GetFlipVram`), not after the emulated frame, when the next
segment may already have drawn. So an unmoved replay is the latest frame exactly: a still scene gives
identical pictures, and a blended or partly covered primitive is not drawn twice. On the GPU path, at any
resolution scale:

- each logged flip (`GpuDrawListener::OnFlip`, during emulation) snapshots `hi` into `snap0`/`snap1`
  alternately (one texture copy), so the scaled pictures of the last two game frames stay on the GPU;
- a synthesized picture (0 < t < 1, pair usable): `BeginReplay` copies the older snapshot to `hi2`,
  attaches the renderer to the scratch GPU and switches recording to `hi2` / `vram1x2` (own dirty grid,
  all dirty: textures upload lazily from the scratch's 1x VRAM); the scratch's `SetVram` only marks
  `vram1x2` stale (`hi2` already holds that VRAM at s×); the replayed packets draw at s×; `EndReplay`
  detaches and switches back;
- t ≤ 0 presents the older snapshot, t ≥ 1 or an unusable pair the newer one (an unusable pair has
  nothing to show in between, so holding the older one would only add latency), each with its display
  area. A picture is never older than the one before it.

The scratch still rasterizes at 1x on the CPU (the interpolator's existing cost, about 3 ms per
synthesized picture). The `--interp` restriction to scale 1 remains for the software renderer only.
`fps_target` 90 uses the same path (phases 0, 2/3, 1/3).

Two things the replay needs from the command log (`Ps1Gpu::SetCommandLog`), for both renderers:

- **VRAM uploads.** The log used to leave A0h uploads out, assuming the end-of-frame VRAM holds every
  texture. The game reuses font glyph slots within a frame (upload a glyph, draw it, upload the next),
  so every glyph of a replay showed the last one uploaded: garbage text (`おおおお`, `00000000`) at 90 and
  120 fps. Uploads are now logged with their data words and replayed in order.
- **Widescreen squeezes.** The scratch GPU has no 2D/3D classifier (its projections are gone), so
  interpolated pictures drew 2D over 3D unsqueezed: the dialogue box alternated between 4:3 and stretched
  (flicker). The live decision (centre relative to the first vertex, factor) is logged as a GP0 NOP entry
  `[3, 00AB0000h, centre - x0, num | den << 12]` before the primitive and applied to its lerped vertices on
  replay (`Ps1Gpu::Squeeze`). Live rendering is unchanged.
- **Only 3D moves.** A polygon with 2 or more GTE vertices (the `Widescreen2D` rule; the engine tracks
  projections whenever `--interp` is on) is tagged `[1, 00D30000h]` before its packet. Only tagged
  polygons are matched and lerped; sprites, portraits, text, windows, rectangles and lines are replayed
  exactly as in the latest frame. Lerping them paired wrong same-key partners and moved 2D by sub-frame
  amounts: portraits (often their lower half) and sprite limbs jittered at 120 fps, also in menus where
  nothing moves. Characters now move at the game's rate while the camera and scenery interpolate.

Measured at 9x, 16:9, `--interp 120`, shrine dialogue: 13.5 ms per emulated frame for 2 presents
(unthrottled), presents every 6.8 ms on average.

## HD textures (`--mods`)

The texture hook (`rendering/vram_texture_hook.h`) tags the VRAM words of every upload; the CPU path
replaces each sampled texel whose upload has an HD PNG. On the GPU path, for a textured primitive the
renderer asks the hook for the upload under the centre of its UV box (`VramTextureHook::Replacement`); if it has an
HD image, that image becomes a GPU texture (uploaded once) and the primitive's vertices carry the upload
rect. The shader maps the texel to the upload (as `VramTextureHook::Texel`) and samples the HD image at the
interpolated sub-texel position (alpha < 0.5 = transparent, black kept opaque as 0421h). Texels outside the
upload rect keep the original. Limit: one upload per primitive (the CPU path decides per texel); a
primitive spanning two replaced uploads shows the texels of the other one unreplaced.

## Metal

SDL_gpu runs on Metal, but needs MSL. SPIRV-Cross (or SDL_shadercross) can turn the build's SPIR-V into
MSL the same way; not added, since there is no macOS build yet.

## Measured cost

Developer machine (Windows 11, D3D12), `docs/examples/disc1_new_game_to_field.txt`,
`--unthrottled --stats --hidden`, one run at a time. `wall_ms` = whole loop per emulated frame
(emulation, CPU 1x raster, GPU submission, present, plus about 1 ms of `--stats` hashing), averaged over
the second before the frame. Before = the prototype (readback every picture), after = direct present.

| | field f18900 before | after | dialogue f10020 before | after |
|---|---|---|---|---|
| software 1x | | 8.4 | | 7.6 |
| gpu 4x | 10.1 (gpu_ms 1.6) | 8.3 (gpu_ms 0.3) | 9.7 (1.5) | 7.3 (0.2) |
| gpu 9x (2880×2160) | 16.8 (gpu_ms 6.9) | 10.7 (gpu_ms 0.4) | 15.9 (6.0) | 7.5 (0.2) |

The GPU path now costs what software 1x costs, at any scale; the 9x f18900 sample includes an emulation
spike (`frame_ms` 9.5 vs 7.3 elsewhere). With `--hidden` the window renderer may skip its swapchain blit;
the scaled drawing and the display pass run either way. About 600 draw calls per field picture.

Accuracy: see [Checked frames](#checked-frames).

## Checked frames

Real disc 1 (JP), `--hidden --unthrottled --dump-dir`, GPU 4x against software 4x of the same build
(pixels that differ / differ by more than one 5-bit step). New game: `docs/examples/disc1_new_game_to_field.txt`;
battles: `docs/examples/disc1_to_first_battle.txt` on a copy of a school 3F card. Every frame was also
looked at.

| scene | frame | GPU vs software | notes |
|---|---|---|---|
| shrine dialogue | f6700 | 2.90% / 0.19% | single-pixel texel and polygon edges; the dialogue box's B/2+F/2 rounding; box border lines match |
| Kismet editor's office | f18900 | 0.83% / 0.18% | identical to the prototype's GPU picture |
| first battle (Slime tutorial) | f4700 | 0.23% / 0.05% | |
| second battle | f13000 | 0.44% / 0.07% | message box, party and demon sprites match |
| shrine, `--widescreen 16:9` | f6700 | 2.37% / 0.27% | dialogue box, name and text squeezed to 4:3 and centred as in software |
| office, `--interp 120` | f18890-18891, t = 0, .25, .5, .75 | 0.75-0.83% / 0.15% | GPU 1x against software 1x interpolation (the software one has no 4x); 4x pictures looked at |
| US shrine, `--widescreen 16:9` (US disc, `us_new_game_to_field.txt`) | f6800 | 2.56% / 0.27% | name, text, box and arrow inside the box, box centred at 4:3, as software (the v0.1.0 prototype had no squeeze on the GPU path) |
| Mizuno, `--widescreen 16:9` | f17000 | 3.37% / 0.55% | portrait and box at 4:3, as software |
| shrine, `--interp 120` / `90`, 4x | f6700-6701 | 0.71-0.77% from the live picture | text correct (was every glyph = the last uploaded one) |
| JP shrine / US shrine / Mizuno, `--interp 120` + 16:9, 4x | f6700, f6800, f17000 | 0.35-0.38% from the live 16:9 picture | box stays 4:3 in synthesized pictures; 9x checked at f6700 |
| office walking, `--interp 120` + 16:9 and `--interp 90`, 4x | f18900-18915, 32 / 24 presents | no picture far from both neighbours (flicker score 0.01 / 0) | |
| map tour areas 33, 35, 36, 37, 58 (`--sdl` states), live and replayed in both display buffers | f19451-19490 | 0% / 0% | every frame identical to software 4x; was 20k-95k pixels off by one step and 1-2 pixels differing between the buffers |
| shrine, `--mods` | f6700 | 6.7% / 3.9% | test pack (every dumped texture 4x with a red hatch): floor, walls, the shrine and Tatsuya's legs replaced as in software; his torso and the old woman stay original (one upload per primitive, see HD textures) |
| Linux, Vulkan (WSL2, Mesa lavapipe): JP shrine, JP office, US shrine; JP shrine and office `--interp 120` | f6700-6703, f18899-18901, f6799-6801 | 0% / 0% | byte-identical to software 4x on Linux and to D3D12 GPU 4x on Windows (default settings, new memory cards) |

Software 4x of this build against the build before: byte-identical (f6700, f18900, SHA-256), so the
software path is unchanged. Not exercised by these scenes: the mask-check pipelines (no mask-checked
primitive in the new-game script: the prototype's fallback log never reported one).

## Next steps

1. Metal shaders (above).
2. HD textures: split a primitive per replaced upload instead of one per primitive.
3. Widescreen without anamorphic 320-px frames: draw the wider view into extra `hi` columns (the GPU is
   not bound to the 1x VRAM layout), so the sides get full horizontal resolution.
