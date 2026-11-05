// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "McpRefs.h"
#include "component/Transform.h"

#include "util/SafeWrite.h"

#include "util/EntityPath.h"
#include "util/FileUtils.h"

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

bool McpRefs::parseSceneType(const std::string& name, SceneType& out) {
    std::string key;
    for (char c : lower(name)) {
        if (c != ' ' && c != '_' && c != '-') key.push_back(c);
    }
    if (key == "3d" || key == "3dscene" || key == "scene3d") out = SceneType::SCENE_3D;
    else if (key == "2d" || key == "2dscene" || key == "scene2d") out = SceneType::SCENE_2D;
    else if (key == "ui" || key == "uiscene" || key == "sceneui") out = SceneType::SCENE_UI;
    else return false;
    return true;
}

SceneProject* McpRefs::runtimeScene(Project* project, SceneProject* scene) {
    if (!project || !scene) return scene;
    const uint32_t runtimeId = project->getPlayRuntimeSceneId(scene->id);
    if (runtimeId != NULL_PROJECT_SCENE && runtimeId != scene->id) {
        if (SceneProject* runtime = project->getScene(runtimeId)) return runtime;
    }
    return scene;
}

std::string McpRefs::componentName(ComponentType type) {
    return lower(Catalog::getComponentName(type, true));
}

bool McpRefs::hasComponent(SceneProject* scene, Entity entity, ComponentType type) {
    if (!scene || !scene->scene || !scene->scene->isEntityCreated(entity)) return false;
    return scene->scene->getSignature(entity).test(Catalog::getComponentId(scene->scene, type));
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
    if (ref.is_object()) {
        // what sceneJson returns: the id wins
        for (const char* key : {"id", "path", "name"}) {
            auto it = ref.find(key);
            if (it != ref.end() && !it->is_null() && !(it->is_string() && it->get<std::string>().empty())) {
                return resolveScene(project, *it, error);
            }
        }
        error = "scene must be a scene id (integer), a scene name, a .scene path, or {\"id\": n}.";
        return nullptr;
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
        // a Play copy of a child scene answers to an id of its own while the game runs
        if (project->isPlayRuntimeScene(id)) {
            if (SceneProject* runtime = project->getScene(id)) return runtime;
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
        const fs::path wanted = FileUtils::pathFromUtf8(text).lexically_normal();
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

std::unordered_map<Entity, std::string> McpRefs::entityPaths(SceneProject* scene) {
    std::unordered_map<Entity, std::string> paths;
    if (!scene || !scene->scene) return paths;
    Scene* registry = scene->scene;
    // Transform order is depth-first with parents first: one pass, counting same-named
    // siblings as EntityPath does
    auto transforms = registry->getComponentArray<Transform>();
    std::map<std::pair<Entity, std::string>, int> seen;
    EntityPath::Cache cache;
    const size_t count = transforms ? transforms->size() : 0;
    for (size_t i = 0; i < count; i++) {
        const Entity entity = transforms->getEntity(i);
        const Entity parent = transforms->getComponentFromIndex(i).parent;
        const std::string name = registry->getEntityName(entity);
        const int index = seen[{parent, name}]++;
        std::string prefix;
        if (parent != NULL_ENTITY) {
            auto it = paths.find(parent);
            prefix = (it != paths.end() ? it->second : EntityPath::of(registry, parent, &cache)) + "/";
        }
        paths[entity] = prefix + name + (index ? "#" + std::to_string(index) : std::string());
    }
    for (Entity entity : scene->entities) {
        if (paths.find(entity) == paths.end() && registry->isEntityCreated(entity)) {
            paths[entity] = registry->getEntityName(entity);
        }
    }
    return paths;
}

Entity McpRefs::resolveEntity(SceneProject* scene, const Json& ref, std::string& error) {
    if (!scene || !scene->scene) {
        error = "The scene is not loaded (open it with open_scene).";
        return NULL_ENTITY;
    }
    const std::string sceneLabel = "scene " + std::to_string(scene->id) + " \"" + scene->name + "\"";

    if (ref.is_object()) {
        // what entityJson returns ({id, name, path}): the id wins, then the path, then the name
        auto id = ref.find("id");
        if (id != ref.end() && id->is_number_integer()) return resolveEntity(scene, *id, error);
        auto path = ref.find("path");
        if (path != ref.end() && path->is_string() && !path->get<std::string>().empty()) {
            std::string text = path->get<std::string>();
            while (!text.empty() && text.front() == '/') text.erase(0, 1);
            const Entity exact = EntityPath::find(*scene, text);
            if (exact != NULL_ENTITY) return exact;
            error = "No GameObject at path \"" + text + "\" in " + sceneLabel +
                    ". Paths are '/'-joined names from the scene root; see get_scene_tree or find_entities.";
            return NULL_ENTITY;
        }
        auto name = ref.find("name");
        if (name != ref.end() && name->is_string() && !name->get<std::string>().empty()) {
            const std::string text = name->get<std::string>();
            const std::vector<Entity> matches = EntityPath::findByName(scene->scene, scene->entities, text);
            if (matches.size() == 1) return matches[0];
            if (matches.empty()) {
                error = "No GameObject named \"" + text + "\" in " + sceneLabel + ". Use find_entities with a name pattern.";
            } else {
                const auto paths = entityPaths(scene);
                error = "\"" + text + "\" names " + std::to_string(matches.size()) + " GameObjects in " + sceneLabel +
                        "; pass an id or a path:";
                for (Entity entity : matches) error += " " + std::to_string(entity) + " (" + paths.at(entity) + ")";
                error += ".";
            }
            return NULL_ENTITY;
        }
        error = "entity object must have \"id\", \"path\" or \"name\".";
        return NULL_ENTITY;
    }

    std::string text;
    if (ref.is_number_integer()) {
        text = std::to_string(ref.get<long long>());
    } else if (ref.is_string()) {
        text = ref.get<std::string>();
    } else {
        error = "entity must be a GameObject id (integer), a name path like \"Player/Gun\", or {\"name\": \"Gun\"}.";
        return NULL_ENTITY;
    }

    if (isDigits(text)) {
        const Entity entity = static_cast<Entity>(std::stoul(text));
        if (hasEntity(scene, entity)) return entity;
        // made by the running game (Play mode): not authored, but addressable
        if (scene->playState != ScenePlayState::STOPPED && entity != NULL_ENTITY && scene->scene->isEntityCreated(entity)) {
            return entity;
        }
        // A GameObject literally named with digits
        if (!ref.is_string()) {
            error = "No GameObject with id " + text + " in " + sceneLabel + ". See get_scene_tree or find_entities.";
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
        const auto paths = entityPaths(scene);
        error = "\"" + path + "\" matches " + std::to_string(matches.size()) + " GameObjects in " + sceneLabel +
                "; pass an id or a full path:";
        for (Entity entity : matches) {
            error += " " + std::to_string(entity) + " (" + paths.at(entity) + ")";
        }
        error += ".";
        return NULL_ENTITY;
    }

    error = "No GameObject \"" + text + "\" in " + sceneLabel +
            ". Paths are '/'-joined names from the scene root; see get_scene_tree or find_entities.";
    return NULL_ENTITY;
}

bool McpRefs::resolveEntities(SceneProject* scene, const Json& refs, std::vector<Entity>& out, std::string& error) {
    out.clear();
    if (refs.is_array()) {
        if (refs.empty()) {
            error = "entities is empty.";
            return false;
        }
        for (const Json& ref : refs) {
            const Entity entity = resolveEntity(scene, ref, error);
            if (entity == NULL_ENTITY) return false;
            if (std::find(out.begin(), out.end(), entity) == out.end()) out.push_back(entity);
        }
        return true;
    }
    const Entity entity = resolveEntity(scene, refs, error);
    if (entity == NULL_ENTITY) return false;
    out.push_back(entity);
    return true;
}

bool McpRefs::resolveEntityArgs(SceneProject* scene, const Json& args, std::vector<Entity>& out, std::string& error) {
    auto many = args.find("entities");
    if (many != args.end() && !many->is_null()) return resolveEntities(scene, *many, out, error);
    auto one = args.find("entity");
    if (one != args.end() && !one->is_null()) return resolveEntities(scene, *one, out, error);
    error = "Pass entity (one GameObject) or entities (an array of them).";
    return false;
}

std::string McpRefs::notOpenError(Project* project, const SceneProject& scene) {
    const std::string path = scene.filepath.empty() || !project
        ? std::string()
        : FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(scene.filepath));
    const std::string ref = path.empty() ? std::to_string(scene.id) : "\"" + path + "\"";
    return "Scene " + std::to_string(scene.id) + " \"" + scene.name + "\" is not open (its contents are not loaded). Open it "
           "first: open_scene {\"scene\": " + ref + ", \"close_previous\": false} (then select_scene to switch back).";
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
        : FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(scene.filepath));
    return out;
}

std::vector<std::string> McpRefs::componentTypeNames() {
    std::vector<std::string> names;
    for (int i = 0; i < ComponentTypeCount; i++) {
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
        const std::string given = ref.get<std::string>();
        // "Audio Source", "audio_source", "AudioSourceComponent", "Sprite Renderer"...
        std::string key;
        for (char c : lower(given)) {
            if (c != ' ' && c != '_' && c != '-') key.push_back(c);
        }
        static const std::map<std::string, std::string> aliases = {
            {"spriterenderer", "sprite"}, {"meshrenderer", "mesh"}, {"meshfilter", "mesh"},
            {"rigidbody3d", "rigidbody"}, {"collider", "rigidbody"}, {"collider2d", "rigidbody2d"},
            {"canvasrenderer", "ui"},
            {"sky", "skybox"}, {"audio", "audiosource"}, {"sound", "audiosource"}, {"scripts", "script"},
            {"lua", "script"}, {"particles", "particlesystem"}, {"lines", "linerenderer"}, {"line", "linerenderer"},
            {"fluid", "fluidsimulation"}, {"video", "videoplayer"}
        };
        auto alias = aliases.find(key);
        for (const std::string& candidate : {given, key, alias != aliases.end() ? alias->second : key}) {
            try {
                out = Catalog::getComponentType(candidate);
                return true;
            } catch (const std::exception&) {
            }
        }
        // Unity's display names ("Rigidbody 2D", "Audio Source")
        for (int i = 0; i < ComponentTypeCount; i++) {
            std::string display;
            for (char c : lower(Catalog::getComponentDisplayName(static_cast<ComponentType>(i)))) {
                if (c != ' ' && c != '_' && c != '-') display.push_back(c);
            }
            if (!display.empty() && display == key) {
                out = static_cast<ComponentType>(i);
                return true;
            }
        }
        error = "Unknown component type \"" + given + "\".";
    }
    error += " Valid names:";
    for (const std::string& name : componentTypeNames()) error += " " + name;
    error += ".";
    return false;
}

bool McpRefs::resolveProjectPath(Project* project, const std::string& requested, fs::path& absolute,
                                 std::string& error, bool mustExist) {
    if (!project) {
        error = "No project is open.";
        return false;
    }
    // an asset's GUID names it wherever a path does (S04): its current path, or a sub-asset's model
    std::string relative = requested;
    if (Guid::isValidText(requested)) {
        const AssetDatabase::Resolution found = project->getAssetDatabase().resolve(Guid::parse(requested), std::string(), AssetType::Unknown, false);
        if (!found.found()) {
            error = "No asset has the GUID " + requested + ".";
            return false;
        }
        std::string container, key;
        relative = splitSubAssetPath(found.path, container, key) ? container : found.path;
    }
    // Agents send UTF-8; a backslash is a separator on every platform, as in project files
    // (Stream::portablePath): textures\wood.png names the same file on Linux as on Windows
    relative = FileUtils::portablePath(relative);
    const fs::path rel = FileUtils::pathFromUtf8(relative).lexically_normal();
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

std::vector<fs::path> McpRefs::trustedRoots(Project* project) {
    if (!project || project->getProjectPath().empty()) return {};
    // the project directory, its configured assets directory and its configured Lua scripts
    // directory, as resolved when the project loaded (or a root setting was applied) and pinned:
    // never resolved again, so a root can't be re-pointed by moving folders around; one that is
    // no longer the folder it was is left out
    std::vector<fs::path> roots;
    for (const fs::path& root : project->getTrustedRoots()) {
        if (safewrite::trustedRootIntact(root)) roots.push_back(root);
    }
    return roots;
}

fs::path McpRefs::rootFor(Project* project, const fs::path& absolute, WriteUse use) {
    return safewrite::trustedRootFor(trustedRoots(project), absolute, use == WriteUse::Entry);
}

namespace {
    // `absolute` inside one of `roots` once links are resolved ("" ok, else the error)
    std::string escapeAgainst(const std::vector<fs::path>& roots, const fs::path& root, const fs::path& absolute, McpRefs::WriteUse use) {
        const fs::path normal = absolute.lexically_normal();
        const fs::path rel = normal.lexically_relative(root.lexically_normal());
        const std::string shown = rel.empty() || *rel.begin() == ".." ? FileUtils::pathToGenericUtf8(normal) : FileUtils::pathToGenericUtf8(rel);
        // a write follows a link at the name (a dangling one too: a plain write creates its target)
        const fs::path where = use == McpRefs::WriteUse::Entry ? safewrite::entryLocation(normal) : safewrite::resolvedPath(normal);
        if (where.empty()) {
            return "\"" + shown + "\" can't be resolved: a symbolic link on its way loops or can't be read. Remove the loop, then try again.";
        }
        for (const fs::path& trusted : roots) {
            const fs::path real = safewrite::resolvedPath(trusted);
            if (!real.empty() && safewrite::isAtOrBelow(where, real)) return std::string();
        }
        return "\"" + shown + "\" resolves outside the project: a symbolic link on its way leads to " + FileUtils::pathToGenericUtf8(where) +
               ". Tools write, create, delete and move files only inside the project and its assets directory (reads may follow links): "
               "use a path that is not behind a link to elsewhere, or replace the link with a real folder.";
    }
}

namespace {
    bool hiddenComponent(const fs::path& rel) {
        for (const fs::path& part : rel) {
            const std::string name = safewrite::comparableName(part);
            if (!name.empty() && name[0] == '.' && name != "." && name != "..") return true;
        }
        return false;
    }
}

std::string McpRefs::writeEscape(Project* project, const fs::path& absolute, WriteUse use) {
    if (!project) return "No project is open.";
    // the trusted roots: the project directory and its configured assets directory (each resolved
    // once: an assets directory linked in from a shared drive works); links below them are checked
    if (std::string escape = escapeAgainst(trustedRoots(project), project->getProjectPath(), absolute, use); !escape.empty()) return escape;
    // a path not under a hidden folder that a link inside the project leads into one (the project's
    // .jasperengine, .git ...): no way in (a path under one as named is the tool's own rules)
    const fs::path normal = absolute.lexically_normal();
    const fs::path rel = normal.lexically_relative(project->getProjectPath().lexically_normal());
    if (!rel.empty() && *rel.begin() != ".." && hiddenComponent(rel)) return std::string();
    const fs::path where = use == WriteUse::Entry ? safewrite::entryLocation(normal) : safewrite::resolvedPath(normal);
    const fs::path projectReal = safewrite::resolvedPath(project->getProjectPath());
    if (where.empty() || projectReal.empty() || !safewrite::isAtOrBelow(where, projectReal)) return std::string();
    const fs::path inside = where.lexically_relative(projectReal);
    if (!hiddenComponent(inside)) return std::string();
    const std::string shown = rel.empty() || *rel.begin() == ".." ? FileUtils::pathToGenericUtf8(normal) : FileUtils::pathToGenericUtf8(rel);
    return "\"" + shown + "\" leads into the project's hidden folder " + FileUtils::pathToGenericUtf8(inside) +
           " through a symbolic link: tools don't write, create, delete or move files there (.jasperengine is the editor's). "
           "Use a path that is not behind that link.";
}

std::string McpRefs::projectOnlyEscape(Project* project, const fs::path& absolute) {
    if (!project) return "No project is open.";
    // a new trusted root (an asset root setting): inside the project directory itself, never via
    // the current assets directory's link
    const std::string escape = escapeAgainst({project->getProjectPath()}, project->getProjectPath(), absolute, WriteUse::Write);
    if (escape.empty()) return escape;
    return escape + " An asset root must be a folder of the project itself.";
}

std::string McpRefs::rootValueRefusal(const std::string& text) {
    const fs::path value = FileUtils::pathFromUtf8(text);
    if (value.is_absolute() || value.has_root_name() || value.has_root_directory()) return "\"" + text + "\" is absolute";
    for (const fs::path& part : value) {
        if (part == "..") return "\"" + text + "\" has a '..' step";
        // an editor-owned or hidden folder (.jasperengine, .trash, .git ...) is never an asset root
        const std::string name = safewrite::comparableName(part);
        if (!name.empty() && name[0] == '.' && name != ".") return "\"" + text + "\" goes through the hidden folder " + FileUtils::pathToUtf8(part);
        if (safewrite::isProjectFileName(part) || name == "cmakelists.txt") return "\"" + text + "\" names a file the editor owns";
    }
    return std::string();
}

std::string McpRefs::trustedRootTouched(Project* project, const fs::path& absolute, bool destination) {
    if (!project) return std::string();
    const fs::path normal = absolute.lexically_normal();
    const fs::path entry = safewrite::entryLocation(normal);
    const fs::path projectNormal = project->getProjectPath().lexically_normal();
    struct Root {
        fs::path configured;
        const char* what;
    };
    const std::vector<Root> roots = {{project->getAssetsPath(), "assets folder"}, {project->getLuaPath(), "Lua scripts folder"}};
    for (const Root& root : roots) {
        if (root.configured.empty() || root.configured.lexically_normal() == projectNormal) continue;
        const fs::path resolved = safewrite::resolvedPath(root.configured);
        const bool touches = safewrite::isAtOrBelow(root.configured, normal) ||
                             (!entry.empty() && !resolved.empty() && safewrite::isAtOrBelow(resolved, entry));
        if (!touches) continue;
        const std::string rel = FileUtils::pathToGenericUtf8(normal.lexically_relative(projectNormal));
        return "\"" + rel + "\" " + (destination ? "would take the place of " : "is or holds ") + "the project's " + root.what +
               " (a trusted root): it can't be " + (destination ? "replaced" : "moved, renamed or deleted") +
               " by a tool. Change the root in Project Settings first, or pick another path.";
    }
    return std::string();
}

std::string McpRefs::internalFoldersEscape(Project* project) {
    if (!project) return "No project is open.";
    const fs::path internal = project->getProjectInternalPath();
    // every link inside the internal folder (any depth, bounded; links not followed): one out of
    // the project there would let a build, Play or an export write or delete through it
    if (const std::string escape = linkOutInside(project, internal); !escape.empty()) return escape;
    for (const fs::path& dir : {internal, internal / "build", internal / "build" / "hotreload", internal / "engine-api",
                                internal / "engine-api" / "lua" / "scripts", internal / "generated", internal / "logs",
                                internal / "externalbuild", project->getImportPath(), project->getThumbsDir(),
                                project->getTerrainMapsDir(), project->getProjectPath() / ".vscode"}) {
        if (const std::string escape = writeEscape(project, dir); !escape.empty()) return escape;
    }
    return std::string();
}

std::string McpRefs::linkOutInside(Project* project, const fs::path& dir, size_t maxEntries) {
    if (!project) return "No project is open.";
    std::error_code ec;
    if (!fs::is_directory(fs::symlink_status(dir, ec))) return std::string();
    size_t entries = 0;
    // directory_options::none: a link to a folder is listed, never entered (no loop, no escape walk)
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::none, ec), end; !ec && it != end; it.increment(ec)) {
        if (++entries > maxEntries) {
            return "\"" + FileUtils::pathToGenericUtf8(dir) + "\" holds more than " + std::to_string(maxEntries) +
                   " entries: too many to check for links leading out of the project. Remove what isn't needed there, then try again.";
        }
        std::error_code statusError;
        const fs::file_status status = it->symlink_status(statusError);
        bool link = fs::is_symlink(status);
#ifdef _MSC_VER
        link = link || status.type() == fs::file_type::junction;
#endif
        if (!link) continue;
        if (const std::string escape = writeEscape(project, it->path()); !escape.empty()) return escape;
    }
    return std::string();
}

namespace {
    // why `rel` (one spelling of a path, relative to a trusted root; `atProjectRoot`: relative to
    // the project directory) is a protected place, "" when it isn't
    std::string protectedSpelling(const fs::path& rel, bool atProjectRoot) {
        if (rel.empty() || *rel.begin() == "..") return std::string();
        size_t parts = 0;
        for (const fs::path& part : rel) {
            const std::string name = safewrite::comparableName(part);
            if (name == ".") continue;
            parts++;
            if (!name.empty() && name[0] == '.') {
                return "it is under a hidden folder (" + FileUtils::pathToUtf8(part) +
                       "; .jasperengine is generated by the editor): tools don't write, move or create files there";
            }
        }
        if (safewrite::isProjectFileName(rel)) {
            return "it is a file named project.yaml: tools don't write, move, copy or create one in any folder (the editor "
                   "writes the project's own: use set_project_settings; another one would be a project to open)";
        }
        if (atProjectRoot && parts == 1 && safewrite::comparableName(rel) == "cmakelists.txt") {
            return "CMakeLists.txt is regenerated on every build; put custom build settings in ProjectBuild.cmake";
        }
        return std::string();
    }
}

std::string McpRefs::protectedPlace(Project* project, const fs::path& absolute, WriteUse use) {
    if (!project) return "No project is open.";
    const fs::path normal = absolute.lexically_normal();
    const fs::path projectNormal = project->getProjectPath().lexically_normal();
    // as named
    if (std::string why = protectedSpelling(normal.lexically_relative(projectNormal), true); !why.empty()) return why;
    if (safewrite::isProjectFileName(normal)) return protectedSpelling(fs::path("project.yaml"), false);
    // where it resolves, against each trusted root holding it
    const fs::path where = use == WriteUse::Entry ? safewrite::entryLocation(normal) : safewrite::resolvedPath(normal);
    if (where.empty()) return std::string();   // a loop: writeEscape refuses it
    if (safewrite::isProjectFileName(where)) return protectedSpelling(fs::path("project.yaml"), false);
    const std::vector<fs::path> roots = trustedRoots(project);
    const fs::path projectReal = safewrite::resolvedPath(project->getProjectPath());
    for (const fs::path& root : roots) {
        if (!safewrite::isAtOrBelow(where, root)) continue;
        const bool projectRoot = !projectReal.empty() && safewrite::isAtOrBelow(root, projectReal) && safewrite::isAtOrBelow(projectReal, root);
        if (std::string why = protectedSpelling(where.lexically_relative(root), projectRoot); !why.empty()) {
            return why + " (\"" + FileUtils::pathToGenericUtf8(normal.lexically_relative(projectNormal)) + "\" leads there through a link)";
        }
    }
    return std::string();
}

bool McpRefs::resolveProjectWritePath(Project* project, const std::string& relative, fs::path& absolute, std::string& error,
                                      bool mustExist, WriteUse use) {
    if (!resolveProjectPath(project, relative, absolute, error, mustExist)) return false;
    const std::string escape = writeEscape(project, absolute, use);
    if (escape.empty()) return true;
    error = escape;
    return false;
}
