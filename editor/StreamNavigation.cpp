// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The navigation components in scenes and prefabs (docs/specs/S19-navigation.md R41): every field
// with its default when it is missing, so a scene from before navigation, or one missing a key,
// reads unchanged. The cells of a NavGrid and a surface's navmesh are assets (.navgrid,
// .navmesh), referenced by GUID; the components' runtime state is never saved.

#include "Stream.h"
#include "StreamSchema.h"
#include "util/YamlNumbers.h"

#include <algorithm>

using namespace jasperengine;

// ---------------------------------------------------------------- NavMesh Surface

YAML::Node editor::Stream::encodeNavMeshSurfaceComponent(const NavMeshSurfaceComponent& surface) {
    YAML::Node node;
    node["enabled"] = surface.enabled;
    node["agentType"] = surface.agentType;
    node["collectObjects"] = StreamSchema::navCollectObjects().toString(surface.collectObjects);
    node["useGeometry"] = StreamSchema::navUseGeometry().toString(surface.useGeometry);
    node["includeLayers"] = surface.includeLayers;
    node["defaultArea"] = surface.defaultArea;
    node["volumeCenter"] = encodeVector3(surface.volumeCenter);
    node["volumeSize"] = encodeVector3(surface.volumeSize);
    node["overrideCellSize"] = surface.overrideCellSize;
    node["cellSize"] = surface.cellSize;
    node["overrideTileSize"] = surface.overrideTileSize;
    node["tileSize"] = surface.tileSize;
    node["minRegionArea"] = surface.minRegionArea;
    node["plane"] = StreamSchema::navPlane().toString(surface.plane);
    node["runtimeCarving"] = surface.runtimeCarving;
    node["maxCarvingObstacles"] = surface.maxCarvingObstacles;
    encodeAssetRef(node, "navMeshData", surface.navMeshData);
    return node;
}

NavMeshSurfaceComponent editor::Stream::decodeNavMeshSurfaceComponent(const YAML::Node& node, const NavMeshSurfaceComponent* oldComponent) {
    NavMeshSurfaceComponent surface;
    if (oldComponent)
        surface = *oldComponent;
    if (node["enabled"]) surface.enabled = YamlNumbers::as<bool>(node["enabled"]);
    if (node["agentType"]) surface.agentType = YamlNumbers::as<int>(node["agentType"]);
    if (node["collectObjects"]) surface.collectObjects = decodeEnum(node, "collectObjects", StreamSchema::navCollectObjects());
    if (node["useGeometry"]) surface.useGeometry = decodeEnum(node, "useGeometry", StreamSchema::navUseGeometry());
    if (node["includeLayers"]) surface.includeLayers = YamlNumbers::as<uint32_t>(node["includeLayers"]);
    if (node["defaultArea"]) surface.defaultArea = YamlNumbers::as<int>(node["defaultArea"]);
    if (node["volumeCenter"]) surface.volumeCenter = decodeVector3(node["volumeCenter"]);
    if (node["volumeSize"]) surface.volumeSize = decodeVector3(node["volumeSize"]);
    if (node["overrideCellSize"]) surface.overrideCellSize = YamlNumbers::as<bool>(node["overrideCellSize"]);
    if (node["cellSize"]) surface.cellSize = YamlNumbers::as<float>(node["cellSize"]);
    if (node["overrideTileSize"]) surface.overrideTileSize = YamlNumbers::as<bool>(node["overrideTileSize"]);
    if (node["tileSize"]) surface.tileSize = YamlNumbers::as<int>(node["tileSize"]);
    if (node["minRegionArea"]) surface.minRegionArea = YamlNumbers::as<float>(node["minRegionArea"]);
    if (node["plane"]) surface.plane = decodeEnum(node, "plane", StreamSchema::navPlane());
    if (node["runtimeCarving"]) surface.runtimeCarving = YamlNumbers::as<bool>(node["runtimeCarving"]);
    // (S74: missing in S19-era scenes, which keep the default 128)
    if (node["maxCarvingObstacles"])
        surface.maxCarvingObstacles = std::clamp(YamlNumbers::as<int>(node["maxCarvingObstacles"]), 1, 4096);
    if (node["navMeshData"]) surface.navMeshData = decodeAssetRef(node, "navMeshData");
    return surface;
}

// ---------------------------------------------------------------- NavMesh Agent

YAML::Node editor::Stream::encodeNavMeshAgentComponent(const NavMeshAgentComponent& agent) {
    YAML::Node node;
    node["enabled"] = agent.enabled;
    node["agentType"] = agent.agentType;
    node["baseOffset"] = agent.baseOffset;
    node["speed"] = agent.speed;
    node["angularSpeed"] = agent.angularSpeed;
    node["acceleration"] = agent.acceleration;
    node["stoppingDistance"] = agent.stoppingDistance;
    node["autoBraking"] = agent.autoBraking;
    node["radius"] = agent.radius;
    node["height"] = agent.height;
    node["obstacleAvoidance"] = StreamSchema::navAvoidanceQuality().toString(agent.obstacleAvoidance);
    node["avoidancePriority"] = agent.avoidancePriority;
    node["autoTraverseOffMeshLink"] = agent.autoTraverseOffMeshLink;
    node["autoRepath"] = agent.autoRepath;
    node["areaMask"] = agent.areaMask;
    node["updatePosition"] = agent.updatePosition;
    node["updateRotation"] = agent.updateRotation;
    node["driver"] = StreamSchema::navAgentDriver().toString(agent.driver);
    node["surface"] = agent.surface;
    node["stuckDetection"] = agent.stuckDetection;
    node["stuckSpeedRatio"] = agent.stuckSpeedRatio;
    node["stuckTimeout"] = agent.stuckTimeout;
    node["repathWhenStuck"] = agent.repathWhenStuck;
    return node;
}

NavMeshAgentComponent editor::Stream::decodeNavMeshAgentComponent(const YAML::Node& node, const NavMeshAgentComponent* oldComponent) {
    NavMeshAgentComponent agent;
    if (oldComponent)
        agent = *oldComponent;
    if (node["enabled"]) agent.enabled = YamlNumbers::as<bool>(node["enabled"]);
    if (node["agentType"]) agent.agentType = YamlNumbers::as<int>(node["agentType"]);
    if (node["baseOffset"]) agent.baseOffset = YamlNumbers::as<float>(node["baseOffset"]);
    if (node["speed"]) agent.speed = YamlNumbers::as<float>(node["speed"]);
    if (node["angularSpeed"]) agent.angularSpeed = YamlNumbers::as<float>(node["angularSpeed"]);
    if (node["acceleration"]) agent.acceleration = YamlNumbers::as<float>(node["acceleration"]);
    if (node["stoppingDistance"]) agent.stoppingDistance = YamlNumbers::as<float>(node["stoppingDistance"]);
    if (node["autoBraking"]) agent.autoBraking = YamlNumbers::as<bool>(node["autoBraking"]);
    if (node["radius"]) agent.radius = YamlNumbers::as<float>(node["radius"]);
    if (node["height"]) agent.height = YamlNumbers::as<float>(node["height"]);
    if (node["obstacleAvoidance"]) agent.obstacleAvoidance = decodeEnum(node, "obstacleAvoidance", StreamSchema::navAvoidanceQuality());
    if (node["avoidancePriority"]) agent.avoidancePriority = YamlNumbers::as<int>(node["avoidancePriority"]);
    if (node["autoTraverseOffMeshLink"]) agent.autoTraverseOffMeshLink = YamlNumbers::as<bool>(node["autoTraverseOffMeshLink"]);
    if (node["autoRepath"]) agent.autoRepath = YamlNumbers::as<bool>(node["autoRepath"]);
    if (node["areaMask"]) agent.areaMask = YamlNumbers::as<int>(node["areaMask"]);
    if (node["updatePosition"]) agent.updatePosition = YamlNumbers::as<bool>(node["updatePosition"]);
    if (node["updateRotation"]) agent.updateRotation = YamlNumbers::as<bool>(node["updateRotation"]);
    if (node["driver"]) agent.driver = decodeEnum(node, "driver", StreamSchema::navAgentDriver());
    if (node["surface"]) agent.surface = YamlNumbers::as<Entity>(node["surface"]);
    if (node["stuckDetection"]) agent.stuckDetection = YamlNumbers::as<bool>(node["stuckDetection"]);
    if (node["stuckSpeedRatio"]) agent.stuckSpeedRatio = YamlNumbers::as<float>(node["stuckSpeedRatio"]);
    if (node["stuckTimeout"]) agent.stuckTimeout = YamlNumbers::as<float>(node["stuckTimeout"]);
    if (node["repathWhenStuck"]) agent.repathWhenStuck = YamlNumbers::as<bool>(node["repathWhenStuck"]);
    return agent;
}

// ---------------------------------------------------------------- NavMesh Obstacle

YAML::Node editor::Stream::encodeNavMeshObstacleComponent(const NavMeshObstacleComponent& obstacle) {
    YAML::Node node;
    node["enabled"] = obstacle.enabled;
    node["shape"] = StreamSchema::navObstacleShape().toString(obstacle.shape);
    node["center"] = encodeVector3(obstacle.center);
    node["size"] = encodeVector3(obstacle.size);
    node["radius"] = obstacle.radius;
    node["height"] = obstacle.height;
    node["carve"] = obstacle.carve;
    node["moveThreshold"] = obstacle.moveThreshold;
    node["timeToStationary"] = obstacle.timeToStationary;
    node["carveOnlyStationary"] = obstacle.carveOnlyStationary;
    return node;
}

NavMeshObstacleComponent editor::Stream::decodeNavMeshObstacleComponent(const YAML::Node& node, const NavMeshObstacleComponent* oldComponent) {
    NavMeshObstacleComponent obstacle;
    if (oldComponent)
        obstacle = *oldComponent;
    if (node["enabled"]) obstacle.enabled = YamlNumbers::as<bool>(node["enabled"]);
    if (node["shape"]) obstacle.shape = decodeEnum(node, "shape", StreamSchema::navObstacleShape());
    if (node["center"]) obstacle.center = decodeVector3(node["center"]);
    if (node["size"]) obstacle.size = decodeVector3(node["size"]);
    if (node["radius"]) obstacle.radius = YamlNumbers::as<float>(node["radius"]);
    if (node["height"]) obstacle.height = YamlNumbers::as<float>(node["height"]);
    if (node["carve"]) obstacle.carve = YamlNumbers::as<bool>(node["carve"]);
    if (node["moveThreshold"]) obstacle.moveThreshold = YamlNumbers::as<float>(node["moveThreshold"]);
    if (node["timeToStationary"]) obstacle.timeToStationary = YamlNumbers::as<float>(node["timeToStationary"]);
    if (node["carveOnlyStationary"]) obstacle.carveOnlyStationary = YamlNumbers::as<bool>(node["carveOnlyStationary"]);
    return obstacle;
}

// ---------------------------------------------------------------- NavMesh Link

YAML::Node editor::Stream::encodeNavMeshLinkComponent(const NavMeshLinkComponent& link) {
    YAML::Node node;
    node["enabled"] = link.enabled;
    node["agentType"] = link.agentType;
    node["startPoint"] = encodeVector3(link.startPoint);
    node["endPoint"] = encodeVector3(link.endPoint);
    node["width"] = link.width;
    node["costModifier"] = link.costModifier;
    node["bidirectional"] = link.bidirectional;
    node["autoUpdatePosition"] = link.autoUpdatePosition;
    node["area"] = link.area;
    node["activated"] = link.activated;
    return node;
}

NavMeshLinkComponent editor::Stream::decodeNavMeshLinkComponent(const YAML::Node& node, const NavMeshLinkComponent* oldComponent) {
    NavMeshLinkComponent link;
    if (oldComponent)
        link = *oldComponent;
    if (node["enabled"]) link.enabled = YamlNumbers::as<bool>(node["enabled"]);
    if (node["agentType"]) link.agentType = YamlNumbers::as<int>(node["agentType"]);
    if (node["startPoint"]) link.startPoint = decodeVector3(node["startPoint"]);
    if (node["endPoint"]) link.endPoint = decodeVector3(node["endPoint"]);
    if (node["width"]) link.width = YamlNumbers::as<float>(node["width"]);
    if (node["costModifier"]) link.costModifier = YamlNumbers::as<float>(node["costModifier"]);
    if (node["bidirectional"]) link.bidirectional = YamlNumbers::as<bool>(node["bidirectional"]);
    if (node["autoUpdatePosition"]) link.autoUpdatePosition = YamlNumbers::as<bool>(node["autoUpdatePosition"]);
    if (node["area"]) link.area = YamlNumbers::as<int>(node["area"]);
    if (node["activated"]) link.activated = YamlNumbers::as<bool>(node["activated"]);
    return link;
}

// ---------------------------------------------------------------- NavMesh Modifier

YAML::Node editor::Stream::encodeNavMeshModifierComponent(const NavMeshModifierComponent& modifier) {
    YAML::Node node;
    node["enabled"] = modifier.enabled;
    node["overrideInclusion"] = StreamSchema::navModifierInclusion().toString(modifier.overrideInclusion);
    node["overrideArea"] = modifier.overrideArea;
    node["area"] = modifier.area;
    node["applyToChildren"] = modifier.applyToChildren;
    node["useVolume"] = modifier.useVolume;
    node["volumeCenter"] = encodeVector3(modifier.volumeCenter);
    node["volumeSize"] = encodeVector3(modifier.volumeSize);
    node["applyAtRuntime"] = modifier.applyAtRuntime;
    return node;
}

NavMeshModifierComponent editor::Stream::decodeNavMeshModifierComponent(const YAML::Node& node, const NavMeshModifierComponent* oldComponent) {
    NavMeshModifierComponent modifier;
    if (oldComponent)
        modifier = *oldComponent;
    if (node["enabled"]) modifier.enabled = YamlNumbers::as<bool>(node["enabled"]);
    if (node["overrideInclusion"]) modifier.overrideInclusion = decodeEnum(node, "overrideInclusion", StreamSchema::navModifierInclusion());
    if (node["overrideArea"]) modifier.overrideArea = YamlNumbers::as<bool>(node["overrideArea"]);
    if (node["area"]) modifier.area = YamlNumbers::as<int>(node["area"]);
    if (node["applyToChildren"]) modifier.applyToChildren = YamlNumbers::as<bool>(node["applyToChildren"]);
    if (node["useVolume"]) modifier.useVolume = YamlNumbers::as<bool>(node["useVolume"]);
    if (node["volumeCenter"]) modifier.volumeCenter = decodeVector3(node["volumeCenter"]);
    if (node["volumeSize"]) modifier.volumeSize = decodeVector3(node["volumeSize"]);
    if (node["applyAtRuntime"]) modifier.applyAtRuntime = YamlNumbers::as<bool>(node["applyAtRuntime"]);
    return modifier;
}

// ---------------------------------------------------------------- Nav Grid

YAML::Node editor::Stream::encodeNavGridComponent(const NavGridComponent& grid) {
    YAML::Node node;
    node["enabled"] = grid.enabled;
    node["layout"] = StreamSchema::navGridLayout().toString(grid.layout);
    node["neighbours"] = grid.neighbours;
    node["cellSize"] = grid.cellSize;
    node["width"] = grid.width;
    node["height"] = grid.height;
    node["plane"] = StreamSchema::navPlane().toString(grid.plane);
    node["maxSlope"] = grid.maxSlope;
    node["maxStep"] = grid.maxStep;
    node["movement"] = StreamSchema::navGridMovement().toString(grid.movement);
    node["reservationLookahead"] = grid.reservationLookahead;
    node["reservationPenalty"] = grid.reservationPenalty;
    node["source"] = StreamSchema::navGridSource().toString(grid.source);
    node["sourceEntity"] = grid.sourceEntity;
    YAML::Node areas(YAML::NodeType::Sequence);
    for (const NavGridTileArea& row : grid.tileAreas) {
        YAML::Node entry;
        entry["rect"] = row.rect;
        entry["area"] = row.area;
        areas.push_back(entry);
    }
    node["tileAreas"] = areas;
    node["emptyCellArea"] = grid.emptyCellArea;
    node["collidersBlock"] = grid.collidersBlock;
    node["autoRebuild"] = grid.autoRebuild;
    encodeAssetRef(node, "gridData", grid.gridData);
    return node;
}

NavGridComponent editor::Stream::decodeNavGridComponent(const YAML::Node& node, const NavGridComponent* oldComponent) {
    NavGridComponent grid;
    if (oldComponent)
        grid = *oldComponent;
    if (node["enabled"]) grid.enabled = YamlNumbers::as<bool>(node["enabled"]);
    if (node["layout"]) grid.layout = decodeEnum(node, "layout", StreamSchema::navGridLayout());
    if (node["neighbours"]) grid.neighbours = YamlNumbers::as<int>(node["neighbours"]) == 4 ? 4 : 8;
    if (node["cellSize"]) grid.cellSize = YamlNumbers::as<float>(node["cellSize"]);
    if (node["width"]) grid.width = YamlNumbers::as<int>(node["width"]);
    if (node["height"]) grid.height = YamlNumbers::as<int>(node["height"]);
    if (node["plane"]) grid.plane = decodeEnum(node, "plane", StreamSchema::navPlane());
    if (node["maxSlope"]) grid.maxSlope = YamlNumbers::as<float>(node["maxSlope"]);
    if (node["maxStep"]) grid.maxStep = YamlNumbers::as<float>(node["maxStep"]);
    if (node["movement"]) grid.movement = decodeEnum(node, "movement", StreamSchema::navGridMovement());
    if (node["reservationLookahead"]) grid.reservationLookahead = YamlNumbers::as<float>(node["reservationLookahead"]);
    if (node["reservationPenalty"]) grid.reservationPenalty = YamlNumbers::as<float>(node["reservationPenalty"]);
    if (node["source"]) grid.source = decodeEnum(node, "source", StreamSchema::navGridSource());
    if (node["sourceEntity"]) grid.sourceEntity = YamlNumbers::as<Entity>(node["sourceEntity"]);
    if (node["tileAreas"] && node["tileAreas"].IsSequence()) {
        grid.tileAreas.clear();
        for (const YAML::Node& entry : node["tileAreas"]) {
            NavGridTileArea row;
            if (entry["rect"]) row.rect = entry["rect"].as<std::string>();
            if (entry["area"]) row.area = YamlNumbers::as<int>(entry["area"]);
            grid.tileAreas.push_back(row);
        }
    }
    if (node["emptyCellArea"]) grid.emptyCellArea = YamlNumbers::as<int>(node["emptyCellArea"]);
    if (node["collidersBlock"]) grid.collidersBlock = YamlNumbers::as<bool>(node["collidersBlock"]);
    if (node["autoRebuild"]) grid.autoRebuild = YamlNumbers::as<bool>(node["autoRebuild"]);
    if (node["gridData"]) grid.gridData = decodeAssetRef(node, "gridData");
    return grid;
}
