cmake_minimum_required(VERSION 3.24)
project(willow LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# Where the Willow tools write their output (out_dir in config.local.toml).
# The game build reads the generated C from <dir>/recomp, and by default the
# executable looks for the extracted disks in <dir>/disks.
set(WILLOW_BUILD_DIR "${CMAKE_SOURCE_DIR}/build" CACHE PATH "Output folder of the Willow tools")

# Link the MinGW runtime statically: the executables then need no compiler DLLs,
# and can't pick up an incompatible libstdc++ from elsewhere on PATH.
if(MINGW)
  add_link_options(-static)
endif()

enable_testing()
add_subdirectory(runtime)
