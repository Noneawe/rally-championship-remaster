# ---------------------------------------------------------------------------
# port/formats - the parsers for the original game's file formats.
#
# They live in tools/formats/ and are compiled from there.  rally_formats is
# the library the game links: it takes buffers, not paths.  The six
# command-line tools (ro3, bun, pcx, sq, cdat, localdat) inspect and export
# the game's files; corpus.c, which walks directories, is compiled into the
# tools only.
#
#   ro3        .ro3 geometry               sq         .sq containers
#   bun        .bun bundles                cdat       level geometry
#   pcx        .pcx images                 localdat   level texture sets
# ---------------------------------------------------------------------------

set(FMT ${RALLY_REPO_ROOT}/tools/formats)

add_library(rally_formats STATIC
  ${FMT}/ro3.c
  ${FMT}/bun.c
  ${FMT}/lwo.c
  ${FMT}/pyrandom.c
  ${FMT}/sq.c
  ${FMT}/cdat.c
  ${FMT}/localdat.c
  ${FMT}/pcx.c
  ${FMT}/png.c
)
target_include_directories(rally_formats PUBLIC ${FMT})
target_link_libraries(rally_formats PRIVATE rally_c_flags)
target_compile_definitions(rally_formats PUBLIC _POSIX_C_SOURCE=200809L)
if(UNIX)
  target_link_libraries(rally_formats PUBLIC m)
endif()

foreach(_tool ro3 bun pcx sq cdat localdat)
  add_executable(${_tool}_tool ${FMT}/${_tool}_tool.c ${FMT}/corpus.c)
  target_link_libraries(${_tool}_tool PRIVATE rally_formats rally_c_flags)
  set_target_properties(${_tool}_tool PROPERTIES OUTPUT_NAME ${_tool})
endforeach()
