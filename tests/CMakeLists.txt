
SET(rpcping_SRCS
  rpcping.c
  )

SET(monitoring_SRCS test_monitoring.c)
add_executable(test_monitoring ${monitoring_SRCS})
target_include_directories(test_monitoring PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/monitoring/include)

target_link_libraries(test_monitoring ntirpcmonitoring)

target_link_libraries(test_monitoring ntirpc
  ${BINARY_LIBRARIES}
  ${CMAKE_THREAD_LIBS_INIT}
  ${LTTNG_LIBRARIES}
  -ldl)
add_executable(rpcping ${rpcping_SRCS})
target_link_libraries(rpcping ntirpc
  ${BINARY_LIBRARIES}
  ${CMAKE_THREAD_LIBS_INIT}
  ${LTTNG_LIBRARIES}
  -ldl)

if(USE_LTTNG)
target_link_libraries(rpcping ntirpc_lttng)
include("${CMAKE_CURRENT_BINARY_DIR}/../ntirpc_lttng_generation_file_properties.cmake")
endif(USE_LTTNG)
