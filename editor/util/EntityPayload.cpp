// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "EntityPayload.h"

#include "Project.h"

#include <algorithm>

using namespace jasperengine;

uint32_t editor::payloadSceneId(const EntityPayload& payload, uint32_t fallbackSceneId) {
    return payload.entitySceneId != 0 ? payload.entitySceneId : fallbackSceneId;
}

std::string editor::sameSceneDropProblem(const Project& project, const EntityPayload& payload, uint32_t targetSceneId, bool moves) {
    const uint32_t sourceSceneId = payloadSceneId(payload, project.getSelectedSceneId());
    if (sourceSceneId == targetSceneId && !(moves && payload.referenceOnly)) {
        return {};
    }
    const SceneProject* source = project.getScene(sourceSceneId);
    const std::string name = source ? source->name : std::to_string(sourceSceneId);
    const std::vector<uint32_t> children = project.getChildScenes(targetSceneId);
    const bool child = std::find(children.begin(), children.end(), sourceSceneId) != children.end();
    return "GameObject belongs to " + std::string(child ? "child scene" : "scene") + " \"" + name + "\"";
}

std::string editor::referenceDropProblem(const Project& project, const EntityPayload& payload, uint32_t ownerSceneId) {
    const uint32_t sourceSceneId = payloadSceneId(payload, project.getSelectedSceneId());
    const SceneProject* source = project.getScene(sourceSceneId);
    if (!source || !source->scene || !source->scene->isEntityCreated(payload.entity)) {
        return "Invalid GameObject";
    }
    if (sourceSceneId == ownerSceneId) {
        return {};
    }
    const std::vector<uint32_t> related = project.getRelatedScenes(ownerSceneId);
    if (std::find(related.begin(), related.end(), sourceSceneId) == related.end()) {
        return "\"" + source->name + "\" is not loaded with this scene (it is not a parent, child or sibling scene of it)";
    }
    return {};
}
