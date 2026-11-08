# recastnavigation 1.6.0 - https://github.com/recastnavigation/recastnavigation (tag v1.6.0,
# commit 6dc1667f580357e8a2154c28b7867bea7e8ad3a7), zlib (License.txt). The navigation meshes,
# queries, crowds and tile caches of JasperEngine's navigation (docs/specs/S19-navigation.md).
# Only the five libraries are vendored (README.jasper.md); this file replaces the upstream build.
# Five local patches, marked "JasperEngine patch" in the sources and listed in PATCHES.md.
#
# recastnavigation: Recast, Detour, DetourCrowd and DetourTileCache in one static library, what
# every game with JASPERENGINE_NAVIGATION links. 32-bit polygon references (no DT_POLYREF64) and
# the native dtQueryFilter (no DT_VIRTUAL_QUERYFILTER): areas are poly flags (spec D5).
# Hidden visibility, so the shared editor engine never exports dt*/rc* symbols: only
# libjasperengine calls it.
#
# recastnavigation_debugdraw: DebugUtils' DebugDraw.cpp (the duDebugDraw primitives, arcs,
# cylinders, boxes; no Detour or Recast symbol), which the editor's Scene view gizmos draw with.
# Exported games never link it (spec D8).

set(RECAST_DIR ${CMAKE_CURRENT_SOURCE_DIR})

set(RECASTNAVIGATION_SOURCES
    ${RECAST_DIR}/Recast/Source/Recast.cpp
    ${RECAST_DIR}/Recast/Source/RecastAlloc.cpp
    ${RECAST_DIR}/Recast/Source/RecastArea.cpp
    ${RECAST_DIR}/Recast/Source/RecastAssert.cpp
    ${RECAST_DIR}/Recast/Source/RecastContour.cpp
    ${RECAST_DIR}/Recast/Source/RecastFilter.cpp
    ${RECAST_DIR}/Recast/Source/RecastLayers.cpp
    ${RECAST_DIR}/Recast/Source/RecastMesh.cpp
    ${RECAST_DIR}/Recast/Source/RecastMeshDetail.cpp
    ${RECAST_DIR}/Recast/Source/RecastRasterization.cpp
    ${RECAST_DIR}/Recast/Source/RecastRegion.cpp
    ${RECAST_DIR}/Detour/Source/DetourAlloc.cpp
    ${RECAST_DIR}/Detour/Source/DetourAssert.cpp
    ${RECAST_DIR}/Detour/Source/DetourCommon.cpp
    ${RECAST_DIR}/Detour/Source/DetourNavMesh.cpp
    ${RECAST_DIR}/Detour/Source/DetourNavMeshBuilder.cpp
    ${RECAST_DIR}/Detour/Source/DetourNavMeshQuery.cpp
    ${RECAST_DIR}/Detour/Source/DetourNode.cpp
    ${RECAST_DIR}/DetourCrowd/Source/DetourCrowd.cpp
    ${RECAST_DIR}/DetourCrowd/Source/DetourLocalBoundary.cpp
    ${RECAST_DIR}/DetourCrowd/Source/DetourObstacleAvoidance.cpp
    ${RECAST_DIR}/DetourCrowd/Source/DetourPathCorridor.cpp
    ${RECAST_DIR}/DetourCrowd/Source/DetourPathQueue.cpp
    ${RECAST_DIR}/DetourCrowd/Source/DetourProximityGrid.cpp
    ${RECAST_DIR}/DetourTileCache/Source/DetourTileCache.cpp
    ${RECAST_DIR}/DetourTileCache/Source/DetourTileCacheBuilder.cpp
)

add_library(recastnavigation STATIC ${RECASTNAVIGATION_SOURCES})
target_include_directories(recastnavigation SYSTEM PUBLIC
    ${RECAST_DIR}/Recast/Include
    ${RECAST_DIR}/Detour/Include
    ${RECAST_DIR}/DetourCrowd/Include
    ${RECAST_DIR}/DetourTileCache/Include)
set_target_properties(recastnavigation PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    POSITION_INDEPENDENT_CODE ON
    FOLDER "libs")

add_library(recastnavigation_debugdraw STATIC ${RECAST_DIR}/DebugUtils/Source/DebugDraw.cpp)
target_include_directories(recastnavigation_debugdraw SYSTEM PUBLIC
    ${RECAST_DIR}/DebugUtils/Include
    ${RECAST_DIR}/Detour/Include)
set_target_properties(recastnavigation_debugdraw PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    FOLDER "libs")
