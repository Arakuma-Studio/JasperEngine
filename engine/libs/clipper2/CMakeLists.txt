# Clipper2's C++ core, the polygon union and clipping of the tilemap collider builder (S50 D8).
add_library(
    clipper2

    STATIC

    src/clipper.engine.cpp
    src/clipper.offset.cpp
    src/clipper.rectclip.cpp
)

# SYSTEM: its headers build under the engine's warnings, quietly
target_include_directories(clipper2 SYSTEM PUBLIC include)
