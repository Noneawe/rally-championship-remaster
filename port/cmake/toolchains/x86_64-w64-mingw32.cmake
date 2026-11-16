# ---------------------------------------------------------------------------
# Cross-compiling toolchain: x86_64-w64-mingw32 (Windows .exe, built in WSL).
#
#   cmake -S port -B port/build-win64 \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/x86_64-w64-mingw32.cmake \
#         -DRALLY_SDL2_MINGW=/path/to/SDL2-2.30.0
#   cmake --build port/build-win64
#
# Every executable lands in port/build-win64/bin/, next to a copy of SDL2.dll.
# Run them FROM WINDOWS (PowerShell), with Windows paths -.
#
# WHY THIS TRIPLE.: the first playable build is for the Windows dev
# machine, and the user's call (2026-10-05) is to build it under WSL, where the
# gates already run, rather than install a Windows toolchain.  It is the same
# compiler family as the gate build (gcc 13, SSE2 on x86-64), which is what
# makes "trace_check.exe matches the Linux build bit for bit" a fair test.
#
# WHAT YOU NEED INSTALLED
#   sudo apt install mingw-w64
#   sudo update-alternatives --config x86_64-w64-mingw32-gcc   # posix
#   sudo update-alternatives --config x86_64-w64-mingw32-g++   # posix
#   The POSIX thread model, not win32: the win32 model has no C11/C++ threads
#   and plat uses threads.  `x86_64-w64-mingw32-gcc -v` prints the model.
#
#   SDL2: Ubuntu ships no MinGW SDL2.  Unpack the official
#   SDL2-devel-<ver>-mingw.tar.gz OUTSIDE the repo and pass its top directory
#   as RALLY_SDL2_MINGW.  Without it, port/plat/CMakeLists.txt SKIPS the SDL2
#   backends exactly as on a host with no SDL2, and only the null backend and
#   the non-graphical tools build.
# ---------------------------------------------------------------------------

set(CMAKE_SYSTEM_NAME      Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(RALLY_TARGET_TRIPLE x86_64-w64-mingw32 CACHE STRING "Cross target triple")
set(RALLY_SDL2_MINGW "" CACHE PATH
    "Top directory of the unpacked SDL2-devel-<ver>-mingw.tar.gz (the one holding x86_64-w64-mingw32/)")

find_program(RALLY_CROSS_CC  ${RALLY_TARGET_TRIPLE}-gcc)
find_program(RALLY_CROSS_CXX ${RALLY_TARGET_TRIPLE}-g++)
find_program(RALLY_CROSS_RC  ${RALLY_TARGET_TRIPLE}-windres)

if(NOT RALLY_CROSS_CC)
  message(FATAL_ERROR
    "No ${RALLY_TARGET_TRIPLE}-gcc on PATH.\n"
    "Debian/Ubuntu:  sudo apt install mingw-w64\n"
    "This file names the toolchain; it cannot supply it.")
endif()

set(CMAKE_C_COMPILER   ${RALLY_CROSS_CC})
if(RALLY_CROSS_CXX)
  set(CMAKE_CXX_COMPILER ${RALLY_CROSS_CXX})
endif()
if(RALLY_CROSS_RC)
  set(CMAKE_RC_COMPILER ${RALLY_CROSS_RC})
endif()

# Look for libraries and headers in the MinGW sysroot (and SDL2's tree), never
# on the host.  Programs are the exception: cmake must still run host tools.
set(CMAKE_FIND_ROOT_PATH /usr/${RALLY_TARGET_TRIPLE})
if(RALLY_SDL2_MINGW)
  list(APPEND CMAKE_FIND_ROOT_PATH "${RALLY_SDL2_MINGW}/${RALLY_TARGET_TRIPLE}")
  # The SDL2 CONFIG package locates itself relative to its own file, so it
  # works from wherever the tarball was unpacked.  Its pkg-config file does
  # not (prefix=/usr/local/...), which is why pkg-config is switched off below.
  set(SDL2_DIR "${RALLY_SDL2_MINGW}/${RALLY_TARGET_TRIPLE}/lib/cmake/SDL2"
      CACHE PATH "SDL2 CONFIG package for the MinGW target")
  # The package adds -mwindows to every link, which makes each .exe a GUI-
  # subsystem program: Windows gives it no console, so everything it prints
  # from PowerShell is lost - rally's per-second line, its exit summary, and
  # its missing-asset errors (found 2026-10-06: a run that "printed
  # nothing").  Every executable here is run from a terminal, so keep the
  # console subsystem.  sdl2-config.cmake honours this (its line 63).
  set(SDL2_NO_MWINDOWS ON)
endif()
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# pkg-config must not answer at all: the host's would hand the cross compiler
# the Linux SDL2's -I and -l flags, and the tarball's carries a hard-coded
# prefix.  An empty search path makes pkg_check_modules fail cleanly, and
# port/plat falls through to the CONFIG package.
set(ENV{PKG_CONFIG_LIBDIR} "/nonexistent-rally-mingw-pkgconfig")
set(ENV{PKG_CONFIG_PATH}   "")
