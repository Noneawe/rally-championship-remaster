# ---------------------------------------------------------------------------
# Cross-compiling toolchain: aarch64-linux-gnu.
#
#   cmake -S port -B port/build-aarch64 \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux-gnu.cmake
#   cmake --build port/build-aarch64
#
# WHY THIS TRIPLE. states it as a REQUIREMENT rather than a
# discovery, so that the port tickets do not wait on a purchase: the Anbernic
# class is aarch64 Linux throughout, Cortex-A53, GLES2, and 16bpp is the
# budget decision.  Whoever closes confirms or corrects it; this file is
# the only place the triple is written down in the build, so correcting it is
# a one-file edit.
#
# -mcpu IS DELIBERATELY NOT SET HERE. says "Cortex-A53, no NEON
# assumed", and owns optimisation, which cannot be measured without a
# device.  A generic aarch64 build is the correct thing to produce until then;
# pass -DRALLY_TUNE=cortex-a53 when there is a device to measure on.
#
# WHAT YOU NEED INSTALLED
#   Debian/Ubuntu:  sudo apt install gcc-aarch64-linux-gnu
#   For the SDL2 backends you additionally need an aarch64 SDL2, which on a
#   Debian host means enabling the arm64 architecture and the ports mirror:
#     sudo dpkg --add-architecture arm64 && sudo apt install libsdl2-dev:arm64
#   Without it, port/plat/CMakeLists.txt SKIPS the two SDL2 selftests exactly
#   as it does on a host with no SDL2 - the null backend is the
#   symbol-completeness gate and is the one that must always build.
# ---------------------------------------------------------------------------

set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(RALLY_TARGET_TRIPLE aarch64-linux-gnu CACHE STRING "Cross target triple")
set(RALLY_TUNE "" CACHE STRING "Value for -mcpu, e.g. cortex-a53.  Empty = generic")

find_program(RALLY_CROSS_CC  ${RALLY_TARGET_TRIPLE}-gcc)
find_program(RALLY_CROSS_CXX ${RALLY_TARGET_TRIPLE}-g++)

if(NOT RALLY_CROSS_CC)
  message(FATAL_ERROR
    "No ${RALLY_TARGET_TRIPLE}-gcc on PATH.\n"
    "Debian/Ubuntu:  sudo apt install gcc-aarch64-linux-gnu\n"
    "This file names the toolchain; it cannot supply it.")
endif()

set(CMAKE_C_COMPILER   ${RALLY_CROSS_CC})
if(RALLY_CROSS_CXX)
  set(CMAKE_CXX_COMPILER ${RALLY_CROSS_CXX})
endif()

if(RALLY_TUNE)
  set(CMAKE_C_FLAGS_INIT   "-mcpu=${RALLY_TUNE}")
  set(CMAKE_CXX_FLAGS_INIT "-mcpu=${RALLY_TUNE}")
endif()

# Look for libraries and headers in the target sysroot, never on the host.
# Programs are the exception: cmake must still be able to run host tools.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

if(RALLY_SYSROOT)
  set(CMAKE_SYSROOT      ${RALLY_SYSROOT})
  set(CMAKE_FIND_ROOT_PATH ${RALLY_SYSROOT})

  # --sysroot alone is NOT enough with Ubuntu's gcc-aarch64-linux-gnu (measured
  # 2026-10-14, step 2): that compiler searches its OWN libc
  # (/usr/aarch64-linux-gnu, glibc 2.39) for headers, crt files and libraries
  # BEFORE the sysroot, so a "sysroot" build still needed GLIBC_2.38 (fmod) and
  # would not start on an older device libc.  Put the sysroot first explicitly.
  set(_rs ${RALLY_SYSROOT})
  set(_rt ${RALLY_TARGET_TRIPLE})
  set(_rally_sysroot_cflags
      "-isystem ${_rs}/usr/include/${_rt} -isystem ${_rs}/usr/include")
  set(_rally_sysroot_ldflags
      "-B${_rs}/usr/lib/${_rt} -L${_rs}/usr/lib/${_rt} -L${_rs}/lib/${_rt} -Wl,-rpath-link,${_rs}/usr/lib/${_rt}:${_rs}/lib/${_rt}")
  string(APPEND CMAKE_C_FLAGS_INIT   " ${_rally_sysroot_cflags}")
  string(APPEND CMAKE_CXX_FLAGS_INIT " ${_rally_sysroot_cflags}")
  set(CMAKE_EXE_LINKER_FLAGS_INIT    "${_rally_sysroot_ldflags}")
  set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_rally_sysroot_ldflags}")

  # with qemu-user-static on the host, ctest runs this tree's gates as
  # ARM code (add_test COMMANDs naming a target get the emulator prefixed).
  # Tests that run a script (bash, python) which in turn runs a target do NOT
  # get it - those are listed in.  No qemu, no emulator: the
  # tree builds as before and its tests fail to exec, as they always did.
  find_program(RALLY_QEMU qemu-aarch64-static)
  if(RALLY_QEMU)
    set(CMAKE_CROSSCOMPILING_EMULATOR "${RALLY_QEMU};-L;${_rs}")
  endif()
endif()

# pkg-config must answer for the TARGET, not the host, or the SDL2 probe in
# port/plat will happily hand the cross compiler the host's -I and -l flags.
# This is the multiarch layout Debian and Ubuntu use.
if(NOT DEFINED ENV{PKG_CONFIG_LIBDIR})
  set(ENV{PKG_CONFIG_LIBDIR}
      "${CMAKE_SYSROOT}/usr/lib/${RALLY_TARGET_TRIPLE}/pkgconfig:${CMAKE_SYSROOT}/usr/share/pkgconfig")
endif()
if(CMAKE_SYSROOT AND NOT DEFINED ENV{PKG_CONFIG_SYSROOT_DIR})
  set(ENV{PKG_CONFIG_SYSROOT_DIR} "${CMAKE_SYSROOT}")
endif()
