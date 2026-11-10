# 🎮 Persona 2: Eternal Punishment - Native Port

> Porting Persona 2: Eternal Punishment to native Windows via static recompilation.

[![License: GPL v3](https://img.shields.io/badge/License-GPL%20v3+-blue.svg)](./LICENSE)
[![GitHub issues](https://img.shields.io/github/issues/Persona2EP/Persona2EP?color=orange)](https://github.com/Persona2EP/Persona2EP/issues)
[![GitHub contributors](https://img.shields.io/github/contributors/Persona2EP/Persona2EP?color=green)](https://github.com/Persona2EP/Persona2EP/graphs/contributors)
[![GitHub stars](https://img.shields.io/github/stars/Persona2EP/Persona2EP?color=gold)](https://github.com/Persona2EP/Persona2EP/stargazers)

---

## 📖 About

**Persona 2: Eternal Punishment** is a role-playing game originally released for the PlayStation 2 in 2005. This project aims to create a **native Windows executable** by statically recompiling the game, eliminating the need for PlayStation 2 hardware or emulators.

> ⚠️ **Important Legal Notice**: This project **does NOT distribute** any game files, ISOs, or copyrighted material. The porting tools are designed to work with a **user-provided legal copy** of the game. Each user is responsible for providing their own legitimate copy.

---

## 🎯 Goals

| Goal | Status |
|------|--------|
| Native Windows executable | 🚧 In Progress |
| Near-native performance | 🚧 In Progress |
| Full gameplay compatibility | 🚧 In Progress |
| Open source & transparent | ✅ Done |
| Comprehensive testing | 🚧 In Progress |
| Cross-platform support | 🚧 Planned |

---

## 📐 Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    Application Layer                      │
│  ┌─────────────┐ ┌─────────────┐ ┌───────────────────┐  │
│  │   Renderer  │ │   Input     │ │     Audio          │  │
│  │   (OpenGL)  │ │   (SDL2)    │ │     (OpenAL)       │  │
│  └─────────────┘ └─────────────┘ └───────────────────┘  │
│                         │                                 │
│  ┌───────────────────────────────────────────────────┐   │
│  │                  Emulation Layer                    │   │
│  │  ┌─────────────┐ ┌─────────────┐ ┌───────────────┐ │   │
│  │  │  Memory     │ │   CPU       │ │     I/O       │ │   │
│  │  │  Manager    │ │  (SPU)      │ │     Controller │ │   │
│  │  └─────────────┘ └─────────────┘ └───────────────┘ │   │
│  └───────────────────────────────────────────────────┘   │
│                         │                                 │
│  ┌───────────────────────────────────────────────────┐   │
│  │                   Recompiler Layer                  │   │
│  │  ┌─────────────┐ ┌─────────────┐ ┌───────────────┐ │   │
│  │  │   JIT       │ │   Interpreter│ │   Cache       │ │   │
│  │  │   Engine    │ │   Engine    │ │   Manager     │ │   │
│  │  └─────────────┘ └─────────────┘ └───────────────┘ │   │
│  └───────────────────────────────────────────────────┘   │
│                         │                                 │
│  ┌───────────────────────────────────────────────────┐   │
│  │                  Game Layer (User-Provided)        │   │
│  │  ┌─────────────┐ ┌─────────────┐ ┌───────────────┐ │   │
│  │  │   PS2       │ │   PS2       │ │   PS2         │ │   │
│  │  │   Binary    │ │   Binary    │ │   Binary      │ │   │
│  │  │   (User)    │ │   (User)    │ │   (User)      │ │   │
│  │  └─────────────┘ └─────────────┘ └───────────────┘ │   │
│  └───────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────┘
```

---

## 🛠️ Tech Stack

- **Language**: C++17 (primary), Python 3 (tools)
- **Build System**: CMake 3.20+
- **Graphics**: OpenGL 4.6 + GLSL
- **Input**: SDL2
- **Audio**: OpenAL Soft
- **Recompiler**: LLVM-based JIT engine
- **Test Framework**: Google Test + Catch2

---

## 📦 Installation

### Prerequisites (Windows)

```powershell
# Install build tools
choco install cmake ninja python3

# Clone this repository
git clone https://github.com/Persona2EP/Persona2EP.git
cd Persona2EP
```

### Build

```powershell
mkdir build
cd build
cmake .. -G Ninja
cmake --build . --config Release
```

### Run

```powershell
.\build\Release\persona2ep.exe
```

> **Note**: You must provide your own legal copy of the game. The porting tools will guide you through the process.

---

## 🧪 Testing

### Running Tests

```bash
# Unit tests
ctest --output-on-failure -R "unit"

# Integration tests
ctest --output-on-failure -R "integration"

# Performance tests
ctest --output-on-failure -R "performance"
```

### CI Status

| Test | Status |
|------|--------|
| Unit Tests | 🟢 Passing |
| Integration Tests | 🟡 In Progress |
| Performance Tests | 🟡 In Progress |
| Code Coverage | 🟡 In Progress |

See [GitHub Actions](./.github/workflows) for CI details.

---

## 📚 Documentation

| Document | Description |
|----------|-------------|
| [ARCHITECTURE.md](./ARCHITECTURE.md) | System architecture and design decisions |
| [AGENT.md](./AGENT.md) | AI agent guidelines for development |
| [CONTRIBUTING.md](./CONTRIBUTING.md) | Contribution guidelines |
| [LICENSE](./LICENSE) | GPL 3.0 or later license |

---

## 🤝 Contributing

We welcome contributions! Please read our [Contributing Guidelines](./CONTRIBUTING.md) first.

### Ways to Contribute

- 🐛 **Bug Reports**: Submit issues on GitHub
- 💡 **Feature Requests**: Propose new features
- 🔧 **Code Contributions**: Submit pull requests
- 📖 **Documentation**: Improve documentation
- 🧪 **Testing**: Write tests

### Code of Conduct

Please note that this project is released with a [Contributor Code of Conduct](./CONTRIBUTING.md#code-of-conduct). By participating in this project you agree to abide by its terms.

---

## ⚖️ License

This project is licensed under **GPL v3.0 or later**.

> ⚠️ **Important**: This license applies **only** to the porting tools and scripts in this repository. It does **NOT** apply to the game software, assets, or any copyrighted material from Persona 2: Eternal Punishment.

### What We DO NOT Distribute

- ❌ Game ISOs or images
- ❌ Game executables
- ❌ Game assets (textures, models, audio)
- ❌ Game code or scripts
- ❌ Any copyrighted game content

### What We DO Distribute

- ✅ Porting tools and libraries
- ✅ Build scripts and configuration
- ✅ Documentation
- ✅ Test suites

### How to Use Legally

1. Obtain a legal copy of Persona 2: Eternal Punishment
2. Install it on your own system
3. Use the porting tools to create a native executable
4. The resulting executable is your own creation

---

## 📈 Project Status

```
Phase 1: Setup          ✅ Complete
Phase 2: Core Components 🟡 In Progress
Phase 3: Integration    🟡 In Progress
Phase 4: Polish         ⚪ Planned
```

### Current Progress

- [x] Project setup
- [x] Build system configuration
- [x] Test infrastructure
- [x] CI/CD pipeline
- [x] Documentation
- [ ] Core recompiler
- [ ] Memory manager
- [ ] CPU emulator
- [ ] Renderer
- [ ] Audio system
- [ ] Game integration

---

## 🙏 Acknowledgments

- **PCSX2** - For reference on PS2 emulation techniques
- **LLVM** - For the JIT compilation infrastructure
- **SDL2** - For input and audio support
- **All contributors** - Thank you for your contributions!

---

## 📞 Contact

- **GitHub**: [Persona2EP/Persona2EP](https://github.com/Persona2EP/Persona2EP)
- **Issues**: [Report a bug](https://github.com/Persona2EP/Persona2EP/issues)
- **Discussions**: [Start a discussion](https://github.com/Persona2EP/Persona2EP/discussions)

---

## 📜 Disclaimer

This project is a **fan project** and is not affiliated with, endorsed by, or sponsored by Atlus or any other copyright holder of Persona 2: Eternal Punishment.

We do **not** distribute, sell, or provide any game files. The porting tools are designed to work with a user-provided legal copy of the game.

By using this project, you agree that you are responsible for ensuring you own a legal copy of the game.

---

Made with ❤️ by the Persona 2 Native Port Team.