# ---------------------------------------------------------------------------
# Rally Championship Remaster - the build.
#
#   cmake -S port -B port/build -G Ninja
#   cmake --build port/build
#
# Cross-compiling:
#
#   Windows   -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/x86_64-w64-mingw32.cmake
#             -DRALLY_SDL2_MINGW=/path/to/SDL2-2.30.0
#   aarch64   -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux-gnu.cmake
#             -DRALLY_SYSROOT=/path/to/sysroot
#
# Nothing generates into the source tree: the original game's data never
# enters the repository, and an out-of-source build is enforced below.
# ---------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.16)

project(rally-port
        VERSION   1.0.0
        LANGUAGES C
        DESCRIPTION "Rally Championship (1999) reimplementation")

if(CMAKE_BINARY_DIR STREQUAL CMAKE_SOURCE_DIR)
  message(FATAL_ERROR
    "In-source builds are refused (nothing generates into the source tree).\n"
    "Use:  cmake -S port -B port/build")
endif()

# ---- language and warnings ------------------------------------------------

set(CMAKE_C_STANDARD          99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS        OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_library(rally_c_flags INTERFACE)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(rally_c_flags INTERFACE -Wall -Wextra -Wpedantic)
elseif(MSVC)
  target_compile_options(rally_c_flags INTERFACE /W4)
endif()

# -O3 by default: the simulation gives bit-identical results at -O2 and -O3
# (no fused multiply-add is emitted), and -O3 is measurably faster on the
# handheld.  -DRALLY_OPT=-O2 goes back; -DRALLY_LTO=ON tries link-time
# optimisation.  An existing build directory keeps its cached value.
set(RALLY_OPT "-O3" CACHE STRING "Optimisation flag for Release")
option(RALLY_LTO "Build Release with -flto" OFF)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  set(CMAKE_C_FLAGS_RELEASE "${RALLY_OPT} -DNDEBUG" CACHE STRING "" FORCE)
  if(RALLY_LTO)
    string(APPEND CMAKE_C_FLAGS_RELEASE " -flto")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${RALLY_OPT} -flto=auto" CACHE STRING "" FORCE)
  endif()
endif()

# ---- the float model is not negotiable ------------------------------------
# -ffast-math links crtfastmath.o, which turns on flush-to-zero and
# denormals-are-zero at startup.  The original game copies an integer {0,1}
# (a car's drivetrain class) through an x87 float load/store and reads it back
# as an array index: that only works because the bit pattern 0x00000001 is a
# denormal that round-trips exactly.  Under flush-to-zero every four-wheel-drive
# car would silently read as class 0 in the damage model.  Several other
# bit-level float idioms in the simulation break the same way, so any flag
# that relaxes the float model is refused.

foreach(_v CMAKE_C_FLAGS CMAKE_C_FLAGS_DEBUG CMAKE_C_FLAGS_RELEASE
           CMAKE_C_FLAGS_RELWITHDEBINFO CMAKE_C_FLAGS_MINSIZEREL
           CMAKE_EXE_LINKER_FLAGS)
  if("${${_v}}" MATCHES "-ffast-math|-Ofast|-funsafe-math-optimizations|/fp:fast")
    message(FATAL_ERROR
      "${_v} relaxes the float model (${${_v}}).\n"
      "Refused: the simulation depends on exact IEEE behaviour, including "
      "denormals.  See the comment above this check.")
  endif()
endforeach()

# The repository root: the format parsers are compiled from tools/formats.
get_filename_component(RALLY_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# A Windows build puts every executable in one directory, next to the copy of
# SDL2.dll that port/plat places there: Windows finds a DLL beside the .exe.
if(WIN32)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

add_subdirectory(plat)
add_subdirectory(formats)
add_subdirectory(stage)
add_subdirectory(obj)
add_subdirectory(gfx2d)
add_subdirectory(sim)
add_subdirectory(game)

# ---- configure-time summary ----------------------------------------------

message(STATUS "")
message(STATUS "rally-port ${PROJECT_VERSION}")
message(STATUS "  system      : ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "  compiler    : ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
message(STATUS "  sdl2        : ${RALLY_SDL2_SUMMARY}")
message(STATUS "  backend     : ${RALLY_GAME_BACKEND}")
message(STATUS "")
