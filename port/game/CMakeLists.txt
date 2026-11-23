# ---------------------------------------------------------------------------
# port/game - the game executable, `rally`.
#
# It links the simulation (through rally_simload), the stage, the objects,
# the 2D layer, the format parsers and ONE platform backend:
#
#   desktop GL (SDL2)   Windows and Linux x86-64
#   GLES2 (SDL2)        aarch64 handhelds
#   plat_null           no SDL2: the whole loop runs, with no picture
#
# The menus run the original game's own front-end logic through a small data
# store (vmstore.c).  -DRALLY_INTERP=ON builds a debugging rally that keeps the
# x86 subset interpreter (x86vm.c) instead; it is never shipped.
# ---------------------------------------------------------------------------

option(RALLY_INTERP "debugging: rally keeps the original-code interpreter (x86vm.c)" OFF)
set(RALLY_VM_SRC vmstore.c)
if(RALLY_INTERP)
  set(RALLY_VM_SRC x86vm.c)
endif()

set(RALLY_SRC rally.c vfs.c vfs.h camview.c camview.h
               viewer_keys.h car_input.c car_input.h car_keys.h
               car_load.c car_load.h wear.c wear.h
               hud.c hud.h frontend.c frontend.h lexicon.c lexicon.h
               ${RALLY_VM_SRC} x86vm.h setup.c setup.h menu3d.c menu3d.h
               fe_logic.c fe_logic.h fe_champ.c fe_champ.h
               fd_service.c fd_service.h fd_service_rects.h fd_service_kinds.h
               fd_stage.c fd_stage.h fd_career.c fd_career.h fd_engine.c fd_engine.h
               fd_standings.c fd_standings.h fd_svcdraw.c fd_svcdraw.h
               fd_cspecs.c fd_cspecs.h
               wav.c wav.h sound.c sound.h sound_plat.c
               carsound.c carsound.h worldsound.c worldsound.h
               weatherfx.c weatherfx.h wipers.c wipers.h driver.c driver.h wheels.c wheels.h
               spray.c spray.h spray_draw.c spray_draw.h
               fireworks.c fireworks.h fireworks_draw.c fireworks_draw.h
               sun.c sun.h carshadow.c carshadow.h rplrec.c rplrec.h
               portcfg.c portcfg.h)
if(ANDROID)
  # SDLActivity loads libmain.so and calls its SDL_main (android_main.c, which
  # calls rally's main renamed).
  add_library(rally SHARED ${RALLY_SRC} android_main.c)
  set_target_properties(rally PROPERTIES OUTPUT_NAME main)
  set_source_files_properties(rally.c PROPERTIES COMPILE_DEFINITIONS main=rally_main)
  target_link_libraries(rally PRIVATE log android)
else()
  add_executable(rally ${RALLY_SRC})
endif()
target_include_directories(rally PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(RALLY_INTERP)
  target_compile_definitions(rally PRIVATE RALLY_INTERP=1)
endif()

# aarch64 builds link the GLES2 backend; everything else desktop GL.
# Override with -DRALLY_GAME_GLES2=ON/OFF.
if((CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$" AND NOT APPLE) OR ANDROID)
  set(_rally_gles2_default ON)
else()
  set(_rally_gles2_default OFF)
endif()
option(RALLY_GAME_GLES2 "rally links plat_sdl2_gles2 instead of plat_sdl2_gl"
       ${_rally_gles2_default})

# RALLY_DESKTOP picks which menu ROWS appear: a handheld has one built-in pad
# and no window to size, so it hides the resolution and controller-device
# rows.  ON for Windows and Linux x86-64, OFF for aarch64.  It never changes
# what the game logic does.  Override with -DRALLY_DESKTOP=ON/OFF.
if((CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$" AND NOT APPLE) OR ANDROID)
  set(_rally_desktop_default OFF)
else()
  set(_rally_desktop_default ON)
endif()
option(RALLY_DESKTOP "rally shows the desktop menu rows (not the handheld's)"
       ${_rally_desktop_default})
if(RALLY_DESKTOP)
  target_compile_definitions(rally PRIVATE RALLY_DESKTOP=1)
endif()

if(RALLY_HAVE_SDL2 AND RALLY_GAME_GLES2)
  set(RALLY_GAME_BACKEND plat_sdl2_gles2)
elseif(RALLY_HAVE_SDL2)
  set(RALLY_GAME_BACKEND plat_sdl2_gl)
else()
  set(RALLY_GAME_BACKEND plat_null)
endif()
set(RALLY_GAME_BACKEND ${RALLY_GAME_BACKEND} PARENT_SCOPE)

target_link_libraries(rally PRIVATE rally_simload rally_objscene rally_stage
                      rally_gfx2d rally_formats rally_sim
                      ${RALLY_GAME_BACKEND} rally_c_flags m)

# wipers.c sets the x87's rounding mode (fesetround) around the original's
# truncating sections: without -frounding-math the compiler may move the
# arithmetic across those calls, or fold it under the default mode.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  set_source_files_properties(wipers.c PROPERTIES COMPILE_OPTIONS -frounding-math)
endif()
