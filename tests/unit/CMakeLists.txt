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
