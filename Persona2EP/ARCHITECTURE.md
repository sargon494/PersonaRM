# Architecture - Persona 2: Eternal Punishment Native Port

## 🏛️ System Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                    Persona 2: EP - Native Port                    │
├─────────────────────────────────────────────────────────────────┤
│                                                                   │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │                    Application Layer                      │   │
│  │  ┌─────────────┐ ┌─────────────┐ ┌───────────────────┐  │   │
│  │  │   Renderer  │ │   Input     │ │     Audio          │  │   │
│  │  │   (OpenGL)  │ │   (SDL2)    │ │     (OpenAL)       │  │   │
│  │  └─────────────┘ └─────────────┘ └───────────────────┘  │   │
│  └─────────────────────────────────────────────────────────┘   │
│                         │                                       │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │                  Emulation Layer                          │   │
│  │  ┌─────────────┐ ┌─────────────┐ ┌───────────────────┐  │   │
│  │  │  Memory     │ │   CPU       │ │     I/O            │  │   │
│  │  │  Manager    │ │  (SPU)      │ │     Controller     │  │   │
│  │  └─────────────┘ └─────────────┘ └───────────────────┘  │   │
│  └─────────────────────────────────────────────────────────┘   │
│                         │                                       │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │                   Recompiler Layer                        │   │
│  │  ┌─────────────┐ ┌─────────────┐ ┌───────────────────┐  │   │
│  │  │   JIT       │ │   Interpreter│ │   Cache Manager   │  │   │
│  │  │   Engine    │ │   Engine    │ │   (Translation)   │  │   │
│  │  └─────────────┘ └─────────────┘ └───────────────────┘  │   │
│  └─────────────────────────────────────────────────────────┘   │
│                         │                                       │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │                  Game Layer (User-Provided)               │   │
│  │  ┌─────────────┐ ┌─────────────┐ ┌───────────────────┐  │   │
│  │  │   PS2       │ │   PS2       │ │   PS2             │  │   │
│  │  │   Binary    │ │   Binary    │ │   Binary          │  │   │
│  │  │   (User)    │ │   (User)    │ │   (User)          │  │   │
│  │  └─────────────┘ └─────────────┘ └───────────────────┘  │   │
│  └─────────────────────────────────────────────────────────┘   │
│                                                                   │
└─────────────────────────────────────────────────────────────────┘
```

## 📐 Component Architecture

### 1. Application Layer

The topmost layer that handles all user-facing functionality.

#### Renderer (`src/renderer/`)
- **Purpose**: Handles all graphical output
- **Technologies**: OpenGL 4.6, GLSL shaders
- **Responsibilities**:
  - Texture management
  - Geometry rendering
  - Shader compilation and caching
  - Framebuffer management
  - Post-processing effects
- **Key Files**:
  - `renderer.cpp` - Main renderer class
  - `texture_manager.cpp` - Texture loading and caching
  - `shader_manager.cpp` - Shader compilation
  - `framebuffer.cpp` - Framebuffer management

#### Input (`src/input/`)
- **Purpose**: Handles all input devices
- **Technologies**: SDL2
- **Responsibilities**:
  - Keyboard input
  - Mouse input
  - Gamepad input
  - Touch input (future)
- **Key Files**:
  - `input.cpp` - Main input class
  - `keyboard.cpp` - Keyboard handling
  - `mouse.cpp` - Mouse handling
  - `gamepad.cpp` - Gamepad handling

#### Audio (`src/audio/`)
- **Purpose**: Handles all audio output
- **Technologies**: OpenAL, OpenAL Soft
- **Responsibilities**:
  - Audio buffer management
  - Sound effect playback
  - Music playback
  - Audio mixing
- **Key Files**:
  - `audio.cpp` - Main audio class
  - `buffer_manager.cpp` - Buffer management
  - `mixer.cpp` - Audio mixing

### 2. Emulation Layer

The core emulation engine that interprets and executes PS2 code.

#### Memory Manager (`src/memory/`)
- **Purpose**: Manages all memory operations
- **Technologies**: Custom memory manager
- **Responsibilities**:
  - Memory mapping
  - Memory protection
  - Memory caching
  - Virtual memory management
- **Key Files**:
  - `memory_manager.cpp` - Main memory manager
  - `memory_mapper.cpp` - Memory mapping
  - `cache_manager.cpp` - Cache management

#### CPU Emulator (`src/cpu/`)
- **Purpose**: Emulates the PS2 CPU (SPU)
- **Technologies**: Custom CPU emulator
- **Responsibilities**:
  - Instruction decoding
  - Instruction execution
  - Register management
  - Branch prediction
- **Key Files**:
  - `cpu.cpp` - Main CPU class
  - `decoder.cpp` - Instruction decoder
  - `executer.cpp` - Instruction executor
  - `register_set.cpp` - Register management

#### I/O Controller (`src/io/`)
- **Purpose**: Handles all I/O operations
- **Technologies**: Custom I/O layer
- **Responsibilities**:
  - Disk I/O
  - Memory I/O
  - Timer I/O
  - Interrupt handling
- **Key Files**:
  - `io_controller.cpp` - Main I/O controller
  - `disk_io.cpp` - Disk I/O
  - `timer_io.cpp` - Timer I/O

### 3. Recompiler Layer

Optimizes emulation performance through recompilation.

#### JIT Engine (`src/recompiler/jit/`)
- **Purpose**: Compiles translated code to native machine code
- **Technologies**: LLVM or custom JIT
- **Responsibilities**:
  - Code generation
  - Code optimization
  - Code caching
  - Hot path detection
- **Key Files**:
  - `jit_engine.cpp` - Main JIT engine
  - `code_generator.cpp` - Code generation
  - `optimizer.cpp` - Code optimization
  - `cache.cpp` - Code cache management

#### Interpreter (`src/recompiler/interpreter/`)
- **Purpose**: Interprets translated code when JIT is not available
- **Technologies**: Custom interpreter
- **Responsibilities**:
  - Instruction interpretation
  - Virtual machine execution
  - Fallback execution
- **Key Files**:
  - `interpreter.cpp` - Main interpreter
  - `virtual_machine.cpp` - VM execution

#### Cache Manager (`src/recompiler/cache/`)
- **Purpose**: Manages the recompiled code cache
- **Technologies**: Custom cache manager
- **Responsibilities**:
  - Cache storage
  - Cache eviction
  - Cache invalidation
  - Cache statistics
- **Key Files**:
  - `cache_manager.cpp` - Cache management
  - `cache_storage.cpp` - Cache storage
  - `cache_stats.cpp` - Cache statistics

### 4. Game Layer

The user-provided game binary that is being ported.

#### Game Loader (`src/game/`)
- **Purpose**: Loads and initializes the game binary
- **Technologies**: Custom game loader
- **Responsibilities**:
  - Binary parsing
  - Resource extraction
  - Initialization
  - Shutdown
- **Key Files**:
  - `game_loader.cpp` - Main game loader
  - `resource_manager.cpp` - Resource management
  - `game_state.cpp` - Game state management

## 🔄 Data Flow

```
User Input ──→ Input Layer ──→ Application Layer
                                      ↓
Game State ←──→ Emulation Layer ←──→ Recompiler Layer
                                      ↓
User Output ←─→ Renderer/Audio ←───→ Game Layer
```

## 🧱 Key Design Decisions

### 1. Layered Architecture
The system is organized in layers, each with a single responsibility. This makes the system modular and maintainable.

### 2. Recompiler-First Approach
Unlike the MHForever project which uses dynamic recompilation, this project uses **static recompilation**. This means:
- The game is recompiled once to a native executable
- No runtime emulation overhead
- Near-native performance
- Simpler deployment

### 3. User-Provided Game Binary
The game binary is NOT distributed. Users must provide their own legal copy. The porting tools only work with a user-provided binary.

### 4. Platform-Neutral Design
The core engine is platform-neutral. Platform-specific code is isolated in platform abstraction layers.

### 5. Testing-First
All components have unit tests. Integration tests validate the full system. Performance tests ensure performance targets are met.

## 📦 Build System

```
CMakeLists.txt
├── CMakeLists.txt (root)
├── src/
│   ├── CMakeLists.txt
│   ├── renderer/
│   │   └── CMakeLists.txt
│   ├── cpu/
│   │   └── CMakeLists.txt
│   └── ...
├── tests/
│   ├── CMakeLists.txt
│   ├── unit/
│   │   └── CMakeLists.txt
│   ├── integration/
│   │   └── CMakeLists.txt
│   └── performance/
│       └── CMakeLists.txt
└── ...
```

## 🧪 Testing Strategy

### Unit Tests
- Test each component in isolation
- Use mocks for dependencies
- Achieve >80% code coverage
- Run on every commit

### Integration Tests
- Test full gameplay scenarios
- Test with actual game data
- Validate against known behaviors
- Run on every PR

### Performance Tests
- Benchmark key operations
- Profile memory usage
- Profile CPU usage
- Optimize bottlenecks

### Compatibility Tests
- Test with various game versions
- Test with different hardware
- Test edge cases

## 📝 Code Organization

```
src/
├── CMakeLists.txt
├── main.cpp
├── renderer/
│   ├── CMakeLists.txt
│   ├── renderer.cpp
│   ├── texture_manager.cpp
│   └── shader_manager.cpp
├── input/
│   ├── CMakeLists.txt
│   ├── input.cpp
│   └── gamepad.cpp
├── audio/
│   ├── CMakeLists.txt
│   ├── audio.cpp
│   └── mixer.cpp
├── memory/
│   ├── CMakeLists.txt
│   ├── memory_manager.cpp
│   └── cache_manager.cpp
├── cpu/
│   ├── CMakeLists.txt
│   ├── cpu.cpp
│   ├── decoder.cpp
│   └── executer.cpp
├── io/
│   ├── CMakeLists.txt
│   ├── io_controller.cpp
│   └── disk_io.cpp
├── recompiler/
│   ├── CMakeLists.txt
│   ├── jit/
│   │   ├── CMakeLists.txt
│   │   ├── jit_engine.cpp
│   │   └── optimizer.cpp
│   └── interpreter/
│       ├── CMakeLists.txt
│       └── interpreter.cpp
└── game/
    ├── CMakeLists.txt
    └── game_loader.cpp
```

## 🚀 Performance Targets

| Metric | Target | Current |
|--------|--------|---------|
| Startup time | < 5s | N/A (static) |
| FPS (main menu) | 60+ | N/A |
| FPS (gameplay) | 55+ | N/A |
| Memory usage | < 2GB | N/A |
| Build time | < 10 min | N/A |

## 🔒 Security Considerations

- No hardcoded secrets
- Input validation on all user input
- Secure memory handling
- No unsafe pointer arithmetic
- Fuzzing for security testing

## 📚 References

- **MHForever**: https://github.com/Mango77x/MHForever
- **PCSX2**: https://pcsx2.net/
- **LLVM**: https://llvm.org/
- **SDL2**: https://www.libsdl.org/
- **OpenAL Soft**: https://openal-soft.org/