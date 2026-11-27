# Native code (recompiled) on your PC

PersonaRM runs the game's MIPS code natively: it translates the code of **your own disc** into C and
compiles it **on your PC** the first time you play. Nothing derived from the game is ever shipped or
downloaded; the release zip holds only PersonaRM's own programs and TinyCC.

## What you see

1. Press **PLAY** in the launcher. The first time for a disc (or after a PersonaRM update) the launcher
   shows *Preparing native code for your disc... (first time only)* with a progress bar. It takes a few
   seconds (Japanese disc 1 on a desktop PC: about 8-16 s).
2. The game starts with native code. Later launches skip the preparation (it answers at once).
3. If anything goes wrong (unreadable disc, unknown game, no space), the game starts anyway with the
   interpreter, and the launcher shows why when it comes back.

Turn it off under **Settings > General > Native code (recompiled)** (the setting `recomp` of
docs/SETTINGS.md; `--no-recomp` on the command line). The game is identical either way: the native code
produces the same frames and RAM as the interpreter, only faster where the CPU is the bottleneck.

## How it works

| Piece | Where |
|---|---|
| `personarm_native <disc.cue> [--json]` reads the disc, finds the code, generates C, compiles it with TinyCC | `tools/personarm_native/` |
| Overlay discovery: each FILEPOS file is tried as code; its load address is the base that puts the most of its own `jal` targets on its function starts (docs/research/OVERLAY_MAP.md, method step 4). A wrong guess costs nothing: a module only runs while RAM holds exactly the code it was built from (code hash) | `native_prep.cpp` |
| C ABI between the game and the generated code: `prm_cpu`, `prm_module`, export `prm_modules` (version `PRM_ABI_VERSION`) | `src/recomp/runtime/prm_abi.h` |
| Loading the DLL at run time | `src/recomp/runtime/module_loader.*` |
| Cache: `%LOCALAPPDATA%\PersonaRM\native\<boot exe>-<FNV-1a 64 of the exe>\modules.dll` + `manifest.json`; Linux: `$XDG_CACHE_HOME/personarm/native/.../modules.so` (default `~/.cache`) | `src/recomp/runtime/native_cache.*` |

The cache is rebuilt when the module ABI, the generator version (`kGeneratorVersion`), the PersonaRM
build or the executable changes. `PRM_NATIVE_CACHE` overrides the cache folder. Delete the folder to
force a rebuild, or run `personarm_native <disc.cue> --force`.

`--json` prints one object per line: `{"stage":"compile","progress":0.42,"message":"..."}` and finally
`{"stage":"done","ok":true,...}` or `{"stage":"error","ok":false,"message":"..."}`. `--check` exits 0 when
the cache is ready and 3 when it is not.

### Compiler choice

TinyCC (libtcc, LGPL-2.1) is fetched at a pinned commit by `cmake/TinyCC.cmake`, built as `libtcc.dll`
(Linux: `libtcc.so`) with its predefined macros compiled in, and the generated code is built
`-nostdlib`: the library is the whole compiler runtime (no include or lib folders). Its licence and
source offer ship in `licenses/`. On Linux, TinyCC writes no `PT_GNU_STACK` header, and glibc 2.41+
refuses to load a library without one (it would need an executable stack); `personarm_native` adds a
non-executable one after linking (`AddGnuStackHeader`).

Measured on JP disc 1 (new game to the first field, `docs/examples/disc1_new_game_to_field.txt`,
19 000 frames, headless `personarm --boot`, 107 modules / 11 750 functions):

| | prepare / build | code size | frames/s (whole run) | vs interpreter |
|---|---|---|---|---|
| interpreter | - | - | 109.5 | - |
| TinyCC, on the PC | 8.3 s (compile 2.6 s) | 115 MB DLL | 179.7 | RAM and 190/190 frames identical |
| MSVC /O2, linked (developers) | 457 s (whole build) | ~45 MB in the exe | 274.5 | RAM and 190/190 frames identical |

In the field scene itself (f18 840-19 260, `personarm_sdl --stats`) the CPU is not the bottleneck: about
6 ms per frame in all three. The US disc (`docs/examples/us_new_game_to_field.txt`, 9 000 frames):
92.0 frames/s interpreted, 209.5 with TinyCC, RAM and 90/90 frames identical.

## Developers

`personarm_native <disc.cue> --emit-c <dir outside the repo>` writes the same C (with one comment per
instruction) for `-DPRM_RECOMP_GENERATED_DIR=<dir>`, which links it into the build with MSVC/Clang
optimisation. Linked modules win over the cache. `tools/recomp_gen` still translates a single image.
Generated code is never committed.
