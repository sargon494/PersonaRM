cmake_minimum_required(VERSION 3.20)

project(PersonaRM
    VERSION 0.2.0
    DESCRIPTION "PersonaRM - Native Launcher & Port Engine (Persona 2: Eternal Punishment)"
    LANGUAGES CXX C
)

# ───────────────────────────────────────────────────────────────────────
# Project-wide settings
# ───────────────────────────────────────────────────────────────────────

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Set default build type
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "Setting build type to 'Release' as none was specified.")
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

# Set output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Release)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Release)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Release)

# ───────────────────────────────────────────────────────────────────────
# Build options
# ───────────────────────────────────────────────────────────────────────

option(BUILD_TESTS "Build tests" OFF)
option(BUILD_DOCS "Build documentation" OFF)
option(BUILD_SHARED_LIBS "Build shared libraries" OFF)
option(PRM_WITH_SDL3 "Build the personarm_sdl window/input/audio host (SDL3)" OFF)

# ───────────────────────────────────────────────────────────────────────
# Subdirectories
# ───────────────────────────────────────────────────────────────────────

add_subdirectory(src)
add_subdirectory(tools)

if(BUILD_TESTS)
    enable_testing()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
        add_subdirectory(tests)
    endif()
endif()

# ───────────────────────────────────────────────────────────────────────
# Install rules
# ───────────────────────────────────────────────────────────────────────

include(GNUInstallDirs)

install(FILES
    README.md
    LICENSE
    CONTRIBUTING.md
    AGENT.md
    ARCHITECTURE.md
    docs/GAMEPLAY_AND_CONTROLS.md
    docs/PROJECT_AUDIT.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
)

# ───────────────────────────────────────────────────────────────────────
# Print summary
# ───────────────────────────────────────────────────────────────────────

message(STATUS "")
message(STATUS "=== PersonaRM - Native Launcher & Port Suite ===")
message(STATUS "Version: ${PROJECT_VERSION}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Target Executable: personarm")
message(STATUS "Tests: ${BUILD_TESTS}")
message(STATUS "================================================")
message(STATUS "")
