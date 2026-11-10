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
│  │  │   (OpenGL)  │ │ (SDL2 Abstr.)   │ │ (OpenAL Soft)   │  │  │
│  │  └─────────────┘ └─────────────────┘ └─────────────────┘  │  │
│  └──────────────────────────────┬────────────────────────────┘  │
│                                 │                               │
│  ┌──────────────────────────────▼────────────────────────────┐  │
│  │                  Emulation & Recompiler                   │  │
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

### 3. Core Engine Layer (`src/core/`, `src/rendering/`, `src/audio/`)
- **Memory Manager (`src/core/`)**: Pool, Buddy, and Arena allocators for high performance and low fragmentation.
- **Renderer (`src/rendering/`)**: Modern OpenGL 4.6 pipeline supporting high-resolution rendering, anisotropic filtering, and widescreen expansion.
- **Audio (`src/audio/`)**: OpenAL Soft spatial audio with master, BGM, SFX, and voice channels.
