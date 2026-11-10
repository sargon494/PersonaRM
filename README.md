# 🎮 PersonaRM — Native Launcher & Port Engine

> **Modern PC Launcher & Native Port Hub for Persona 2: Eternal Punishment (PS1)**

[![License: GPL v3](https://img.shields.io/badge/License-GPL%20v3+-blue.svg)](./LICENSE)
[![Platform: Windows / Linux](https://img.shields.io/badge/Platform-Windows%20%7C%20Linux-lightgrey.svg)](#build--installation)
[![Game: Persona 2 EP](https://img.shields.io/badge/Target%20Game-Persona%202%3A%20Eternal%20Punishment-purple.svg)](#about-personarm)
[![CI Status](https://github.com/sargon494/PersonaRM/actions/workflows/ci.yml/badge.svg)](https://github.com/sargon494/PersonaRM/actions)

---

## 📖 About PersonaRM

**PersonaRM** is an open-source native PC runtime engine and launcher hub for **Persona 2: Eternal Punishment**.

Inspired by modern community native recompilations and ports like **[MHForever](https://github.com/Mango77x/MHForever)**, PersonaRM adopts a strict **BYOC (Bring Your Own Copy)** architecture:
- **No Copyrighted Files**: The project distributes zero game binaries, copyrighted ROMs, or game assets.
- **Player-Provided Disc Image**: Players supply their own legal copy of *Persona 2: Eternal Punishment* (`.iso`, `.bin`, `.cue`).
- **Modern Enhancements**: Provides modern widescreen viewports (16:9 and 21:9), controller mapping with dynamic glyphs (PlayStation DualSense & Xbox), high-performance turbo speed, and instant quicksaving.

> 📚 **New to the project?** Read our **[Developer & Contributor Guide](docs/DEVELOPER_GUIDE.md)** and **[System Architecture](ARCHITECTURE.md)** for a complete walkthrough of the codebase and execution pipeline.

---

## 🚦 Implementation Status

The project is actively developed in modular stages. All implemented subsystems are covered by automated unit and integration tests across Windows and Linux.

| Subsystem / Feature | Status | Notes |
|---|---|---|
| **BYOC Launcher & Config** | ✅ Prototype | Key-value config (`personarm_config.json`), automated binary scanning |
| **Dual-Disc Manager (P2 EP)** | ✅ Working | Detects Disc 1 & Disc 2 (`SLPS-02785`, `SLPS-02786`, `SLUS-01158`, `SLUS-01339`) |
| **Disc Loader & CUE Parser** | ✅ Working | ISO 9660 & raw 2352 CD-ROM XA Mode 2 Form 1, `.cue` sheet resolution |
| **PS-X EXE Loader** | ✅ Working | PS-X and ELF header parsing, code segment loading with memfill |
| **MIPS-I CPU Interpreter** | ✅ Working | R3000A instruction set, branch & load delay slots, COP0 `RFE` |
| **GTE (Geometry Transform)** | ✅ Working | COP2 vector-matrix math, RTPS/RTPT, NCLIP, AVSZ3/AVSZ4 |
| **PS1 GPU & VRAM** | ✅ Working | 1 MB VRAM (1024x512), GP0/GP1 packet processors |
| **Widescreen Viewport** | ✅ Working | Original 4:3 (pillarboxed), 16:9 Widescreen, 21:9 UltraWide, Fill |
| **MDEC Video Decoder** | ✅ Working | 8x8 IDCT, PlayStation quantization, YCbCr→RGBA macroblocks |
| **SPU 24-Voice Mixer** | ✅ Working | 24 audio voices, ADPCM decoding, stereo mixing, `.VAG` decoder |
| **BIOS HLE Direct-RAM** | ✅ Working | Syscall vectors A (`0xA0`), B (`0xB0`), C (`0xC0`) executing on RAM |
| **Controller SPI Bus** | ✅ Working | Hardware port at `0x1F801040`/`0x1F801044`, 5-step serial SPI exchange |
| **Turbo Mode** | ✅ Working | Dynamic execution multiplier (1x up to 16x) with hold detection |
| **Save Manager** | ✅ Working | Virtual 128 KB PS1 Memory Card & PRMSTATE QuickSave manager |
| **Physical SDL2/OpenGL Display**| 🟡 In Progress | Headless presentation pipeline working; physical GL/SDL window in progress |

---

## 📐 Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                          PersonaRM Launcher & Portal                        │
│  ┌──────────────────────┐  ┌──────────────────────┐  ┌───────────────────┐  │
│  │ Display & Widescreen │  │ Controls & Gamepad   │  │ Dual-Disc Manager │  │
│  │ (1080p/4K, 16:9/21:9)│  │ (DualSense/Xbox/KBM) │  │ (Disc 1 / Disc 2) │  │
│  └──────────────────────┘  └──────────────────────┘  └───────────────────┘  │
└──────────────────────────────────────┬──────────────────────────────────────┘
                                       │ (Boots Game / Swaps Disc)
                                       ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                   PersonaRM Engine (Central Coordinator)                    │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │ Memory Bus Router (KUSEG/KSEG0/KSEG1 Virtual-to-Physical Translation) │  │
│  └───────┬─────────────┬─────────────┬─────────────┬─────────────┬───────┘  │
│          ▼             ▼             ▼             ▼             ▼          │
│     Main RAM      Scratchpad       PS1 GPU       PS1 SPU     Joypad/MC SPI  │
│      (2 MB)         (1 KB)        (1MB VRAM)   (24 Voices)   (0x1F801040)   │
└─────────────────────────────────────────────────────────────────────────────┘
```

For full hardware specifications and bus routing details, see **[ARCHITECTURE.md](ARCHITECTURE.md)**.

---

## 🛠️ Build & Installation

### Prerequisites

- **CMake** ≥ 3.20
- **Ninja** or standard Make
- **Python 3** (for integration tests)
- **C++17 Compiler**:
  - Windows: Visual Studio 2022 Build Tools (MSVC) or Clang
  - Linux: GCC ≥ 9 or Clang

### Building on Windows (MSVC)

Open a **Developer PowerShell for VS 2022**:
```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```
The executable is produced at `build/Release/personarm.exe`.

### Building on Linux (GCC / Clang)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```
The executable is produced at `build/Release/personarm`.

---

## 🧪 Testing & Verification

PersonaRM includes both C++ unit tests and a multi-platform Python integration test runner:

```bash
# 1. Run C++ Unit Tests (22+ subsystem test cases)
./build/Release/personarm_tests

# 2. Run Automated Integration Test Suite
python tests/integration/run_tests.py
```

### Command-Line Diagnostics & Self-Tests

You can invoke individual subsystem self-tests directly from the CLI:

```bash
# Component Self-Tests
./personarm --test-portal       # Verify PersonaRM Portal & BYOC disc management
./personarm --test-engine       # Verify unified core engine & frame execution loop
./personarm --test-window       # Verify window context & GL presentation pipeline
./personarm --test-mdec         # Verify PS1 MDEC video macroblock IDCT decoder
./personarm --test-save         # Verify 128KB Memory Card & QuickSave manager
./personarm --test-gpu          # Verify PS1 GPU VRAM & GP0/GP1 commands
./personarm --test-bios         # Verify PS1 BIOS HLE system call dispatcher
./personarm --test-gte          # Verify GTE 3D math & perspective transformations
./personarm --test-spu          # Verify PS1 SPU 24-voice audio mixer
./personarm --test-str          # Verify PS1 STR/CD-ROM XA video demuxer
./personarm --test-performance  # Run engine frametime benchmarks
```

---

## 🤝 Contributing

Contributions are welcome! Please read **[CONTRIBUTING.md](CONTRIBUTING.md)** and **[docs/DEVELOPER_GUIDE.md](docs/DEVELOPER_GUIDE.md)** before submitting pull requests.

## 📄 License

PersonaRM is licensed under the **GNU General Public License v3.0 or later (GPL v3+)**. See [LICENSE](./LICENSE) for details.
