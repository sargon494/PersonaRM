# ───────────────────────────────────────────────────────────────────────
# src/ CMakeLists.txt
# PersonaRM - Native Launcher & Port Engine
# ───────────────────────────────────────────────────────────────────────

# Launcher Sources
set(LAUNCHER_SOURCES
    launcher/launcher.cpp
    launcher/launcher.h
)

# Core & Memory Sources
set(CORE_SOURCES
    core/memory_manager.cpp
    core/memory_manager.h
)

# Placeholder source lists for subsystems
set(AUDIO_SOURCES "")
set(CPU_SOURCES "")
set(LOADER_SOURCES "")
set(RENDERING_SOURCES "")
set(INPUT_SOURCES "")

# ───────────────────────────────────────────────────────────────────────
# Main Executable: personarm (PersonaRM Launcher & Runtime)
# ───────────────────────────────────────────────────────────────────────

add_executable(personarm
    main.cpp
    ${LAUNCHER_SOURCES}
    ${CORE_SOURCES}
    ${AUDIO_SOURCES}
    ${CPU_SOURCES}
    ${LOADER_SOURCES}
    ${RENDERING_SOURCES}
    ${INPUT_SOURCES}
)

target_include_directories(personarm PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Alias for backwards compatibility with tests / scripts expecting persona2ep
add_custom_target(persona2ep ALL
    DEPENDS personarm
)

# ───────────────────────────────────────────────────────────────────────
# Compiler Settings
# ───────────────────────────────────────────────────────────────────────

target_compile_features(personarm PRIVATE cxx_std_17)

if(MSVC)
    target_compile_options(personarm PRIVATE /W4)
else()
    target_compile_options(personarm PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ───────────────────────────────────────────────────────────────────────
# Static Analysis Target
# ───────────────────────────────────────────────────────────────────────

if(EXISTS "${CMAKE_SOURCE_DIR}/cmake/modules/StaticAnalysis.cmake")
    include(${CMAKE_SOURCE_DIR}/cmake/modules/StaticAnalysis.cmake)
endif()
