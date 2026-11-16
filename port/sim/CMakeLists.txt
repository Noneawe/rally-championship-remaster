# ---------------------------------------------------------------------------
# port/sim - the simulation: physics, damage, the race, the AI.
#
#   rally_sim         the simulation itself.  Buffer- and struct-taking: no
#                     GL, no SDL, no file system.
#   rally_sim_layout  compiles car.h on its own: car.h carries compile-time
#                     offset assertions against the original's 1,668-byte car
#                     record, so a wrong offset fails the build.
#   rally_simload     the loader: the level's object tables, the image's
#                     tables, the level and the car's collision points.  It
#                     opens files, which is why it is not part of rally_sim.
# ---------------------------------------------------------------------------

add_library(rally_sim STATIC
    arcade.c
    camera.c
    car_params.c
    collision.c
    damage.c
    damage_acc.c
    damage_warn.c
    drivetrain.c
    gearbox.c
    ground.c
    image.c
    input.c
    objects.c
    overlap.c
    pacenote.c
    parts_left.c
    physics.c
    place.c
    race.c
    soundfeed.c
    weather.c
    surface.c
    tick.c
    tyre.c)
target_include_directories(rally_sim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rally_sim PRIVATE rally_c_flags)

add_library(rally_sim_layout STATIC car_layout.c)
target_include_directories(rally_sim_layout PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rally_sim_layout PRIVATE rally_c_flags)

add_library(rally_simload STATIC objload.c objload.h simlevel.c simlevel.h)
target_include_directories(rally_simload PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rally_simload
  PUBLIC  rally_sim rally_formats rally_stage
  PRIVATE rally_c_flags)
