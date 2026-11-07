// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The C++ an exported game builds its navigation components from (docs/specs/S19-navigation.md):
// every saved field. The baked .navmesh and .navgrid assets ship with the game's assets and load
// by path; the components' runtime state is not exported.

#include "Factory.h"
#include "reflect/Enums.h"

#include <sstream>

using namespace jasperengine;

namespace {

    // the C++ name of an enum value, from its reflection table (an unlisted value: the fallback's)
    template<typename E>
    const char* enumCode(const editor::reflect::EnumDesc& desc, E value) {
        const editor::reflect::EnumItem* item = editor::reflect::enumItem(desc, static_cast<int>(value));
        if (!item) item = editor::reflect::enumItem(desc, desc.fallback);
        return item ? item->cppName : "";
    }

}

std::string editor::Factory::createNavMeshSurfaceComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName,
                                                          std::string entityName, bool assignExisting,
                                                          const std::unordered_map<Entity, std::string>* entityVarNames) {
    (void)entityVarNames;
    if (!scene->findComponent<NavMeshSurfaceComponent>(entity)) return "";
    const NavMeshSurfaceComponent& surface = scene->getComponent<NavMeshSurfaceComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << ind << "NavMeshSurfaceComponent navSurface;\n";
    code << ind << "navSurface.agentType = " << surface.agentType << ";\n";
    code << ind << "navSurface.collectObjects = " << enumCode(editor::reflect::enums::kNavCollectObjects, surface.collectObjects) << ";\n";
    code << ind << "navSurface.useGeometry = " << enumCode(editor::reflect::enums::kNavUseGeometry, surface.useGeometry) << ";\n";
    code << ind << "navSurface.includeLayers = " << surface.includeLayers << "u;\n";
    code << ind << "navSurface.defaultArea = " << surface.defaultArea << ";\n";
    code << ind << "navSurface.volumeCenter = " << formatVector3(surface.volumeCenter) << ";\n";
    code << ind << "navSurface.volumeSize = " << formatVector3(surface.volumeSize) << ";\n";
    code << ind << "navSurface.overrideCellSize = " << formatBool(surface.overrideCellSize) << ";\n";
    code << ind << "navSurface.cellSize = " << formatFloat(surface.cellSize) << ";\n";
    code << ind << "navSurface.overrideTileSize = " << formatBool(surface.overrideTileSize) << ";\n";
    code << ind << "navSurface.tileSize = " << surface.tileSize << ";\n";
    code << ind << "navSurface.minRegionArea = " << formatFloat(surface.minRegionArea) << ";\n";
    code << ind << "navSurface.plane = " << enumCode(editor::reflect::enums::kNavPlane, surface.plane) << ";\n";
    code << ind << "navSurface.runtimeCarving = " << formatBool(surface.runtimeCarving) << ";\n";
    code << ind << "navSurface.maxCarvingObstacles = " << surface.maxCarvingObstacles << ";\n";
    code << ind << "navSurface.navMeshData = " << formatString(surface.navMeshData.path) << ";\n";
    if (!surface.enabled) code << ind << "navSurface.enabled = false;\n";
    addComponentCode(code, ind, sceneName, entityName, entity, "NavMeshSurfaceComponent", "navSurface", assignExisting);
    return code.str();
}

std::string editor::Factory::createNavMeshAgentComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName,
                                                        std::string entityName, bool assignExisting,
                                                        const std::unordered_map<Entity, std::string>* entityVarNames) {
    if (!scene->findComponent<NavMeshAgentComponent>(entity)) return "";
    const NavMeshAgentComponent& agent = scene->getComponent<NavMeshAgentComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << ind << "NavMeshAgentComponent navAgent;\n";
    code << ind << "navAgent.agentType = " << agent.agentType << ";\n";
    code << ind << "navAgent.baseOffset = " << formatFloat(agent.baseOffset) << ";\n";
    code << ind << "navAgent.speed = " << formatFloat(agent.speed) << ";\n";
    code << ind << "navAgent.angularSpeed = " << formatFloat(agent.angularSpeed) << ";\n";
    code << ind << "navAgent.acceleration = " << formatFloat(agent.acceleration) << ";\n";
    code << ind << "navAgent.stoppingDistance = " << formatFloat(agent.stoppingDistance) << ";\n";
    code << ind << "navAgent.autoBraking = " << formatBool(agent.autoBraking) << ";\n";
    code << ind << "navAgent.radius = " << formatFloat(agent.radius) << ";\n";
    code << ind << "navAgent.height = " << formatFloat(agent.height) << ";\n";
    code << ind << "navAgent.obstacleAvoidance = " << enumCode(editor::reflect::enums::kNavAvoidanceQuality, agent.obstacleAvoidance) << ";\n";
    code << ind << "navAgent.avoidancePriority = " << agent.avoidancePriority << ";\n";
    code << ind << "navAgent.autoTraverseOffMeshLink = " << formatBool(agent.autoTraverseOffMeshLink) << ";\n";
    code << ind << "navAgent.autoRepath = " << formatBool(agent.autoRepath) << ";\n";
    code << ind << "navAgent.areaMask = " << agent.areaMask << ";\n";
    code << ind << "navAgent.updatePosition = " << formatBool(agent.updatePosition) << ";\n";
    code << ind << "navAgent.updateRotation = " << formatBool(agent.updateRotation) << ";\n";
    code << ind << "navAgent.driver = " << enumCode(editor::reflect::enums::kNavAgentDriver, agent.driver) << ";\n";
    code << ind << "navAgent.surface = " << formatEntity(agent.surface, entityVarNames) << ";\n";
    if (!agent.enabled) code << ind << "navAgent.enabled = false;\n";
    addComponentCode(code, ind, sceneName, entityName, entity, "NavMeshAgentComponent", "navAgent", assignExisting);
    return code.str();
}

std::string editor::Factory::createNavMeshObstacleComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName,
                                                           std::string entityName, bool assignExisting,
                                                           const std::unordered_map<Entity, std::string>* entityVarNames) {
    (void)entityVarNames;
    if (!scene->findComponent<NavMeshObstacleComponent>(entity)) return "";
    const NavMeshObstacleComponent& obstacle = scene->getComponent<NavMeshObstacleComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << ind << "NavMeshObstacleComponent navObstacle;\n";
    code << ind << "navObstacle.shape = " << enumCode(editor::reflect::enums::kNavObstacleShape, obstacle.shape) << ";\n";
    code << ind << "navObstacle.center = " << formatVector3(obstacle.center) << ";\n";
    code << ind << "navObstacle.size = " << formatVector3(obstacle.size) << ";\n";
    code << ind << "navObstacle.radius = " << formatFloat(obstacle.radius) << ";\n";
    code << ind << "navObstacle.height = " << formatFloat(obstacle.height) << ";\n";
    code << ind << "navObstacle.carve = " << formatBool(obstacle.carve) << ";\n";
    code << ind << "navObstacle.moveThreshold = " << formatFloat(obstacle.moveThreshold) << ";\n";
    code << ind << "navObstacle.timeToStationary = " << formatFloat(obstacle.timeToStationary) << ";\n";
    code << ind << "navObstacle.carveOnlyStationary = " << formatBool(obstacle.carveOnlyStationary) << ";\n";
    if (!obstacle.enabled) code << ind << "navObstacle.enabled = false;\n";
    addComponentCode(code, ind, sceneName, entityName, entity, "NavMeshObstacleComponent", "navObstacle", assignExisting);
    return code.str();
}

std::string editor::Factory::createNavMeshLinkComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName,
                                                       std::string entityName, bool assignExisting,
                                                       const std::unordered_map<Entity, std::string>* entityVarNames) {
    (void)entityVarNames;
    if (!scene->findComponent<NavMeshLinkComponent>(entity)) return "";
    const NavMeshLinkComponent& link = scene->getComponent<NavMeshLinkComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << ind << "NavMeshLinkComponent navLink;\n";
    code << ind << "navLink.agentType = " << link.agentType << ";\n";
    code << ind << "navLink.startPoint = " << formatVector3(link.startPoint) << ";\n";
    code << ind << "navLink.endPoint = " << formatVector3(link.endPoint) << ";\n";
    code << ind << "navLink.width = " << formatFloat(link.width) << ";\n";
    code << ind << "navLink.costModifier = " << formatFloat(link.costModifier) << ";\n";
    code << ind << "navLink.bidirectional = " << formatBool(link.bidirectional) << ";\n";
    code << ind << "navLink.autoUpdatePosition = " << formatBool(link.autoUpdatePosition) << ";\n";
    code << ind << "navLink.area = " << link.area << ";\n";
    if (!link.activated) code << ind << "navLink.activated = false;\n";
    if (!link.enabled) code << ind << "navLink.enabled = false;\n";
    addComponentCode(code, ind, sceneName, entityName, entity, "NavMeshLinkComponent", "navLink", assignExisting);
    return code.str();
}

std::string editor::Factory::createNavMeshModifierComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName,
                                                           std::string entityName, bool assignExisting,
                                                           const std::unordered_map<Entity, std::string>* entityVarNames) {
    (void)entityVarNames;
    if (!scene->findComponent<NavMeshModifierComponent>(entity)) return "";
    const NavMeshModifierComponent& modifier = scene->getComponent<NavMeshModifierComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << ind << "NavMeshModifierComponent navModifier;\n";
    code << ind << "navModifier.overrideInclusion = " << enumCode(editor::reflect::enums::kNavModifierInclusion, modifier.overrideInclusion) << ";\n";
    code << ind << "navModifier.overrideArea = " << formatBool(modifier.overrideArea) << ";\n";
    code << ind << "navModifier.area = " << modifier.area << ";\n";
    code << ind << "navModifier.applyToChildren = " << formatBool(modifier.applyToChildren) << ";\n";
    code << ind << "navModifier.useVolume = " << formatBool(modifier.useVolume) << ";\n";
    code << ind << "navModifier.volumeCenter = " << formatVector3(modifier.volumeCenter) << ";\n";
    code << ind << "navModifier.volumeSize = " << formatVector3(modifier.volumeSize) << ";\n";
    if (modifier.applyAtRuntime) code << ind << "navModifier.applyAtRuntime = true;\n";
    if (!modifier.enabled) code << ind << "navModifier.enabled = false;\n";
    addComponentCode(code, ind, sceneName, entityName, entity, "NavMeshModifierComponent", "navModifier", assignExisting);
    return code.str();
}

std::string editor::Factory::createNavGridComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName,
                                                   std::string entityName, bool assignExisting,
                                                   const std::unordered_map<Entity, std::string>* entityVarNames) {
    if (!scene->findComponent<NavGridComponent>(entity)) return "";
    const NavGridComponent& grid = scene->getComponent<NavGridComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << ind << "NavGridComponent navGrid;\n";
    code << ind << "navGrid.layout = " << enumCode(editor::reflect::enums::kNavGridLayout, grid.layout) << ";\n";
    code << ind << "navGrid.neighbours = " << grid.neighbours << ";\n";
    code << ind << "navGrid.cellSize = " << formatFloat(grid.cellSize) << ";\n";
    code << ind << "navGrid.width = " << grid.width << ";\n";
    code << ind << "navGrid.height = " << grid.height << ";\n";
    code << ind << "navGrid.plane = " << enumCode(editor::reflect::enums::kNavPlane, grid.plane) << ";\n";
    code << ind << "navGrid.maxSlope = " << formatFloat(grid.maxSlope) << ";\n";
    code << ind << "navGrid.maxStep = " << formatFloat(grid.maxStep) << ";\n";
    code << ind << "navGrid.movement = " << enumCode(editor::reflect::enums::kNavGridMovement, grid.movement) << ";\n";
    code << ind << "navGrid.reservationLookahead = " << formatFloat(grid.reservationLookahead) << ";\n";
    code << ind << "navGrid.reservationPenalty = " << formatFloat(grid.reservationPenalty) << ";\n";
    code << ind << "navGrid.source = " << enumCode(editor::reflect::enums::kNavGridSource, grid.source) << ";\n";
    code << ind << "navGrid.sourceEntity = " << formatEntity(grid.sourceEntity, entityVarNames) << ";\n";
    for (const NavGridTileArea& row : grid.tileAreas)
        code << ind << "navGrid.tileAreas.push_back({" << formatString(row.rect) << ", " << row.area << "});\n";
    code << ind << "navGrid.emptyCellArea = " << grid.emptyCellArea << ";\n";
    code << ind << "navGrid.collidersBlock = " << formatBool(grid.collidersBlock) << ";\n";
    code << ind << "navGrid.autoRebuild = " << formatBool(grid.autoRebuild) << ";\n";
    code << ind << "navGrid.gridData = " << formatString(grid.gridData.path) << ";\n";
    if (!grid.enabled) code << ind << "navGrid.enabled = false;\n";
    addComponentCode(code, ind, sceneName, entityName, entity, "NavGridComponent", "navGrid", assignExisting);
    return code.str();
}
