# earcut.hpp 2.2.4 - https://github.com/mapbox/earcut.hpp (tag v2.2.4), ISC. The polygon
# triangulator of Title Art's 3D mesher (engine/core/titleart/three_d, docs/specs/S86-title-3d.md
# §6.12). Header-only; vendored unchanged. Built only with JASPERENGINE_TITLE_ART_MESHER.

add_library(earcut INTERFACE)
target_include_directories(earcut SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
