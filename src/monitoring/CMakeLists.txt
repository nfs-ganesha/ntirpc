########### next target ###############

set(CMAKE_CXX_STANDARD 17)

SET(ntirpcmonitoring_SRCS
  monitoring.cc
)

add_library(ntirpcmonitoring ${ntirpcmonitoring_SRCS})
add_sanitizers(ntirpcmonitoring)
set_target_properties(ntirpcmonitoring PROPERTIES COMPILE_FLAGS "-fPIC")
target_include_directories(ntirpcmonitoring     PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/prometheus-cpp-lite/core/include)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pedantic-errors -Werror -Wall -Wextra")

########### install files ###############
