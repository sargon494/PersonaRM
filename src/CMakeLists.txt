# ───────────────────────────────────────────────────────────────────────
# src/ CMakeLists.txt
# PersonaRM - Native Launcher & Port Engine
# ───────────────────────────────────────────────────────────────────────

# Subsystem source collections
set(LAUNCHER_SOURCES
    launcher/launcher.cpp
    launcher/launcher.h
    launcher/config_io.cpp
    launcher/config_io.h
    launcher/portal.cpp
    launcher/portal.h
)

set(CORE_SOURCES
    core/memory_manager.cpp
    core/memory_manager.h
    core/logger.cpp
    core/logger.h
    core/json_lite.cpp
    core/json_lite.h
    core/hash.h
    recomp/mips_decode.h
    recomp/mips_decode.cpp
    recomp/cfg.h
    recomp/cfg.cpp
    recomp/codegen.h
    recomp/codegen.cpp
    recomp/runtime/recomp_runtime.h
    recomp/runtime/recomp_runtime.cpp
    core/save_manager.cpp
    core/save_manager.h
    trace/trace.cpp
    trace/trace.h
)

set(INPUT_SOURCES
    input/input_manager.cpp
    input/input_manager.h
    input/input_profile.cpp
    input/input_profile.h
    input/mouse_navigation.cpp
    input/mouse_navigation.h
)

set(AUDIO_SOURCES
    audio/audio_system.cpp
    audio/audio_system.h
    audio/vag_decoder.cpp
    audio/vag_decoder.h
    audio/spu_emulator.cpp
    audio/spu_emulator.h
    audio/xa_adpcm.cpp
    audio/xa_adpcm.h
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
    loader/png_codec.cpp
    loader/png_codec.h
    loader/ps1_bios_hle.cpp
    loader/ps1_bios_hle.h
    loader/exe_loader.cpp
    loader/exe_loader.h
    loader/str_demuxer.cpp
    loader/str_demuxer.h
)
set(RENDERING_SOURCES
    rendering/renderer.cpp
    rendering/renderer.h
    rendering/texture_override.cpp
    rendering/texture_override.h
    rendering/ps1_gpu.cpp
    rendering/ps1_gpu.h
    rendering/window_context.cpp
    rendering/window_context.h
    rendering/gl_renderer.cpp
    rendering/gl_renderer.h
    rendering/mdec_decoder.cpp
    rendering/mdec_decoder.h
    rendering/bs_decoder.cpp
    rendering/bs_decoder.h
)
set(HW_SOURCES
    hw/cdrom.cpp
    hw/cdrom.h
    hw/dma.cpp
    hw/dma.h
    hw/mdec.cpp
    hw/mdec.h
)
set(ENGINE_SOURCES
    engine/engine.cpp
    engine/engine.h
)

# ───────────────────────────────────────────────────────────────────────
# Core Engine Static Library: personarm_core
# ───────────────────────────────────────────────────────────────────────

add_library(personarm_core STATIC
    ${LAUNCHER_SOURCES}
    ${CORE_SOURCES}
    ${INPUT_SOURCES}
    ${AUDIO_SOURCES}
    ${CPU_SOURCES}
    ${LOADER_SOURCES}
    ${RENDERING_SOURCES}
    ${ENGINE_SOURCES}
    ${HW_SOURCES}
)


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
# SDL3 host: personarm_sdl (optional, -DPRM_WITH_SDL3=ON)
# ───────────────────────────────────────────────────────────────────────

if(PRM_WITH_SDL3)
    find_package(SDL3 CONFIG QUIET)
    if(NOT SDL3_FOUND)
        message(STATUS "SDL3 not found: fetching release-3.4.18 (zlib license)")
        include(FetchContent)
        set(SDL_SHARED ON CACHE BOOL "" FORCE)
        set(SDL_STATIC OFF CACHE BOOL "" FORCE)
        set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(SDL3
            GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
            GIT_TAG release-3.4.18
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(SDL3)
    endif()

    add_executable(personarm_sdl
        host/sdl_main.cpp
        host/host_logic.cpp
        host/host_logic.h
    )
    target_link_libraries(personarm_sdl PRIVATE personarm_core SDL3::SDL3)
    if(MSVC)
        target_compile_options(personarm_sdl PRIVATE /W4)
    else()
        target_compile_options(personarm_sdl PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    if(WIN32)
        # Put SDL3.dll next to the executable.
        add_custom_command(TARGET personarm_sdl POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:personarm_sdl> $<TARGET_FILE_DIR:personarm_sdl>
            COMMAND_EXPAND_LISTS)
    endif()
    install(TARGETS personarm_sdl RUNTIME DESTINATION bin)
endif()

# ───────────────────────────────────────────────────────────────────────
# Static Analysis Target
# ───────────────────────────────────────────────────────────────────────

if(EXISTS "${CMAKE_SOURCE_DIR}/cmake/modules/StaticAnalysis.cmake")
    include(${CMAKE_SOURCE_DIR}/cmake/modules/StaticAnalysis.cmake)
endif()
