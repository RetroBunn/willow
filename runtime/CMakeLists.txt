# The runtime library, its headless tests, and (once the tools have generated
# the C) the game executable.

# ymfm (BSD-3-Clause), a git submodule pinned in .gitmodules: the YM2151.
set(YMFM_DIR "${PROJECT_SOURCE_DIR}/external/ymfm/src")
if(NOT EXISTS "${YMFM_DIR}/ymfm_opm.cpp")
  message(FATAL_ERROR "ymfm is missing: run `git submodule update --init`")
endif()
add_library(ymfm STATIC "${YMFM_DIR}/ymfm_opm.cpp")
target_include_directories(ymfm SYSTEM PUBLIC "${YMFM_DIR}")  # third-party: not held to our warnings

add_library(willow_core STATIC
  src/boot.cpp
  src/common.cpp
  src/devices.cpp
  src/filesystem.cpp
  src/human68k.cpp
  src/input.cpp
  src/iocs.cpp
  src/machine.cpp
  src/memory.cpp
  src/mods.cpp
  src/png.cpp
  src/settings.cpp
  src/sound.cpp
  src/toml.cpp
  src/video.cpp
)
target_include_directories(willow_core PUBLIC include PRIVATE src)
target_link_libraries(willow_core PRIVATE ymfm)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(willow_core PRIVATE -Wall -Wextra)
endif()

add_executable(willow_tests tests/test_runtime.cpp)
target_include_directories(willow_tests PRIVATE src)
target_link_libraries(willow_tests PRIVATE willow_core)
add_test(NAME runtime COMMAND willow_tests)

set(WILLOW_RECOMP_DIR "${WILLOW_BUILD_DIR}/recomp")
if(NOT EXISTS "${WILLOW_RECOMP_DIR}/programs.c")
  message(STATUS "No generated code in ${WILLOW_RECOMP_DIR}; run `python -m tools.willow recomp` "
                 "to build the game. Building the runtime library and its tests only.")
  return()
endif()

# The recompiled programs. Process exits unwind through this code as C++
# exceptions, so it needs unwind tables.
file(GLOB_RECURSE WILLOW_GENERATED CONFIGURE_DEPENDS "${WILLOW_RECOMP_DIR}/*.c")
add_library(willow_game STATIC ${WILLOW_GENERATED})
target_include_directories(willow_game PRIVATE "${WILLOW_RECOMP_DIR}")
target_link_libraries(willow_game PUBLIC willow_core)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(willow_game PRIVATE -fexceptions -Wno-unused-label)
endif()

# Headless runner for development: fixed frame count, snapshots, no window or sound.
add_executable(willow_dump src/frontend/dump.cpp)
target_include_directories(willow_dump PRIVATE src)
target_link_libraries(willow_dump PRIVATE willow_game willow_core)
target_compile_definitions(willow_dump PRIVATE WILLOW_DEFAULT_DATA_DIR="${WILLOW_BUILD_DIR}/disks")

include(FetchContent)
FetchContent_Declare(SDL3
  URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz
  URL_HASH SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL3)

add_executable(willow src/frontend/main.cpp)
target_include_directories(willow PRIVATE src)
target_link_libraries(willow PRIVATE willow_game willow_core SDL3::SDL3-static)
target_compile_definitions(willow PRIVATE WILLOW_DEFAULT_DATA_DIR="${WILLOW_BUILD_DIR}/disks")
