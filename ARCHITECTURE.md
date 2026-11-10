# Architecture — PersonaRM (Native Launcher & Port Suite)

## 🏛️ System Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                    PersonaRM Launcher & Engine                  │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │                    PersonaRM Launcher                     │  │
│  │  ┌───────────────┐ ┌─────────────────┐ ┌───────────────┐  │  │
│  │  │ Video Config  │ │ Input & Mappings│ │ Asset Scanner │  │  │
│  │  │ (1080p/4K/60) │ │ (KBM / Gamepad) │ │ & ROM Verify  │  │  │
│  │  └───────────────┘ └─────────────────┘ └───────────────┘  │  │
│  └──────────────────────────────┬────────────────────────────┘  │
│                                 │                               │
│  ┌──────────────────────────────▼────────────────────────────┐  │
│  │                    Application Layer                      │  │
│  │  ┌─────────────┐ ┌─────────────────┐ ┌─────────────────┐  │  │
│  │  │   Renderer  │ │ Virtual Input   │ │     Audio       │  │  │
│  │  │ (OpenGL,    │ │ (SDL2 Abstr.,   │ │ (SDL Audio /    │  │  │
│  │  planned)   │ │  planned)       │ │  miniaudio)     │  │  │
│  │  └─────────────┘ └─────────────────┘ └─────────────────┘  │  │
│  └──────────────────────────────┬────────────────────────────┘  │
│                                 │                               │
│  ┌──────────────────────────────▼────────────────────────────┐  │
│  │              Emulation (interpreter, HLE BIOS)            │  │
│  │  ┌─────────────┐ ┌─────────────────┐ ┌─────────────────┐  │  │
│  │  │   Memory    │ │ Instruction     │ │ Disc / ELF      │  │  │
│  │  │   Manager   │ │ Execution / JIT │ │ Loader          │  │  │
│  │  └─────────────┘ └─────────────────┘ └─────────────────┘  │  │
│  └───────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
```

## 📐 Component Architecture

### 1. Launcher Layer (`src/launcher/`)
- **Config Storage**: Manages resolution, aspect ratios (16:9 widescreen patches), framerate limits, and input mappings in `personarm_config.json`.
- **Validation**: Scans directories for user-supplied legitimate game copies (`.iso`, `.bin`, `.cue`, `.elf`).
- **Interactive UI**: CLI menu and graphics frontend for launching into the game seamlessly.

### 2. Virtual Input Layer (`src/input/`)
- Complete abstraction of inputs into semantic actions (`Move`, `Camera`, `Interact`, `Cancel`, `Turbo`, `QuickMenu`).
- Hot-plugging of modern gamepads (Xbox, PlayStation DualSense, Nintendo Switch Pro).
- Full Keyboard & Mouse support: point-and-click menus, mouse look, battle number hotkeys.
- Detailed in [docs/GAMEPLAY_AND_CONTROLS.md](docs/GAMEPLAY_AND_CONTROLS.md).

### 3. Core Engine Layer (`src/core/`, `src/rendering/`, `src/audio/`, `src/loader/`)
- **Memory Manager (`src/core/`)**: Pool, Buddy, and Arena allocators for high performance and low fragmentation under `personarm::` namespace.
- **Diagnostics Logger (`src/core/`)**: Thread-safe multi-sink logging system (`DEBUG`, `INFO`, `WARN`, `ERROR`) outputting to console and logfile.
- **Resource Parsers (`src/loader/`)**:
  - `TimParser`: Decodes PlayStation 1 `.TIM` textures (4-bit/8-bit indexed CLUT, 15-bit, 24-bit direct) directly to 32-bit RGBA buffers.
  - `DiscReader`: Parses ISO 9660 volume descriptors and PS-X executable headers.
- **Audio Synthesizer (`src/audio/`)**:
  - `AudioSystem`: 4-channel audio mixing (Master, BGM, SFX, Voice) with dynamic volume scaling.
  - `VagDecoder`: Decodes PlayStation SPU ADPCM audio (`.VAG`) to 16-bit uncompressed PCM using Sony hardware predictor coefficients.
- **CPU Context (`src/cpu/`)**: MIPS R3000A register file, cycle tracking, and basic instruction decoder.
- **Renderer (`src/rendering/`)**: Planned OpenGL 3.3-core pipeline (not implemented yet) supporting high-resolution rendering, anisotropic filtering, and widescreen expansion.
- **Audio (`src/audio/`)**: Planned audio output via SDL Audio or miniaudio with an in-house mixer (master, BGM, SFX, voice channels). Currently a stub.
