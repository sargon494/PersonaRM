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
    core/state_io.h
    recomp/mips_decode.h
    recomp/mips_decode.cpp
    recomp/cfg.h
    recomp/cfg.cpp
    recomp/codegen.h
    recomp/codegen.cpp
    recomp/runtime/recomp_runtime.h
    recomp/runtime/recomp_runtime.cpp
    recomp/runtime/prm_abi.h
    recomp/runtime/module_loader.h
    recomp/runtime/module_loader.cpp
    recomp/runtime/native_cache.h
    recomp/runtime/native_cache.cpp
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
    rendering/vram_texture_hook.cpp
    rendering/vram_texture_hook.h
    rendering/ps1_gpu.cpp
    rendering/ps1_gpu.h
    rendering/frame_interp.cpp
    rendering/frame_interp.h
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
    engine/file_mods.cpp
    engine/file_mods.h
    engine/gameplay_patches.cpp
    engine/gameplay_patches.h
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
target_link_libraries(personarm_core PUBLIC ${CMAKE_DL_LIBS})  # module_loader: dlopen

# Save states record the build that wrote them (Engine::BuildId; another build loads with a warning).
# shortcut: the commit is read at configure time, so a rebuild after a new commit keeps the old id
# until CMake reruns; upgrade to a configure_file regenerated every build if that bites.
execute_process(COMMAND git rev-parse --short=12 HEAD WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                OUTPUT_VARIABLE PRM_GIT_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT PRM_GIT_COMMIT)
    set(PRM_GIT_COMMIT "dev")
endif()
set_source_files_properties(engine/engine.cpp PROPERTIES
    COMPILE_DEFINITIONS "PRM_BUILD_ID=\"${PROJECT_VERSION}-${PRM_GIT_COMMIT}\"")

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

# Recompiled game code linked at build time (developers, local only). Generated .c files come from
# personarm_native --emit-c or tools/recomp_gen and must stay outside the repository; default builds,
# CI and releases link the empty registry (players get the same code built on their PC as a DLL,
# recomp/runtime/module_loader.h). One library, linked by personarm and personarm_sdl.
add_library(personarm_recomp_modules STATIC)
target_link_libraries(personarm_recomp_modules PUBLIC personarm_core)
set(PRM_RECOMP_GENERATED_DIR "" CACHE PATH "Directory (outside the repo) with generated C, one <module>.c per module")
if(PRM_RECOMP_GENERATED_DIR)
    get_filename_component(_prm_gen_dir "${PRM_RECOMP_GENERATED_DIR}" ABSOLUTE)
    string(FIND "${_prm_gen_dir}/" "${CMAKE_SOURCE_DIR}/" _prm_gen_pos)
    if(_prm_gen_pos EQUAL 0)
        message(FATAL_ERROR "PRM_RECOMP_GENERATED_DIR must be outside the repository (generated game code is never committed)")
    endif()
    file(GLOB _prm_gen_sources CONFIGURE_DEPENDS "${_prm_gen_dir}/*.c")
    if(NOT _prm_gen_sources)
        message(FATAL_ERROR "PRM_RECOMP_GENERATED_DIR=${_prm_gen_dir} has no .c files (regenerate older .cpp output)")
    endif()
    set(_prm_registry "${CMAKE_CURRENT_BINARY_DIR}/recomp_generated_registry.cpp")
    set(_decl "")
    set(_list "")
    foreach(_src ${_prm_gen_sources})
        get_filename_component(_mod "${_src}" NAME_WE)
        string(APPEND _decl "extern \"C\" const prm_module kRecompModule_${_mod};\n")
        string(APPEND _list "FromAbi(&kRecompModule_${_mod}), ")
    endforeach()
    file(CONFIGURE OUTPUT "${_prm_registry}" CONTENT
        "// Generated by CMake from PRM_RECOMP_GENERATED_DIR. Do not edit.\n#include \"recomp/runtime/generated_modules.h\"\n${_decl}\nnamespace personarm::recomp {\nstd::vector<const Module*> GeneratedModules() { return {${_list}}; }\n}\n")
    target_sources(personarm_recomp_modules PRIVATE ${_prm_gen_sources} "${_prm_registry}")
    target_include_directories(personarm_recomp_modules PRIVATE recomp/runtime)  # prm_abi.h
    # Generated code: big functions full of labels; warnings there are noise.
    set_source_files_properties(${_prm_gen_sources} PROPERTIES COMPILE_OPTIONS "$<IF:$<C_COMPILER_ID:MSVC>,/W0;/bigobj,-w>")
    message(STATUS "Recompiled modules linked into personarm: ${_prm_gen_sources}")
else()
    target_sources(personarm_recomp_modules PRIVATE recomp/runtime/generated_modules_none.cpp)
endif()
target_link_libraries(personarm PRIVATE personarm_recomp_modules)

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
        host/settings.cpp
        host/settings.h
        rendering/gpu_renderer.cpp
        rendering/gpu_renderer.h
    )
    target_link_libraries(personarm_sdl PRIVATE personarm_core personarm_recomp_modules SDL3::SDL3)

    # GPU renderer on Vulkan: its HLSL compiled to SPIR-V at build time (Windows compiles it at run time for
    # D3D12). Without glslangValidator (apt: glslang-tools) --renderer gpu falls back to software here.
    if(NOT WIN32)
        find_program(GLSLANG_VALIDATOR glslangValidator)
        if(GLSLANG_VALIDATOR)
            set(spirv_dir ${CMAKE_BINARY_DIR}/generated/gpu_spirv)
            add_custom_command(OUTPUT ${spirv_dir}/gpu_spirv.h
                COMMAND ${CMAKE_COMMAND} -E make_directory ${spirv_dir}
                COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/rendering/gpu_renderer.cpp -DOUT=${spirv_dir}
                        -DGLSLANG=${GLSLANG_VALIDATOR} -P ${PROJECT_SOURCE_DIR}/cmake/gpu_spirv.cmake
                DEPENDS rendering/gpu_renderer.cpp ${PROJECT_SOURCE_DIR}/cmake/gpu_spirv.cmake
                COMMENT "GPU renderer shaders: HLSL -> SPIR-V")
            add_custom_target(prm_gpu_spirv DEPENDS ${spirv_dir}/gpu_spirv.h)
            add_library(prm_gpu_shaders INTERFACE)
            target_include_directories(prm_gpu_shaders INTERFACE ${spirv_dir})
            target_compile_definitions(prm_gpu_shaders INTERFACE PRM_GPU_SPIRV)
            add_dependencies(prm_gpu_shaders prm_gpu_spirv)
            target_link_libraries(personarm_sdl PRIVATE prm_gpu_shaders)
        else()
            message(STATUS "glslangValidator not found: no Vulkan shaders, --renderer gpu falls back to software")
        endif()
    endif()
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

    # Optional launcher (Rust, docs/LAUNCHER.md): `cmake --build . --target launcher` builds it with cargo
    # and copies it next to personarm_sdl. Not part of ALL, so the C++ build never needs Rust.
    find_program(CARGO_EXECUTABLE cargo HINTS "$ENV{USERPROFILE}/.cargo/bin" "$ENV{HOME}/.cargo/bin")
    if(CARGO_EXECUTABLE)
        add_custom_target(launcher
            COMMAND ${CARGO_EXECUTABLE} build --release --manifest-path ${CMAKE_SOURCE_DIR}/launcher/Cargo.toml
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_SOURCE_DIR}/launcher/target/release/PersonaRM${CMAKE_EXECUTABLE_SUFFIX} $<TARGET_FILE_DIR:personarm_sdl>
            COMMENT "Building the launcher with cargo")
        add_dependencies(launcher personarm_sdl)
    endif()
endif()

# ───────────────────────────────────────────────────────────────────────
# Static Analysis Target
# ───────────────────────────────────────────────────────────────────────

if(EXISTS "${CMAKE_SOURCE_DIR}/cmake/modules/StaticAnalysis.cmake")
    include(${CMAKE_SOURCE_DIR}/cmake/modules/StaticAnalysis.cmake)
endif()
