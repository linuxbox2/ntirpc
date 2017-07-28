# New TI-RPC  Cmake

# Current version as of Fedora 16.  Not tested with earlier.

cmake_minimum_required(VERSION 2.6.3)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/modules/")

# Add maintainer mode for (mainly) strict builds

include(${CMAKE_SOURCE_DIR}/cmake/maintainer_mode.cmake)

project(NTIRPC C)

# version numbers
set(NTIRPC_MAJOR_VERSION 1)
set(NTIRPC_MINOR_VERSION 6)
set(NTIRPC_PATCH_LEVEL 0)
set(VERSION_COMMENT
  "Full-duplex and bi-directional ONC RPC on TCP."
)
# version string used for packaging
set(NTIRPC_VERSION
  "${NTIRPC_MAJOR_VERSION}.${NTIRPC_MINOR_VERSION}.${NTIRPC_PATCH_LEVEL}")
# Up scope for embedding in ganesha
set(NTIRPC_VERSION_EMBED "${NTIRPC_VERSION}" PARENT_SCOPE)
set(NTIRPC_ABI_EMBED "${NTIRPC_MAJOR_VERSION}.${NTIRPC_MINOR_VERSION}" PARENT_SCOPE)

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

option(USE_RPC_RDMA "platform supports RDMA" OFF)
if (USE_RPC_RDMA)
  find_package(RDMA REQUIRED)
  include_directories(${RDMA_INCLUDE_DIR})
  set(SYSTEM_LIBRARIES ${SYSTEM_LIBRARIES} ${RDMA_LIBRARY})
endif(USE_RPC_RDMA)

# MSPAC support -lwbclient link flag
option(_MSPAC_SUPPORT "enable mspac Winbind support" OFF)

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

if(${CMAKE_SYSTEM_NAME} MATCHES "FreeBSD")
  set(FREEBSD ON)
endif(${CMAKE_SYSTEM_NAME} MATCHES "FreeBSD")

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
find_package(Krb5 REQUIRED gssapi)
find_package(EPOLL REQUIRED)
set(TIRPC_EPOLL ${EPOLL_FOUND})

if(KRB5_FOUND)
  set(HAVE_KRB5 ON)
  set(KRB5_VERSION 194)  # hand code until we do krb5-config --version magic
  set(_HAVE_GSSAPI ON)
endif(KRB5_FOUND)

if(_MSPAC_SUPPORT)
  find_package(WBclient REQUIRED)
  set(SYSTEM_LIBRARIES ${WBclient_LIBRARIES} ${SYSTEM_LIBRARIES})
endif(_MSPAC_SUPPORT)

if (FREEBSD)
   set(EXTRA_INCLUDE_DIR "/opt/ganesha/include")
else()
  # workaround bug in some include_directories when no extra includes
  set(EXTRA_INCLUDE_DIR "${NTIRPC_BASE_DIR}/ntirpc/")
endif(FREEBSD)

add_definitions(-DHAVE_CONFIG_H)

if (MSVC)
   add_definitions(-D_CRT_SECURE_NO_WARNINGS)
endif(MSVC)

include_directories(BEFORE
  "${PROJECT_BINARY_DIR}"
  "${NTIRPC_BASE_DIR}/ntirpc/"
  "${EXTRA_INCLUDE_DIR}"
)

# Find misc system libs
find_library(LIBRT rt)   # extended Pthreads functions
find_library(LIBNSL nsl) # sockets

set(SYSTEM_LIBRARIES
  ${LIBTIRPC_LIBRARIES}
  ${KRB5_LIBRARIES}
  ${SYSTEM_LIBRARIES}
  ${LIBRT}
  ${LIBNSL}
)

set(LIBNTIRPC_MAP "${PROJECT_BINARY_DIR}/src/libntirpc.map")
# subst files (need add_custom_command for dependency, fyi)
configure_file(
  "${NTIRPC_BASE_DIR}/src/libntirpc.map.in.cmake"
  "${PROJECT_BINARY_DIR}/libntirpc.map"
)

add_subdirectory(src)

# display configuration vars

message(STATUS)
message(STATUS "-------------------------------------------------------")
message(STATUS "TIRPC_EPOLL = ${TIRPC_EPOLL}")
message(STATUS "USE_RPC_RDMA = ${USE_RPC_RDMA}")

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
)

configure_file(
  "${NTIRPC_BASE_DIR}/libntirpc.spec-in.cmake"
  "${PROJECT_SOURCE_DIR}/libntirpc.spec"
)

# Define CPACK component (to deal with sub packages)
set(CPACK_COMPONENTS_ALL daemon fsal headers )
set(CPACK_COMPONENT_DAEMON_DISPLAY_NAME "libntirpc")

# Include custom config and cpack module
include(${CMAKE_SOURCE_DIR}/cmake/cpack_config.cmake)
include(CPack)

set( PKG_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}.tar.gz")
add_custom_target(dist COMMAND ${CMAKE_MAKE_PROGRAM} package_source)

########### install files ###############

install(FILES  ${PROJECT_BINARY_DIR}/libntirpc.pc DESTINATION ${LIB_INSTALL_DIR}/pkgconfig)
install(DIRECTORY  ${NTIRPC_BASE_DIR}/ntirpc DESTINATION include)
install(FILES  ${PROJECT_BINARY_DIR}/ntirpc/version.h DESTINATION include/ntirpc)
