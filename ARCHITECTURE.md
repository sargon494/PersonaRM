# Architecture — PersonaRM (Native Launcher & Port Suite)

## 🏛️ System Architecture Overview

**PersonaRM** is an open-source native PC port runtime and hub for *Persona 2: Eternal Punishment*. It follows a **BYOC (Bring Your Own Copy)** architecture inspired by projects like *MHForever*: the repository contains zero copyrighted game assets or ROMs; instead, a modern launcher and portal validates user-provided disc dumps, mounts game data, and boots the native engine.

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
│          │             │             │             │             │          │
│          ▼             ▼             ▼             ▼             ▼          │
│   ┌─────────────┐┌─────────────┐┌─────────────┐┌─────────────┐┌───────────┐ │
│   │ Main RAM    ││ Data Cache  ││ PS1 GPU     ││ PS1 SPU     ││ Joypad/MC │ │
│   │ (2 MB)      ││ Scratchpad  ││ (1 MB VRAM, ││ (24 Voices, ││ SPI Bus   │ │
│   │             ││ (1 KB)      ││ GP0 / GP1)  ││ ADPCM Mixer)││ (0x1F80..)│ │
│   └─────────────┘└─────────────┘└──────┬──────┘└─────────────┘└─────┬─────┘ │
│          ▲                             │                            │       │
│          │                             ▼                            ▼       │
│   ┌──────┴──────┐               ┌─────────────┐              ┌────────────┐ │
│   │ MIPS R3000A │               │ GlRenderer  │              │ Input      │ │
│   │ CPU + GTE   │               │ & Window    │              │ Manager    │ │
│   │ Coprocessor │               │ Context     │              │ (Actions)  │ │
│   └─────────────┘               └─────────────┘              └────────────┘ │
│          │                                                                  │
│          ▼                                                                  │
│   ┌──────────────────────────────────────────────────────────────────────┐  │
│   │ BIOS HLE Dispatcher (Direct RAM Memory/File Operations: A, B, C)     │  │
│   │ MDEC Macroblock Decoder (8x8 IDCT, YCbCr->RGBA, FMV / Movies)        │  │
│   │ DiscReader (ISO 9660 & Mode 2/2352 CD-ROM XA Demuxer)                │  │
│   │ SaveManager (128 KB Virtual Memory Card & Full-State QuickSaves)     │  │
│   └──────────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 📐 Subsystems Breakdown

### 1. Launcher & Portal (`src/launcher/`)
- **`LauncherConfig`**: Key-value configuration store for display settings, input deadzones, audio volumes, and disc image paths (`personarm_config.json`).
- **Dual-Disc BYOC Architecture**:
  - Manages **Disc 1** (Main Story) and **Disc 2** (*Sumaru TV Special Preview Disc* / Deluxe Pack or US Disc 2).
  - Validates disc serials using `DiscReader::IdentifyPersona2` (`SLPS-02785`, `SLPS-02786`, `SLUS-01158`, `SLUS-01339`).
  - Supports automated ROM detection across `./`, `./discs/`, `./roms/`, and `docs/research/`.
- **Runtime Disc Swapping**: Allows hot-swapping active discs without restarting the application.

### 2. Central Engine Coordinator (`src/engine/`)
- **`Engine`**: Singleton central coordinator holding all hardware subsystems.
- **Hardware Clocks**: PS1 CPU runs at 33.8688 MHz; a standard NTSC frame corresponds to ~564,480 cycles.
- **Memory Bus Router (`ReadBus` / `WriteBus`)**:
  - Translates MIPS virtual addresses (KUSEG `0x00000000..0x7FFFFFFF`, KSEG0 `0x80000000..0x9FFFFFFF`, KSEG1 `0xA0000000..0xBFFFFFFF`) to physical memory addresses via bitmask (`address & 0x1FFFFFFF`).
  - Routes accesses to Main RAM, Scratchpad, Controller Bus, GPU, and SPU.
- **Turbo / Fast-Forward**: Dynamic execution speed multiplier (1x up to 16x) with turbo hold action detection.

### 3. CPU & Mathematics (`src/cpu/`)
- **`CpuContext`**: MIPS-I (R3000A) architecture interpreter.
  - Full 32 general-purpose registers (GPRs), Program Counter (PC), HI/LO registers.
  - Branch delay slot and load delay slot emulation.
  - Arithmetic, logical, branch, jump, memory access, and COP0 instructions (including `RFE` - Restore From Exception).
- **`Gte` (Geometry Transformation Engine - Coprocessor 2)**:
  - 3D coordinate transformations (`RTPS`, `RTPT`), vector-matrix multiplications (`MVMVA`).
  - Normal clipping (`NCLIP`) and depth averaging (`AVSZ3`, `AVSZ4`).

### 4. GPU & Rendering (`src/rendering/`)
- **`Ps1Gpu`**: Hardware-accurate PlayStation 1 GPU.
  - 1024x512 16-bit VRAM (1 Megabyte).
  - GP0 command packet FIFO processor (polygons, lines, sprites, VRAM CPU-to-VRAM and VRAM-to-CPU transfers).
  - GP1 control register processor (display mode, reset, DMA direction, display area).
- **`GlRenderer` & `WindowContext`**:
  - Headless/windowed presentation pipeline.
  - Widescreen expansion: Dynamic viewport calculation for Original 4:3 (pillarboxed), Widescreen 16:9, UltraWide 21:9, and Stretch/FillWindow.
- **`MdecDecoder`**:
  - PS1 Macroblock IDCT video decoder.
  - 8x8 2D Inverse Discrete Cosine Transform with PlayStation quantization matrices.
  - Color space conversion from YCbCr (YUV 4:2:0) to 32-bit RGBA.

### 5. Audio & SPU (`src/audio/`)
- **`SpuEmulator`**:
  - 24 independent hardware audio voices.
  - ADPCM decoding, pitch modulation, volume envelopes, stereo mixing.
- **`VagDecoder`**:
  - Decodes Sony SPU ADPCM (`.VAG`) samples into 16-bit signed PCM audio using hardware predictor filters.
- **`AudioSystem`**:
  - 4-channel logical mixer (Master, BGM, SFX, Voice) with dynamic volume scaling.

### 6. Loader & BIOS HLE (`src/loader/`)
- **`DiscReader`**:
  - Reads ISO 9660 file structures and directories.
  - Supports raw 2352-byte CD-ROM images (`MODE2/2352` CD-ROM XA Form 1 and `MODE1/2352`) and `.cue` sheets.
  - `DetectBootExecutable()` parses `SYSTEM.CNF` to locate the main game executable.
  - `IdentifyPersona2()` identifies all known Japanese and US release serials.
- **`ExeLoader`**:
  - Parses PS-X EXE (and ELF) headers.
  - Validates entry point, initial SP base, GP register, and loads code segments into RAM with memfill support.
- **`Ps1BiosHle`**:
  - High-Level Emulation of BIOS vector tables A (`0xA0`), B (`0xB0`), and C (`0xC0`).
  - Operates directly on the physical 2 MB RAM buffer for memory calls (`memcpy` 0x2A, `memset` 0x2B, `bzero` 0x13).
  - File I/O (`FileOpen`, `FileRead`, `FileClose`) routed directly through `DiscReader`.
  - Kernel TTY logging for debug tracking.
- **`TimParser`**:
  - PlayStation `.TIM` texture format parser (4-bit CLUT, 8-bit CLUT, 15-bit direct, 24-bit direct color).
- **`StrDemuxer`**:
  - CD-ROM XA streaming video/audio demuxer for `.STR` FMVs.

### 7. Input & Hardware Controller Bus (`src/input/`)
- **`InputManager`**:
  - Action-based input mapping (`VirtualAction::Confirm`, `Cancel`, `Menu`, `MoveUp`, `Turbo`, etc.).
  - Controller button glyph generator (PlayStation DualSense △ ◯ ✕ ◻, Xbox Y B A X).
- **Controller Serial Bus (SPI)**:
  - Emulated at memory address `0x1F801040` (`JOY_DATA`) and `0x1F801044` (`JOY_STAT`).
  - Full 5-step serial exchange protocol: Device Select (0x01) → Poll (0x42) → Header End (0x5A) → Buttons Byte 1 → Buttons Byte 2.

### 8. Save & State Management (`src/core/`)
- **`SaveManager`**:
  - **Memory Card Emulation**: 128 KB standard PS1 memory card layout (15 slots of 8 KB, directory frames, block allocation tables, XOR checksums).
  - **Modern QuickSave System**: PRMSTATE format serializing CPU registers, full 2 MB RAM, 1 KB Scratchpad, 1 MB VRAM, and frame counter.

---

## 🗺️ Physical Memory Map

The memory bus router in `Engine` maps 32-bit virtual MIPS addresses to the physical PlayStation 1 hardware map:

| Physical Address Range | Size | Hardware Device / Subsystem | Routed Handler |
|---|---|---|---|
| `0x00000000` - `0x001FFFFF` | 2 MB | Main RAM (mirrored up to 8 MB) | `m_ram` buffer |
| `0x1F800000` - `0x1F8003FF` | 1 KB | Data Scratchpad (Fast D-Cache) | `m_scratchpad` buffer |
| `0x1F801040` | 1 Byte | Controller & Memory Card Data (JOY_DATA) | SPI FIFO queue |
| `0x1F801044` | 4 Bytes | Controller Status (JOY_STAT) | Status register & ACK flags |
| `0x1F801810` | 4 Bytes | GPU GP0 Command Register / VRAM FIFO | `Ps1Gpu::WriteGP0` / `ReadGP0` |
| `0x1F801814` | 4 Bytes | GPU GP1 Control Register / GPUSTAT | `Ps1Gpu::WriteGP1` / `ReadGPUSTAT` |
| `0x1F801C00` - `0x1F801DFF` | 512 B | SPU Voice & Control Registers | `SpuEmulator` |
| `0x1FC00000` - `0x1FC7FFFF` | 512 KB | BIOS ROM Area | HLE Dispatcher / NOP traps |
