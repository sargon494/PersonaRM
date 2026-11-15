# GPU renderer (`--renderer gpu`)

Status: prototype. The software GPU (`src/rendering/ps1_gpu.*`) stays the emulation's source of truth and
the default renderer; without `--renderer gpu` nothing changes (byte-identical output). The GPU renderer is
display only: it redraws the game's primitives at s× on the host GPU, so high internal resolutions run in
real time. Code: `src/rendering/gpu_renderer.*` (linked into `personarm_sdl` only).

```
personarm_sdl disc.cue --renderer gpu --resolution-scale 4      # 1-9, or auto
```

## API: SDL_gpu

SDL3's GPU API (D3D12 / Vulkan / Metal) over OpenGL 3.3:

- SDL3 is already the host dependency; SDL_gpu needs no GL loader and no second windowing path.
- Modern backends on every target (D3D12 on Windows, Metal on macOS, where GL is deprecated and stuck at 4.1).
- Explicit copy passes and command buffers fit the work: interleaved texture uploads, VRAM→VRAM copies
  and draws, all in order in one command buffer per picture.
- SDL 3.4 can wrap an `SDL_GPUTexture` as an `SDL_Texture` (`SDL_CreateGPURenderer`,
  `SDL_PROP_TEXTURE_CREATE_GPU_TEXTURE_POINTER`), the zero-copy present path (see Next steps).

The one real cost is shaders: SDL_gpu takes backend bytecode (DXBC/DXIL, SPIR-V, MSL), not GLSL source.
The prototype compiles its HLSL at run time with `d3dcompiler_47.dll` (shipped with Windows) to DXBC SM5.1,
so it runs on D3D12 with no build-time tool. Vulkan/Metal need SPIR-V/MSL from the same HLSL, generated
offline with SDL_shadercross and committed; until then other platforms log
`[gpu] renderer unavailable (...)` and use the software renderer. This is not a blocker.

## Resources

| Resource | Format | Size | Role |
|---|---|---|---|
| `vram1x` | R16_UNORM | 1024×512 | copy of the 1x VRAM: every texel and CLUT lookup reads it (`Load`, exact 16-bit) |
| `hi` | RGBA8 | 1024s×512s | the scaled VRAM: render target; 5-bit channels as c5/31, mask bit in alpha |
| `tmp` | RGBA8 | 1024s×512s | staging for overlapping 80h copies |
| `out` | RGBA8 | (w·s)×(h·s) | display area converted with the 1x formula (c5 << 3) |

Memory at 4x: 64 MB; at 9x: 340 MB (9216×4608 is under the 16384 texture limit of D3D12/Vulkan).

## Primitive mapping

`Ps1Gpu` calls `GpuDrawListener::OnGp0` with every complete GP0 packet before it executes, and
`OnVramWrite` after every non-raster write. The renderer parses packets exactly as `Ps1Gpu` does and
appends vertices; consecutive primitives with the same pipeline and scissor become one draw call.

- **Vertices**: signed 11-bit + drawing offset (E5h), times s. Polygons are shifted by +0.5 target pixel:
  the software samples at integer coordinates, the GPU at pixel centres. With the D3D top-left rule this
  reproduces `DrawTriangle`'s coverage. Quads are (v0,v1,v2)+(v1,v2,v3); triangles over 1023×511 are dropped.
- **Rectangles**: a quad from (x·s, y·s) to ((x+w)·s, (y+h)·s), no shift. UV corners u0 and u0+w
  (flipped: u0+1 and u0+1−w), so fine pixel dx samples u0 + (dx+0.5)/s, as `DrawRectangle`. Never dithered.
- **UV**: interpolated as float texel coordinates (+0.5, the software's half-texel shift) and floored in
  the shader, then texture window, page, 4/8/15-bit decode and CLUT lookup on `vram1x`, as `FetchTexel`.
  Texel 0000h discards. Modulation `(t5 * c8) >> 4`, raw `t5 << 3`.
- **Dithering**: the 4×4 table indexed by (pixel / s) & 3, applied in the 8-bit domain before `>> 3`.
- **Semi-transparency**: fixed-function blending on c5/31 values: B/2+F/2 = `ONE, CONSTANT(0.5)` with
  the shader halving F; B+F = `ONE, ONE`; B−F = reverse subtract; B+F/4 = `ONE, ONE` with the shader
  writing `c5 >> 2`. Untextured semi primitives blend all pixels. Textured ones are drawn twice: opaque
  pipeline keeping texels with bit 15 clear, then the blend pipeline keeping texels with bit 15 set.
  B/2+F/2 rounds where the software truncates (≤ 1 step of 31).
- **Mask bit**: the set bit (E6h.0) and texel bit 15 go to alpha. The check bit (E6h.1) needs a
  destination test (planned: a stencil plane mirroring alpha); for now it falls back (below).
- **Draw area**: scissor rect (x1·s, y1·s)–((x2+1)·s, (y2+1)·s).

**Fallback** (unsupported: lines and polylines, mask-checked primitives): the software 1x rasterizer
draws them as always; before the next packet their rect (draw area for polylines) is re-uploaded from the
1x VRAM and nearest-upscaled into `hi`. One log line per kind:
`[gpu] fallback: lines drawn by the software rasterizer at 1x (nearest upscale)`.
24bpp display (FMV) is presented by the software path at 1x; the GPU work is still flushed.

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
presented picture.

## Scale up to 9x (4K TVs)

The scale is not capped at 4: `--renderer gpu` accepts `--resolution-scale 1-9` (240 × 9 = 2160 lines)
and `--resolution-scale auto`, which picks ceil(desktop height / 240) (2160p → 9, 1440p → 6,
1080p → 5). The CPU cost does not depend on s; the GPU cost is fill rate (about 81× the 1x pixels at
9x, a few ms on a desktop GPU) plus the readback (below). The settings file still stores 1-4; a
`"resolution_scale": "auto"` value belongs with the launcher work.

## Widescreen

Widescreen (GTE projection, another branch) will push geometry outside the 320-wide draw area. The
renderer keeps no 4:3 assumption: draw area clip = scissor and display size = `Present(d)`'s rect, so a
wider draw area and a wider display rect just work, provided the hack also widens the draw area/display
rect it reports. If the game's buffers are packed so a wider area would overlap other VRAM, `hi` can be
allocated with extra columns and the display pass widened, without touching the 1x VRAM.

## Composing with `--interp`

The interpolator snapshots the 1x VRAM and replays the GP0 command log at a fraction t. With this
renderer the same log already reaches `OnGp0`; interpolation becomes: keep frame N's draw list, replay
frame N+1's list with lerped vertices into a second `hi` target (a GPU snapshot of `hi`/`vram1x` at the
flip = one texture copy, not 1 MB CPU copies), present it. That removes the "scale > 1 cannot be combined
with --interp" restriction and makes 4x@120 a GPU job. Not implemented yet; `--interp` with
`--renderer gpu` falls back to software with a log line.

## Expected and measured cost

CPU: the 1x rasterizer (unchanged) plus packet parsing (microseconds per frame). GPU: a few hundred draw
calls per field frame; fill rate is trivial at 4x. The prototype reads every picture back to the CPU
(fence wait + copy, feeding the existing `SDL_Renderer` streaming texture, `--screenshot`, `--dump-dir`):
that stall is most of its present time. Measured numbers: see the PR / `--stats` (`frame_ms` = emulation
incl. CPU raster, `gpu_ms` = submit + wait + readback).

## Next steps

1. Present without readback: `SDL_CreateGPURenderer` on the same device, wrap `out` as an `SDL_Texture`;
   read back only for `--screenshot`/`--dump-dir`.
2. Mask check via a stencil plane; lines as thin quads at s×.
3. Offline SPIR-V/MSL (SDL_shadercross) for Vulkan/Metal.
4. `--interp` on the GPU (above); settings/launcher entries for the renderer and `auto` scale.
5. HD texture replacement (`--mods`) is ignored by the GPU path for now (it reads `vram1x` only).
