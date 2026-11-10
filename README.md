# 🎮 PersonaRM — Native Launcher & Port Engine

> **Modern PC Launcher & Native Port Hub for Persona 2: Eternal Punishment**

[![License: GPL v3](https://img.shields.io/badge/License-GPL%20v3+-blue.svg)](./LICENSE)
[![Platform: Windows / Linux](https://img.shields.io/badge/Platform-Windows%20%7C%20Linux-lightgrey.svg)](#installation)
[![Game: Persona 2 EP](https://img.shields.io/badge/Target%20Game-Persona%202%3A%20Eternal%20Punishment-purple.svg)](#about)

---

## 📖 About PersonaRM

**PersonaRM** is a native PC launcher and runtime engine designed to run **Persona 2: Eternal Punishment** natively on modern Windows and Linux PCs without relying on heavy emulators.

PersonaRM provides:
1. **The PersonaRM Launcher**: A modern interface for selecting display resolutions (1080p, 1440p, 4K), 60 FPS / VSync settings, native 16:9/21:9 widescreen expansion, audio mixing, and input profile mapping.
2. **Modern Controller & KBM Architecture**: Full support for Keyboard + Mouse (interactive pointer in menus, scroll wheel, combat shortcuts) and modern gamepads (Xbox, DualSense, Switch Pro) with dynamic button glyphs.
3. **Native Recompiled Runtime**: Direct C++17 execution with high performance, instant loading times, and zero emulation lag.

> ⚠️ **Important Legal Notice**: This project **does NOT distribute** any copyrighted game files, ISOs, audio, or assets. PersonaRM requires a **user-provided legitimate copy** of the game to extract and execute assets locally.

---

## 🎯 Key Features & Modernization

| Feature | PersonaRM Implementation |
|---|---|
| **Launcher Hub** | Centralized configuration for graphics, audio, game directories, and mods |
| **Input Support** | Full Keyboard & Mouse (point-and-click menus, WASD) + Gamepad Hot-plugging |
| **Display** | Native 16:9 / 21:9 Widescreen, 4K internal resolution scaling, 60+ FPS |
| **Quality of Life** | Turbo mode (2x/4x toggle), Instant text, Dialogue backlog, Unlimited quick saves |
| **Audio** | Spatial OpenAL Soft 3D audio, independent volume channels |
| **Cross-Platform** | Native Windows (MSVC 2022) & Linux (Clang/GCC) |

For in-depth specifications, read [docs/GAMEPLAY_AND_CONTROLS.md](docs/GAMEPLAY_AND_CONTROLS.md).

---

## 📐 Architecture Overview

```
┌─────────────────────────────────────────────────────────────┐
│                      PersonaRM Launcher                     │
│  ┌───────────────────┐ ┌───────────────────┐ ┌───────────┐  │
│  │ Display / 60 FPS  │ │ Input & KBM Mapper│ │ Legal ROM │  │
│  │ Settings & VSync  │ │ & Glyphs Config   │ │ Validator │  │
│  └───────────────────┘ └───────────────────┘ └───────────┘  │
└──────────────────────────────┬──────────────────────────────┘
                               │ (Boots)
                               ▼
┌─────────────────────────────────────────────────────────────┐
│                   Engine & Emulation Layer                  │
│  ┌─────────────┐ ┌─────────────┐ ┌───────────────────────┐  │
│  │   Renderer  │ │ Audio Engine│ │  Virtual Action       │  │
│  │ (OpenGL 4.6)│ │(OpenAL Soft)│ │  Mapper (SDL2)        │  │
│  └─────────────┘ └─────────────┘ └───────────────────────┘  │
│  ┌─────────────┐ ┌─────────────┐ ┌───────────────────────┐  │
│  │   Memory    │ │ Instruction │ │ Disc / ELF Loader     │  │
│  │   Manager   │ │ Recompiler  │ │ & Asset Streamer      │  │
│  └─────────────┘ └─────────────┘ └───────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

---

## 🛠️ Build & Installation

### Prerequisites (Windows)

```powershell
# Install build tools
choco install cmake ninja python3
```

### Build

```powershell
mkdir build
cd build
cmake .. -G Ninja
cmake --build . --config Release
```

### Run the Launcher

```powershell
# Run the interactive Launcher
.\build\Release\personarm.exe

# Or boot directly with current config
.\build\Release\personarm.exe --launch

# Scan for legal game images (.iso / .bin)
.\build\Release\personarm.exe --scan "D:\Games\Persona2"
```

---

## 📚 Documentation Index

| Document | Description |
|---|---|
| [docs/GAMEPLAY_AND_CONTROLS.md](docs/GAMEPLAY_AND_CONTROLS.md) | Official Controls, KBM, Gamepad, and QoL Specification |
| [docs/PROJECT_AUDIT.md](docs/PROJECT_AUDIT.md) | Architecture audit & component directory map |
| [ARCHITECTURE.md](./ARCHITECTURE.md) | Technical system architecture & subsystem breakdown |
| [CONTRIBUTING.md](./CONTRIBUTING.md) | Contribution guidelines and style guide |
| [LICENSE](./LICENSE) | GNU General Public License v3.0 |

---

Made with ❤️ by the PersonaRM Team.
