# PersonaRM — Developer & Contributor Guide

Welcome to the **PersonaRM** developer guide. This document is designed so that any engineer opening this repository for the first time can immediately understand the architecture, design choices, codebase structure, and workflows.

---

## 🧭 1. Project Philosophy: The BYOC Model

PersonaRM is an open-source native PC port runtime for *Persona 2: Eternal Punishment* (PS1).

Following the design of successful community native ports such as **[MHForever](https://github.com/Mango77x/MHForever)** (*Monster Hunter Freedom Unite* PC Port):
- **Zero Copyrighted Assets in Git**: The repository contains no game binaries, ISOs, audio files, textures, or copyrighted Atlus/Sony assets.
- **Bring Your Own Copy (BYOC)**: The user provides their own legally obtained disc image (`.iso`, `.bin`, `.cue`).
- **The Launcher / Portal**:
  1. Detects and verifies the user's disc image (identifies serials like `SLPS-02785` or `SLUS-01158`).
  2. Mounts the disc filesystem (ISO 9660 / Mode 2 Form 1 CD-ROM XA).
  3. Provides modern settings (widescreen viewports, 4K internal scaling, modern controller glyphs, turbo multiplier).
  4. Manages multi-disc switching (Persona 2 EP is a 2-disc game: Disc 1 Main Story, Disc 2 Special Preview Disc / Bonus Disc).
  5. Boots the native runtime engine.

---

## 🗂️ 2. Repository & Codebase Directory Tour

The source code is organized into modular subsystems under `src/`:

```
PersonaRM/
├── src/
│   ├── main.cpp                 # Application entry point, CLI flag dispatcher, self-test harnesses
│   │
│   ├── launcher/                # BYOC Launcher, Settings Hub, and User Portal
│   │   ├── launcher.h/.cpp      # Config management (personarm_config.json), game copy verification
│   │   └── portal.h/.cpp        # Dual-disc portal (Disc 1 & Disc 2), interactive menu, disc swapper
│   │
│   ├── engine/                  # Central Hardware Coordinator
│   │   └── engine.h/.cpp        # Frame loop (~564,480 cycles), physical memory bus router, turbo mode
│   │
│   ├── cpu/                     # Processor & Mathematics
│   │   ├── cpu_context.h/.cpp   # MIPS-I (R3000A) interpreter, pipeline delay slots, registers, RFE
│   │   └── gte.h/.cpp           # Geometry Transformation Engine (COP2), 3D math, perspective, clipping
│   │
│   ├── rendering/               # Video & Presentation Pipeline
│   │   ├── ps1_gpu.h/.cpp       # PS1 GPU hardware emulation, 1MB VRAM, GP0/GP1 packet processors
│   │   ├── window_context.h/.cpp# Window & display surface abstraction (headless & windowed modes)
│   │   ├── gl_renderer.h/.cpp   # Presentation renderer, dynamic viewports (4:3, 16:9, 21:9, Fill)
│   │   ├── mdec_decoder.h/.cpp  # PS1 MDEC video decoder: 8x8 IDCT, YCbCr->RGBA macroblocks
│   │   └── renderer.h/.cpp      # Viewport letterbox/pillarbox calculation helpers
│   │
│   ├── audio/                   # Sound & SPU Emulation
│   │   ├── spu_emulator.h/.cpp  # 24-voice hardware SPU mixer, ADPCM decoding, stereo output
│   │   ├── vag_decoder.h/.cpp   # Sony SPU ADPCM sample decoder with hardware predictor filters
│   │   └── audio_system.h/.cpp  # 4-channel logical mixer (Master, BGM, SFX, Voice)
│   │
│   ├── loader/                  # Disc, Executable, and Media Loaders
│   │   ├── disc_reader.h/.cpp   # ISO 9660 & Mode 2/2352 CD-ROM XA reader, .cue parser, serial identification
│   │   ├── exe_loader.h/.cpp    # PS-X EXE (and ELF) parser, memory layout loader with memfill
│   │   ├── ps1_bios_hle.h/.cpp  # BIOS HLE dispatcher: vector tables A/B/C operating directly on RAM
│   │   ├── tim_parser.h/.cpp    # PS1 .TIM texture decoder (4-bit, 8-bit, 15-bit, 24-bit to RGBA)
│   │   └── str_demuxer.h/.cpp   # CD-ROM XA video stream demuxer for FMV playback
│   │
│   ├── input/                   # Virtual Controls & Hardware Controller Bus
│   │   └── input_manager.h/.cpp # Action mappings, glyph generator (DualSense/Xbox), SPI serial bus
│   │
│   └── core/                    # Low-level Utilities & State Management
│       ├── memory_manager.h/.cpp# Pooled, Buddy, and Arena memory allocators
│       ├── save_manager.h/.cpp  # 128 KB PS1 Memory Card emulation & PRMSTATE QuickSave manager
│       └── logger.h/.cpp        # Thread-safe multi-sink diagnostics logger
│
├── tests/
│   ├── unit/
│   │   └── test_core.cpp        # Comprehensive C++ unit test suite (22+ subsystem test cases)
│   └── integration/
│       └── run_tests.py         # Automated Python integration test harness
│
├── docs/                        # Architecture, research, gameplay, and developer documentation
└── .github/workflows/ci.yml     # GitHub Actions multi-platform CI matrix (Windows & Linux)
```

---

## ⚡ 3. Engine Execution Pipeline

The core execution cycle works as follows:

1. **Boot**:
   - `Launcher` or `Portal` verifies the disc image and extracts the boot executable (`DetectBootExecutable()` parses `SYSTEM.CNF`).
   - `Engine::BootFromDisc()` calls `ExeLoader::LoadExe()`, setting the initial Program Counter (PC), Stack Pointer (SP), and Global Pointer (GP), and loading code segments into the 2 MB Main RAM.
2. **Frame Loop (`Engine::RunFrame`)**:
   - Runs **564,480 cycles** per frame (representing ~1/60th second at 33.8688 MHz).
   - If Turbo mode is active (e.g. `VirtualAction::Turbo` held), cycle count scales by `m_speed_multiplier`.
   - Each cycle calls `CpuContext::Step()`:
     - Fetches instruction at PC via `ReadBus()`.
     - Advances PC (with delay slot handling).
     - Intercepts BIOS system calls at `0xA0`, `0xB0`, `0xC0` using `Ps1BiosHle`.
   - Dispatches GPU GP0/GP1 commands to `Ps1Gpu`.
   - Routes controller polling exchanges at `0x1F801040` (JOY_DATA).
3. **Presentation**:
   - `GlRenderer::CalculateViewport()` computes aspect ratio (Original 4:3, Widescreen 16:9, or UltraWide 21:9).
   - `Engine::GetCurrentFrame()` converts VRAM into RGBA for display.

---

## 💿 4. Multi-Disc Architecture (Persona 2: Eternal Punishment)

Persona 2 EP is a 2-disc title:
- **Disc 1**: Main Game Story (`SLPS-02785` JP Deluxe, `SLPS-02825` JP Standard, `SLUS-01158` US).
- **Disc 2**: *Sumaru TV Special Preview Disc* (`SLPS-02786` JP Deluxe) or Second Game Disc (`SLUS-01339` US, `SLPS-02826` JP Standard).

The `Portal` maintains independent slots for **Disc 1** and **Disc 2**, allows configuring both paths, and handles runtime disc swapping via `Portal::SwapDisc()`.

### Physical Disc Formats Supported
- **ISO 9660 Mode 1**: Standard 2048 bytes/sector.
- **CD-ROM XA Mode 2 Form 1**: Raw 2352 bytes/sector (24 bytes header/subheader, 2048 bytes user data, 280 bytes ECC/EDC).
- **CUE Sheets (`.cue`)**: Automatically parsed to find the accompanying `.bin` raw image.

---

## 🛠️ 5. Building & Compiling

### Windows (MSVC 2022)
Open *Developer PowerShell for VS 2022*:
```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```
Binaries will be placed in `build/Release/personarm.exe`.

### Linux (GCC or Clang)
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```
Binaries will be placed in `build/Release/personarm`.

---

## 🧪 6. Testing & CI Strategy

PersonaRM maintains strict test coverage. Every PR must pass all 8 GitHub Actions jobs (Windows & Linux matrices for Build, Unit Tests, Code Quality, and Integration Tests).

### Running Unit Tests
```bash
./build/Release/personarm_tests
```

### Running Integration Tests
```bash
python tests/integration/run_tests.py
```

### Command-Line Self-Tests
The `personarm` executable contains built-in self-test flags:
- `--test-engine`: Unified core engine and frame execution loop.
- `--test-portal`: PersonaRM Portal & BYOC dual-disc management.
- `--test-window`: Window context & GL presentation pipeline.
- `--test-mdec`: PS1 MDEC video macroblock IDCT decoder.
- `--test-save`: 128 KB Memory Card & QuickSave state manager.
- `--test-performance`: Engine frametime benchmarks.
- `--test-gpu`: PS1 GPU VRAM & GP0/GP1 command processing.
- `--test-bios`: BIOS HLE system call dispatcher.
- `--test-gte`: GTE vector and matrix mathematics.
- `--test-spu`: SPU 24-voice audio mixer.
- `--test-str`: CD-ROM XA STR video demuxer.
- `--test-tim`: TIM texture format parser.
- `--test-vag`: VAG SPU audio decoder.

---

## 📝 7. Coding Standards

- **Standard**: Modern C++17.
- **Dependencies**: Keep core dependencies zero/minimal. Subsystems use modular interfaces so tests run headless without requiring physical GPU or audio hardware.
- **Namespace**: All engine types live under `namespace personarm`.
- **Formatting**: 4 spaces indentation, no tabs.
- **Commits**: Follow [Conventional Commits](https://www.conventionalcommits.org/) (`feat:`, `fix:`, `docs:`, `test:`, `refactor:`).



## Análisis de discos (`psx_analyze`)

Herramienta de línea de comandos que lee una imagen de disco de PS1 y genera un informe Markdown con **solo metadatos** (nombres, tamaños, direcciones y contadores; nunca contenido del juego). Se compila junto al resto del proyecto y queda en `build/Release/psx_analyze`.

```
psx_analyze <disco.cue|.bin|.iso> [<disco2> ...] [--out informe.md] [--hash] [--max-scan-mb N] [--full-paths]
```

- Acepta ISO de 2048 bytes y BIN/CUE de 2352 bytes. Para discos de dos CD, pasa ambos en la misma ejecución.
- `--hash` añade el CRC32 de la imagen (tarda en discos de 700 MB). `--full-paths` muestra la ruta tal cual; por defecto solo se imprime el nombre del archivo.
- El informe incluye: serial reconocido, `SYSTEM.CNF`, cabecera del ejecutable de arranque, accesos directos a hardware, llamadas a BIOS, candidatos a overlay, tipos de archivo y tipo de sector.
- Las secciones marcadas *heurística* son una lectura lineal de instrucciones MIPS-I, no un desensamblado: sirven para decidir, no para dar nada por demostrado.
- Antes de compartir un informe, revisa que no contenga nada que no quieras publicar. No subas imágenes de disco ni archivos extraídos al repositorio.
