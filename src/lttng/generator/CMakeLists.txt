cmake_minimum_required(VERSION 3.18)

project(generate_lttng LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS 1)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_C_STANDARD_INCLUDE_DIRECTORIES ${CMAKE_C_IMPLICIT_INCLUDE_DIRECTORIES})


find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})

add_library(generate_lttng generate_lttng.cpp)
target_include_directories(generate_lttng PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(generate_lttng PRIVATE ${LLVM_DEFINITIONS_LIST})
target_link_libraries(generate_lttng
    LLVM
    clang-cpp
    stdc++fs
)

add_executable(generate_lttng_for_files generate_lttng_for_files.cpp)
add_executable(generate_lttng_for_project generate_lttng_for_project.cpp)

target_include_directories(generate_lttng_for_files PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(generate_lttng_for_files PRIVATE ${LLVM_DEFINITIONS_LIST})

target_link_libraries(generate_lttng_for_files
    generate_lttng
    LLVM
    clang-cpp
    stdc++fs
)

target_include_directories(generate_lttng_for_project PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(generate_lttng_for_project PRIVATE ${LLVM_DEFINITIONS_LIST})

target_link_libraries(generate_lttng_for_project
    generate_lttng
    LLVM
    clang-cpp
    stdc++fs
)

target_compile_options(generate_lttng_for_files PRIVATE "-Wall" "-Werror")
target_compile_options(generate_lttng_for_project PRIVATE "-Wall" "-Werror")

add_subdirectory(test)

# We create a library to compile failure test code
# We need to compile it so it has compile commands that can be used by the test
add_library(failure_tests STATIC EXCLUDE_FROM_ALL
    ${CMAKE_CURRENT_SOURCE_DIR}/failure_tests/test_code_failure_too_many_arguments.c
    ${CMAKE_CURRENT_SOURCE_DIR}/failure_tests/test_code_failure_too_many_arguments_with_arrays.c
    ${CMAKE_CURRENT_SOURCE_DIR}/failure_tests/test_code_failure_too_many_arguments_for_format.c
    ${CMAKE_CURRENT_SOURCE_DIR}/failure_tests/test_code_failure_not_enough_arguments_for_format.c
    ${CMAKE_CURRENT_SOURCE_DIR}/failure_tests/test_code_failure_invalid_format1.c
    ${CMAKE_CURRENT_SOURCE_DIR}/failure_tests/test_code_failure_invalid_format2.c
    ${CMAKE_CURRENT_SOURCE_DIR}/failure_tests/test_code_failure_invalid_format3.c
    ${CMAKE_CURRENT_SOURCE_DIR}/failure_tests/test_code_failure_invalid_format4.c
    ${CMAKE_CURRENT_SOURCE_DIR}/failure_tests/test_code_failure_unsupported_struct.c
    ${CMAKE_CURRENT_SOURCE_DIR}/failure_tests/test_code_failure_unsupported_typedef_struct.c
    ${CMAKE_CURRENT_SOURCE_DIR}/failure_tests/test_code_failure_unsupported_union.c
    ${CMAKE_CURRENT_SOURCE_DIR}/failure_tests/test_code_failure_missing_log_level.c
    ${CMAKE_CURRENT_SOURCE_DIR}/failure_tests/test_code_failure_missing_log_format.c
    ${CMAKE_CURRENT_SOURCE_DIR}/failure_tests/test_code_failure_bad_fnc_format.c
    ${CMAKE_CURRENT_SOURCE_DIR}/failure_tests/test_code_failure_has_side_effects_func_call.c
    ${CMAKE_CURRENT_SOURCE_DIR}/failure_tests/test_code_failure_has_side_effects_func_expression.c
    ${CMAKE_CURRENT_SOURCE_DIR}/failure_tests/test_code_failure_has_side_effects_inc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/failure_tests/test_code_failure_has_side_effects_volatile.c
)
target_compile_options(failure_tests PRIVATE "-DLTTNG_PARSING")
target_include_directories(failure_tests PRIVATE
    include
    test
    ${LTTNGUST_INCLUDE_DIRS}
)

# Create symlinks for executables that need to be used by the test
execute_process(
    COMMAND ${CMAKE_COMMAND} -E create_symlink
        ${CMAKE_CURRENT_BINARY_DIR}/generate_lttng_for_files
        ${CMAKE_CURRENT_SOURCE_DIR}/generate_lttng_for_files
)

# Create symlink for compile commands
if(NOT ${CMAKE_BINARY_DIR} STREQUAL ${CMAKE_CURRENT_SOURCE_DIR})
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_BINARY_DIR}/compile_commands.json
            ${CMAKE_CURRENT_SOURCE_DIR}/compile_commands.json
    )
endif(NOT ${CMAKE_BINARY_DIR} STREQUAL ${CMAKE_CURRENT_SOURCE_DIR})

# Run tests at the end of the build
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/ran_tests
     COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/run_tests.sh ${} && touch ${CMAKE_CURRENT_BINARY_DIR}/ran_tests
     DEPENDS generate_lttng_for_files generate_lttng_for_project test_bin
     COMMENT "Run tests"
)

add_custom_target(tests_target
     ALL
     DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/ran_tests
)
