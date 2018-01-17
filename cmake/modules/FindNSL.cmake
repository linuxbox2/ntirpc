# - Find NSL
#
# This module defines the following variables:
#    NSL_FOUND       = Was NSL found or not?
#
# On can set NSL_PATH_HINT before using find_package(NSL) and the
# module with use the PATH as a hint to find NSL.
#
# The hint can be given on the command line too:
#   cmake -DNSL_PATH_HINT=/DATA/ERIC/NSL /path/to/source

#find_path(NSL_INCLUDE_DIR
  #NAMES yp.h
  #PATHS ${NSL_HINT_PATH}
  #PATH_SUFFIXES nsl/rpcsvc
  #DOC "The NSL include headers")

find_library(NSL_LIBRARY nsl PATH_SUFFIXES nsl)

# handle the QUIETLY and REQUIRED arguments and set PRELUDE_FOUND to TRUE if
# all listed variables are TRUE
include(FindPackageHandleStandardArgs)
#FIND_PACKAGE_HANDLE_STANDARD_ARGS(NSL REQUIRED_VARS NSL_INCLUDE_DIR NSL_LIBRARY)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(NSL REQUIRED_VARS NSL_LIBRARY)

mark_as_advanced(NSL_INCLUDE_DIR)
mark_as_advanced(NSL_LIBRARY)
