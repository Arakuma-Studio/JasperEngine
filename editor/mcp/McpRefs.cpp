// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "McpRefs.h"

#include "util/EntityPath.h"

#include <algorithm>
#include <cctype>
#include <map>

namespace fs = std::filesystem;

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::mcp;

namespace {

bool isDigits(const std::string& text) {
    return !text.empty() && text.size() < 11 &&
           std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c); });
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool hasEntity(SceneProject* scene, Entity entity) {
    return std::find(scene->entities.begin(), scene->entities.end(), entity) != scene->entities.end();
}

std::string sceneListText(Project* project) {
    std::string text;
    for (SceneProject* scene : project->getScenesAndPrefabStages()) {
        if (!text.empty()) text += ", ";
        text += std::to_string(scene->id) + " \"" + scene->name + "\"";
    }
    return text.empty() ? "(none)" : text;
}

} // namespace

std::string McpRefs::sceneTypeName(SceneType type) {
    switch (type) {
        case SceneType::SCENE_3D: return "3D";
        case SceneType::SCENE_2D: return "2D";
        case SceneType::SCENE_UI: return "UI";
    }
    return "unknown";
}

std::string McpRefs::playStateName(ScenePlayState state) {
    switch (state) {
        case ScenePlayState::STOPPED: return "stopped";
        case ScenePlayState::SAVING: return "saving";
        case ScenePlayState::LOADING: return "loading";
        case ScenePlayState::PLAYING: return "playing";
        case ScenePlayState::PAUSED: return "paused";
        case ScenePlayState::CANCELLING: return "cancelling";
    }
    return "unknown";
}

SceneProject* McpRefs::resolveScene(Project* project, const Json& ref, std::string& error) {
    if (!project) {
        error = "No project is open.";
        return nullptr;
    }
    if (ref.is_null()) {
        SceneProject* selected = project->getSelectedScene();
        if (!selected) error = "No scene is selected. Pass scene (id or name). Scenes: " + sceneListText(project) + ".";
        return selected;
    }

    std::string text;
    if (ref.is_number_integer()) {
        text = std::to_string(ref.get<long long>());
    } else if (ref.is_string()) {
        text = ref.get<std::string>();
    } else {
        error = "scene must be a scene id (integer), a scene name or a .scene path.";
        return nullptr;
    }

    if (isDigits(text)) {
        const uint32_t id = static_cast<uint32_t>(std::stoul(text));
        for (SceneProject* scene : project->getScenesAndPrefabStages()) {
            if (scene->id == id) return scene;
        }
        error = "No scene with id " + text + ". Scenes: " + sceneListText(project) + ".";
        return nullptr;
    }

    std::vector<SceneProject*> matches;
    for (SceneProject* scene : project->getScenesAndPrefabStages()) {
        if (scene->name == text) matches.push_back(scene);
    }
    if (matches.empty()) {
        // A .scene path, project-relative (or as stored)
        const fs::path wanted = fs::path(text).lexically_normal();
        for (SceneProject* scene : project->getScenesAndPrefabStages()) {
            if (scene->filepath.empty()) continue;
            const fs::path stored = scene->filepath.lexically_normal();
            const fs::path relative = project->normalizeToProjectRelative(scene->filepath).lexically_normal();
            if (stored == wanted || relative == wanted) matches.push_back(scene);
        }
    }
    if (matches.size() == 1) return matches[0];
    if (matches.empty()) {
        error = "No scene named or at \"" + text + "\". Scenes: " + sceneListText(project) + ".";
    } else {
        error = "Several scenes are named \"" + text + "\"; pass the scene id instead:";
        for (SceneProject* scene : matches) error += " " + std::to_string(scene->id);
        error += ".";
    }
    return nullptr;
}

std::string McpRefs::entityPath(SceneProject* scene, Entity entity) {
    if (!scene || !scene->scene) return {};
    return EntityPath::of(*scene, entity);
}

Json McpRefs::entityJson(SceneProject* scene, Entity entity) {
    Json out = {{"id", entity}};
    if (scene && scene->scene) {
        out["name"] = scene->scene->getEntityName(entity);
        out["path"] = entityPath(scene, entity);
    }
    return out;
}

Entity McpRefs::resolveEntity(SceneProject* scene, const Json& ref, std::string& error) {
    if (!scene || !scene->scene) {
        error = "The scene is not loaded.";
        return NULL_ENTITY;
    }
    const std::string sceneLabel = "scene " + std::to_string(scene->id) + " \"" + scene->name + "\"";

    std::string text;
    if (ref.is_number_integer()) {
        text = std::to_string(ref.get<long long>());
    } else if (ref.is_string()) {
        text = ref.get<std::string>();
    } else {
        error = "entity must be a GameObject id (integer) or a name path like \"Player/Gun\".";
        return NULL_ENTITY;
    }

    if (isDigits(text)) {
        const Entity entity = static_cast<Entity>(std::stoul(text));
        if (hasEntity(scene, entity)) return entity;
        // A GameObject literally named with digits
        if (!ref.is_string()) {
            error = "No GameObject with id " + text + " in " + sceneLabel + ". List them with list_scene_entities.";
            return NULL_ENTITY;
        }
    }

    std::string path = text;
    while (!path.empty() && path.front() == '/') path.erase(0, 1);
    while (!path.empty() && path.back() == '/') path.pop_back();
    if (path.empty()) {
        error = "entity is empty.";
        return NULL_ENTITY;
    }

    const std::vector<Entity> matches = EntityPath::resolve(*scene, path);
    if (matches.size() == 1) return matches[0];
    if (matches.size() > 1) {
        EntityPath::Cache cache;
        error = "\"" + path + "\" matches " + std::to_string(matches.size()) + " GameObjects in " + sceneLabel +
                "; pass an id or a full path:";
        for (Entity entity : matches) {
            error += " " + std::to_string(entity) + " (" + EntityPath::of(*scene, entity, &cache) + ")";
        }
        error += ".";
        return NULL_ENTITY;
    }

    error = "No GameObject \"" + text + "\" in " + sceneLabel +
            ". Paths are '/'-joined names from the scene root; list them with list_scene_entities.";
    return NULL_ENTITY;
}

Json McpRefs::sceneJson(Project* project, const SceneProject& scene) {
    Json out = {
        {"id", scene.id},
        {"name", scene.name},
        {"type", sceneTypeName(scene.sceneType)},
        {"opened", scene.opened},
        {"modified", scene.isModified},
        {"play_state", playStateName(scene.playState)}
    };
    out["path"] = scene.filepath.empty() || !project
        ? std::string()
        : project->normalizeToProjectRelative(scene.filepath).generic_string();
    return out;
}

std::vector<std::string> McpRefs::componentTypeNames() {
    std::vector<std::string> names;
    const int last = static_cast<int>(ComponentType::ReflectionProbeComponent);
    for (int i = 0; i <= last; i++) {
        std::string name = lower(Catalog::getComponentName(static_cast<ComponentType>(i), true));
        if (name.empty()) continue;
        // only names Catalog::getComponentType reads back
        try {
            if (Catalog::getComponentType(name) == static_cast<ComponentType>(i)) names.push_back(name);
        } catch (const std::exception&) {
        }
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

bool McpRefs::resolveComponent(const Json& ref, ComponentType& out, std::string& error) {
    if (!ref.is_string() || ref.get<std::string>().empty()) {
        error = "component must be a component type name.";
    } else {
        try {
            out = Catalog::getComponentType(ref.get<std::string>());
            return true;
        } catch (const std::exception&) {
            error = "Unknown component type \"" + ref.get<std::string>() + "\".";
        }
    }
    error += " Valid names:";
    for (const std::string& name : componentTypeNames()) error += " " + name;
    error += ".";
    return false;
}

bool McpRefs::resolveProjectPath(Project* project, const std::string& relative, fs::path& absolute,
                                 std::string& error, bool mustExist) {
    if (!project) {
        error = "No project is open.";
        return false;
    }
    const fs::path rel = fs::path(relative).lexically_normal();
    if (relative.empty() || rel.is_absolute() || rel.has_root_name() || rel.has_root_directory()) {
        error = "\"" + relative + "\" must be a project-relative path (not absolute).";
        return false;
    }
    for (const fs::path& part : rel) {
        if (part == "..") {
            error = "\"" + relative + "\" must stay inside the project (no '..').";
            return false;
        }
    }
    absolute = project->getProjectPath() / rel;
    if (mustExist && !fs::exists(absolute)) {
        error = "\"" + relative + "\" does not exist in the project.";
        return false;
    }
    return true;
}
