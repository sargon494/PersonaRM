cmake_minimum_required(VERSION 3.20)

project(Persona2EP
    VERSION 0.1.0
    DESCRIPTION "Persona 2: Eternal Punishment - Native Port"
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

# ───────────────────────────────────────────────────────────────────────
# Find dependencies
# ───────────────────────────────────────────────────────────────────────

# SDL2
find_package(SDL2 REQUIRED COMPONENTS SDL2)

# OpenGL
find_package(OpenGL REQUIRED)

# OpenAL
find_package(OpenAL REQUIRED)

# PNG
find_package(PNG REQUIRED)

# JPEG
find_package(JPEG REQUIRED)

# ZLIB
find_package(ZLIB REQUIRED)

# ───────────────────────────────────────────────────────────────────────
# Add subdirectories
# ───────────────────────────────────────────────────────────────────────

add_subdirectory(src)

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ───────────────────────────────────────────────────────────────────────
# Install rules
# ───────────────────────────────────────────────────────────────────────

include(GNUInstallDirs)

install(TARGETS persona2ep
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES
    README.md
    LICENSE
    CONTRIBUTING.md
    AGENT.md
    ARCHITECTURE.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
)

# ───────────────────────────────────────────────────────────────────────
# Print summary
# ───────────────────────────────────────────────────────────────────────

message(STATUS "")
message(STATUS "=== Persona 2: Eternal Punishment - Native Port ===")
message(STATUS "Version: ${PROJECT_VERSION}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "SDL2: ${SDL2_FOUND}")
message(STATUS "OpenAL: ${OpenAL_FOUND}")
message(STATUS "PNG: ${PNG_FOUND}")
message(STATUS "JPEG: ${JPEG_FOUND}")
message(STATUS "Tests: ${BUILD_TESTS}")
message(STATUS "===========================================")
message(STATUS "")