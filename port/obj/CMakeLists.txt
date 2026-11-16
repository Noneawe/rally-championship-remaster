# ---------------------------------------------------------------------------
# port/obj - objects standing on the stage.
#
#   rally_obj        the object transform: an instance record's angles, scale
#                    and anchor turned into the matrix its model is drawn
#                    through.  Pure arithmetic; links nothing.
#   rally_objscene   loads the level's objects, parses their .ro3 models and
#                    submits them (objlit.c: lighting and the reflection pass,
#                    also used by the menus' 3D models).  Links no backend.
# ---------------------------------------------------------------------------

add_library(rally_obj STATIC objxform.c objxform.h)
target_include_directories(rally_obj PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rally_obj PRIVATE rally_c_flags)
if(UNIX)
  target_link_libraries(rally_obj PUBLIC m)
endif()

add_library(rally_objscene STATIC objscene.c objscene.h objlit.c objlit.h)
target_include_directories(rally_objscene PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(rally_objscene PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../plat)
target_link_libraries(rally_objscene
  PUBLIC  rally_obj rally_formats rally_stage
  PRIVATE rally_c_flags)
