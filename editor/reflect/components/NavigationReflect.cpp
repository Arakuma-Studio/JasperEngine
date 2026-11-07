// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S19's navigation components (docs/specs/S19-navigation.md, docs/specs/S08-reflection.md): the
// NavMesh Surface, Agent, Obstacle, Link and Modifier and the Nav Grid keep S19's codecs
// (editor/StreamNavigation.cpp, every key written) and exported code (editor/FactoryNavigation.cpp),
// as S22's gameplay components do; their tables give the properties, the schema, the Inspector
// metadata, the asset locators and the generic copy of a value, and their enums are the tables
// the scene codec, MCP, the Inspector and the exported code read. Runtime state (an agent's path
// state, a grid's loaded map, the events) is no field.

#include "reflect/Components.h"

#include "Factory.h"
#include "Stream.h"
#include "component/NavGridComponent.h"
#include "component/NavMeshAgentComponent.h"
#include "component/NavMeshLinkComponent.h"
#include "component/NavMeshModifierComponent.h"
#include "component/NavMeshObstacleComponent.h"
#include "component/NavMeshSurfaceComponent.h"

namespace jasperengine::editor::reflect {

namespace {

    template<typename E>
    constexpr int navValue(E value) { return static_cast<int>(value); }

    // ---------------------------------------------------------------- enums
    // YAML tokens as S19 wrote them; labels as its Inspector shows them

    constexpr EnumItem kNavCollectObjectsItems[] = {
        {navValue(NavCollectObjects::ALL), "all", "All Game Objects", "NavCollectObjects::ALL"},
        {navValue(NavCollectObjects::VOLUME), "volume", "Volume", "NavCollectObjects::VOLUME"},
        {navValue(NavCollectObjects::CHILDREN), "children", "Current Object Hierarchy", "NavCollectObjects::CHILDREN"},
    };

    constexpr EnumItem kNavUseGeometryItems[] = {
        {navValue(NavUseGeometry::RENDER_MESHES), "render_meshes", "Render Meshes", "NavUseGeometry::RENDER_MESHES"},
        {navValue(NavUseGeometry::PHYSICS_COLLIDERS), "physics_colliders", "Physics Colliders", "NavUseGeometry::PHYSICS_COLLIDERS"},
        {navValue(NavUseGeometry::BOTH), "both", "Both", "NavUseGeometry::BOTH"},
    };

    constexpr EnumItem kNavPlaneItems[] = {
        {navValue(nav::NavPlane::XZ), "xz", "XZ (3D)", "nav::NavPlane::XZ"},
        {navValue(nav::NavPlane::XY), "xy", "XY (2D)", "nav::NavPlane::XY"},
    };

    constexpr EnumItem kNavAvoidanceQualityItems[] = {
        {navValue(nav::AvoidanceQuality::None), "none", "None", "nav::AvoidanceQuality::None"},
        {navValue(nav::AvoidanceQuality::Low), "low", "Low Quality", "nav::AvoidanceQuality::Low"},
        {navValue(nav::AvoidanceQuality::Medium), "medium", "Medium Quality", "nav::AvoidanceQuality::Medium"},
        {navValue(nav::AvoidanceQuality::Good), "good", "Good Quality", "nav::AvoidanceQuality::Good"},
        {navValue(nav::AvoidanceQuality::High), "high", "High Quality", "nav::AvoidanceQuality::High"},
    };

    constexpr EnumItem kNavAgentDriverItems[] = {
        {navValue(NavAgentDriver::TRANSFORM), "transform", "Transform", "NavAgentDriver::TRANSFORM"},
        {navValue(NavAgentDriver::RIGIDBODY), "rigidbody", "Rigidbody", "NavAgentDriver::RIGIDBODY"},
        {navValue(NavAgentDriver::CHARACTER_CONTROLLER), "character_controller", "Character Controller", "NavAgentDriver::CHARACTER_CONTROLLER"},
        {navValue(NavAgentDriver::NONE), "none", "None", "NavAgentDriver::NONE"},
    };

    constexpr EnumItem kNavObstacleShapeItems[] = {
        {navValue(NavObstacleShape::CAPSULE), "capsule", "Capsule", "NavObstacleShape::CAPSULE"},
        {navValue(NavObstacleShape::BOX), "box", "Box", "NavObstacleShape::BOX"},
    };

    constexpr EnumItem kNavModifierInclusionItems[] = {
        {navValue(NavModifierInclusion::AUTO), "auto", "Surface Policy", "NavModifierInclusion::AUTO"},
        {navValue(NavModifierInclusion::INCLUDE), "include", "Add or Modify Object", "NavModifierInclusion::INCLUDE"},
        {navValue(NavModifierInclusion::EXCLUDE), "exclude", "Remove Object", "NavModifierInclusion::EXCLUDE"},
    };

    constexpr EnumItem kNavGridLayoutItems[] = {
        {navValue(nav::GridLayout::Square), "square", "Square", "nav::GridLayout::Square"},
        {navValue(nav::GridLayout::Hex), "hex", "Hex (pointy-top)", "nav::GridLayout::Hex"},
    };

    constexpr EnumItem kNavGridMovementItems[] = {
        {navValue(nav::GridMovement::Free), "free", "Free (smoothed)", "nav::GridMovement::Free"},
        {navValue(nav::GridMovement::CellCentres), "cell_centres", "Cell Centres", "nav::GridMovement::CellCentres"},
    };

    constexpr EnumItem kNavGridSourceItems[] = {
        {navValue(NavGridSource::NONE), "none", "None (painted)", "NavGridSource::NONE"},
        {navValue(NavGridSource::TILEMAP), "tilemap", "Tilemap", "NavGridSource::TILEMAP"},
        {navValue(NavGridSource::TERRAIN), "terrain", "Terrain", "NavGridSource::TERRAIN"},
    };

}

namespace enums {

    // the type names are the ones S19's scene codec reported unknown tokens under
    const EnumDesc kNavCollectObjects{"NavCollectObjects", kNavCollectObjectsItems, navValue(NavCollectObjects::ALL)};
    const EnumDesc kNavUseGeometry{"NavUseGeometry", kNavUseGeometryItems, navValue(NavUseGeometry::RENDER_MESHES)};
    const EnumDesc kNavPlane{"NavPlane", kNavPlaneItems, navValue(nav::NavPlane::XZ)};
    const EnumDesc kNavAvoidanceQuality{"ObstacleAvoidanceType", kNavAvoidanceQualityItems, navValue(nav::AvoidanceQuality::High)};
    const EnumDesc kNavAgentDriver{"NavAgentDriver", kNavAgentDriverItems, navValue(NavAgentDriver::TRANSFORM)};
    const EnumDesc kNavObstacleShape{"NavObstacleShape", kNavObstacleShapeItems, navValue(NavObstacleShape::BOX)};
    const EnumDesc kNavModifierInclusion{"NavModifierInclusion", kNavModifierInclusionItems, navValue(NavModifierInclusion::AUTO)};
    const EnumDesc kNavGridLayout{"GridLayout", kNavGridLayoutItems, navValue(nav::GridLayout::Square)};
    const EnumDesc kNavGridMovement{"GridMovement", kNavGridMovementItems, navValue(nav::GridMovement::Free)};
    const EnumDesc kNavGridSource{"NavGridSource", kNavGridSourceItems, navValue(NavGridSource::NONE)};

}

namespace {

    // agent types and areas are ids of the project's navigation settings (Inspector dropdowns of
    // them); area masks a bit per area

    // ---------------------------------------------------------------- NavMesh Surface

    constexpr FieldDesc kNavMeshSurfaceFields[] = {
        JE_FIELD(NavMeshSurfaceComponent, enabled),
        JE_FIELD(NavMeshSurfaceComponent, agentType, .tooltip = "The agent type it bakes for (Window > AI > Navigation > Agents): its radius, height, step and slope."),
        JE_FIELD(NavMeshSurfaceComponent, collectObjects, .enumDesc = &enums::kNavCollectObjects, .tooltip = "What is collected: every static GameObject, the ones in the volume box, or this hierarchy."),
        JE_FIELD(NavMeshSurfaceComponent, useGeometry, .enumDesc = &enums::kNavUseGeometry, .tooltip = "Collect render meshes, collider shapes (no physics world needed) or both."),
        JE_FIELD(NavMeshSurfaceComponent, includeLayers, .tooltip = "GameObject layers collected."),
        JE_FIELD(NavMeshSurfaceComponent, defaultArea, .tooltip = "The area of what is collected unless a NavMesh Modifier says otherwise."),
        JE_FIELD(NavMeshSurfaceComponent, volumeCenter, .unit = Unit::Meters, .label = "Center"),
        JE_FIELD(NavMeshSurfaceComponent, volumeSize, .unit = Unit::Meters, .label = "Size"),
        JE_FIELD(NavMeshSurfaceComponent, overrideCellSize, .label = "Override Voxel Size"),
        JE_FIELD(NavMeshSurfaceComponent, cellSize, .unit = Unit::Meters, .label = "Voxel Size", .tooltip = "Metres per voxel (default: the agent radius / 3)."),
        JE_FIELD(NavMeshSurfaceComponent, overrideTileSize),
        JE_FIELD(NavMeshSurfaceComponent, tileSize, .tooltip = "Voxels per tile side (carving: at most 255)."),
        JE_FIELD(NavMeshSurfaceComponent, minRegionArea, .label = "Minimum Region Area", .tooltip = "Islands smaller than this (square metres) are dropped."),
        JE_FIELD(NavMeshSurfaceComponent, plane, .enumDesc = &enums::kNavPlane, .tooltip = "XZ for 3D scenes (Y up); XY for 2D scenes: Collider2D footprints on the scene's plane."),
        JE_FIELD(NavMeshSurfaceComponent, runtimeCarving, .tooltip = "Keep tile-cache layers so carving NavMesh Obstacles cut it at runtime (about 30% more memory)."),
        JE_FIELD(NavMeshSurfaceComponent, maxCarvingObstacles, .min = 1.0f, .max = 4096.0f,
                 .tooltip = "Carving NavMesh Obstacles it cuts at once (Runtime Carving); more are avoided instead, with a warning. Read when the navmesh is created."),
        JE_FIELD(NavMeshSurfaceComponent, navMeshData, .label = "NavMesh Data", .tooltip = "The baked .navmesh asset."),
    };

    const ComponentCodec kNavMeshSurfaceCodec = JE_STREAM_CODEC(NavMeshSurfaceComponent, encodeNavMeshSurfaceComponent,
                                                                decodeNavMeshSurfaceComponent);

    constexpr const char* kNavMeshSurfaceKnownKeys[] = {"agentType", "cellSize", "collectObjects", "defaultArea", "enabled", "includeLayers",
        "maxCarvingObstacles", "minRegionArea", "navMeshData", "navMeshDataGuid", "overrideCellSize", "overrideTileSize", "plane", "runtimeCarving", "tileSize",
        "useGeometry", "volumeCenter", "volumeSize"};

    // ---------------------------------------------------------------- NavMesh Agent

    constexpr FieldDesc kNavMeshAgentFields[] = {
        JE_FIELD(NavMeshAgentComponent, enabled),
        JE_FIELD(NavMeshAgentComponent, agentType, .tooltip = "The agent type (Window > AI > Navigation > Agents): it walks the surfaces baked for it."),
        JE_FIELD(NavMeshAgentComponent, baseOffset, .unit = Unit::Meters, .tooltip = "Height of the pivot above the navmesh."),
        JE_FIELD(NavMeshAgentComponent, speed, .unit = Unit::MetersPerSecond, .tooltip = "Top speed (m/s; metres on 2D surfaces too)."),
        JE_FIELD(NavMeshAgentComponent, angularSpeed, .tooltip = "Turn rate (degrees/s) while Update Rotation is on."),
        JE_FIELD(NavMeshAgentComponent, acceleration, .tooltip = "m/s2 to speed up and to brake."),
        JE_FIELD(NavMeshAgentComponent, stoppingDistance, .unit = Unit::Meters, .tooltip = "It stops this far from the destination."),
        JE_FIELD(NavMeshAgentComponent, autoBraking, .tooltip = "Slow down before the destination so it stops there without overshooting."),
        JE_FIELD(NavMeshAgentComponent, radius, .unit = Unit::Meters),
        JE_FIELD(NavMeshAgentComponent, height, .unit = Unit::Meters),
        JE_FIELD(NavMeshAgentComponent, obstacleAvoidance, .enumDesc = &enums::kNavAvoidanceQuality, .label = "Quality", .tooltip = "How carefully it steers around other agents (None: it walks through them)."),
        JE_FIELD(NavMeshAgentComponent, avoidancePriority, .label = "Priority", .tooltip = "0-99, lower is more important: it avoids agents of its priority or lower and ignores higher numbers."),
        JE_FIELD(NavMeshAgentComponent, autoTraverseOffMeshLink),
        JE_FIELD(NavMeshAgentComponent, autoRepath),
        JE_FIELD(NavMeshAgentComponent, areaMask),
        JE_FIELD(NavMeshAgentComponent, updatePosition),
        JE_FIELD(NavMeshAgentComponent, updateRotation),
        JE_FIELD(NavMeshAgentComponent, driver, .enumDesc = &enums::kNavAgentDriver, .tooltip = "What moves it: its Transform, its Rigidbody's velocity, its Character Controller, or nothing (scripts)."),
        JE_FIELD(NavMeshAgentComponent, surface, .ops = &ops::Entity, .tooltip = "The NavMesh Surface or Nav Grid it walks; none: the surface of its type under it, else a grid holding it."),
        // ---- S76 agents: the stuck watchdog (R25)
        JE_FIELD(NavMeshAgentComponent, stuckDetection, .label = "Detect Stuck", .tooltip = "Fire onStuck when it makes no progress along its path for Stuck After seconds."),
        JE_FIELD(NavMeshAgentComponent, stuckSpeedRatio, .tooltip = "Progress counts once it moved this fraction of Speed x Stuck After from where it was."),
        JE_FIELD(NavMeshAgentComponent, stuckTimeout, .unit = Unit::Seconds, .label = "Stuck After", .tooltip = "Seconds without progress before onStuck fires."),
        JE_FIELD(NavMeshAgentComponent, repathWhenStuck, .tooltip = "Ask for the destination again when stuck."),
    };

    const ComponentCodec kNavMeshAgentCodec = JE_STREAM_CODEC(NavMeshAgentComponent, encodeNavMeshAgentComponent, decodeNavMeshAgentComponent);

    constexpr const char* kNavMeshAgentKnownKeys[] = {"acceleration", "agentType", "angularSpeed", "areaMask", "autoBraking", "autoRepath",
        "autoTraverseOffMeshLink", "avoidancePriority", "baseOffset", "driver", "enabled", "height", "obstacleAvoidance", "radius",
        "repathWhenStuck", "speed", "stoppingDistance", "stuckDetection", "stuckSpeedRatio", "stuckTimeout", "surface", "updatePosition",
        "updateRotation"};

    // ---------------------------------------------------------------- NavMesh Obstacle

    constexpr FieldDesc kNavMeshObstacleFields[] = {
        JE_FIELD(NavMeshObstacleComponent, enabled),
        JE_FIELD(NavMeshObstacleComponent, shape, .enumDesc = &enums::kNavObstacleShape),
        JE_FIELD(NavMeshObstacleComponent, center, .unit = Unit::Meters),
        JE_FIELD(NavMeshObstacleComponent, size, .unit = Unit::Meters),
        JE_FIELD(NavMeshObstacleComponent, radius, .unit = Unit::Meters),
        JE_FIELD(NavMeshObstacleComponent, height, .unit = Unit::Meters),
        JE_FIELD(NavMeshObstacleComponent, carve, .tooltip = "Cut a hole in the navmesh (needs Runtime Carving on its surface); off: agents steer around it."),
        JE_FIELD(NavMeshObstacleComponent, moveThreshold, .unit = Unit::Meters, .tooltip = "It is carved again once it moved this far."),
        JE_FIELD(NavMeshObstacleComponent, timeToStationary, .unit = Unit::Seconds, .tooltip = "Seconds without moving before it counts as stationary."),
        JE_FIELD(NavMeshObstacleComponent, carveOnlyStationary, .tooltip = "Carve only while stationary (moving, it is avoided)."),
    };

    const ComponentCodec kNavMeshObstacleCodec = JE_STREAM_CODEC(NavMeshObstacleComponent, encodeNavMeshObstacleComponent,
                                                                 decodeNavMeshObstacleComponent);

    constexpr const char* kNavMeshObstacleKnownKeys[] = {"carve", "carveOnlyStationary", "center", "enabled", "height", "moveThreshold",
        "radius", "shape", "size", "timeToStationary"};

    // ---------------------------------------------------------------- NavMesh Link

    constexpr FieldDesc kNavMeshLinkFields[] = {
        JE_FIELD(NavMeshLinkComponent, enabled),
        JE_FIELD(NavMeshLinkComponent, agentType),
        JE_FIELD(NavMeshLinkComponent, startPoint, .unit = Unit::Meters, .tooltip = "In this GameObject's space; drag the handles in the Scene view."),
        JE_FIELD(NavMeshLinkComponent, endPoint, .unit = Unit::Meters, .tooltip = "In this GameObject's space; drag the handles in the Scene view."),
        JE_FIELD(NavMeshLinkComponent, width, .unit = Unit::Meters, .tooltip = "0: one point-to-point connection; wider: connections side by side, one per agent diameter."),
        JE_FIELD(NavMeshLinkComponent, costModifier, .label = "Cost Override", .tooltip = "Negative: the area's cost; otherwise the cost per metre of this link."),
        JE_FIELD(NavMeshLinkComponent, bidirectional, .tooltip = "Agents may cross it both ways."),
        JE_FIELD(NavMeshLinkComponent, autoUpdatePosition, .label = "Auto Update Positions", .tooltip = "Follow the GameObject when it moves (the link is updated at runtime)."),
        JE_FIELD(NavMeshLinkComponent, area, .label = "Area Type"),
        JE_FIELD(NavMeshLinkComponent, activated, .tooltip = "Agents may use it; off: paths and agents ignore it (an agent on it finishes). Not baked."),
    };

    const ComponentCodec kNavMeshLinkCodec = JE_STREAM_CODEC(NavMeshLinkComponent, encodeNavMeshLinkComponent, decodeNavMeshLinkComponent);

    constexpr const char* kNavMeshLinkKnownKeys[] = {"activated", "agentType", "area", "autoUpdatePosition", "bidirectional", "costModifier", "enabled",
        "endPoint", "startPoint", "width"};

    // ---------------------------------------------------------------- NavMesh Modifier

    constexpr FieldDesc kNavMeshModifierFields[] = {
        JE_FIELD(NavMeshModifierComponent, enabled),
        JE_FIELD(NavMeshModifierComponent, overrideInclusion, .enumDesc = &enums::kNavModifierInclusion, .label = "Mode",
                 .tooltip = "Surface Policy: collected as the surface decides (moving bodies, agents and obstacles are left out); Add or Modify Object: collected even when the surface would leave it out; Remove Object: never collected."),
        JE_FIELD(NavMeshModifierComponent, overrideArea, .tooltip = "Give what it covers its own area."),
        JE_FIELD(NavMeshModifierComponent, area, .label = "Area Type"),
        JE_FIELD(NavMeshModifierComponent, applyToChildren, .tooltip = "Its descendants too (the nearest modifier wins)."),
        JE_FIELD(NavMeshModifierComponent, useVolume, .label = "Volume", .tooltip = "Mark a box (this GameObject's space) instead of the GameObject's geometry."),
        JE_FIELD(NavMeshModifierComponent, volumeCenter, .unit = Unit::Meters, .label = "Center"),
        JE_FIELD(NavMeshModifierComponent, volumeSize, .unit = Unit::Meters, .label = "Size"),
        JE_FIELD(NavMeshModifierComponent, applyAtRuntime, .tooltip = "The bake only splits the navmesh along the volume; while enabled, the polygons inside take its Area Type (a door, a bridge)."),
    };

    const ComponentCodec kNavMeshModifierCodec = JE_STREAM_CODEC(NavMeshModifierComponent, encodeNavMeshModifierComponent,
                                                                 decodeNavMeshModifierComponent);

    constexpr const char* kNavMeshModifierKnownKeys[] = {"applyAtRuntime", "applyToChildren", "area", "enabled", "overrideArea", "overrideInclusion",
        "useVolume", "volumeCenter", "volumeSize"};

    // ---------------------------------------------------------------- Nav Grid

    constexpr FieldDesc kNavGridFields[] = {
        JE_FIELD(NavGridComponent, enabled),
        JE_FIELD(NavGridComponent, layout, .enumDesc = &enums::kNavGridLayout, .tooltip = "Square cells, or hex cells (pointy-top, odd rows shifted right by half a cell)."),
        JE_FIELD(NavGridComponent, neighbours, .tooltip = "Square grids: 4 (no diagonals) or 8 neighbours."),
        JE_FIELD(NavGridComponent, cellSize, .unit = Unit::Meters),
        JE_FIELD(NavGridComponent, width),
        JE_FIELD(NavGridComponent, height),
        JE_FIELD(NavGridComponent, plane, .enumDesc = &enums::kNavPlane),
        JE_FIELD(NavGridComponent, maxSlope, .unit = Unit::Degrees, .tooltip = "With heights (Terrain source): steps steeper than this (degrees) are blocked."),
        JE_FIELD(NavGridComponent, maxStep, .unit = Unit::Meters, .tooltip = "With heights: steps higher than this are blocked."),
        JE_FIELD(NavGridComponent, movement, .enumDesc = &enums::kNavGridMovement, .tooltip = "Free: straight lines between turns (line of sight); Cell Centres: from centre to centre."),
        JE_FIELD(NavGridComponent, reservationLookahead, .unit = Unit::Seconds, .tooltip = "Seconds of their path agents reserve ahead; others pay the penalty on reserved cells."),
        JE_FIELD(NavGridComponent, reservationPenalty, .tooltip = "Cost multiplier of a cell another agent reserved."),
        JE_FIELD(NavGridComponent, source, .enumDesc = &enums::kNavGridSource, .tooltip = "Where Rebuild takes the cells from: painted only, a Tilemap's tiles or a Terrain's heights."),
        JE_FIELD(NavGridComponent, sourceEntity, .ops = &ops::Entity, .label = "Source Object", .tooltip = "The Tilemap or Terrain; none: this GameObject's."),
        // the tilemap rect -> area table: one value (the Inspector edits it as a table)
        JE_FIELD(NavGridComponent, tileAreas, .ops = &ops::Opaque<std::vector<NavGridTileArea>>, .flags = NoDefault),
        JE_FIELD(NavGridComponent, emptyCellArea, .label = "Cells Without Tiles", .tooltip = "The area of cells no tile covers."),
        JE_FIELD(NavGridComponent, collidersBlock, .tooltip = "Static Collider2D footprints block the cells under them."),
        JE_FIELD(NavGridComponent, autoRebuild),
        JE_FIELD(NavGridComponent, gridData),
    };

    const ComponentCodec kNavGridCodec = JE_STREAM_CODEC(NavGridComponent, encodeNavGridComponent, decodeNavGridComponent);

    constexpr const char* kNavGridKnownKeys[] = {"autoRebuild", "cellSize", "collidersBlock", "emptyCellArea", "enabled", "gridData",
        "gridDataGuid", "height", "layout", "maxSlope", "maxStep", "movement", "neighbours", "plane", "reservationLookahead",
        "reservationPenalty", "source", "sourceEntity", "tileAreas", "width"};

}

const ComponentDesc kNavMeshSurfaceDesc{
    .type = ComponentType::NavMeshSurfaceComponent, .cppName = "NavMeshSurfaceComponent", .key = "navmeshsurface",
    .displayName = "NavMesh Surface", .icon = ICON_FA_ROUTE,
    .body = JE_STRUCT_DESC(NavMeshSurfaceComponent, "NavMeshSurfaceComponent", kNavMeshSurfaceFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(NavMeshSurfaceComponent),
    .codec = &kNavMeshSurfaceCodec,
    .decodeApply = DecodeApply::MarkOnly,
    .codegen = JE_FACTORY_CODEGEN(createNavMeshSurfaceComponent),
    .extraKeys = kNavMeshSurfaceKnownKeys,
};

const ComponentDesc kNavMeshAgentDesc{
    .type = ComponentType::NavMeshAgentComponent, .cppName = "NavMeshAgentComponent", .key = "navmeshagent",
    .displayName = "NavMesh Agent", .icon = ICON_FA_PERSON_WALKING_ARROW_RIGHT,
    .body = JE_STRUCT_DESC(NavMeshAgentComponent, "NavMeshAgentComponent", kNavMeshAgentFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(NavMeshAgentComponent),
    .codec = &kNavMeshAgentCodec,
    .decodeApply = DecodeApply::MarkOnly,
    .codegen = JE_FACTORY_CODEGEN(createNavMeshAgentComponent),
    .extraKeys = kNavMeshAgentKnownKeys,
};

const ComponentDesc kNavMeshObstacleDesc{
    .type = ComponentType::NavMeshObstacleComponent, .cppName = "NavMeshObstacleComponent", .key = "navmeshobstacle",
    .displayName = "NavMesh Obstacle", .icon = ICON_FA_BAN,
    .body = JE_STRUCT_DESC(NavMeshObstacleComponent, "NavMeshObstacleComponent", kNavMeshObstacleFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(NavMeshObstacleComponent),
    .codec = &kNavMeshObstacleCodec,
    .decodeApply = DecodeApply::MarkOnly,
    .codegen = JE_FACTORY_CODEGEN(createNavMeshObstacleComponent),
    .extraKeys = kNavMeshObstacleKnownKeys,
};

const ComponentDesc kNavMeshLinkDesc{
    .type = ComponentType::NavMeshLinkComponent, .cppName = "NavMeshLinkComponent", .key = "navmeshlink",
    .displayName = "NavMesh Link", .icon = ICON_FA_BRIDGE,
    .body = JE_STRUCT_DESC(NavMeshLinkComponent, "NavMeshLinkComponent", kNavMeshLinkFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(NavMeshLinkComponent),
    .codec = &kNavMeshLinkCodec,
    .decodeApply = DecodeApply::MarkOnly,
    .codegen = JE_FACTORY_CODEGEN(createNavMeshLinkComponent),
    .extraKeys = kNavMeshLinkKnownKeys,
};

const ComponentDesc kNavMeshModifierDesc{
    .type = ComponentType::NavMeshModifierComponent, .cppName = "NavMeshModifierComponent", .key = "navmeshmodifier",
    .displayName = "NavMesh Modifier", .icon = ICON_FA_DRAW_POLYGON,
    .body = JE_STRUCT_DESC(NavMeshModifierComponent, "NavMeshModifierComponent", kNavMeshModifierFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(NavMeshModifierComponent),
    .codec = &kNavMeshModifierCodec,
    .decodeApply = DecodeApply::MarkOnly,
    .codegen = JE_FACTORY_CODEGEN(createNavMeshModifierComponent),
    .extraKeys = kNavMeshModifierKnownKeys,
};

const ComponentDesc kNavGridDesc{
    .type = ComponentType::NavGridComponent, .cppName = "NavGridComponent", .key = "navgrid",
    .displayName = "Nav Grid", .icon = ICON_FA_TABLE_CELLS,
    .body = JE_STRUCT_DESC(NavGridComponent, "NavGridComponent", kNavGridFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(NavGridComponent),
    .codec = &kNavGridCodec,
    .decodeApply = DecodeApply::MarkOnly,
    .codegen = JE_FACTORY_CODEGEN(createNavGridComponent),
    .extraKeys = kNavGridKnownKeys,
};

}
