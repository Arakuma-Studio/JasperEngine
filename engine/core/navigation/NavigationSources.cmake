# Navigation (docs/specs/S19-navigation.md, docs/navigation.md): the core library
# (engine/core/navigation, no ECS: bake input, tile builder, navmesh, crowds, grids), the
# scene's NavigationSystem, the Lua-facing objects and their bindings. Recast/Detour
# (engine/libs/recastnavigation) is linked when JASPERENGINE_NAVIGATION is ON; OFF compiles
# the navmesh API as stubs that fail and log once (grids need no library and keep working).

set(JASPERENGINE_NAVIGATION_SRCS
    core/navigation/NavTypes.cpp
    core/navigation/NavBakeInput.cpp
    core/navigation/NavBakeCollector.cpp
    core/navigation/NavBakeScheduler.cpp
    core/navigation/NavMeshBuilder.cpp
    core/navigation/NavMesh.cpp
    core/navigation/NavCrowd.cpp
    core/navigation/NavGridMap.cpp
    core/navigation/GridPathfinder.cpp
    core/navigation/NavTerrainWorld.cpp
    core/util/MeshTriangles.cpp
    core/subsystem/NavigationSystem.cpp
    core/object/navigation/NavMeshAgent.cpp
    core/object/navigation/NavMeshSurface.cpp
    core/object/navigation/NavGrid.cpp
    core/script/binding/NavigationClassesLua.cpp
)
if(JASPERENGINE_NAVIGATION)
    list(APPEND JASPERENGINE_NAVIGATION_SRCS core/navigation/NavDetourSupport.cpp)
endif()
