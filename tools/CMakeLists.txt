# ───────────────────────────────────────────────────────────────────────
# tools/ — offline developer tools (not part of the game runtime)
# ───────────────────────────────────────────────────────────────────────

add_library(psx_analyzer STATIC psx_analyze/analyzer.cpp psx_analyze/analyzer.h)
target_include_directories(psx_analyzer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/psx_analyze)
target_link_libraries(psx_analyzer PUBLIC personarm_core)
target_compile_features(psx_analyzer PUBLIC cxx_std_17)

add_executable(psx_analyze psx_analyze/main.cpp)
target_link_libraries(psx_analyze PRIVATE psx_analyzer)

if(MSVC)
    target_compile_options(psx_analyzer PRIVATE /W4)
    target_compile_definitions(psx_analyzer PUBLIC _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS NOMINMAX)
else()
    target_compile_options(psx_analyzer PRIVATE -Wall -Wextra -Wpedantic)
endif()
