# TinyCC (libtcc, LGPL-2.1) for on-device recompilation: personarm_native compiles the C generated
# from the player's own disc into a DLL on the player's PC. Fetched at configure time from the
# official repository at a pinned commit (never vendored) and built as libtcc.dll, linked dynamically
# (LGPL). CONFIG_TCC_PREDEFS compiles TinyCC's predefined macros into the DLL, and the generated
# code is built -nostdlib, so libtcc.dll is the whole compiler runtime: no include or lib folder.
#
# Defines the target `libtcc` (shared) and TINYCC_SOURCE_DIR / TINYCC_COMMIT for packaging.

set(TINYCC_COMMIT 205f264b9e01b47d18f9182e4009eadfc4019219)  # mob, 2026-11-20

include(FetchContent)
FetchContent_Declare(tinycc
    GIT_REPOSITORY https://repo.or.cz/tinycc.git
    GIT_TAG ${TINYCC_COMMIT}
    SOURCE_SUBDIR no-cmake)  # TinyCC has no CMakeLists: fetch only, the targets are below
FetchContent_MakeAvailable(tinycc)
set(TINYCC_SOURCE_DIR ${tinycc_SOURCE_DIR})

file(READ ${TINYCC_SOURCE_DIR}/VERSION _tcc_version)
string(STRIP "${_tcc_version}" _tcc_version)
set(_tcc_gen ${CMAKE_CURRENT_BINARY_DIR}/tinycc_gen)
file(WRITE ${_tcc_gen}/config.h.in "#define TCC_VERSION \"${_tcc_version}\"\n#define TCC_GITHASH \"${TINYCC_COMMIT}\"\n#define CONFIG_TCC_PREDEFS 1\n")
configure_file(${_tcc_gen}/config.h.in ${_tcc_gen}/config.h COPYONLY)

# c2str (TinyCC's conftest.c): include/tccdefs.h -> tccdefs_.h, the predefined macros as C strings.
add_executable(tinycc_c2str ${TINYCC_SOURCE_DIR}/conftest.c)
target_compile_definitions(tinycc_c2str PRIVATE C2STR=1 _CRT_SECURE_NO_WARNINGS)
add_custom_command(OUTPUT ${_tcc_gen}/tccdefs_.h
    COMMAND tinycc_c2str ${TINYCC_SOURCE_DIR}/include/tccdefs.h ${_tcc_gen}/tccdefs_.h
    DEPENDS tinycc_c2str ${TINYCC_SOURCE_DIR}/include/tccdefs.h)

add_library(libtcc SHARED ${TINYCC_SOURCE_DIR}/libtcc.c ${_tcc_gen}/tccdefs_.h)
set_target_properties(libtcc PROPERTIES PREFIX "" OUTPUT_NAME libtcc C_STANDARD 99)
target_include_directories(libtcc PRIVATE ${_tcc_gen} PUBLIC $<BUILD_INTERFACE:${TINYCC_SOURCE_DIR}>)
target_compile_definitions(libtcc PRIVATE ONE_SOURCE=1 LIBTCC_AS_DLL _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
if(WIN32)
    target_compile_definitions(libtcc PRIVATE TCC_TARGET_PE TCC_TARGET_X86_64)
else()
    target_compile_definitions(libtcc PRIVATE TCC_TARGET_X86_64)
endif()
# Licence and source offer travel with the DLL (LGPL-2.1 section 6): the release copies both.
file(WRITE ${_tcc_gen}/TinyCC-SOURCE.txt
    "libtcc${CMAKE_SHARED_LIBRARY_SUFFIX} is TinyCC ${_tcc_version} (GNU LGPL 2.1, see TinyCC-LICENSE.txt), built unmodified from\n"
    "https://repo.or.cz/tinycc.git commit ${TINYCC_COMMIT}\n"
    "by cmake/TinyCC.cmake in https://github.com/sargon494/PersonaRM. Source: either repository.\n")
add_custom_command(TARGET libtcc POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${TINYCC_SOURCE_DIR}/COPYING $<TARGET_FILE_DIR:libtcc>/TinyCC-LICENSE.txt
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_tcc_gen}/TinyCC-SOURCE.txt $<TARGET_FILE_DIR:libtcc>/TinyCC-SOURCE.txt)
if(MSVC)
    target_compile_options(libtcc PRIVATE /W0)
else()
    target_compile_options(libtcc PRIVATE -w)
    target_link_libraries(libtcc PRIVATE ${CMAKE_DL_LIBS} m)
endif()
