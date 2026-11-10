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

