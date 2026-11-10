# ───────────────────────────────────────────────────────────────────────
# Static Analysis Targets
# ───────────────────────────────────────────────────────────────────────

# Add static analysis target
add_custom_target(static-analysis
    COMMAND ${CMAKE_COMMAND} -E echo "Static analysis targets available:"
    COMMAND ${CMAKE_COMMAND} -E echo "  - clang-tidy (if available)"
    COMMAND ${CMAKE_COMMAND} -E echo "  - cppcheck (if available)"
    COMMAND ${CMAKE_COMMAND} -E echo "  - cppclean (if available)"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "To enable static analysis, set:"
    COMMAND ${CMAKE_COMMAND} -E echo "  -DENABLE_CLANG_TIDY=ON"
    COMMAND ${CMAKE_COMMAND} -E echo "  -DENABLE_CPPCHECK=ON"
)

# Add clang-tidy target
if(ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY_EXECUTABLE
        NAMES clang-tidy
        PATHS
            $ENV{PATH}
            /usr/local/bin
            /usr/bin
    )

    if(CLANG_TIDY_EXECUTABLE)
        add_custom_target(clang-tidy
            COMMAND ${CLANG_TIDY_EXECUTABLE}
                    --config-file=${CMAKE_SOURCE_DIR}/.clang-tidy
                    --export-fixes=${CMAKE_BINARY_DIR}/.clang-tidy-fixes.yaml
                    -- ${CMAKE_SOURCE_DIR}/src
            COMMENT "Running clang-tidy..."
        )

        add_custom_target(clang-tidy-fix
            COMMAND ${CLANG_TIDY_EXECUTABLE}
                    --config-file=${CMAKE_SOURCE_DIR}/.clang-tidy
                    --fix
                    --export-fixes=${CMAKE_BINARY_DIR}/.clang-tidy-fixes.yaml
                    -- ${CMAKE_SOURCE_DIR}/src
            COMMENT "Applying clang-tidy fixes..."
        )
    endif()
endif()

# Add cppcheck target
if(ENABLE_CPPCHECK)
    find_program(CPPCHECK_EXECUTABLE
        NAMES cppcheck
        PATHS
            $ENV{PATH}
            /usr/local/bin
            /usr/bin
    )

    if(CPPCHECK_EXECUTABLE)
        add_custom_target(cppcheck
            COMMAND ${CPPCHECK_EXECUTABLE}
                    --enable=style,warning,performance,portability
                    --inline-suppr
                    --suppress=*:*:*.h
                    --suppress=*:*:*.inl
                    --suppress=*:*:third_party/*
                    --suppress=*:*:vendor/*
                    --suppress=*:*:generated/*
                    --check-level=2
                    ${CMAKE_SOURCE_DIR}/src
            COMMENT "Running cppcheck..."
        )
    endif()
endif()

# Add cppclean target
if(ENABLE_CPPCLEAN)
    find_program(CPPCLEAN_EXECUTABLE
        NAMES cppclean
        PATHS
            $ENV{PATH}
            /usr/local/bin
            /usr/bin
    )

    if(CPPCLEAN_EXECUTABLE)
        add_custom_target(cppclean
            COMMAND ${CPPCLEAN_EXECUTABLE}
                    -i ${CMAKE_SOURCE_DIR}/src
            COMMENT "Running cppclean..."
        )
    endif()
endif()