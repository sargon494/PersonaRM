# ───────────────────────────────────────────────────────────────────────
# Test Helper Functions
# ───────────────────────────────────────────────────────────────────────

# Function to add a unit test executable
function(add_unit_test NAME)
    set(TEST_NAME ${NAME})

    # Get the source files for this test
    get_target_properties(${NAME} PROPERTIES SOURCES)
    set(SOURCES ${ARGN})

    # Create the test executable
    add_executable(${NAME} ${SOURCES})

    # Link against the main library
    target_link_libraries(${NAME} PRIVATE persona2ep)

    # Add to the test suite
    add_test(NAME ${TEST_NAME} COMMAND ${NAME})
    set_tests_properties(${TEST_NAME} PROPERTIES
        LABELS "unit"
        TIMEOUT 300
    )
endfunction()

# Function to add an integration test executable
function(add_integration_test NAME)
    set(TEST_NAME ${NAME})

    # Get the source files for this test
    get_target_properties(${NAME} PROPERTIES SOURCES)
    set(SOURCES ${ARGN})

    # Create the test executable
    add_executable(${NAME} ${SOURCES})

    # Link against the main library
    target_link_libraries(${NAME} PRIVATE persona2ep)

    # Add to the test suite
    add_test(NAME ${TEST_NAME} COMMAND ${NAME})
    set_tests_properties(${TEST_NAME} PROPERTIES
        LABELS "integration"
        TIMEOUT 600
    )
endfunction()

# Function to add a performance test executable
function(add_performance_test NAME)
    set(TEST_NAME ${NAME})

    # Get the source files for this test
    get_target_properties(${NAME} PROPERTIES SOURCES)
    set(SOURCES ${ARGN})

    # Create the test executable
    add_executable(${NAME} ${SOURCES})

    # Link against the main library
    target_link_libraries(${NAME} PRIVATE persona2ep)

    # Add to the test suite
    add_test(NAME ${TEST_NAME} COMMAND ${NAME})
    set_tests_properties(${TEST_NAME} PROPERTIES
        LABELS "performance"
        TIMEOUT 1800
    )
endfunction()

# Function to register a benchmark
function(add_benchmark NAME)
    set(BENCHMARK_NAME ${NAME})

    # Get the source files for this benchmark
    get_target_properties(${NAME} PROPERTIES SOURCES)
    set(SOURCES ${ARGN})

    # Create the benchmark executable
    add_executable(${NAME} ${SOURCES})

    # Link against the main library
    target_link_libraries(${NAME} PRIVATE persona2ep benchmark)

    # Add to the test suite
    add_test(NAME ${BENCHMARK_NAME} COMMAND ${NAME})
    set_tests_properties(${BENCHMARK_NAME} PROPERTIES
        LABELS "performance"
        TIMEOUT 1800
    )
endfunction()