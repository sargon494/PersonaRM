<div align="center">

<img src="site/assets/logo.png" alt="PersonaRM" width="360" />

# PersonaRM

**A native PC runtime and launcher for *Persona 2: Eternal Punishment* (PlayStation 1)**

[![License: GPL v3+](https://img.shields.io/badge/License-GPL%20v3%2B-d7172a.svg)](./LICENSE)
[![Platform: Windows | Linux](https://img.shields.io/badge/Platform-Windows%20%7C%20Linux-lightgrey.svg)](#building-from-source)
[![Language: C++17](https://img.shields.io/badge/C%2B%2B-17-00599C.svg)](#building-from-source)
[![CI](https://github.com/sargon494/PersonaRM/actions/workflows/ci.yml/badge.svg)](https://github.com/sargon494/PersonaRM/actions)
[![Project site](https://img.shields.io/badge/Website-GitHub%20Pages-6e0b14.svg)](https://sargon494.github.io/PersonaRM/)

[Website](https://sargon494.github.io/PersonaRM/) · [Status](#status) · [Quick start](#quick-start) · [Building](#building-from-source) · [Documentation](#documentation) · [Contributing](#contributing)

</div>

---

## Overview

PersonaRM is an open-source runtime that executes *Persona 2: Eternal Punishment* natively on Windows and Linux. It emulates the PlayStation 1 hardware the game depends on (MIPS R3000A CPU, Geometry Transform Engine, GPU, MDEC video decoder, SPU audio, and controller interface), and wraps it in a launcher that manages your disc images, controls, and display options.

PersonaRM follows a strict **Bring Your Own Copy (BYOC)** model:

- **No game content is distributed.** The project contains no game binaries, ROMs, or assets.
- **You supply your own disc.** Provide a legally obtained image (`.cue`/`.bin`, `.iso`, or `.img`) from a copy you own.
- **Modern enhancements.** Widescreen viewports, controller glyphs for PlayStation, Xbox, and Nintendo layouts, turbo speed up to 16×, and quicksave.

New to the codebase? Start with the [Developer Guide](docs/DEVELOPER_GUIDE.md) and the [Architecture overview](ARCHITECTURE.md).

## Features

| Area | What it does |
|---|---|
| **Disc management** | Detects Disc 1 and Disc 2 of the game (`SLPS-02785`, `SLPS-02786`, `SLUS-01158`, `SLUS-01339`), and swaps discs at runtime. |
| **Disc loading** | ISO 9660 and raw 2352-byte CD-ROM XA Mode 2 Form 1 images, with `.cue` sheet resolution. |
| **Executable loading** | PS-X EXE and ELF header parsing, with code segment loading and memory fill. |
| **CPU** | MIPS-I (R3000A) interpreter with branch and load delay slots and COP0 `RFE`. |
| **Geometry** | GTE (COP2) vector and matrix math, including `RTPS`/`RTPT`, `NCLIP`, and `AVSZ3`/`AVSZ4`. |
| **Graphics** | PS1 GPU with 1 MB VRAM (1024×512), GP0/GP1 packet processing, and an MDEC macroblock decoder. |
| **Audio** | SPU with 24 voices, ADPCM decoding, stereo mixing, and `.VAG` support. |
| **Display** | 4:3 (pillarboxed), 16:9, 21:9 UltraWide, and Fill viewports. |
| **Input** | Keyboard, mouse, and gamepad with auto-detected profiles for PlayStation, Xbox, and Nintendo controllers. |
| **Speed** | Turbo mode from 1× to 16×, activated by hold detection. |
| **Saves** | Virtual 128 KB PS1 memory card and instant quicksave (`PRMSTATE`). |

## Status

PersonaRM is under active development. Each subsystem is covered by automated unit and integration tests on Windows and Linux.

| Subsystem | Status |
|---|---|
| BYOC launcher and configuration | ✅ Prototype |
| Dual-disc manager and disc loader | ✅ Working |
| PS-X EXE / ELF loader | ✅ Working |
| MIPS-I CPU interpreter | ✅ Working |
| GTE (geometry engine) | ✅ Working |
| PS1 GPU and VRAM | ✅ Working |
| MDEC video decoder | ✅ Working |
| SPU 24-voice mixer | ✅ Working |
| BIOS HLE (syscalls A0, B0, C0) | ✅ Working |
| Controller SPI bus | ✅ Working |
| Turbo mode | ✅ Working |
| Save manager (memory card and quicksave) | ✅ Working |
| Windowed SDL2 / OpenGL display | 🟡 In progress (headless presentation works) |

## Quick start

> **Note:** The on-screen SDL2/OpenGL window is still in progress, so the current build is a technical preview for testing and development.

**Requirements**

- A legally obtained copy of *Persona 2: Eternal Punishment*, Disc 1 and/or Disc 2.
- Windows 10/11 or a modern Linux distribution.

**Usage**

```bash
# Find disc images in a folder
personarm --scan "/path/to/your/discs"

# Check that a disc image is recognized
personarm --verify-disc "/path/to/disc1.cue"

# Link both discs
personarm --disc1 "/path/to/disc1.cue" --disc2 "/path/to/disc2.cue"

# Open the interactive Portal (game hub and configuration)
personarm --portal

# Start the game
personarm --launch

# Show every option
personarm --help
```

Settings are stored in `personarm_config.json`.

## Building from source

### Prerequisites

- CMake 3.20 or newer
- Ninja, or a standard Make toolchain
- Python 3 (for integration tests)
- A C++17 compiler:
  - **Windows:** Visual Studio 2022 Build Tools (MSVC) or Clang
  - **Linux:** GCC 9 or newer, or Clang

### Windows (MSVC)

Open a *Developer PowerShell for VS 2022* and run:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The executable is written to `build/Release/personarm.exe`.

### Linux (GCC or Clang)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

The executable is written to `build/Release/personarm`.

## Testing

PersonaRM ships C++ unit tests and a cross-platform Python integration runner.

```bash
# C++ unit tests
./build/Release/personarm_tests

# Integration suite
python tests/integration/run_tests.py
```

Each subsystem also exposes a self-test flag:

```bash
./personarm --test-portal     # Portal and disc management
./personarm --test-engine     # Engine core and frame loop
./personarm --test-window     # Window context and presentation
./personarm --test-mdec       # MDEC macroblock decoder
./personarm --test-save       # Memory card and quicksave
./personarm --test-gpu        # GPU VRAM and GP0/GP1 commands
./personarm --test-bios       # BIOS HLE syscall dispatcher
./personarm --test-gte        # GTE math and projection
./personarm --test-spu        # SPU voice mixer
./personarm --test-str        # STR / CD-ROM XA demuxer
./personarm --test-performance  # Frame-time benchmarks
```

## Architecture

```
┌──────────────────────────────────────────────────────────────┐
│  Launcher & Portal                                           │
│  Display · Controls · Disc manager                           │
└──────────────────────────────┬───────────────────────────────┘
                               ▼
┌──────────────────────────────────────────────────────────────┐
│  Engine (central coordinator)                                │
│  Memory bus: KUSEG / KSEG0 / KSEG1 address translation       │
├──────────┬────────────┬────────────┬────────────┬────────────┤
│ Main RAM │ Scratchpad │  GPU+VRAM  │    SPU     │ Joypad/MC  │
│  2 MB    │   1 KB     │   1 MB     │ 24 voices  │ SPI 0x1F80 │
└──────────┴────────────┴────────────┴────────────┴────────────┘
```

Full hardware specifications and bus routing are in [ARCHITECTURE.md](ARCHITECTURE.md).

## Documentation

| Document | Contents |
|---|---|
| [Architecture](ARCHITECTURE.md) | Hardware model, memory bus, and execution pipeline |
| [Developer Guide](docs/DEVELOPER_GUIDE.md) | Codebase walkthrough and workflow |
| [Gameplay and Controls](docs/GAMEPLAY_AND_CONTROLS.md) | Controller mapping and in-game options |
| [Project Audit](docs/PROJECT_AUDIT.md) | Technical audit and known issues |
| [Contributing](CONTRIBUTING.md) | Bug reports, feature requests, and pull request rules |

## Contributing

Contributions are welcome. Please read [CONTRIBUTING.md](CONTRIBUTING.md) and the [Developer Guide](docs/DEVELOPER_GUIDE.md) before opening a pull request. Bug reports and feature requests go in the issue tracker.

## Legal notice

PersonaRM is an independent, unofficial project. It is not affiliated with, endorsed by, or sponsored by Atlus, Sega, or Sony Interactive Entertainment. The project does not distribute game discs, binaries, ROMs, or assets; users must supply their own legally obtained copy. All trademarks belong to their respective owners.

## License

PersonaRM is licensed under the **GNU General Public License v3.0 or later**. See [LICENSE](./LICENSE).