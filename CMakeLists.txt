
# Ganesha CMake integration

if(TIRPC_EPOLL)
  add_definitions(-DTIRPC_EPOLL)
endif(TIRPC_EPOLL)

if(USE_NFS_MSK)
  add_definitions(-DUSE_RPC_RDMA)
endif(USE_NFS_MSK)

include_directories(
  "${PROJECT_BINARY_DIR}/include"
  "${PROJECT_SOURCE_DIR}/libntirpc/ntirpc/"
  "${PROJECT_SOURCE_DIR}/include"
  "${OS_INCLUDE_DIR}"
  "${PROJECT_SOURCE_DIR}/libntirpc/src/"
)

add_subdirectory(src)
