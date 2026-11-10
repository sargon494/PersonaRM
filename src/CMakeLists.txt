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
)

set(INPUT_SOURCES
    input/input_manager.cpp
    input/input_manager.h
)

set(AUDIO_SOURCES "")
set(CPU_SOURCES "")
set(LOADER_SOURCES "")
set(RENDERING_SOURCES "")

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
else()
    target_compile_options(personarm PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ───────────────────────────────────────────────────────────────────────
# Static Analysis Target
# ───────────────────────────────────────────────────────────────────────

if(EXISTS "${CMAKE_SOURCE_DIR}/cmake/modules/StaticAnalysis.cmake")
    include(${CMAKE_SOURCE_DIR}/cmake/modules/StaticAnalysis.cmake)
endif()
