# Clipper2 2.0.1 - https://github.com/AngusJohnson/Clipper2 (tag Clipper2_2.0.1), Boost Software
# License 1.0. The polygon union and clipping of the tilemap collider builder (S50 D8), and the
# polygon offsetting and boolean operations of Title Art's outlines (face weight, plate closing,
# stroke outlines: engine/core/titleart/geometry/Offset.cpp). Only the C++ library
# (CPP/Clipper2Lib) is vendored, unchanged; upstream's CMakeLists.txt, which also builds its
# utilities, examples and tests, is replaced by this file. Built with JASPERENGINE_PHYSICS_2D or
# JASPERENGINE_TITLE_ART (one target, added by whichever comes first).

add_library(

    clipper2

    STATIC

    src/clipper.engine.cpp
    src/clipper.offset.cpp
    src/clipper.rectclip.cpp
    src/clipper.triangulation.cpp

)

target_include_directories(clipper2 SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

# upstream's default (PathsD scale to Paths64 with at most 8 decimals); the engine uses 3
target_compile_definitions(clipper2 PUBLIC CLIPPER2_MAX_DECIMAL_PRECISION=8)

# hidden, as HarfBuzz: nothing of Clipper2 belongs in libjasperengine's exported symbols
if(NOT MSVC)
    target_compile_options(clipper2 PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
endif()

if(EMSCRIPTEN AND EMSCRIPTEN_THREAD_SUPPORT)
    target_compile_options(clipper2 PRIVATE -pthread)
endif()
