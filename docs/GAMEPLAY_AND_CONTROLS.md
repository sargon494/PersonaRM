# 🎮 Especificación de Modernización de Controles, Inputs y Experiencia de Juego

> **Documento Oficial de Diseño Técnico & Game Feel**  
> **Proyecto**: PersonaRM (Persona Native Launcher & Port)  
> **Módulo**: Persona 2: Eternal Punishment  
> **Autor**: Tech Lead / Senior Software Engineer  
> **Estado**: Aprobado para Implementación  

---

## 1. 🔍 Diagnóstico: ¿Por qué el control original se siente tosco en PC?

Persona 2: Eternal Punishment (diseñado originalmente en PS1 y adaptado a PSP) se concibió bajo las limitaciones físicas de los mandos de finales de los años 90:
- **Movimiento tanque / 8 direcciones rígidas**: Diseñado para cruceta digital (D-Pad), no para sticks analógicos modernos ni WASD fluido.
- **Cámara por pasos**: Rotación de cámara en incrementos discretos de 45°/90° asignada a botones laterales (L1/R1).
- **Navegación de combate por menús profundos**: Demasiadas confirmaciones necesarias para comandos básicos en batallas por turnos y en el sistema de Negociación/Contacto con demonios.
- **Sin soporte de ratón**: Los menús, opciones de diálogo y selección de objetivos exigen navegar con flechas una a una.
- **Ritmo de combate lento**: Animaciones fijas que en su día requerían paciencia y hoy demandan funciones de Turbo/Skip.

---

## 2. 🕹️ Sistema de Input & Arquitectura de Abstracción

Para que el juego lea los controles sin latencia y soporte cualquier dispositivo en caliente, PersonaRM implementa una capa de abstracción desacoplada:

```
┌─────────────────────────────────────────────────────────────┐
│                       Hardware Layer                        │
│   Xbox Controller │ DualSense / DS4 │ Switch Pro │ Teclado + Ratón │
└──────────────────────────────┬──────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────┐
│                 SDL2 GameController Subsystem               │
│   (Hot-plugging, remapeo SDL_GameControllerDB, polling de ejes) │
└──────────────────────────────┬──────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────┐
│                 Input Actions Abstraction                   │
│   (Move, Camera, Interact, Cancel, Menu, Map, Turbo, QuickSlot)│
└──────────────────────────────┬──────────────────────────────┘
                               │
            ┌──────────────────┴──────────────────┐
            ▼                                     ▼
┌───────────────────────────┐         ┌───────────────────────────┐
│     Gamepad Context       │         │       KBM Context         │
│ - Deadzone configurable   │         │ - WASD / Point & Click    │
│ - Glifos dinámicos (UI)   │         │ - Cursor virtual interactivo│
│ - Vibración háptica       │         │ - Rueda de scroll en logs │
└───────────────────────────┘         └───────────────────────────┘
```

---

## 3. ⌨️🖱️ Experiencia con Teclado y Ratón (KBM) de Primer Nivel

No basta con mapear teclas a botones del mando; debe sentirse un juego de PC nativo:

### A. Exploración y Movimiento
- **WASD**: Movimiento analógico simulado (8 direcciones suaves con aceleración e interpolación).
- **Shift Izquierdo**: Correr / Alternar modo carrera (Toggle/Hold configurable).
- **Ratón (Rotación de cámara)**:
  - Mantener **Click Derecho (RMB)** y arrastrar para orbitar suavemente la cámara en 3D.
  - Teclas **Q / E**: Rotación rápida en incrementos de 45° o 90°.
- **Espacio / E**: Interactuar, hablar con NPCs, abrir cofres.
- **Escape**: Pausa / Cancelar / Salir de menús.
- **Tab / M**: Abrir minimapa / mapa de mazmorra al instante.

### B. Interfaz y Menús: Soporte Ratón Completo
- **Cursor UI nativo**:
  - Puntero visible en todos los menús, diálogos y pantallas de compra/venta.
  - *Hover states*: Los elementos se iluminan al pasar el cursor por encima.
  - **Click Izquierdo (LMB)**: Selecciona directamente la opción sin tener que desplazarse con flechas.
  - **Click Derecho (RMB)**: Regresar / Cancelar.
  - **Rueda de desplazamiento (Scroll Wheel)**: Navegación rápida por listas largas (inventario de Persona, lista de objetos, registro de rumores de la Agencia Kuzunoha).

### C. Sistema de Combate con Atajos Directos
- **Teclas 1 al 6**: Selección rápida de comandos de combate:
  - `1`: Atacar
  - `2`: Persona / Habilidades
  - `3`: Hechizos de Fusión (Fusion Spells)
  - `4`: Contactar Demonio (Negotiation)
  - `5`: Objetos
  - `6`: Estrategia / Auto-Battle
- **R**: Activar / Desactivar Auto-Battle.
- **Espacio**: Confirmar objetivo seleccionado.

---

## 4. 🎮 Soporte para Mandos Modernos (Xbox, PlayStation, Switch, Steam Deck)

### A. Layout Ergonómico y Moderno
| Acción | Xbox | PlayStation (DualSense/DS4) | Switch / Deck |
|---|---|---|---|
| **Moverse** | Stick Izquierdo (L-Stick) | Stick Izquierdo (L-Stick) | Stick Izquierdo (L-Stick) |
| **Girar Cámara** | Stick Derecho / LB-RB | Stick Derecho / L1-R1 | Stick Derecho / L-R |
| **Interactuar / Confirmar** | `A` | `X` (Cross) | `B` (o configurable estilo Nintendo) |
| **Cancelar / Correr** | `B` | `O` (Circle) | `A` |
| **Menú Principal** | `Y` o `Menu` | `Triangle` o `Options` | `X` o `+` |
| **Mapa** | `X` o `View/Back` | `Square` o `Touchpad` | `Y` o `-` |
| **Turbo / Acelerador** | `RT` (Gatillo der.) | `R2` | `ZR` |
| **Auto-Battle (Batalla)** | `RB` | `R1` | `R` |

### B. Glifos Dinámicos en Pantalla (Button Prompts)
- El juego detecta el modelo del mando mediante `SDL_GameControllerGetType()` y muestra automáticamente los iconos correctos:
  - Iconos verdes/rojos/azules/amarillos para Xbox.
  - Cruz, Círculo, Cuadrado, Triángulo para PlayStation.
  - Iconos de teclas (`E`, `Space`, `Tab`, `ESC`) en cuanto el usuario toca una tecla o mueve el ratón.

### C. Características Hápticas
- Vibración sutil al contactar demonios exitosamente, recibir golpes críticos o al invocar Personas.

---

## 5. 🚀 Mejoras de Calidad de Vida (QoL) para que sea Disfrutable

Para que el juego no se sienta anticuado ni frustrante, se requieren estas funciones modernas integradas en PersonaRM:

### 1. Modo Turbo / Aceleración (2x / 4x)
- En JRPGs clásicos con batallas aleatorias, la velocidad normal puede resultar tediosa.
- Permitir **mantener `RT` / `Tab`** para acelerar el juego al 200% o 300% durante desplazamientos o animaciones de batalla.

### 2. Texto Instantáneo y Registro de Diálogos (Log / Backlog)
- Opción de velocidad de texto: Lenta, Media, Rápida o **Instantánea**.
- **Historial de Diálogos**: Poder abrir un registro con la rueda del ratón hacia arriba o `L2/LT` para repasar conversaciones anteriores, vital en un juego con tantos rumores y deducciones como P2:EP.

### 3. Sistema de Guardado Moderno
- Eliminar las limitaciones de tarjetas de memoria PS1/PS2 (15 bloques).
- **Slots ilimitados con capturas de pantalla, fecha y ubicación**.
- **Guardado rápido (F5 Quick Save / F9 Quick Load)** y **Autoguardado** al cambiar de sala o antes de jefes.

### 4. Gráficos y Renderizado en Alta Definición
- **Soporte Panorámico Nativo (16:9 y 21:9)**: Sin estirar personajes; expandiendo el viewport 3D y manteniendo la interfaz en posición correcta.
- **Resolución Interna Escalable**: 1080p, 1440p y 4K con filtrado de texturas aniso 16x.
- **60 FPS con Interpolación**: Suavizar el movimiento de cámara y personajes respecto a los 20/30 FPS originales.
- **Tipografía HD**: Fuente de texto nítida y legible para monitores modernos, con opción de activar la tipografía pixel art original.

### 5. Opciones de Accesibilidad
- Remapeo completo de cualquier acción a cualquier botón o tecla.
- Ajuste de sensibilidad de cámara y zona muerta de sticks (Deadzone).
- Controles de volumen independientes: Master, Música (BGM), Efectos (SFX) y Voces.

---

## 6. 🗺️ Hoja de Ruta de Implementación de Inputs

```
Milestone A: Subcontrolador de Dispositivos (SDL2)
  ├── Inicializar SDL_INIT_GAMECONTROLLER
  ├── Detección de hot-plugging (conectar/desconectar sin reiniciar)
  └── Base de datos SDL_GameControllerDB integrada

Milestone B: Mapeador de Acciones Virtuales (Action Mapper)
  ├── Tabla de mapeo Acción -> Lista de Bindings (Teclas/Botones)
  ├── Detección de Deadzone para sticks analógicos
  └── Cambio instantáneo de esquema visual (KBM vs Gamepad)

Milestone C: Capa de Interacción KBM
  ├── Raycasting / selección con puntero en menús de batalla y diálogo
  ├── Rueda de scroll conectada a listas e inventario
  └── Controles de cámara por arrastre de ratón

Milestone D: Menú de Ajustes de Controles en Juego & Launcher
  ├── Pantalla de configuración para reasignar teclas
  ├── Calibración de zona muerta y sensibilidad
  └── Perfiles de guardado (config.json)
```

