# Ganesha CMake integration

# version numbers
set(NTIRPC_MAJOR_VERSION 1)
set(NTIRPC_MINOR_VERSION 3)
set(NTIRPC_PATCHLEVEL 0)

# version string used for packaging
set(NTIRPC_VERSION
  "${NTIRPC_MAJOR_VERSION}.${NTIRPC_MINOR_VERSION}.${NTIRPC_PATCHLEVEL}")

# LIB_INSTALL_DIR set by Ganesha

set(LIBNTIRPC_MAP "${PROJECT_BINARY_DIR}/libntirpc/src/libntirpc.map")
# subst files (need add_custom_command for dependency, fyi)
configure_file(
  "${PROJECT_SOURCE_DIR}/libntirpc/src/libntirpc.map.in.cmake"
  "${LIBNTIRPC_MAP}"
)

if(TIRPC_EPOLL)
  add_definitions(-DTIRPC_EPOLL)
endif(TIRPC_EPOLL)

if(USE_NFS_RDMA)
  add_definitions(-DUSE_RPC_RDMA)
endif(USE_NFS_RDMA)

include_directories(
  "${PROJECT_BINARY_DIR}/include"
  "${PROJECT_SOURCE_DIR}/libntirpc/ntirpc/"
  "${PROJECT_SOURCE_DIR}/include"
  "${OS_INCLUDE_DIR}"
  "${PROJECT_SOURCE_DIR}/libntirpc/src/"
)

add_subdirectory(../src ${PROJECT_BINARY_DIR}/src/libntirpc/src)
