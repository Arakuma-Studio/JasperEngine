// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "vcs/scene/SceneDiffJson.h"

namespace jasperengine::editor::vcs::scene {

namespace {

    using Json = mcp::Json;

    Json idOf(uint64_t id) {
        return id == kSceneSettingsId ? Json("settings") : Json(id);
    }

    const char* marker(ChangeState state) {
        switch (state) {
            case ChangeState::Added: return "+";
            case ChangeState::Removed: return "-";
            case ChangeState::Modified: return "~";
            case ChangeState::Unchanged: return " ";
        }
        return " ";
    }

}

mcp::Json sceneDiffJson(const SceneDiffResult& diff) {
    Json objects = Json::array();
    for (const ObjectDiff& object : diff.objects) {
        Json item = {{"id", idOf(object.id)}, {"name", object.name}, {"state", changeStateName(object.state)}};
        if (object.settings) item["settings"] = true;
        if (object.reparented) {
            item["reparented"] = true;
            item["base_parent"] = object.baseParent;
            item["parent"] = object.currentParent;
        }
        if (object.reordered) {
            item["reordered"] = true;
            item["base_index"] = object.baseIndex;
            item["index"] = object.currentIndex;
        }
        if (object.prefabMember) {
            item["prefab_member"] = true;
            item["instance_root"] = object.instanceRoot;
            item["registry_entity"] = object.registryEntity;
        }
        Json components = Json::array();
        for (const ComponentDiff& component : object.components) {
            Json properties = Json::array();
            for (const PropertyDiff& property : component.properties) {
                Json entry = {{"path", propertyPath(component, property)}};
                if (property.inBase) entry["base"] = property.baseValue;
                if (property.inCurrent) entry["current"] = property.currentValue;
                properties.push_back(entry);
            }
            components.push_back({{"key", component.key}, {"state", changeStateName(component.state)}, {"properties", properties}});
        }
        item["components"] = components;
        objects.push_back(item);
    }
    return {{"added", diff.added}, {"removed", diff.removed}, {"modified", diff.modified}, {"objects", objects}};
}

std::string sceneDiffText(const SceneDiffResult& diff) {
    if (diff.empty()) return "No changes.\n";
    std::string out;
    for (const ObjectDiff& object : diff.objects) {
        out += std::string(marker(object.state == ChangeState::Unchanged ? ChangeState::Modified : object.state)) + " " + object.name;
        if (!object.settings) out += " (" + std::to_string(object.id) + ")";
        if (object.prefabMember) out += " [prefab instance member of " + std::to_string(object.instanceRoot) + "]";
        if (object.reparented) out += " [reparented " + std::to_string(object.baseParent) + " -> " + std::to_string(object.currentParent) + "]";
        if (object.reordered) out += " [reordered " + std::to_string(object.baseIndex) + " -> " + std::to_string(object.currentIndex) + "]";
        out += "\n";
        for (const ComponentDiff& component : object.components) {
            if (component.state == ChangeState::Added || component.state == ChangeState::Removed) {
                out += std::string("    ") + marker(component.state) + " " + component.key +
                       (component.state == ChangeState::Added ? " (component added)\n" : " (component removed)\n");
                continue;
            }
            for (const PropertyDiff& property : component.properties) {
                out += "    " + propertyPath(component, property) + ": " + (property.inBase ? property.baseValue : std::string("(none)")) +
                       " -> " + (property.inCurrent ? property.currentValue : std::string("(none)")) + "\n";
            }
        }
    }
    out += std::to_string(diff.added) + " added, " + std::to_string(diff.removed) + " removed, " + std::to_string(diff.modified) + " modified\n";
    return out;
}

}
