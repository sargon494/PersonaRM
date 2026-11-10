# ───────────────────────────────────────────────────────────────────────
# src/ CMakeLists.txt
# Persona 2: Eternal Punishment - Native Port
# ───────────────────────────────────────────────────────────────────────

# Collect core source files
set(CORE_SOURCES
    core/memory_manager.cpp
    core/memory_manager.h
)

# Placeholder source lists for future modules
set(AUDIO_SOURCES "")
set(CPU_SOURCES "")
set(LOADER_SOURCES "")
set(RENDERING_SOURCES "")

# ───────────────────────────────────────────────────────────────────────
# Main executable
# ───────────────────────────────────────────────────────────────────────

add_executable(persona2ep
    main.cpp
    ${CORE_SOURCES}
    ${AUDIO_SOURCES}
    ${CPU_SOURCES}
    ${LOADER_SOURCES}
    ${RENDERING_SOURCES}
)

target_include_directories(persona2ep PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# ───────────────────────────────────────────────────────────────────────
# Link dependencies (as they become available)
# ───────────────────────────────────────────────────────────────────────

# For now, only link core system libraries.
# Uncomment as modules are implemented:
#
# target_link_libraries(persona2ep PRIVATE
#     SDL2::SDL2
#     OpenGL::GL
#     OpenAL::OpenAL
#     PNG::PNG
#     JPEG::JPEG
#     ZLIB::ZLIB
# )

# ───────────────────────────────────────────────────────────────────────
# Compiler settings
# ───────────────────────────────────────────────────────────────────────

target_compile_features(persona2ep PRIVATE cxx_std_17)

if(MSVC)
    target_compile_options(persona2ep PRIVATE /W4)
else()
    target_compile_options(persona2ep PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ───────────────────────────────────────────────────────────────────────
# Static analysis support
# ───────────────────────────────────────────────────────────────────────

include(${CMAKE_SOURCE_DIR}/cmake/modules/StaticAnalysis.cmake)

