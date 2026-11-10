# ───────────────────────────────────────────────────────────────────────
# Find GoogleTest
# ───────────────────────────────────────────────────────────────────────

find_package(PkgConfig QUIET)

if(PkgConfig_FOUND)
    pkg_check_modules(GOOGLETEST QUIET gtest gtest_main gmock gmock_main)
endif()

if(NOT GOOGLETEST_FOUND)
    find_path(GOOGLETEST_INCLUDE_DIR
        NAMES gtest/gtest.h
        PATHS
            ${CMAKE_SOURCE_DIR}/third_party/googletest/include
            ${CMAKE_SOURCE_DIR}/third_party/googletest
            $ENV{gtest_dir}
            $ENV{gtest_ROOT}
            ${gtest_DIR}
        PATH_SUFFIXES include
    )

    find_library(GOOGLETEST_LIBRARY
        NAMES gtest
        PATHS
            ${CMAKE_SOURCE_DIR}/third_party/googletest/lib
            ${CMAKE_SOURCE_DIR}/third_party/googletest/lib/cmake/gtest
            $ENV{gtest_dir}
            $ENV{gtest_ROOT}
            ${gtest_DIR}
        PATH_SUFFIXES lib
    )

    find_library(GOOGLETEST_MAIN_LIBRARY
        NAMES gtest_main
        PATHS
            ${CMAKE_SOURCE_DIR}/third_party/googletest/lib
            ${CMAKE_SOURCE_DIR}/third_party/googletest/lib/cmake/gtest
            $ENV{gtest_dir}
            $ENV{gtest_ROOT}
            ${gtest_DIR}
        PATH_SUFFIXES lib
    )
endif()

set(GOOGLETEST_FOUND OFF)

if(GOOGLETEST_INCLUDE_DIR AND GOOGLETEST_LIBRARY)
    set(GOOGLETEST_FOUND ON)
    set(GOOGLETEST_INCLUDE_DIRS ${GOOGLETEST_INCLUDE_DIR})
    set(GOOGLETEST_LIBRARIES ${GOOGLETEST_LIBRARY})
    set(GOOGLETEST_MAIN_LIBRARY ${GOOGLETEST_MAIN_LIBRARY})
endif()