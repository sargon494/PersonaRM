# ───────────────────────────────────────────────────────────────────────
# Linux Toolchain Configuration
# ───────────────────────────────────────────────────────────────────────

# Set the compiler if not already set
if(NOT CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER "g++")
endif()

if(NOT CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER "gcc")
endif()

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release")

# Compiler and Linker flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0 -DDEBUG")
