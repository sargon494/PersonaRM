# ───────────────────────────────────────────────────────────────────────
# tests/unit/ CMakeLists.txt
# ───────────────────────────────────────────────────────────────────────

add_executable(unit_test_core
    test_core.cpp
)

target_link_libraries(unit_test_core PRIVATE
    personarm_core
)

target_compile_features(unit_test_core PRIVATE cxx_std_17)

if(MSVC)
    target_compile_definitions(unit_test_core PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS NOMINMAX)
endif()

add_test(NAME unit_test_core COMMAND unit_test_core)
set_tests_properties(unit_test_core PROPERTIES
    LABELS "unit"
    TIMEOUT 120
)


add_executable(unit_test_phase7 test_phase7.cpp)
target_link_libraries(unit_test_phase7 PRIVATE personarm_core)
target_include_directories(unit_test_phase7 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(unit_test_phase7 PRIVATE cxx_std_17)
add_test(NAME unit_test_phase7 COMMAND unit_test_phase7)
set_tests_properties(unit_test_phase7 PROPERTIES LABELS "unit" TIMEOUT 120)

add_executable(unit_test_psx_analyze test_psx_analyze.cpp)
target_link_libraries(unit_test_psx_analyze PRIVATE psx_analyzer)
target_compile_features(unit_test_psx_analyze PRIVATE cxx_std_17)
add_test(NAME unit_test_psx_analyze COMMAND unit_test_psx_analyze)
set_tests_properties(unit_test_psx_analyze PROPERTIES LABELS "unit" TIMEOUT 120)

add_executable(unit_test_recomp test_recomp.cpp)
target_link_libraries(unit_test_recomp PRIVATE personarm_core)
target_compile_features(unit_test_recomp PRIVATE cxx_std_17)
add_test(NAME unit_test_recomp COMMAND unit_test_recomp)
set_tests_properties(unit_test_recomp PROPERTIES LABELS "unit" TIMEOUT 120)

add_executable(unit_test_cpu_conformance test_cpu_conformance.cpp)
target_link_libraries(unit_test_cpu_conformance PRIVATE personarm_core)
target_compile_features(unit_test_cpu_conformance PRIVATE cxx_std_17)
add_test(NAME unit_test_cpu_conformance COMMAND unit_test_cpu_conformance)
set_tests_properties(unit_test_cpu_conformance PROPERTIES LABELS "unit" TIMEOUT 120)

add_executable(unit_test_gte test_gte.cpp)
target_link_libraries(unit_test_gte PRIVATE personarm_core)
target_compile_features(unit_test_gte PRIVATE cxx_std_17)
add_test(NAME unit_test_gte COMMAND unit_test_gte)
set_tests_properties(unit_test_gte PROPERTIES LABELS "unit" TIMEOUT 120)
add_executable(unit_test_gpu test_gpu.cpp)
target_link_libraries(unit_test_gpu PRIVATE personarm_core)
target_compile_features(unit_test_gpu PRIVATE cxx_std_17)
add_test(NAME unit_test_gpu COMMAND unit_test_gpu)
set_tests_properties(unit_test_gpu PROPERTIES LABELS "unit" TIMEOUT 120)

add_executable(unit_test_trace test_trace.cpp)
target_link_libraries(unit_test_trace PRIVATE personarm_core)
target_compile_features(unit_test_trace PRIVATE cxx_std_17)
add_test(NAME unit_test_trace COMMAND unit_test_trace)
set_tests_properties(unit_test_trace PROPERTIES LABELS "unit" TIMEOUT 120)

add_executable(unit_test_spu test_spu.cpp)
target_link_libraries(unit_test_spu PRIVATE personarm_core)
target_compile_features(unit_test_spu PRIVATE cxx_std_17)
add_test(NAME unit_test_spu COMMAND unit_test_spu)
set_tests_properties(unit_test_spu PROPERTIES LABELS "unit" TIMEOUT 120)
add_executable(unit_test_cdrom test_cdrom.cpp)
target_link_libraries(unit_test_cdrom PRIVATE personarm_core)
target_compile_features(unit_test_cdrom PRIVATE cxx_std_17)
add_test(NAME unit_test_cdrom COMMAND unit_test_cdrom)
set_tests_properties(unit_test_cdrom PROPERTIES LABELS "unit" TIMEOUT 120)

add_executable(unit_test_dma test_dma.cpp)
target_link_libraries(unit_test_dma PRIVATE personarm_core)
target_compile_features(unit_test_dma PRIVATE cxx_std_17)
add_test(NAME unit_test_dma COMMAND unit_test_dma)
set_tests_properties(unit_test_dma PROPERTIES LABELS "unit" TIMEOUT 120)

add_executable(unit_test_hle_kernel test_hle_kernel.cpp)
target_link_libraries(unit_test_hle_kernel PRIVATE personarm_core)
target_compile_features(unit_test_hle_kernel PRIVATE cxx_std_17)
add_test(NAME unit_test_hle_kernel COMMAND unit_test_hle_kernel)
set_tests_properties(unit_test_hle_kernel PROPERTIES LABELS "unit" TIMEOUT 120)

add_executable(unit_test_engine_bus test_engine_bus.cpp)
target_link_libraries(unit_test_engine_bus PRIVATE personarm_core)
target_compile_features(unit_test_engine_bus PRIVATE cxx_std_17)
add_test(NAME unit_test_engine_bus COMMAND unit_test_engine_bus)
set_tests_properties(unit_test_engine_bus PROPERTIES LABELS "unit" TIMEOUT 120)

add_executable(unit_test_mdec test_mdec.cpp)
target_link_libraries(unit_test_mdec PRIVATE personarm_core)
target_compile_features(unit_test_mdec PRIVATE cxx_std_17)
add_test(NAME unit_test_mdec COMMAND unit_test_mdec)
set_tests_properties(unit_test_mdec PROPERTIES LABELS "unit" TIMEOUT 120)

add_executable(unit_test_xa test_xa.cpp)
target_link_libraries(unit_test_xa PRIVATE personarm_core)
target_compile_features(unit_test_xa PRIVATE cxx_std_17)
add_test(NAME unit_test_xa COMMAND unit_test_xa)
set_tests_properties(unit_test_xa PROPERTIES LABELS "unit" TIMEOUT 120)

add_executable(unit_test_decoder_crosscheck test_decoder_crosscheck.cpp)
target_link_libraries(unit_test_decoder_crosscheck PRIVATE personarm_core)
target_compile_features(unit_test_decoder_crosscheck PRIVATE cxx_std_17)
add_test(NAME unit_test_decoder_crosscheck COMMAND unit_test_decoder_crosscheck)
set_tests_properties(unit_test_decoder_crosscheck PROPERTIES LABELS "unit" TIMEOUT 300)

add_executable(unit_test_host_logic test_host_logic.cpp ${CMAKE_SOURCE_DIR}/src/host/host_logic.cpp)
target_include_directories(unit_test_host_logic PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(unit_test_host_logic PRIVATE cxx_std_17)
add_test(NAME unit_test_host_logic COMMAND unit_test_host_logic)
set_tests_properties(unit_test_host_logic PROPERTIES LABELS "unit" TIMEOUT 120)

# Gate G2 (synthetic): the synthetic program is written at build time, tools/recomp_gen turns it
# into C++, and the test compares that code against the interpreter.
add_executable(g2_synth_writer g2_synth_writer.cpp)
target_compile_features(g2_synth_writer PRIVATE cxx_std_17)
set(G2_BIN ${CMAKE_CURRENT_BINARY_DIR}/g2_synth.bin)
set(G2_ENTRIES ${CMAKE_CURRENT_BINARY_DIR}/g2_synth.entries)
set(G2_GEN ${CMAKE_CURRENT_BINARY_DIR}/g2_synth_gen.cpp)
add_custom_command(OUTPUT ${G2_BIN} ${G2_ENTRIES}
    COMMAND g2_synth_writer ${G2_BIN} ${G2_ENTRIES}
    DEPENDS g2_synth_writer)
add_custom_command(OUTPUT ${G2_GEN}
    COMMAND recomp_gen ${G2_BIN} 0x80010000 ${G2_GEN} --entries-file ${G2_ENTRIES} --module g2
    DEPENDS recomp_gen ${G2_BIN} ${G2_ENTRIES})
add_executable(unit_test_recomp_g2 test_recomp_g2.cpp ${G2_GEN})
target_link_libraries(unit_test_recomp_g2 PRIVATE personarm_core)
target_compile_features(unit_test_recomp_g2 PRIVATE cxx_std_17)
add_test(NAME unit_test_recomp_g2 COMMAND unit_test_recomp_g2)
set_tests_properties(unit_test_recomp_g2 PROPERTIES LABELS "unit" TIMEOUT 300)

# Recompiled execution inside the Engine (synthetic): the boot program of recomp_engine_program.h is
# generated at build time like the G2 program and run against the interpreter frame by frame.
add_executable(recomp_engine_writer recomp_engine_writer.cpp)
target_compile_features(recomp_engine_writer PRIVATE cxx_std_17)
set(RENGINE_BIN ${CMAKE_CURRENT_BINARY_DIR}/recomp_engine.bin)
set(RENGINE_ENTRIES ${CMAKE_CURRENT_BINARY_DIR}/recomp_engine.entries)
set(RENGINE_GEN ${CMAKE_CURRENT_BINARY_DIR}/recomp_engine_gen.cpp)
add_custom_command(OUTPUT ${RENGINE_BIN} ${RENGINE_ENTRIES}
    COMMAND recomp_engine_writer ${RENGINE_BIN} ${RENGINE_ENTRIES}
    DEPENDS recomp_engine_writer)
add_custom_command(OUTPUT ${RENGINE_GEN}
    COMMAND recomp_gen ${RENGINE_BIN} 0x80010000 ${RENGINE_GEN} --entries-file ${RENGINE_ENTRIES} --module rengine
    DEPENDS recomp_gen ${RENGINE_BIN} ${RENGINE_ENTRIES})
# Overlays (synthetic): a resident main and two overlays of one slot, each generated as its own module.
add_executable(recomp_overlay_writer recomp_overlay_writer.cpp)
target_compile_features(recomp_overlay_writer PRIVATE cxx_std_17)
set(ROVL_STEM ${CMAKE_CURRENT_BINARY_DIR}/rovl)
add_custom_command(OUTPUT ${ROVL_STEM}_main.bin ${ROVL_STEM}_main.entries ${ROVL_STEM}_a.bin ${ROVL_STEM}_a.entries
                          ${ROVL_STEM}_b.bin ${ROVL_STEM}_b.entries
    COMMAND recomp_overlay_writer ${ROVL_STEM}
    DEPENDS recomp_overlay_writer)
set(ROVL_GEN)
foreach(_part main:0x80010000 a:0x80040000 b:0x80040000)
    string(REPLACE ":" ";" _pb ${_part})
    list(GET _pb 0 _p)
    list(GET _pb 1 _base)
    add_custom_command(OUTPUT ${ROVL_STEM}_${_p}_gen.cpp
        COMMAND recomp_gen ${ROVL_STEM}_${_p}.bin ${_base} ${ROVL_STEM}_${_p}_gen.cpp --entries-file ${ROVL_STEM}_${_p}.entries --module rovl_${_p}
        DEPENDS recomp_gen ${ROVL_STEM}_${_p}.bin ${ROVL_STEM}_${_p}.entries)
    list(APPEND ROVL_GEN ${ROVL_STEM}_${_p}_gen.cpp)
endforeach()
add_executable(unit_test_recomp_engine test_recomp_engine.cpp ${RENGINE_GEN} ${ROVL_GEN})
target_link_libraries(unit_test_recomp_engine PRIVATE personarm_core)
target_include_directories(unit_test_recomp_engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(unit_test_recomp_engine PRIVATE cxx_std_17)
add_test(NAME unit_test_recomp_engine COMMAND unit_test_recomp_engine)
set_tests_properties(unit_test_recomp_engine PROPERTIES LABELS "unit" TIMEOUT 300)

add_executable(unit_test_frame_interp test_frame_interp.cpp)
target_link_libraries(unit_test_frame_interp PRIVATE personarm_core)
target_compile_features(unit_test_frame_interp PRIVATE cxx_std_17)
add_test(NAME unit_test_frame_interp COMMAND unit_test_frame_interp)
set_tests_properties(unit_test_frame_interp PROPERTIES LABELS "unit" TIMEOUT 120)
