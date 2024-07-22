
SET(rpcping_SRCS
  rpcping.c
  )
add_executable(rpcping ${rpcping_SRCS})
target_link_libraries(rpcping ntirpc
  ${BINARY_LIBRARIES}
  ${CMAKE_THREAD_LIBS_INIT}
  ${LTTNG_LIBRARIES}
  -ldl)

if(USE_LTTNG)
target_link_libraries(rpcping ntirpc_lttng)
endif(USE_LTTNG)
