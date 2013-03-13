# New TI-RPC  Cmake

# Current version as of Fedora 16.  Not tested with earlier.

cmake_minimum_required(VERSION 2.6.3)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/modules/")

# Add maintainer mode for (mainly) strict builds

include(${CMAKE_SOURCE_DIR}/cmake/maintainer_mode.cmake)

project(NTIRPC C)

set(${PROJECT_NAME}_MAJOR_VERSION 1)
set(${PROJECT_NAME}_MINOR_VERSION 0)
set(${PROJECT_NAME}_PATCH_LEVEL 0)
set(VERSION_COMMENT
  "Full-duplex and bi-directional ONC RPC on TCP."
)
include(GetGitRevisionDescription)
get_git_head_revision(GIT_REFSPEC _GIT_HEAD_COMMIT)
git_describe(_GIT_DESCRIBE)

# Build configure options

option (USE_GSS "enable RPCSEC_GSS support" ON)

option(TIRPC_EPOLL "platform supports EPOLL or emulation" OFF)

# MSPAC support -lwbclient link flag
option(_MSPAC_SUPPORT "enable mspac Winbind support" ON)

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

if(KRB5_FOUND)
  set(HAVE_KRB5 ON)
  set(KRB5_VERSION 194)  # hand code until we do krb5-config --version magic
  set(_HAVE_GSSAPI ON)
endif(KRB5_FOUND)

set(SYSTEM_LIBRARIES ${SYSTEM_LIBRARIES})

if(_MSPAC_SUPPORT)
  find_package(WBclient REQUIRED)
  set(SYSTEM_LIBRARIES ${WBclient_LIBRARIES} ${SYSTEM_LIBRARIES})
endif(_MSPAC_SUPPORT)

if (FREEBSD)
   set(EXTRA_INCLUDE_DIR "/opt/ganesha/include")
endif(FREEBSD)

add_definitions(-DHAVE_CONFIG_H)

if (MSVC)
   add_definitions(-D_CRT_SECURE_NO_WARNINGS)
endif(MSVC)

include_directories(
  "${PROJECT_BINARY_DIR}"
  "${PROJECT_SOURCE_DIR}/tirpc/"
  "${EXTRA_INCLUDE_DIR}"
)

# Find misc system libs

find_library(LIBRT rt)   # extended Pthreads functions
find_library(LIBNSL nsl) # sockets

set(SYSTEM_LIBRARIES
  ${LIBTIRPC_LIBRARIES}
  ${KRB5_LIBRARIES}
  gssapi_krb5
  ${SYSTEM_LIBRARIES}
  ${LIBDL}
  ${CMAKE_THREAD_LIBS_INIT}
  ${LIBRT}
)

add_subdirectory(src)

# display configuration vars

message(STATUS)
message(STATUS "-------------------------------------------------------")
message(STATUS "TIRPC_EPOLL = ${TIRPC_EPOLL}")

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
  "${PROJECT_SOURCE_DIR}/config-h.in.cmake"
  "${PROJECT_BINARY_DIR}/config.h"
)

configure_file(
  "${PROJECT_SOURCE_DIR}/libntirpc.pc.in.cmake"
  "${PROJECT_BINARY_DIR}/libntirpc.pc"
)

########### install files ###############

install(FILES  libntirpc.pc DESTINATION bin)


