# Editor only: exported games load the .glb files it produces
add_library(ufbx STATIC ufbx.c)

target_include_directories(ufbx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

set_target_properties(ufbx PROPERTIES POSITION_INDEPENDENT_CODE ON)
