add_subdirectory(generator ${CMAKE_CURRENT_SOURCE_DIR}/generator/build)

include_directories(
  ${LTTNG_INCLUDE_DIR}
)

set(ntirpc_tracepoints_LIB_SRCS
  lttng_probes.c
)

add_library(ntirpc_tracepoints MODULE ${ntirpc_tracepoints_LIB_SRCS})
#add_sanitizers(ntirpc_tracepoints)

target_link_libraries(ntirpc_tracepoints
  ${LTTNG_LIBRARIES}
)

add_dependencies(ntirpc_tracepoints ntirpc_generate_lttng_trace_headers)

set_target_properties(ntirpc_tracepoints PROPERTIES
  VERSION ${NTIRPC_VERSION}
  SOVERSION "${NTIRPC_MAJOR_VERSION}${NTIRPC_MINOR_VERSION}"
)

install(TARGETS ntirpc_tracepoints COMPONENT tracing DESTINATION ${LIB_INSTALL_DIR} )

add_library(ntirpc_lttng STATIC lttng_defines.c)
#add_sanitizers(ntirpc_lttng)

target_link_libraries(ntirpc_lttng
  ${LTTNG_LIBRARIES}
)

add_dependencies(ntirpc_lttng ntirpc_generate_lttng_trace_headers)
