execute_process(
	COMMAND ${CMAKE_COMMAND} -E make_directory
		"${CMAKE_CURRENT_BINARY_DIR}/generated_include/auto"
	COMMAND_ERROR_IS_FATAL ANY
)

# Generate build rules for generated test traces
execute_process(
    COMMAND bash -c "${CMAKE_CURRENT_SOURCE_DIR}/../generate_cmake_rules.py \
        --project_path ${CMAKE_CURRENT_SOURCE_DIR} \
        --provider_include_base_path auto/ \
        --traces_output_dir ${CMAKE_CURRENT_SOURCE_DIR}/auto/ \
        --targets_output_path ${CMAKE_CURRENT_BINARY_DIR}/lttng_generation_rules.cmake \
        --file_properties_output_path ${CMAKE_CURRENT_BINARY_DIR}/lttng_generation_file_properties.cmake"
    COMMAND_ERROR_IS_FATAL ANY
)

include("${CMAKE_CURRENT_BINARY_DIR}/lttng_generation_rules.cmake")
include("${CMAKE_CURRENT_BINARY_DIR}/lttng_generation_file_properties.cmake")

set(TestBinFiles
    test_code.c
    test_code2.c
    test_code-tp.c
)

add_executable(test_bin
    ${TestBinFiles}
)
add_dependencies(test_bin generate_lttng_trace_headers)
find_package(LTTngUST REQUIRED MODULE)

if(NOT ${LTTNGUST_FOUND})
message(FATAL_ERROR "LTTNG library not found")
endif(NOT ${LTTNGUST_FOUND})

target_link_libraries(test_bin
    LTTng::UST
)

target_include_directories(test_bin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../include/
    "${CMAKE_CURRENT_BINARY_DIR}/generated_include/"
    ${LTTNGUST_INCLUDE_DIRS}
)

# Create symlinks for executables that need to be used by the test
execute_process(
    COMMAND ${CMAKE_COMMAND} -E create_symlink
        ${CMAKE_CURRENT_BINARY_DIR}/test_bin
        ${CMAKE_CURRENT_SOURCE_DIR}/test_bin
)