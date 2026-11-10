# ───────────────────────────────────────────────────────────────────────
# Linux Toolchain
# ───────────────────────────────────────────────────────────────────────

# Set the Linux toolchain
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_VERSION 1)

# Set the compiler
set(CMAKE_CXX_COMPILER "g++")
set(CMAKE_C_COMPILER "gcc")

# Set the linker
set(CMAKE_LINKER "ld")

# Set the archiver
set(CMAKE_AR "ar")

# Set the assembler
set(CMAKE_ASM_COMPILER "gcc")

# Set the object file suffix
set(CMAKE_OBJECT_FILE_SUFFIX ".o")

# Set the executable file suffix
set(CMAKE_EXECUTABLE_SUFFIX "")

# Set the library file suffix
set(CMAKE_STATIC_LIBRARY_SUFFIX ".a")
set(CMAKE_SHARED_LIBRARY_SUFFIX ".so")

# Set the debug library file suffix
set(CMAKE_STATIC_LIBRARY_SUFFIX_DEBUG ".a")
set(CMAKE_SHARED_LIBRARY_SUFFIX_DEBUG ".so")

# Set the debug executable file suffix
set(CMAKE_EXECUTABLE_SUFFIX_DEBUG "")

# Set the debug object file suffix
set(CMAKE_OBJECT_FILE_SUFFIX_DEBUG ".o")

# Set the intermediate directory
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/Release")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/Release")

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/Release")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/Release")

# Set the compiler flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic -Wconversion -Wcast-qual")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g3 -O0 -DDEBUG")

# Set the linker flags
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--as-needed -Wl,--no-undefined")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} -s")

# ───────────────────────────────────────────────────────────────────────
# Add custom targets
# ───────────────────────────────────────────────────────────────────────

add_custom_target(clean-linux
    COMMAND rm -rf "${CMAKE_BINARY_DIR}/Release"
    COMMENT "Cleaning Linux build files"
)