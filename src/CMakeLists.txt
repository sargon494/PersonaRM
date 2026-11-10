# ───────────────────────────────────────────────────────────────────────
# src/ CMakeLists.txt
# PersonaRM - Native Launcher & Port Engine
# ───────────────────────────────────────────────────────────────────────

# Subsystem source collections
set(LAUNCHER_SOURCES
    launcher/launcher.cpp
    launcher/launcher.h
)

set(CORE_SOURCES
    core/memory_manager.cpp
    core/memory_manager.h
    core/logger.cpp
    core/logger.h
)

set(INPUT_SOURCES
    input/input_manager.cpp
    input/input_manager.h
)

set(AUDIO_SOURCES
    audio/audio_system.cpp
    audio/audio_system.h
    audio/vag_decoder.cpp
    audio/vag_decoder.h
)
set(CPU_SOURCES
    cpu/cpu_context.cpp
    cpu/cpu_context.h
    cpu/gte.cpp
    cpu/gte.h
)
set(LOADER_SOURCES
    loader/disc_reader.cpp
    loader/disc_reader.h
    loader/tim_parser.cpp
    loader/tim_parser.h
    loader/ps1_bios_hle.cpp
    loader/ps1_bios_hle.h
    loader/exe_loader.cpp
    loader/exe_loader.h
)
set(RENDERING_SOURCES
    rendering/renderer.cpp
    rendering/renderer.h
    rendering/ps1_gpu.cpp
    rendering/ps1_gpu.h
)

# ───────────────────────────────────────────────────────────────────────
# Core Engine Static Library: personarm_core (aliased as persona2ep)
# ───────────────────────────────────────────────────────────────────────

add_library(personarm_core STATIC
    ${LAUNCHER_SOURCES}
    ${CORE_SOURCES}
    ${INPUT_SOURCES}
    ${AUDIO_SOURCES}
    ${CPU_SOURCES}
    ${LOADER_SOURCES}
    ${RENDERING_SOURCES}
)

# Provide alias 'persona2ep' for backwards compatibility and test helpers
add_library(persona2ep ALIAS personarm_core)

target_include_directories(personarm_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(personarm_core PUBLIC cxx_std_17)

if(MSVC)
    target_compile_options(personarm_core PRIVATE /W4)
    target_compile_definitions(personarm_core PUBLIC _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS NOMINMAX)
else()
    target_compile_options(personarm_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ───────────────────────────────────────────────────────────────────────
# Main Executable: personarm (PersonaRM Launcher & Runtime)
# ───────────────────────────────────────────────────────────────────────

add_executable(personarm
    main.cpp
)

target_link_libraries(personarm PRIVATE
    personarm_core
)

target_compile_features(personarm PRIVATE cxx_std_17)

if(MSVC)
    target_compile_options(personarm PRIVATE /W4)
    target_compile_definitions(personarm PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS NOMINMAX)
else()
    target_compile_options(personarm PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS personarm
    RUNTIME DESTINATION bin
)

# ───────────────────────────────────────────────────────────────────────
# Static Analysis Target
# ───────────────────────────────────────────────────────────────────────

if(EXISTS "${CMAKE_SOURCE_DIR}/cmake/modules/StaticAnalysis.cmake")
    include(${CMAKE_SOURCE_DIR}/cmake/modules/StaticAnalysis.cmake)
endif()
