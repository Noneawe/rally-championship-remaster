# ---------------------------------------------------------------------------
# port/gfx2d - the 2D layer: the menus, fonts, buttons and HUD sprites.
#
# gfx2d.c blits sprites from the game's .bun bundles; sheet.c cuts the
# frontend's buttons and fonts out of their PCX sheets.  Like port/stage it
# calls plat.h and links no backend.
# ---------------------------------------------------------------------------

add_library(rally_gfx2d STATIC gfx2d.c gfx2d.h sheet.c sheet.h)
target_include_directories(rally_gfx2d PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(rally_gfx2d PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../plat)
target_link_libraries(rally_gfx2d PRIVATE rally_c_flags)
