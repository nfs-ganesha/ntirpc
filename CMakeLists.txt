# New TI-RPC  Cmake

# Current version as of Fedora 16.  Not tested with earlier.

cmake_minimum_required(VERSION 2.6.3)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/modules/")

# Add maintainer mode for (mainly) strict builds

include(${CMAKE_SOURCE_DIR}/cmake/maintainer_mode.cmake)

project(NTIRPC C)

# version numbers
set(NTIRPC_MAJOR_VERSION 4)
# This is .0 for a release, .N for a stable branch, blank for development
set(NTIRPC_MINOR_VERSION .1)
# -something for dev releases
set(NTIRPC_VERSION_EXTRA )
set(VERSION_COMMENT
  "Full-duplex and bi-directional ONC RPC on TCP."
)
# version string used for packaging
set(NTIRPC_VERSION
  "${NTIRPC_MAJOR_VERSION}${NTIRPC_MINOR_VERSION}${NTIRPC_VERSION_EXTRA}")
set(NTIRPC_VERSION_BASE
  "${NTIRPC_MAJOR_VERSION}${NTIRPC_MINOR_VERSION}")
# Up scope for embedding in ganesha
set(NTIRPC_VERSION_EMBED "${NTIRPC_VERSION}" PARENT_SCOPE)
set(NTIRPC_VERSION_BASE_EMBED "${NTIRPC_VERSION_BASE}" PARENT_SCOPE)
set(NTIRPC_VERSION_EXTRA_EMBED "${NTIRPC_VERSION_EXTRA}" PARENT_SCOPE)
set(NTIRPC_ABI_EMBED "${NTIRPC_MAJOR_VERSION}${NTIRPC_MINOR_VERSION}" PARENT_SCOPE)

set( PACKNAME "${NTIRPC_VERSION}" )

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Install destination, if built standalone
get_property(USE_LIB64 GLOBAL PROPERTY FIND_LIBRARY_USE_LIB64_PATHS)
if (USE_LIB64)
	set(LIB_INSTALL_DIR lib64 CACHE PATH
		"Specify name of libdir inside install path")
else (USE_LIB64)
	set(LIB_INSTALL_DIR lib CACHE PATH
		"Specify name of libdir inside install path")
endif (USE_LIB64)

set(SYSTEM_LIBRARIES ${SYSTEM_LIBRARIES})
set(BINARY_LIBRARIES ${BINARY_LIBRARIES})

include(GetGitRevisionDescription)
get_git_head_revision(GIT_REFSPEC _GIT_HEAD_COMMIT)
git_describe(_GIT_DESCRIBE)

# Allow overriding of prefix
if (OVERRIDE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX ${OVERRIDE_INSTALL_PREFIX} CACHE PATH
    "Install prefix" FORCE)
endif(OVERRIDE_INSTALL_PREFIX)
message(STATUS "OVERRIDE_INSTALL_PREFIX = ${OVERRIDE_INSTALL_PREFIX}")

# Set default base directory
set(NTIRPC_BASE_DIR ${PROJECT_SOURCE_DIR} CACHE
  PATH "Path to base source directory of NTIRPC")

# Build configure options

option (USE_GSS "enable RPCSEC_GSS support" ON)
if (USE_GSS)
  find_package(Krb5 REQUIRED gssapi)
  if(KRB5_FOUND)
    set(HAVE_KRB5 ON)
    set(_HAVE_GSSAPI ON)
  else(KRB5_FOUND)
    set(USE_GSS OFF)
  endif(KRB5_FOUND)
endif (USE_GSS)

option(USE_RPC_RDMA "platform supports RDMA" OFF)
if (USE_RPC_RDMA)
  find_package(RDMA REQUIRED)
  include_directories(${RDMA_INCLUDE_DIR})
  set(SYSTEM_LIBRARIES ${SYSTEM_LIBRARIES} ${RDMA_LIBRARY})
endif(USE_RPC_RDMA)

# MSPAC support -lwbclient link flag
option(_MSPAC_SUPPORT "enable mspac Winbind support" OFF)

option(USE_PROFILE "Build with gperf profiling" OFF)
if (USE_PROFILE)
  find_package(Gperftools)
  if(GPERFTOOLS_FOUND)
    set(BINARY_LIBRARIES ${BINARY_LIBRARIES} ${GPERFTOOLS_LIBRARIES})
  else(GPERFTOOLS_FOUND)
    message(WARNING "Couldn't find gperftools. Disabling USE_PROFILE")
    set(USE_PROFILE OFF)
  endif(GPERFTOOLS_FOUND)
endif(USE_PROFILE)

option(USE_LTTNG "Enable LTTng tracing" OFF)
if(USE_LTTNG)
  # Set LTTNG_PATH_HINT on the command line
  # if your LTTng is not in a standard place
  find_package(LTTng REQUIRED)
  if(LTTNG_FOUND)
    include_directories(${LTTNG_INCLUDE_DIR})
    set(USE_LTTNG_NTIRPC ON)
    set(BINARY_LIBRARIES ${BINARY_LIBRARIES} ntirpc_lttng)
  else(LTTNG_FOUND)
    message(WARNING "LTTng libraries not found. Disabling USE_LTTNG_NTIRPC")
    set(USE_LTTNG_NTIRPC OFF)
  endif(LTTNG_FOUND)
else(USE_LTTNG)
  set(USE_LTTNG_NTIRPC OFF)
endif(USE_LTTNG)

find_library(LIBURCU_LIB urcu-bp)
find_path(LIBURCU_INC urcu-bp.h)
include_directories(${LIBURCU_INC})
set(SYSTEM_LIBRARIES ${SYSTEM_LIBRARIES} ${LIBURCU_LIB})

# Choose a shortcut build config

IF(BUILD_CONFIG)
  INCLUDE(
  ${CMAKE_SOURCE_DIR}/cmake/build_configurations/${BUILD_CONFIG}.cmake)
ENDIF()

# Build source locations and parameters

set(ALLOCATOR "jemalloc" CACHE STRING
  "specify the memory allocator to use: jemalloc|tcmalloc|libc")

# Find packages and libs we need for building
include(CheckIncludeFiles)
include(TestBigEndian)

if(${CMAKE_SYSTEM_NAME} MATCHES "Linux")
  set(LINUX ON)
endif(${CMAKE_SYSTEM_NAME} MATCHES "Linux")

if(${CMAKE_SYSTEM_NAME} MATCHES "FreeBSD|Darwin")
  set(BSDBASED ON)
endif(${CMAKE_SYSTEM_NAME} MATCHES "FreeBSD|Darwin")

if(${CMAKE_SYSTEM_NAME} MATCHES "Windows")
  set(WINDOWS ON)
  if(${CMAKE_CXX_COMPILER_ID} MATCHES "MSVC")
    set(MSVC ON)
  endif(${CMAKE_CXX_COMPILER_ID} MATCHES "MSVC")
endif(${CMAKE_SYSTEM_NAME} MATCHES "Windows")

check_include_files(stdbool.h HAVE_STDBOOL_H)
check_include_files(strings.h HAVE_STRINGS_H)
check_include_files(string.h HAVE_STRING_H)

TEST_BIG_ENDIAN(BIGENDIAN)
if(${BIGENDIAN})
  set(WORDS_BIGENDIAN ON)
else()
  set(WORDS_BIGENDIAN OFF)
endif(${BIGENDIAN})

find_package(Threads REQUIRED)
find_package(EPOLL REQUIRED)
set(TIRPC_EPOLL ${EPOLL_FOUND})
find_package(Sanitizers)

if(_MSPAC_SUPPORT)
  find_package(WBclient REQUIRED)
  set(SYSTEM_LIBRARIES ${WBclient_LIBRARIES} ${SYSTEM_LIBRARIES})
endif(_MSPAC_SUPPORT)

if (BSDBASED)
   set(EXTRA_INCLUDE_DIR "/opt/ganesha/include")
else()
  # workaround bug in some include_directories when no extra includes
  set(EXTRA_INCLUDE_DIR "${NTIRPC_BASE_DIR}/ntirpc/")
endif(BSDBASED)

add_definitions(-DHAVE_CONFIG_H)

if (MSVC)
   add_definitions(-D_CRT_SECURE_NO_WARNINGS)
endif(MSVC)

include_directories(BEFORE
  "${PROJECT_BINARY_DIR}"
  "${NTIRPC_BASE_DIR}/ntirpc/"
  "${EXTRA_INCLUDE_DIR}"
)

set(SYSTEM_LIBRARIES
  ${LIBTIRPC_LIBRARIES}
  ${KRB5_LIBRARIES}
  ${SYSTEM_LIBRARIES}
)

if (NOT BSDBASED)
  find_package(NSL) # sockets
  set(SYSTEM_LIBRARIES
    ${SYSTEM_LIBRARIES}
    ${NSL_LIBRARY}
  )
endif (NOT BSDBASED)

set(LIBNTIRPC_MAP "${PROJECT_BINARY_DIR}/src/libntirpc.map")
# subst files (need add_custom_command for dependency, fyi)
configure_file(
  "${NTIRPC_BASE_DIR}/src/libntirpc.map.in.cmake"
  "${PROJECT_BINARY_DIR}/libntirpc.map"
)

add_subdirectory(src)
add_subdirectory(tests)

# display configuration vars

message(STATUS)
message(STATUS "-------------------------------------------------------")
message(STATUS "TIRPC_EPOLL = ${TIRPC_EPOLL}")
message(STATUS "USE_RPC_RDMA = ${USE_RPC_RDMA}")
message(STATUS "USE_GSS = ${USE_GSS}")
message(STATUS "USE_PROFILE = ${USE_PROFILE}")
message(STATUS "USE_LTTNG_NTIRPC = ${USE_LTTNG_NTIRPC}")

#force command line options to be stored in cache
set(_MSPAC_SUPPORT ${_MSPAC_SUPPORT}
  CACHE BOOL
   "compile with MSPAC extensions"
   FORCE)

set(TIRPC_EPOLL ${TIRPC_EPOLL}
  CACHE BOOL
   "platform has EPOLL or emulation"
   FORCE)

# grist files
configure_file(
  "${NTIRPC_BASE_DIR}/config-h.in.cmake"
  "${PROJECT_BINARY_DIR}/config.h"
)

configure_file(
  "${NTIRPC_BASE_DIR}/version-h.in.cmake"
  "${PROJECT_BINARY_DIR}/ntirpc/version.h"
)

configure_file(
  "${NTIRPC_BASE_DIR}/libntirpc.pc.in.cmake"
  "${PROJECT_BINARY_DIR}/libntirpc.pc"
  @ONLY
)

configure_file(
  "${NTIRPC_BASE_DIR}/libntirpc.spec-in.cmake"
  "${PROJECT_SOURCE_DIR}/libntirpc.spec"
)

if (NOT TARGET dist)
set(CPACK_COMPONENT_DAEMON_DISPLAY_NAME "libntirpc")

# Include custom config and cpack module
include(${CMAKE_SOURCE_DIR}/cmake/cpack_config.cmake)
include(CPack)

set( PKG_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}.tar.gz")
	add_custom_target(dist COMMAND ${CMAKE_MAKE_PROGRAM} package_source)
endif (NOT TARGET dist)

if (NOT TARGET rpm)
add_custom_target( rpm DEPENDS dist)
add_custom_command(TARGET rpm
                  COMMAND sh -c "rpmbuild -ta ${PKG_NAME}"
		  VERBATIM
		  DEPENDS dist)

set(RPMDEST "--define '_srcrpmdir ${CMAKE_CURRENT_BINARY_DIR}'")
add_custom_target( srpm DEPENDS dist)
add_custom_command(TARGET srpm
                  COMMAND sh -c "rpmbuild ${RPMDEST} -ts ${PKG_NAME}"
		  VERBATIM
		  DEPENDS dist)
endif (NOT TARGET rpm)
########### install files ###############

install(FILES  ${PROJECT_BINARY_DIR}/libntirpc.pc DESTINATION ${LIB_INSTALL_DIR}/pkgconfig)
install(DIRECTORY  ${NTIRPC_BASE_DIR}/ntirpc DESTINATION include)
install(FILES  ${PROJECT_BINARY_DIR}/ntirpc/version.h DESTINATION include/ntirpc)
