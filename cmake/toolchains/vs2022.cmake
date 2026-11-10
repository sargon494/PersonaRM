# ───────────────────────────────────────────────────────────────────────
# Windows Toolchain - Visual Studio 2022
# ───────────────────────────────────────────────────────────────────────

# Set the Visual Studio toolchain
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)

# Find the Visual Studio installation
find_program(VS_DEV_PATH
    NAMES vcvarsall.bat
    PATHS
        "C:/Program Files (x86)/Microsoft Visual Studio/2022/Community"
        "C:/Program Files (x86)/Microsoft Visual Studio/2022/Professional"
        "C:/Program Files (x86)/Microsoft Visual Studio/2022/Enterprise"
        "C:/Program Files/Microsoft Visual Studio/2022/Community"
        "C:/Program Files/Microsoft Visual Studio/2022/Professional"
        "C:/Program Files/Microsoft Visual Studio/2022/Enterprise"
    NO_DEFAULT_PATH
)

if(NOT VS_DEV_PATH)
    message(FATAL_ERROR "Visual Studio 2022 not found. Please install Visual Studio 2022 with the C++ workload.")
endif()

# Set environment variables from vcvarsall.bat
execute_process(
    COMMAND ${VS_DEV_PATH} amd64 --quiet
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    RESULT_VARIABLE VC_RESULT
)

if(NOT VC_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to run vcvarsall.bat")
endif()

# Set the compiler
set(CMAKE_CXX_COMPILER "cl.exe")
set(CMAKE_C_COMPILER "cl.exe")

# Set the linker
set(CMAKE_LINKER "link.exe")

# Set the archiver
set(CMAKE_AR "lib.exe")

# Set the assembler
set(CMAKE_ASM_COMPILER "ml64.exe")

# Set the object file suffix
set(CMAKE_OBJECT_FILE_SUFFIX ".obj")

# Set the executable file suffix
set(CMAKE_EXECUTABLE_SUFFIX ".exe")

# Set the library file suffix
set(CMAKE_STATIC_LIBRARY_SUFFIX ".lib")
set(CMAKE_SHARED_LIBRARY_SUFFIX ".lib")

# Set the debug library file suffix
set(CMAKE_STATIC_LIBRARY_SUFFIX_DEBUG ".lib")
set(CMAKE_SHARED_LIBRARY_SUFFIX_DEBUG ".lib")

# Set the debug executable file suffix
set(CMAKE_EXECUTABLE_SUFFIX_DEBUG ".exe")

# Set the debug object file suffix
set(CMAKE_OBJECT_FILE_SUFFIX_DEBUG ".obj")

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
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W4 /WX- /Od /Ob2 /D WIN32 /D _WINDOWS")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /O2 /Ob2 /DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /Zi /Ob0 /Od /RTC1")

# Set the linker flags
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /OPT:REF /OPT:ICF")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /OPT:REF /OPT:ICF")

# Set the linker script
set(CMAKE_LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/cmake/linker.txt")

# Set the preprocessor definitions
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DWIN32_LEAN_AND_MEAN")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DNOMINMAX")

# ───────────────────────────────────────────────────────────────────────
# Add custom targets
# ───────────────────────────────────────────────────────────────────────

add_custom_target(vcvars
    COMMAND ${VS_DEV_PATH} amd64 --quiet
    COMMENT "Running vcvarsall.bat"
)

add_custom_target(clean_vs
    COMMAND ${VS_DEV_PATH} amd64 --clean --quiet
    COMMENT "Cleaning Visual Studio build files"
)