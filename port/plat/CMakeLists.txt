# ---------------------------------------------------------------------------
# port/plat - the platform layer: window, input, graphics, audio, timing.
#
#   plat_null         no window, no sound: runs the whole loop with no picture
#   plat_sdl2_gl      SDL2 + desktop OpenGL        (Windows, Linux x86-64)
#   plat_sdl2_gles2   SDL2 + OpenGL ES 2           (aarch64 handhelds)
#
# The two SDL2 backends are ONE source compiled twice (plat_sdl2.c with or
# without PLAT_SDL2_GLES2), so neither can drift from the other.  plat_gl.c
# resolves every GL entry point through SDL_GL_GetProcAddress, so nothing
# links -lGL or -lGLESv2.  Audio is miniaudio (port/third_party), mixed in
# plat_audio_ma.c and fed to SDL2's audio device.
#
# Without SDL2 the SDL2 backends are skipped and rally links plat_null.
# ---------------------------------------------------------------------------

set(PLAT_HEADERS plat.h)

# ---- SDL2 discovery: pkg-config first, then SDL2's CMake CONFIG package ----

set(RALLY_HAVE_SDL2 OFF)
set(RALLY_SDL2_SUMMARY "not found - SDL2 backends skipped")

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(SDL2PC QUIET sdl2)
endif()

if(TARGET SDL2::SDL2)
  # SDL2 built in the same tree (the Android project's add_subdirectory)
  add_library(rally_sdl2 INTERFACE)
  target_link_libraries(rally_sdl2 INTERFACE SDL2::SDL2)
  set(RALLY_HAVE_SDL2 ON)
  set(RALLY_SDL2_SUMMARY "in-tree")
elseif(SDL2PC_FOUND)
  add_library(rally_sdl2 INTERFACE)
  target_include_directories(rally_sdl2 SYSTEM INTERFACE ${SDL2PC_INCLUDE_DIRS})
  target_link_libraries(rally_sdl2 INTERFACE ${SDL2PC_LINK_LIBRARIES})
  target_compile_options(rally_sdl2 INTERFACE ${SDL2PC_CFLAGS_OTHER})
  set(RALLY_HAVE_SDL2 ON)
  set(RALLY_SDL2_SUMMARY "${SDL2PC_VERSION} (pkg-config)")
else()
  find_package(SDL2 CONFIG QUIET)
  if(SDL2_FOUND)
    add_library(rally_sdl2 INTERFACE)
    if(TARGET SDL2::SDL2)
      target_link_libraries(rally_sdl2 INTERFACE SDL2::SDL2)
    else()
      target_include_directories(rally_sdl2 SYSTEM INTERFACE ${SDL2_INCLUDE_DIRS})
      target_link_libraries(rally_sdl2 INTERFACE ${SDL2_LIBRARIES})
    endif()
    set(RALLY_HAVE_SDL2 ON)
    set(RALLY_SDL2_SUMMARY "${SDL2_VERSION} (CONFIG package)")
  endif()
endif()

# Windows: copy SDL2.dll next to the executables.
if(RALLY_HAVE_SDL2 AND WIN32 AND TARGET SDL2::SDL2)
  get_target_property(_sdl2_dll SDL2::SDL2 IMPORTED_LOCATION)
  if(_sdl2_dll AND EXISTS "${_sdl2_dll}")
    file(COPY "${_sdl2_dll}" DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
    set(RALLY_SDL2_SUMMARY "${RALLY_SDL2_SUMMARY}, SDL2.dll copied to ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
  else()
    message(WARNING "SDL2 found, but no SDL2.dll beside it: the .exe will not start")
  endif()
endif()

set(RALLY_HAVE_SDL2    ${RALLY_HAVE_SDL2}    PARENT_SCOPE)
set(RALLY_SDL2_SUMMARY ${RALLY_SDL2_SUMMARY} PARENT_SCOPE)

# ---- the null backend -----------------------------------------------------

add_library(plat_null STATIC plat_null.c ${PLAT_HEADERS})
target_include_directories(plat_null PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(plat_null PRIVATE rally_c_flags)

# ---- the mixer: miniaudio -------------------------------------------------
# Third-party code, in its own target with its own flags (-w: it is not held
# to this tree's warning level).  Its configuration is PUBLIC because
# ma_engine's layout depends on it and every includer must see the same one:
#   MA_NO_DEVICE_IO   SDL2 is the device layer; no second audio stack
#   MA_NO_FLAC/MP3    the game's sounds are RIFF PCM only
#   MA_NO_ENCODING    nothing is written
find_package(Threads REQUIRED)
set(RALLY_MINIAUDIO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/miniaudio)
add_library(rally_miniaudio STATIC ${RALLY_MINIAUDIO_DIR}/miniaudio.c)
target_include_directories(rally_miniaudio SYSTEM PUBLIC ${RALLY_MINIAUDIO_DIR})
target_compile_definitions(rally_miniaudio PUBLIC
  MA_NO_DEVICE_IO MA_NO_FLAC MA_NO_MP3 MA_NO_ENCODING)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(rally_miniaudio PRIVATE -w)
endif()
target_link_libraries(rally_miniaudio PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
if(NOT WIN32)
  target_link_libraries(rally_miniaudio PUBLIC m)
endif()

add_library(plat_audio_ma STATIC plat_audio_ma.c plat_audio_ma.h ${PLAT_HEADERS})
target_include_directories(plat_audio_ma PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(plat_audio_ma PRIVATE rally_c_flags PUBLIC rally_miniaudio)

# ---- the SDL2 backends ----------------------------------------------------

if(RALLY_HAVE_SDL2)
  add_library(plat_sdl2_gl STATIC plat_sdl2.c plat_gl.c plat_glthread.c ${PLAT_HEADERS})
  target_include_directories(plat_sdl2_gl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(plat_sdl2_gl PRIVATE rally_c_flags
                        PUBLIC rally_sdl2 plat_audio_ma)

  add_library(plat_sdl2_gles2 STATIC plat_sdl2.c plat_gl.c plat_glthread.c ${PLAT_HEADERS})
  target_include_directories(plat_sdl2_gles2 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(plat_sdl2_gles2 PRIVATE PLAT_SDL2_GLES2)
  target_link_libraries(plat_sdl2_gles2 PRIVATE rally_c_flags
                        PUBLIC rally_sdl2 plat_audio_ma)
endif()
