# ---------------------------------------------------------------------------
# port/stage - the stage renderer.
#
# stage.c decides which texture plate is drawn and where; plate.c turns a
# plate id into a texture (reading the game's .pcx sheets, and any override
# PNGs from overrides/).  rally_stage calls plat.h and links no backend, so
# the same object file draws through plat_null, plat_sdl2_gl and
# plat_sdl2_gles2: plat_* resolves at the executable's final link.
# ---------------------------------------------------------------------------

add_library(rally_stage STATIC stage.c stage.h fog_noise.c plate.c plate.h
            platecache.c platecache.h)

target_include_directories(rally_stage PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(rally_stage PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../plat)
# stb_image (public domain / MIT, v2.30) decodes the override PNGs.
target_include_directories(rally_stage PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/stb)

# At -O3 gcc reports stb_image.h's `tc16[0]` as maybe-uninitialized (the PNG
# tRNS path).  Only that warning class is silenced, in plate.c, for gcc.
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  set_source_files_properties(plate.c PROPERTIES
                              COMPILE_OPTIONS "-Wno-maybe-uninitialized")
endif()

target_link_libraries(rally_stage PUBLIC rally_formats PRIVATE rally_c_flags)
