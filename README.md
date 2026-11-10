# 🎮 PersonaRM — Native Launcher & Port Engine

> **Modern PC Launcher & Native Port Hub for Persona 2: Eternal Punishment**

[![License: GPL v3](https://img.shields.io/badge/License-GPL%20v3+-blue.svg)](./LICENSE)
[![Platform: Windows / Linux](https://img.shields.io/badge/Platform-Windows%20%7C%20Linux-lightgrey.svg)](#installation)
[![Game: Persona 2 EP](https://img.shields.io/badge/Target%20Game-Persona%202%3A%20Eternal%20Punishment-purple.svg)](#about)

---

## 📖 About PersonaRM

**PersonaRM** es un proyecto en desarrollo temprano cuyo objetivo es ejecutar **Persona 2: Eternal Punishment** en PC moderno (Windows y Linux) sin depender de un emulador genérico. La dirección técnica actual es un **emulador de PS1 con BIOS en HLE y renderer propio**; la decisión de fondo (emulador vs. reimplementación) sigue abierta, ver [docs/research](docs/research/).

## 🚦 Estado actual

Hoy el proyecto es un **launcher de consola y un conjunto de prototipos de componentes**. **No ejecuta el juego todavía.**

| Componente | Estado |
|---|---|
| Launcher de consola (escaneo de discos, configuración JSON) | Prototype |
| Cargador PS-X EXE e identificación de discos Persona 2 EP | Prototype (con tests) |
| Lector ISO 9660 / BIN de 2352 bytes | Prototype (sin XA Form 2) |
| CPU MIPS-I, GTE, GPU GP0/GP1, SPU, MDEC, BIOS HLE | Prototype parcial |
| Bus de PS1: IRQ, DMA, timers, CD-ROM | **Not started** |
| Ventana SDL2, renderer OpenGL, salida de audio real | **Not started** (hoy son stubs) |

> ⚠️ **Important Legal Notice**: This project **does NOT distribute** any copyrighted game files, ISOs, audio, or assets. PersonaRM requires a **user-provided legitimate copy** of the game to extract and execute assets locally.

---

## 🎯 Objetivos y estado

| Característica | Estado |
|---|---|
| Launcher centralizado (gráficos, audio, directorios) | Prototype (consola) |
| Teclado y ratón + mandos con glifos dinámicos | Prototype (lógica sin SDL) |
| Widescreen 16:9 / 21:9 | Prototype (cálculo de viewport); requiere parches por juego en el juego real |
| Resolución interna 4K, 60+ FPS | Planned |
| Turbo (2x/4x) | Prototype (multiplicador en el engine) |
| Texto instantáneo, backlog de diálogos | Planned |
| Quicksaves ilimitados | Prototype (`SaveManager`) |
| Audio (salida mediante SDL Audio o miniaudio; mezclador propio) | Planned |
| Windows (MSVC 2022) y Linux (GCC/Clang) | CI en configuración |

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
│  │  (OpenGL)   │ │ (SDL / mini-│ │  Mapper (SDL2)        │  │
│  │  [planned]  │ │  audio)     │ │  [planned]            │  │
│  └─────────────┘ └─────────────┘ └───────────────────────┘  │
│  ┌─────────────┐ ┌─────────────┐ ┌───────────────────────┐  │
│  │   Memory    │ │ MIPS-I CPU  │ │ PS-X EXE / ISO 9660   │  │
│  │   Manager   │ │ (interprete)│ │ loader                │  │
│  └─────────────┘ └─────────────┘ └───────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

---

## 🛠️ Build & Installation

### Prerequisites

- CMake ≥ 3.20, Ninja, Python 3
- Windows: **Visual Studio 2022 Build Tools (MSVC)** y compilar desde un *Developer PowerShell for VS 2022* (o LLVM/Clang)
- Linux: GCC ≥ 9 o Clang

```powershell
# Windows
choco install cmake ninja python3
```

### Build

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

El ejecutable se genera en `build/Release/personarm` (`build\Release\personarm.exe` en Windows) porque el proyecto fija ese directorio de salida. Con el generador de Visual Studio (`-G "Visual Studio 17 2022"` y `--config Release`) la ruta sería `build\Release\Release\`.

### Tests

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
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
| [docs/research/](docs/research/) | Investigación técnica (hardware, formatos, discos, arquitectura, roadmap, auditoría, bibliografía) y errata |
| [ARCHITECTURE.md](./ARCHITECTURE.md) | Technical system architecture & subsystem breakdown |
| [CONTRIBUTING.md](./CONTRIBUTING.md) | Contribution guidelines and style guide |
| [LICENSE](./LICENSE) | GNU General Public License v3.0 |

---

Made with ❤️ by the PersonaRM contributors.
