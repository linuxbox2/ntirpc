
# Ganesha CMake integration

add_subdirectory(src)

include_directories(
  "${PROJECT_BINARY_DIR}/include"
  "${PROJECT_SOURCE_DIR}/libntirpc/tirpc/"
  "${PROJECT_SOURCE_DIR}/include"
  "${OS_INCLUDE_DIR}"
  "${PROJECT_SOURCE_DIR}/libntirpc/src/"
)


