// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// W2: the Unity-style prefab workflow (editor/project/PREFABS.md): create prefabs and
// variants, instantiate, replace, unpack, add to / remove from a prefab, inspect instances
// and assets (levels, nesting, usage, overrides with their apply targets), apply / revert
// overrides at any level, Prefab Mode (prefab stages), the standalone prefab list.
// Every edit runs through the editor's commands (one undo step per call).
#include "McpTools.h"
#include "component/Transform.h"

#include "mcp/McpDataSafety.h"
#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpValues.h"

#include "Catalog.h"
#include "Out.h"
#include "Project.h"
#include "Stream.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/AddEntityToPrefabCmd.h"
#include "command/type/CreatePrefabCmd.h"
#include "command/type/CreatePrefabVariantAssetCmd.h"
#include "command/type/DeleteEntityCmd.h"
#include "command/type/EntityNameCmd.h"
#include "command/type/ImportPrefabCmd.h"
#include "command/type/PrefabOverrideCmd.h"
#include "command/type/PrefabOverridesCmd.h"
#include "command/type/RemoveEntityFromPrefabCmd.h"
#include "command/type/ReplacePrefabCmd.h"
#include "command/type/UnpackPrefabCmd.h"
#include "pose/PoseEditSession.h"
#include "util/FileUtils.h"
#include "util/ProjectUtils.h"
#include "util/Util.h"
#include "window/ProjectWindow.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <set>
#include <unordered_set>

namespace fs = std::filesystem;

namespace jasperengine::editor::mcp {

namespace {

using Kind = PrefabOverrideEntry::Kind;
using Operation = PrefabOverrideCmd::Operation;

// ------------------------------------------------------------------ small helpers

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

// The component name McpRefs::resolveComponent reads back
std::string componentName(ComponentType type) {
    return lower(Catalog::getComponentName(type, true));
}

// Paths for the agent: UTF-8 (generic_string() is the ANSI code page on Windows)
std::string generic(const fs::path& path) {
    return FileUtils::pathToGenericUtf8(path);
}

const char* kindName(Kind kind) {
    switch (kind) {
        case Kind::Property: return "property";
        case Kind::State: return "state";
        case Kind::AddedComponent: return "added_component";
        case Kind::RemovedComponent: return "removed_component";
        case Kind::AddedGameObject: return "added_gameobject";
        case Kind::RemovedGameObject: return "removed_gameobject";
    }
    return "unknown";
}

bool parseKind(const std::string& name, Kind& out) {
    static const std::map<std::string, Kind> kinds = {
        {"property", Kind::Property}, {"state", Kind::State},
        {"added_component", Kind::AddedComponent}, {"removed_component", Kind::RemovedComponent},
        {"added_gameobject", Kind::AddedGameObject}, {"removed_gameobject", Kind::RemovedGameObject},
    };
    auto it = kinds.find(name);
    if (it == kinds.end()) return false;
    out = it->second;
    return true;
}

const std::vector<std::string> KIND_NAMES = {"property", "state", "added_component", "removed_component",
                                             "added_gameobject", "removed_gameobject"};
const std::vector<std::string> STATE_NAMES = {"name", "active", "tag", "layer"};

Json stateNames(uint8_t bits) {
    Json out = Json::array();
    if (bits & PrefabObjectOverrides::StateName) out.push_back("name");
    if (bits & PrefabObjectOverrides::StateActive) out.push_back("active");
    if (bits & PrefabObjectOverrides::StateTag) out.push_back("tag");
    if (bits & PrefabObjectOverrides::StateLayer) out.push_back("layer");
    return out;
}

bool parseStateBits(const Json& value, uint8_t& bits, std::string& error) {
    bits = 0;
    Json list = value.is_string() ? Json::array({value}) : value;
    if (!list.is_array()) {
        error = "state must be a list of: name, active, tag, layer.";
        return false;
    }
    for (const Json& item : list) {
        const std::string name = item.is_string() ? lower(item.get<std::string>()) : std::string();
        if (name == "name") bits |= PrefabObjectOverrides::StateName;
        else if (name == "active") bits |= PrefabObjectOverrides::StateActive;
        else if (name == "tag") bits |= PrefabObjectOverrides::StateTag;
        else if (name == "layer") bits |= PrefabObjectOverrides::StateLayer;
        else {
            error = "Unknown state \"" + (item.is_string() ? item.get<std::string>() : item.dump()) +
                    "\". Valid: name, active, tag, layer.";
            return false;
        }
    }
    return true;
}

// Errors and warnings Out logged since `since` (why a command failed), newest last
std::string problemsSince(uint64_t since, size_t max = 6) {
    std::string text;
    std::vector<LogEntry> entries = Out::getLogsSince(since, 0, true);
    if (entries.size() > max) entries.erase(entries.begin(), entries.end() - static_cast<long>(max));
    for (const LogEntry& entry : entries) {
        if (!text.empty()) text += " | ";
        text += entry.message;
    }
    return text;
}

Json problemsJson(uint64_t since, size_t max = 20) {
    Json out = Json::array();
    std::vector<LogEntry> entries = Out::getLogsSince(since, 0, true);
    if (entries.size() > max) entries.erase(entries.begin(), entries.end() - static_cast<long>(max));
    for (const LogEntry& entry : entries) {
        out.push_back({{"type", Out::logTypeName(entry.type)}, {"message", entry.message}});
    }
    return out;
}

// Runs a command on a history as one undo step; false when it did nothing (deleted)
bool runCommand(CommandHistory* history, Command* command) {
    command->setNoMerge();
    return history->addCommand(command);
}

void refreshProjectWindow(const CallPtr& call) {
    if (ProjectWindow* window = call->ctx().projectWindow()) window->refreshCurrentDirectory();
}

Project* requireProject(const CallPtr& call) {
    Project* project = call->ctx().project();
    if (!project) call->fail("No project is open.");
    return project;
}

SceneProject* sceneArg(const CallPtr& call, Project* project) {
    std::string error;
    const Json ref = call->args().contains("scene") ? call->args()["scene"] : Json();
    SceneProject* scene = McpRefs::resolveScene(project, ref, error);
    if (!scene) {
        call->fail(error);
        return nullptr;
    }
    if (!scene->scene) {
        call->fail(McpRefs::notOpenError(call->ctx().project(), *scene));
        return nullptr;
    }
    return scene;
}

Entity entityArg(const CallPtr& call, SceneProject* scene, const char* key, bool required = true) {
    const Json& args = call->args();
    if (!args.contains(key) || args[key].is_null()) {
        if (required) call->fail(std::string(key) + " is required (a GameObject id or a name path like \"Player/Gun\").");
        return NULL_ENTITY;
    }
    std::string error;
    Entity entity = McpRefs::resolveEntity(scene, args[key], error);
    if (entity == NULL_ENTITY) call->fail(std::string(key) + ": " + error);
    return entity;
}

// "entity" or "entities" (one of them required)
bool entitiesArg(const CallPtr& call, SceneProject* scene, std::vector<Entity>& out) {
    const Json& args = call->args();
    if (args.contains("entities") && !args["entities"].is_null()) {
        if (!args["entities"].is_array() || args["entities"].empty()) {
            call->fail("entities must be a non-empty array of GameObject references.");
            return false;
        }
        for (const Json& ref : args["entities"]) {
            std::string error;
            Entity entity = McpRefs::resolveEntity(scene, ref, error);
            if (entity == NULL_ENTITY) {
                call->fail("entities: " + error);
                return false;
            }
            if (std::find(out.begin(), out.end(), entity) == out.end()) out.push_back(entity);
        }
        return true;
    }
    Entity entity = entityArg(call, scene, "entity");
    if (entity == NULL_ENTITY) return false;
    out.push_back(entity);
    return true;
}

// A project-relative .prefab path. mustExist: the file is there (a prefab to use);
// otherwise ".prefab" is added when missing (a prefab to create). A prefab the tool writes (created,
// or `write`: changed) must stay inside the project once links are resolved
// (McpRefs::resolveProjectWritePath); one it only reads may be reached through a link.
bool prefabPathArg(Project* project, const std::string& text, bool mustExist, fs::path& relative, std::string& error, bool write = false) {
    if (text.empty()) {
        error = "A project-relative .prefab path is required.";
        return false;
    }
    fs::path rel = FileUtils::pathFromUtf8(text).lexically_normal();
    if (!mustExist && rel.extension() != ".prefab") rel += ".prefab";
    fs::path absolute;
    if ((!mustExist || write) ? !McpRefs::resolveProjectWritePath(project, generic(rel), absolute, error, false)
                            : !McpRefs::resolveProjectPath(project, generic(rel), absolute, error, false)) {
        return false;
    }
    if (!Util::isPrefabFile(rel.string())) {
        error = "\"" + text + "\" is not a .prefab file.";
        return false;
    }
    std::error_code ec;
    if (mustExist && !fs::exists(absolute, ec)) {
        error = "No prefab at \"" + generic(rel) + "\". List the project's prefabs with list_prefabs.";
        return false;
    }
    // an existing prefab is the asset, whichever path of its file was given
    relative = mustExist ? project->getPrefabAssetPath(rel) : rel;
    return true;
}

std::string safeFileName(std::string name) {
    for (char& c : name) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
    }
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) name.pop_back();
    return name.empty() ? std::string("Prefab") : name;
}

fs::path uniquePrefabPath(Project* project, const fs::path& dir, const std::string& base, const std::set<std::string>& taken) {
    const std::string stem = safeFileName(base);
    fs::path candidate = dir / (stem + ".prefab");
    std::error_code ec;
    for (int counter = 1; (fs::exists(project->getProjectPath() / candidate, ec) || taken.count(generic(candidate))) && counter < 10000; counter++) {
        candidate = dir / (stem + " " + std::to_string(counter) + ".prefab");
    }
    return candidate.lexically_normal();
}

// The prefab path a GameObject's own PrefabComponent names (an instance root), else empty
fs::path rootPrefabPath(Project* project, SceneProject* scene, Entity entity) {
    fs::path path;
    if (project->findInstanceRootedAt(scene, entity, &path)) return path;
    return {};
}

// The Inspector's labels for apply targets (InspectorPrefabBar.cpp applyTargetLabel)
std::string applyTargetLabel(Project* project, const std::vector<fs::path>& targets, size_t t, uint32_t sceneId) {
    const std::string name = "'" + FileUtils::pathToUtf8(targets[t].stem()) + "'";
    const bool variant = !project->getPrefabFileBase(targets[t]).empty();
    if (t > 0) return (variant ? "Apply as Override in Prefab Variant " : "Apply as Override in Prefab ") + name;
    std::vector<fs::path> over(targets.begin() + 1, targets.end());
    if (const PrefabStage* stage = project->getPrefabStage(sceneId)) over.push_back(stage->assetPath);
    for (const fs::path& outer : over) {
        const std::vector<fs::path> chain = project->getPrefabVariantChain(outer);
        if (chain.size() > 1 && std::find(chain.begin() + 1, chain.end(), targets[t]) != chain.end()) {
            return (variant ? "Apply to Base Prefab Variant " : "Apply to Base Prefab ") + name;
        }
    }
    return (variant ? "Apply to Prefab Variant " : "Apply to Prefab ") + name;
}

// The outermost instance root holding the GameObject (itself when it is one). A GameObject
// added below an instance (an added-GameObject override, at any depth) belongs to the
// instance of its nearest instance ancestor; `added` tells. NULL outside any instance.
Entity outermostRoot(Project* project, uint32_t sceneId, Entity entity, bool* added = nullptr) {
    if (added) *added = false;
    SceneProject* scene = project->getScene(sceneId);
    Entity current = entity;
    while (current != NULL_ENTITY) {
        const std::vector<Entity> roots = project->getOutermostPrefabRoots(sceneId, {current});
        if (!roots.empty() && roots.front() != NULL_ENTITY) {
            if (added) *added = current != entity;
            return roots.front();
        }
        Transform* transform = scene && scene->scene ? scene->scene->findComponent<Transform>(current) : nullptr;
        current = transform ? transform->parent : NULL_ENTITY;
    }
    return NULL_ENTITY;
}

// ------------------------------------------------------------------ override entries

// Where an override entry can be applied (the Overrides dropdown's choice)
std::vector<fs::path> entryTargets(Project* project, uint32_t sceneId, const PrefabOverrideEntry& entry) {
    std::vector<fs::path> targets;
    if (entry.kind == Kind::AddedGameObject) {
        targets = project->getPrefabApplyTargets(sceneId, entry.parent);
        if (targets.empty()) {
            const fs::path own = project->findPrefabPathFor(sceneId, entry.parent);
            if (!own.empty()) targets.push_back(own);
        }
    } else if (entry.kind == Kind::RemovedGameObject) {
        targets = project->getPrefabApplyTargets(sceneId, entry.removal.instanceRoot);
        targets.erase(targets.begin(), std::find(targets.begin(), targets.end(), entry.removal.path));
        if (targets.empty()) targets.push_back(entry.removal.path);
    } else {
        targets = project->getPrefabApplyTargets(sceneId, entry.entity);
    }
    return targets;
}

bool entryCanApply(Project* project, uint32_t sceneId, const PrefabOverrideEntry& entry, const fs::path& target, std::string& reason) {
    reason.clear();
    if (!project->canApplyPrefabOverrides(&reason)) return false;
    if (entry.kind == Kind::Property) {
        const std::vector<std::string> names(entry.properties.begin(), entry.properties.end());
        return project->canApplyPrefabOverride(sceneId, entry.entity, entry.type, names, target, &reason);
    }
    if (entry.kind == Kind::AddedComponent) {
        return project->canApplyPrefabOverride(sceneId, entry.entity, entry.type, {}, target, &reason);
    }
    return true;
}

std::string entrySummary(SceneProject* scene, const PrefabOverrideEntry& entry) {
    auto path = [&](Entity entity) { return McpRefs::entityPath(scene, entity); };
    switch (entry.kind) {
        case Kind::Property: {
            std::string names;
            for (const std::string& name : entry.properties) names += (names.empty() ? "" : ",") + name;
            return path(entry.entity) + " " + componentName(entry.type) + "[" + names + "]";
        }
        case Kind::State: {
            std::string names;
            for (const Json& name : stateNames(entry.state)) names += (names.empty() ? "" : ",") + name.get<std::string>();
            return path(entry.entity) + " state[" + names + "]";
        }
        case Kind::AddedComponent: return path(entry.entity) + " +" + componentName(entry.type);
        case Kind::RemovedComponent: return path(entry.entity) + " -" + componentName(entry.type);
        case Kind::AddedGameObject: return "+GameObject " + path(entry.entity);
        case Kind::RemovedGameObject: return "-GameObject " + entry.removal.name + " (of " + generic(entry.removal.path) + ")";
    }
    return "?";
}

// One override entry as JSON. values: the current and source values of overridden properties
Json entryJson(Project* project, SceneProject* scene, const PrefabOverrideEntry& entry, bool withTargets, bool values) {
    const uint32_t sceneId = scene->id;
    Json out = {{"kind", kindName(entry.kind)}};
    switch (entry.kind) {
        case Kind::Property: {
            out["entity"] = McpRefs::entityJson(scene, entry.entity);
            out["component"] = componentName(entry.type);
            out["properties"] = Json(std::vector<std::string>(entry.properties.begin(), entry.properties.end()));
            if (entry.properties.count("*")) out["whole_component"] = true;
            if (values) {
                Json valueMap = Json::object();
                for (const std::string& name : entry.properties) {
                    if (name == "*") continue;
                    Json item = Json::object();
                    PropertyData current = Catalog::findProperty(scene->scene, entry.entity, entry.type, name);
                    if (current.ref) item["value"] = McpValues::toJson(project, name, current);
                    PrefabSourceValue source;
                    if (project->getPrefabSourceValue(sceneId, entry.entity, entry.type, name, source) && source.valid()) {
                        item["source"] = McpValues::toJson(project, name, source.property);
                    }
                    valueMap[name] = item;
                }
                out["values"] = valueMap;
            }
            break;
        }
        case Kind::State: {
            out["entity"] = McpRefs::entityJson(scene, entry.entity);
            out["state"] = stateNames(entry.state);
            if (values) {
                std::string name, tag;
                bool active = true;
                int layer = 0;
                Json item = {
                    {"value", {{"name", scene->scene->getEntityName(entry.entity)}, {"active", scene->scene->isEntityActive(entry.entity)},
                               {"tag", scene->scene->getEntityTag(entry.entity)}, {"layer", scene->scene->getEntityLayer(entry.entity)}}}
                };
                if (project->getPrefabSourceState(sceneId, entry.entity, name, active, tag, layer)) {
                    item["source"] = {{"name", name}, {"active", active}, {"tag", tag}, {"layer", layer}};
                }
                out["values"] = item;
            }
            break;
        }
        case Kind::AddedComponent:
        case Kind::RemovedComponent:
            out["entity"] = McpRefs::entityJson(scene, entry.entity);
            out["component"] = componentName(entry.type);
            break;
        case Kind::AddedGameObject:
            out["entity"] = McpRefs::entityJson(scene, entry.entity);
            out["parent"] = McpRefs::entityJson(scene, entry.parent);
            break;
        case Kind::RemovedGameObject:
            out["name"] = entry.removal.name;
            out["prefab"] = generic(entry.removal.path);
            out["registry_entity"] = entry.removal.registryEntity;
            out["instance_root"] = McpRefs::entityJson(scene, entry.removal.instanceRoot);
            if (entry.removal.belowRemoved) out["below_removed"] = true;
            break;
    }
    if (withTargets) {
        const std::vector<fs::path> targets = entryTargets(project, sceneId, entry);
        Json list = Json::array();
        for (size_t t = 0; t < targets.size(); t++) {
            std::string reason;
            const bool can = entryCanApply(project, sceneId, entry, targets[t], reason);
            Json item = {{"prefab", generic(targets[t])}, {"label", applyTargetLabel(project, targets, t, sceneId)}, {"can_apply", can}};
            if (!can && !reason.empty()) item["reason"] = reason;
            list.push_back(item);
        }
        out["apply_targets"] = list;
    }
    return out;
}

// ------------------------------------------------------------------ asset / stage / instance info

Json stageJson(Project* project, const PrefabStage& stage) {
    SceneProject& sp = const_cast<PrefabStage&>(stage).sceneProject;
    Json out = {
        {"scene_id", sp.id},
        {"depth", project->getPrefabStageDepth(sp.id)},
        {"prefab", generic(stage.assetPath)},
        {"guid", stage.guid},
        {"root", McpRefs::entityJson(&sp, stage.root)},
        {"in_context", stage.inContext()},
        {"modified", project->hasUnsavedPrefabStage(sp.id)},
        {"return_scene_id", stage.returnSceneId}
    };
    if (stage.inContext()) {
        out["context_scene_id"] = stage.contextSceneId;
        if (SceneProject* context = project->getScene(stage.contextSceneId)) {
            out["context_root"] = McpRefs::entityJson(context, stage.contextRoot);
        }
    }
    const fs::path base = project->getPrefabFileBase(stage.assetPath);
    if (!base.empty()) out["variant_of"] = generic(base);
    return out;
}

const char* contextVisibilityName(PrefabContextVisibility visibility) {
    switch (visibility) {
        case PrefabContextVisibility::Normal: return "normal";
        case PrefabContextVisibility::Gray: return "gray";
        case PrefabContextVisibility::Hidden: return "hidden";
    }
    return "gray";
}

Json prefabModeJson(Project* project) {
    Json stages = Json::array();
    for (size_t i = 0; i < project->getPrefabStageCount(); i++) {
        if (PrefabStage* stage = project->getPrefabStageAt(i)) stages.push_back(stageJson(project, *stage));
    }
    Json out = {
        {"active", project->getPrefabStageCount() > 0},
        {"stages", stages},
        {"auto_save", project->isPrefabAutoSave()},
        {"context", contextVisibilityName(project->getPrefabContextVisibility())},
        {"selected_scene_id", project->getSelectedSceneId()}
    };
    if (PrefabStage* top = project->getActivePrefabStage()) out["shown_stage_scene_id"] = top->sceneProject.id;
    return out;
}

Json usageJson(const PrefabUsage& usage) {
    Json scenes = Json::array();
    for (const PrefabSceneUsage& scene : usage.scenes) {
        Json item = {{"scene", scene.sceneName}, {"loaded", scene.loaded}, {"instances", scene.instances}, {"nested", scene.nested}};
        if (scene.loaded) item["scene_id"] = scene.sceneId;
        if (scene.missing) item["missing"] = scene.missing;
        scenes.push_back(item);
    }
    Json nestedIn = Json::array();
    for (const fs::path& path : usage.nestedIn) nestedIn.push_back(generic(path));
    return {{"total_instances", usage.totalInstances()}, {"scenes", scenes}, {"nested_in", nestedIn}};
}

const PrefabUsage* findUsage(const std::map<std::string, PrefabUsage>& usages, const std::string& guid, const fs::path& path) {
    if (!guid.empty()) {
        auto it = usages.find(guid);
        if (it != usages.end()) return &it->second;
    }
    for (const auto& [key, usage] : usages) {
        if (usage.path.lexically_normal() == path.lexically_normal()) return &usage;
    }
    return nullptr;
}

// What a prefab asset is. contents: its GameObjects (file ids, names, parents, components)
Json assetJson(Project* project, const fs::path& relative, bool withUsage, bool contents, size_t limit,
               const std::map<std::string, PrefabUsage>* usages) {
    std::error_code ec;
    const bool exists = fs::exists(project->getProjectPath() / relative, ec);
    const std::string guid = exists ? project->getPrefabGuid(relative) : std::string();
    Json out = {{"path", generic(relative)}, {"guid", guid}, {"exists", exists}};
    if (!exists) return out;

    bool broken = false;
    const std::vector<fs::path> chain = project->getPrefabVariantChain(relative, &broken);
    const fs::path base = project->getPrefabFileBase(relative);
    out["is_variant"] = !base.empty();
    if (!base.empty()) {
        out["base"] = generic(base);
        Json chainJson = Json::array();
        for (const fs::path& path : chain) chainJson.push_back(generic(path));
        out["variant_chain"] = chainJson;
        if (broken) out["variant_chain_broken"] = true;
    }
    Json variants = Json::array();
    for (const fs::path& path : project->findPrefabVariantsOf(relative)) variants.push_back(generic(path));
    out["variants"] = variants;
    out["standalone"] = project->isStandalonePrefab(relative);

    Prefab* prefab = project->loadPrefabAsset(relative);
    if (prefab && prefab->registry) {
        EntityRegistry* registry = prefab->registry.get();
        if (prefab->root != NULL_ENTITY && registry->isEntityCreated(prefab->root)) {
            out["root_name"] = registry->getEntityName(prefab->root);
        }
        out["gameobject_count"] = prefab->registryEntities.size();
        out["unsaved_changes"] = prefab->isModified;
        Json nested = Json::array();
        for (Entity entity : prefab->registryEntities) {
            if (entity != prefab->root && prefab->isNestedRoot(entity)) {
                nested.push_back({{"id", entity}, {"name", registry->getEntityName(entity)}, {"prefab", generic(project->instancePathAt(*prefab, entity))}});
            }
        }
        out["nested_prefabs"] = nested;
        if (contents) {
            Json list = Json::array();
            size_t count = 0;
            for (Entity entity : prefab->registryEntities) {
                if (count++ >= limit) {
                    out["contents_truncated"] = true;
                    break;
                }
                Json item = {{"id", entity}, {"name", registry->getEntityName(entity)}};
                const Entity parent = prefab->getRegistryParent(entity);
                if (parent != NULL_ENTITY) item["parent"] = parent;
                if (entity == prefab->root) item["root"] = true;
                Json components = Json::array();
                for (ComponentType type : Catalog::findComponents(registry, entity)) {
                    if (type == ComponentType::PrefabComponent) continue;
                    components.push_back(componentName(type));
                }
                item["components"] = components;
                if (prefab->isInstanceRoot(entity)) item["instance_of"] = generic(project->instancePathAt(*prefab, entity));
                list.push_back(item);
            }
            out["contents"] = list;
        }
    } else {
        out["readable"] = false;
    }

    for (size_t i = 0; i < project->getPrefabStageCount(); i++) {
        PrefabStage* stage = project->getPrefabStageAt(i);
        if (stage && stage->assetPath.lexically_normal() == relative.lexically_normal()) {
            out["open_in_prefab_mode"] = stageJson(project, *stage);
        }
    }

    if (withUsage && usages) {
        if (const PrefabUsage* usage = findUsage(*usages, guid, relative)) out["usage"] = usageJson(*usage);
        else out["usage"] = {{"total_instances", 0}, {"scenes", Json::array()}, {"nested_in", Json::array()}};
    }
    return out;
}

// ------------------------------------------------------------------ tools

// create_prefab
void createPrefabTool(const CallPtr& call) {
    Project* project = requireProject(call);
    if (!project) return;
    SceneProject* scene = sceneArg(call, project);
    if (!scene) return;
    if (project->isPrefabStage(scene->id)) {
        call->fail("create_prefab works on scene GameObjects. In Prefab Mode, close the stage first (prefab_mode action=close) "
                   "or instantiate other prefabs into the stage with instantiate_prefab.");
        return;
    }
    std::vector<Entity> selected;
    if (!entitiesArg(call, scene, selected)) return;
    Scene* registry = scene->scene;
    const uint32_t sceneId = scene->id;

    // Top-level ones only (a selected descendant goes with its ancestor)
    std::unordered_set<Entity> selectedSet(selected.begin(), selected.end());
    std::vector<Entity> tops;
    for (Entity entity : selected) {
        bool below = false;
        Transform* transform = registry->findComponent<Transform>(entity);
        Entity parent = transform ? transform->parent : NULL_ENTITY;
        while (parent != NULL_ENTITY) {
            if (selectedSet.count(parent)) { below = true; break; }
            Transform* parentTransform = registry->findComponent<Transform>(parent);
            parent = parentTransform ? parentTransform->parent : NULL_ENTITY;
        }
        if (!below) tops.push_back(entity);
    }
    for (Entity entity : tops) {
        if (project->isMissingPrefab(sceneId, entity)) {
            call->fail(McpRefs::entityPath(scene, entity) + " is a Missing Prefab instance: relink it with replace_prefab or unpack it first.");
            return;
        }
        PrefabObjectRef ref;
        if (project->findPrefabObject(sceneId, entity, ref) && !ref.isRoot) {
            call->fail(McpRefs::entityPath(scene, entity) + " is a GameObject of an instance of " + generic(ref.path) +
                       ": a prefab member cannot become a prefab of its own. Unpack the instance first (unpack_prefab), "
                       "or edit the prefab in Prefab Mode.");
            return;
        }
    }

    // As the Hierarchy's payload: the GameObjects with their virtual (action) children
    std::vector<Entity> exportEntities = tops;
    const std::vector<Entity> virtualChildren = ProjectUtils::getVirtualChildren(registry, tops);
    exportEntities.insert(exportEntities.end(), virtualChildren.begin(), virtualChildren.end());
    // a posed character's prefab gets its rest bones (the pose comes back on the next frame)
    PoseEditSession::suspendActiveFor(sceneId, exportEntities);
    YAML::Node payload = exportEntities.size() == 1
        ? Stream::encodeEntity(tops[0], registry, project, scene)
        : Stream::encodeEntitySelection(exportEntities, registry, project, scene);

    // One prefab per root (ProjectWindow::saveEntityFile), actions with the root they act on
    const std::string transformKey = Catalog::getComponentName(ComponentType::Transform, true);
    const std::string actionKey = Catalog::getComponentName(ComponentType::ActionComponent, true);
    std::vector<YAML::Node> roots;
    std::vector<YAML::Node> loose;
    if (payload["members"] && payload["members"].IsSequence()) {
        for (YAML::Node member : payload["members"]) {
            const YAML::Node components = member["components"];
            (components && components.IsMap() && components[transformKey] ? roots : loose).push_back(member);
        }
    } else {
        roots.push_back(payload);
    }
    if (roots.empty() && !loose.empty()) {
        roots.push_back(loose.front());
        loose.erase(loose.begin());
    }
    std::vector<std::vector<YAML::Node>> groups(roots.size());
    for (YAML::Node& node : loose) {
        size_t owner = 0;
        const YAML::Node action = node["components"] ? node["components"][actionKey] : YAML::Node();
        if (action && action["target"]) {
            const Entity target = action["target"].as<Entity>();
            for (size_t i = 0; i < roots.size(); i++) {
                std::vector<Entity> ids;
                ProjectUtils::collectEntities(roots[i], ids);
                if (std::find(ids.begin(), ids.end(), target) != ids.end()) { owner = i; break; }
            }
        }
        groups[owner].push_back(node);
    }

    const Json& args = call->args();
    const std::string pathArg = args.value("path", std::string());
    if (!pathArg.empty() && roots.size() != 1) {
        call->fail("path names one prefab, but " + std::to_string(roots.size()) + " root GameObjects were given: pass directory instead "
                   "(each root becomes <directory>/<name>.prefab).");
        return;
    }
    fs::path dir = FileUtils::pathFromUtf8(args.value("directory", std::string("prefabs"))).lexically_normal();
    if (pathArg.empty()) {
        fs::path absolute;
        std::string error;
        if (!McpRefs::resolveProjectWritePath(project, generic(dir), absolute, error, false)) {
            call->fail("directory: " + error);
            return;
        }
    }

    std::vector<std::pair<fs::path, YAML::Node>> prefabs;
    std::set<std::string> taken;
    Json results = Json::array();
    for (size_t i = 0; i < roots.size(); i++) {
        YAML::Node prefabNode = YAML::Clone(roots[i]);
        if (!groups[i].empty()) {
            YAML::Node group(YAML::NodeType::Map);
            group["type"] = "Prefab";
            YAML::Node members(YAML::NodeType::Sequence);
            members.push_back(YAML::Clone(roots[i]));
            for (const YAML::Node& node : groups[i]) members.push_back(YAML::Clone(node));
            group["members"] = members;
            prefabNode.reset(group);
        }
        fs::path relative;
        if (!pathArg.empty()) {
            std::string error;
            if (!prefabPathArg(project, pathArg, false, relative, error)) {
                call->fail("path: " + error);
                return;
            }
            std::error_code ec;
            if (fs::exists(project->getProjectPath() / relative, ec)) {
                call->fail("A file already exists at \"" + generic(relative) + "\". Pick another path (prefabs are never overwritten); "
                           "to make an existing instance follow another prefab use replace_prefab.");
                return;
            }
        } else {
            const std::string name = roots[i]["name"] ? roots[i]["name"].as<std::string>() : std::string("Prefab");
            relative = uniquePrefabPath(project, dir, name.empty() ? "Prefab" : name, taken);
        }
        taken.insert(generic(relative));
        std::error_code ec;
        FileUtils::createDirectories((project->getProjectPath() / relative).parent_path(), ec);
        prefabs.emplace_back(relative, prefabNode);
        const Entity root = roots[i]["entity"] ? roots[i]["entity"].as<Entity>() : NULL_ENTITY;
        results.push_back({{"path", generic(relative)}, {"root_id", root}});
    }

    const uint64_t since = Out::lastLogId();
    if (!runCommand(CommandHandle::get(sceneId), new CreatePrefabCmd(project, sceneId, prefabs))) {
        const std::string why = problemsSince(since);
        call->fail("Create Prefab failed" + (why.empty() ? std::string(".") : ": " + why));
        return;
    }
    refreshProjectWindow(call);

    std::string text = "Created " + std::to_string(results.size()) + " prefab(s):";
    for (Json& item : results) {
        const fs::path relative = item["path"].get<std::string>();
        item["guid"] = project->getPrefabGuid(relative);
        const Entity root = item["root_id"].get<Entity>();
        item.erase("root_id");
        if (root != NULL_ENTITY && registry->isEntityCreated(root)) item["instance"] = McpRefs::entityJson(scene, root);
        text += " " + generic(relative);
    }
    text += ". Each GameObject is now the root of an instance of its prefab (one undo step). "
            "Prefab files are written now; later Apply/Revert edits are written when the scene is saved.";
    call->ok(text, Json{{"scene_id", sceneId}, {"prefabs", results}});
}

// create_prefab_variant
void createPrefabVariantTool(const CallPtr& call) {
    Project* project = requireProject(call);
    if (!project) return;
    const Json& args = call->args();
    const bool fromEntity = args.contains("entity") && !args["entity"].is_null();
    const std::string baseArg = args.value("base", std::string());
    if (fromEntity == !baseArg.empty()) {
        call->fail("Pass either entity (a prefab instance root in a scene: it becomes an instance of the new variant, its "
                   "overrides the variant's) or base (a .prefab path: a new variant asset with no instance).");
        return;
    }
    const std::string pathArg = args.value("path", std::string());
    fs::path relative;
    std::string error;
    if (!pathArg.empty()) {
        if (!prefabPathArg(project, pathArg, false, relative, error)) {
            call->fail("path: " + error);
            return;
        }
        std::error_code ec;
        if (fs::exists(project->getProjectPath() / relative, ec)) {
            call->fail("A file already exists at \"" + generic(relative) + "\". Pick another path.");
            return;
        }
        FileUtils::createDirectories((project->getProjectPath() / relative).parent_path(), ec);
    }

    const uint64_t since = Out::lastLogId();
    if (!fromEntity) {
        fs::path base;
        if (!prefabPathArg(project, baseArg, true, base, error)) {
            call->fail("base: " + error);
            return;
        }
        if (relative.empty()) relative = CreatePrefabVariantAssetCmd::defaultPath(project, base);
        if (!runCommand(project->getProjectCommandHistory(), new CreatePrefabVariantAssetCmd(project, base, relative))) {
            const std::string why = problemsSince(since);
            call->fail("Create Prefab Variant failed" + (why.empty() ? std::string(".") : ": " + why));
            return;
        }
        refreshProjectWindow(call);
        call->ok("Created Prefab Variant " + generic(relative) + " of " + generic(base) +
                 " (undo with undo scope=project). Instantiate it with instantiate_prefab.",
                 Json{{"path", generic(relative)}, {"guid", project->getPrefabGuid(relative)}, {"base", generic(base)}});
        return;
    }

    SceneProject* scene = sceneArg(call, project);
    if (!scene) return;
    const Entity root = entityArg(call, scene, "entity");
    if (root == NULL_ENTITY) return;
    std::string reason;
    if (!project->canCreatePrefabVariant(scene->id, root, &reason)) {
        call->fail("Cannot make a variant of " + McpRefs::entityPath(scene, root) + ": " +
                   (reason.empty() ? std::string("it is not an outermost (or added) live prefab instance root.") : reason) +
                   " Pass the instance root (get_prefab_info shows it) or use base with a .prefab path.");
        return;
    }
    const fs::path base = rootPrefabPath(project, scene, root);
    if (relative.empty()) relative = CreatePrefabVariantAssetCmd::defaultPath(project, base);
    // a posed character's prefab gets its rest bones
    PoseEditSession::suspendActiveFor(scene->id, {root});
    std::vector<std::pair<fs::path, YAML::Node>> items = {{relative, Stream::encodeEntity(root, scene->scene, project, scene)}};
    if (!runCommand(CommandHandle::get(scene->id), new CreatePrefabCmd(project, scene->id, items, true))) {
        const std::string why = problemsSince(since);
        call->fail("Create Prefab Variant failed" + (why.empty() ? std::string(".") : ": " + why));
        return;
    }
    refreshProjectWindow(call);
    call->ok("Created Prefab Variant " + generic(relative) + " of " + generic(base) + "; " + McpRefs::entityPath(scene, root) +
             " is now an instance of it and its overrides became the variant's (one undo step).",
             Json{{"path", generic(relative)}, {"guid", project->getPrefabGuid(relative)}, {"base", generic(base)},
                  {"scene_id", scene->id}, {"instance", McpRefs::entityJson(scene, root)}});
}

// instantiate_prefab
void instantiatePrefabTool(const CallPtr& call) {
    Project* project = requireProject(call);
    if (!project) return;
    const Json& args = call->args();
    fs::path relative;
    std::string error;
    if (!prefabPathArg(project, args.value("prefab", std::string()), true, relative, error)) {
        call->fail("prefab: " + error);
        return;
    }
    SceneProject* scene = sceneArg(call, project);
    if (!scene) return;
    const uint32_t sceneId = scene->id;
    Entity parent = NULL_ENTITY;
    if (args.contains("parent") && !args["parent"].is_null()) {
        parent = entityArg(call, scene, "parent");
        if (parent == NULL_ENTITY) return;
        if (!scene->scene->findComponent<Transform>(parent)) {
            call->fail("parent " + McpRefs::entityPath(scene, parent) + " has no Transform; pick a GameObject with one.");
            return;
        }
    }
    if (!project->canInstantiatePrefabIn(sceneId, relative)) {
        call->fail("An instance of " + generic(relative) + " cannot go into this prefab stage: the prefab would contain itself.");
        return;
    }
    Vector3 position;
    const bool hasPosition = args.contains("position") && !args["position"].is_null();
    if (hasPosition && !McpValues::parseVector3(args["position"], position)) {
        call->fail("position must be [x,y,z].");
        return;
    }
    const std::string name = args.value("name", std::string());

    CommandHistory* history = CommandHandle::get(sceneId);
    const size_t mark = history->mark();
    const uint64_t since = Out::lastLogId();
    ImportPrefabCmd* command = new ImportPrefabCmd(project, sceneId, relative, parent, true);
    command->setQuiet(true);
    if (hasPosition) command->setDropPosition(position, false);
    if (args.value("nest_in_parent_prefab", false)) command->setAddToParentPrefab(true);
    if (!runCommand(history, command)) {
        const std::string why = problemsSince(since);
        call->fail("Instantiate failed" + (why.empty() ? std::string(".") : ": " + why));
        return;
    }
    const Entity root = command->getRootEntity();
    const size_t members = command->getImportedEntities().size();
    if (!name.empty() && root != NULL_ENTITY) {
        runCommand(history, new EntityNameCmd(project, sceneId, root, name));
        history->groupSince(mark);
    }
    if (root != NULL_ENTITY) project->setSelectedEntity(sceneId, root);

    Json data = {
        {"scene_id", sceneId}, {"prefab", generic(relative)}, {"guid", project->getPrefabGuid(relative)},
        {"instance", McpRefs::entityJson(scene, root)}, {"gameobjects", members}
    };
    if (parent != NULL_ENTITY) data["parent"] = McpRefs::entityJson(scene, parent);
    if (project->isEditingPrefabAsset(scene)) data["in_prefab_stage"] = true;
    call->ok("Instantiated " + generic(relative) + " as " + McpRefs::entityPath(scene, root) + " (id " + std::to_string(root) +
             ") in scene " + std::to_string(sceneId) + ". Edits on it are instance overrides until applied.", data);
}

// replace_prefab
void replacePrefabTool(const CallPtr& call) {
    Project* project = requireProject(call);
    if (!project) return;
    const Json& args = call->args();
    fs::path relative;
    std::string error;
    if (!prefabPathArg(project, args.value("prefab", std::string()), true, relative, error)) {
        call->fail("prefab: " + error);
        return;
    }
    SceneProject* scene = sceneArg(call, project);
    if (!scene) return;
    std::vector<Entity> targets;
    if (!entitiesArg(call, scene, targets)) return;
    const uint32_t sceneId = scene->id;

    PrefabReplaceSettings settings;
    const std::string match = args.value("match", std::string("name"));
    if (match == "name") settings.match = PrefabReplaceSettings::Match::ByName;
    else if (match == "hierarchy") settings.match = PrefabReplaceSettings::Match::ByHierarchy;
    else if (match == "none") settings.match = PrefabReplaceSettings::Match::None;
    else {
        call->fail("match must be name, hierarchy or none.");
        return;
    }
    settings.keepOverrides = args.value("keep_overrides", true);
    settings.renameRoot = args.value("rename_root", true);

    const std::vector<Entity> roots = ReplacePrefabCmd::replaceableRoots(project, sceneId, targets);
    if (roots.empty()) {
        call->fail("Nothing to replace: pass the root of an outermost (or added) prefab instance, or a Missing Prefab root. "
                   "A nested instance belongs to its prefab (open that prefab in Prefab Mode to change it), and nothing "
                   "can be replaced while playing.");
        return;
    }
    const uint64_t since = Out::lastLogId();
    if (!runCommand(CommandHandle::get(sceneId), new ReplacePrefabCmd(project, sceneId, roots, relative, settings))) {
        const std::string why = problemsSince(since);
        call->fail("Replace Prefab failed" + (why.empty() ? std::string(".") : ": " + why));
        return;
    }
    Json list = Json::array();
    for (Entity root : roots) list.push_back(McpRefs::entityJson(scene, root));
    call->ok("Replaced " + std::to_string(roots.size()) + " instance(s) with " + generic(relative) +
             " (match " + match + ", one undo step). Dropped overrides are in log.",
             Json{{"scene_id", sceneId}, {"prefab", generic(relative)}, {"instances", list}, {"log", problemsJson(since)}});
}

// unpack_prefab
void unpackPrefabTool(const CallPtr& call) {
    Project* project = requireProject(call);
    if (!project) return;
    SceneProject* scene = sceneArg(call, project);
    if (!scene) return;
    std::vector<Entity> targets;
    if (!entitiesArg(call, scene, targets)) return;
    const uint32_t sceneId = scene->id;
    const bool completely = call->args().value("completely", false);

    std::vector<Entity> missing, live;
    for (Entity entity : targets) (project->isMissingPrefab(sceneId, entity) ? missing : live).push_back(entity);
    const uint64_t since = Out::lastLogId();
    CommandHistory* history = CommandHandle::get(sceneId);
    if (!missing.empty() && live.empty()) {
        if (!runCommand(history, new UnpackMissingPrefabCmd(project, sceneId, missing))) {
            call->fail("Unpack failed: " + problemsSince(since));
            return;
        }
        Json list = Json::array();
        for (Entity entity : missing) list.push_back(McpRefs::entityJson(scene, entity));
        call->ok("Unpacked " + std::to_string(missing.size()) + " Missing Prefab instance(s) into plain GameObjects.",
                 Json{{"scene_id", sceneId}, {"unpacked", list}});
        return;
    }
    if (!missing.empty()) {
        call->fail("Mix of Missing Prefab roots and live instances: unpack them in separate calls.");
        return;
    }
    if (!UnpackPrefabCmd::canUnpack(project, sceneId, live)) {
        call->fail(project->isAnyScenePlaying() ? "Stop the game first: instances cannot be unpacked while playing."
                                                : "None of these GameObjects is part of a prefab instance.");
        return;
    }
    std::vector<Entity> roots;
    for (Entity root : project->getOutermostPrefabRoots(sceneId, live)) {
        if (root != NULL_ENTITY && std::find(roots.begin(), roots.end(), root) == roots.end()) roots.push_back(root);
    }
    if (!runCommand(history, new UnpackPrefabCmd(project, sceneId, live, completely))) {
        const std::string why = problemsSince(since);
        call->fail("Unpack failed" + (why.empty() ? std::string(".") : ": " + why));
        return;
    }
    Json list = Json::array();
    for (Entity root : roots) {
        Json item = McpRefs::entityJson(scene, root);
        const fs::path now = rootPrefabPath(project, scene, root);
        item["now_instance_of"] = now.empty() ? Json() : Json(generic(now));
        list.push_back(item);
    }
    call->ok(std::string(completely ? "Unpacked completely " : "Unpacked one level of ") + std::to_string(roots.size()) +
             " outermost instance(s). " + (completely ? "Every GameObject is plain now." :
             "Nested instances stay instances; a variant instance became an instance of its base (now_instance_of)."),
             Json{{"scene_id", sceneId}, {"completely", completely}, {"instances", list}});
}

// add_to_prefab
// The prefab asset saves of one tool call: a degraded asset is written only with allow_data_loss
// (docs/data-safety.md); refused() fails the call with degraded_document
class PrefabWriteScope {
public:
    explicit PrefabWriteScope(const CallPtr& call): call(call), project(call->ctx().project()) {
        if (project) {
            project->beginPrefabSaveScope(call->args().value("allow_data_loss", false) ? DegradedSavePolicy::SaveAnyway : DegradedSavePolicy::Refuse);
        }
    }
    ~PrefabWriteScope() {
        if (project && open) project->endPrefabSaveScope();
    }
    bool refused() {
        if (!project || !open) return false;
        open = false;
        const std::vector<fs::path> paths = project->endPrefabSaveScope();
        if (paths.empty()) return false;
        const Prefab* prefab = project->getPrefab(paths.front());
        LoadReport report = prefab ? prefab->loadReport : LoadReport();
        Json data = degradedDocumentError(generic(paths.front()), report);
        call->fail("The change is made in the editor, but the prefab asset " + generic(paths.front()) + " was not written: it did not "
                   "load completely and saving it would remove " + std::to_string(report.dropped()) + " value(s) this editor could not "
                   "read. Pass allow_data_loss=true to write anyway.", data);
        return true;
    }
private:
    CallPtr call;
    Project* project;
    bool open = true;
};

void addToPrefabTool(const CallPtr& call) {
    PrefabWriteScope prefabWrites(call);
    Project* project = requireProject(call);
    if (!project) return;
    SceneProject* scene = sceneArg(call, project);
    if (!scene) return;
    const Entity entity = entityArg(call, scene, "entity");
    if (entity == NULL_ENTITY) return;
    const uint32_t sceneId = scene->id;
    Entity parent = NULL_ENTITY;
    if (call->args().contains("parent") && !call->args()["parent"].is_null()) {
        parent = entityArg(call, scene, "parent");
        if (parent == NULL_ENTITY) return;
    } else if (Transform* transform = scene->scene->findComponent<Transform>(entity)) {
        parent = transform->parent;
    }
    if (parent == NULL_ENTITY || !(project->isEntityInPrefab(sceneId, parent) || project->isPrefabInstanceRoot(sceneId, parent))) {
        call->fail("parent must be a GameObject of a prefab instance (default: the GameObject's Transform parent). " +
                   McpRefs::entityPath(scene, entity) + "'s parent is not in a prefab instance.");
        return;
    }
    PrefabObjectRef ref;
    if (project->findPrefabObject(sceneId, entity, ref) && !ref.isRoot) {
        call->fail(McpRefs::entityPath(scene, entity) + " is already a GameObject of " + generic(ref.path) + ".");
        return;
    }
    fs::path target;
    const std::string targetArg = call->args().value("target", std::string());
    std::string error;
    if (!targetArg.empty() && !prefabPathArg(project, targetArg, true, target, error, true)) {
        call->fail("target: " + error);
        return;
    }
    if (!target.empty()) {
        std::vector<fs::path> targets = project->getPrefabApplyTargets(sceneId, parent);
        if (targets.empty()) targets.push_back(project->findPrefabPathFor(sceneId, parent));
        if (std::find(targets.begin(), targets.end(), target) == targets.end()) {
            std::string valid;
            for (const fs::path& path : targets) valid += " " + generic(path);
            call->fail("target " + generic(target) + " does not hold " + McpRefs::entityPath(scene, parent) + ". Valid targets:" + valid + ".");
            return;
        }
    }
    const uint64_t since = Out::lastLogId();
    if (!runCommand(CommandHandle::get(sceneId), new AddEntityToPrefabCmd(project, sceneId, entity, parent, target))) {
        const std::string why = problemsSince(since);
        call->fail("Add to Prefab failed" + (why.empty() ? std::string(".") : ": " + why));
        return;
    }
    const fs::path into = target.empty() ? project->findPrefabPathFor(sceneId, parent) : target;
    if (prefabWrites.refused()) return;
    call->ok("Added " + McpRefs::entityPath(scene, entity) + " to prefab " + generic(into) + " below " + McpRefs::entityPath(scene, parent) +
             "; every instance gets it (one undo step; the prefab file is written when the scene is saved).",
             Json{{"scene_id", sceneId}, {"entity", McpRefs::entityJson(scene, entity)}, {"parent", McpRefs::entityJson(scene, parent)},
                  {"prefab", generic(into)}});
}

// remove_from_prefab
void removeFromPrefabTool(const CallPtr& call) {
    PrefabWriteScope prefabWrites(call);
    Project* project = requireProject(call);
    if (!project) return;
    SceneProject* scene = sceneArg(call, project);
    if (!scene) return;
    const Entity entity = entityArg(call, scene, "entity");
    if (entity == NULL_ENTITY) return;
    const uint32_t sceneId = scene->id;
    PrefabObjectRef ref;
    if (!project->findPrefabObject(sceneId, entity, ref)) {
        call->fail(McpRefs::entityPath(scene, entity) + " is not a GameObject of a prefab instance.");
        return;
    }
    if (ref.isRoot) {
        call->fail(McpRefs::entityPath(scene, entity) + " is an instance root: a prefab keeps its root. Use unpack_prefab to "
                   "turn the instance into plain GameObjects.");
        return;
    }
    Transform* transform = scene->scene->findComponent<Transform>(entity);
    const Entity parent = transform ? transform->parent : NULL_ENTITY;
    const uint64_t since = Out::lastLogId();
    if (!runCommand(CommandHandle::get(sceneId), new RemoveEntityFromPrefabCmd(project, sceneId, entity, parent))) {
        const std::string why = problemsSince(since);
        call->fail("Remove from Prefab failed" + (why.empty() ? std::string(".") : ": " + why));
        return;
    }
    if (prefabWrites.refused()) return;
    call->ok("Removed " + McpRefs::entityPath(scene, entity) + " from prefab " + generic(ref.path) +
             ": every other instance loses it, here it stays as a plain GameObject (one undo step).",
             Json{{"scene_id", sceneId}, {"entity", McpRefs::entityJson(scene, entity)}, {"prefab", generic(ref.path)}});
}

// get_prefab_info
void getPrefabInfoTool(const CallPtr& call) {
    Project* project = requireProject(call);
    if (!project) return;
    const Json& args = call->args();
    const bool withUsage = args.value("usage", true);
    const bool withValues = args.value("values", false);
    const bool contents = args.value("contents", false);
    const size_t limit = static_cast<size_t>(std::max(1, args.value("limit", 200)));

    const bool hasEntity = args.contains("entity") && !args["entity"].is_null();
    const std::string prefabArg = args.value("prefab", std::string());
    if (!hasEntity && prefabArg.empty()) {
        call->fail("Pass entity (a GameObject of a scene or prefab stage) or prefab (a .prefab path).");
        return;
    }
    std::map<std::string, PrefabUsage> usages;
    if (withUsage) usages = project->collectPrefabUsage();

    if (!hasEntity) {
        fs::path relative;
        std::string error;
        if (!prefabPathArg(project, prefabArg, true, relative, error)) {
            call->fail("prefab: " + error);
            return;
        }
        Json data = assetJson(project, relative, withUsage, contents, limit, &usages);
        std::string text = "Prefab " + generic(relative) + (data.value("is_variant", false) ? " (variant of " + data.value("base", std::string()) + ")" : std::string());
        if (data.contains("usage")) text += ", " + std::to_string(data["usage"].value("total_instances", 0)) + " instance(s) in scenes";
        call->ok(text + ".", data);
        return;
    }

    SceneProject* scene = sceneArg(call, project);
    if (!scene) return;
    const Entity entity = entityArg(call, scene, "entity");
    if (entity == NULL_ENTITY) return;
    const uint32_t sceneId = scene->id;
    Scene* registry = scene->scene;

    Json data = {{"scene_id", sceneId}, {"entity", McpRefs::entityJson(scene, entity)}};
    if (PrefabStage* stage = project->getPrefabStage(sceneId)) {
        data["prefab_stage"] = {{"scene_id", sceneId}, {"prefab", generic(stage->assetPath)}};
        data["is_prefab_asset_object"] = project->isEditingPrefabAsset(sceneId, entity);
    }
    if (const MissingPrefabInstance* missing = project->getMissingPrefab(sceneId, entity)) {
        data["missing_prefab"] = {{"guid", missing->guid}, {"last_path", missing->path}};
    }

    PrefabObjectRef ref;
    const bool inInstance = project->findPrefabObject(sceneId, entity, ref);
    bool added = false;
    const Entity root = outermostRoot(project, sceneId, entity, &added);
    data["in_prefab_instance"] = inInstance || (root != NULL_ENTITY && !added);
    if (added) data["added_gameobject_of"] = McpRefs::entityJson(scene, root);
    data["is_instance_root"] = project->isPrefabInstanceRoot(sceneId, entity);
    if (inInstance) {
        data["owner"] = {{"prefab", generic(ref.path)}, {"registry_entity", ref.registryEntity}, {"is_root", ref.isRoot}};
    }

    // The instance roots holding it, outermost first, with their levels (variant chains)
    Json nesting = Json::array();
    {
        std::vector<Entity> chain;
        Entity current = entity;
        while (current != NULL_ENTITY) {
            if (project->isPrefabInstanceRoot(sceneId, current)) chain.push_back(current);
            Transform* transform = registry->findComponent<Transform>(current);
            current = transform ? transform->parent : NULL_ENTITY;
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            Json item = {{"root", McpRefs::entityJson(scene, *it)}, {"prefab", generic(rootPrefabPath(project, scene, *it))}};
            Json levels = Json::array();
            for (const fs::path& level : project->getInstanceRootLevels(sceneId, *it)) levels.push_back(generic(level));
            item["levels"] = levels;
            nesting.push_back(item);
        }
    }
    data["nesting"] = nesting;

    if (root != NULL_ENTITY) {
        data["instance_root"] = McpRefs::entityJson(scene, root);
        const fs::path rootPath = rootPrefabPath(project, scene, root);
        if (!rootPath.empty()) data["prefab"] = assetJson(project, rootPath, withUsage, contents, limit, &usages);

        // apply targets of the GameObject itself
        const std::vector<fs::path> targets = project->getPrefabApplyTargets(sceneId, entity);
        Json targetList = Json::array();
        for (size_t t = 0; t < targets.size(); t++) {
            targetList.push_back({{"prefab", generic(targets[t])}, {"label", applyTargetLabel(project, targets, t, sceneId)}});
        }
        data["apply_targets"] = targetList;
        std::string reason;
        data["can_apply"] = project->canApplyPrefabOverrides(&reason);
        if (!reason.empty()) data["cannot_apply_reason"] = reason;

        // Overrides of the whole outermost instance (Unity's Overrides dropdown); only_entity
        // keeps the ones of this GameObject
        const bool onlyEntity = args.value("only_entity", false);
        const std::vector<PrefabOverrideEntry> entries = project->getPrefabOverrideEntries(sceneId, root);
        Json list = Json::array();
        size_t shown = 0;
        size_t matched = 0;
        for (const PrefabOverrideEntry& entry : entries) {
            if (onlyEntity && entry.entity != entity && !(entry.kind == Kind::RemovedGameObject && entry.removal.instanceRoot == entity)) continue;
            matched++;
            if (shown >= limit) continue;
            list.push_back(entryJson(project, scene, entry, true, withValues));
            shown++;
        }
        data["overrides"] = list;
        data["override_count"] = matched;
        if (matched > shown) data["overrides_truncated"] = true;
    } else if (!prefabArg.empty()) {
        fs::path relative;
        std::string error;
        if (prefabPathArg(project, prefabArg, true, relative, error)) {
            data["prefab"] = assetJson(project, relative, withUsage, contents, limit, &usages);
        }
    }

    std::string text = McpRefs::entityPath(scene, entity);
    if (data.contains("missing_prefab")) {
        text += " is a Missing Prefab instance (relink with replace_prefab, or unpack_prefab).";
    } else if (root == NULL_ENTITY) {
        text += data.value("is_prefab_asset_object", false)
            ? " is a GameObject of the prefab asset open in Prefab Mode: edits change the asset (no overrides)."
            : " is not part of a prefab instance.";
    } else {
        text += std::string(added ? " is a GameObject added to (an override of) the instance of " : " is in an instance of ") +
                generic(rootPrefabPath(project, scene, root)) + " rooted at " + McpRefs::entityPath(scene, root) +
                "; " + std::to_string(data.value("override_count", 0)) + " override(s).";
    }
    call->ok(text, data);
}

// ---- prefab_overrides

struct Selector {
    bool hasKind = false;
    Kind kind = Kind::Property;
    Entity entity = NULL_ENTITY;
    bool hasComponent = false;
    ComponentType component = ComponentType::Transform;
    std::vector<std::string> properties;
    uint8_t state = 0;
    std::string name;
    Entity registryEntity = NULL_ENTITY;
    fs::path prefab;
};

bool parseSelector(const CallPtr& call, Project* project, SceneProject* scene, const Json& item, Selector& out, std::string& error) {
    if (!item.is_object()) {
        error = "each override selector must be an object like {\"entity\": \"Ship/Wing\", \"component\": \"transform\", \"properties\": [\"scale\"]}.";
        return false;
    }
    (void)call;
    if (item.contains("kind") && !item["kind"].is_null()) {
        if (!item["kind"].is_string() || !parseKind(item["kind"].get<std::string>(), out.kind)) {
            error = "kind must be one of: property, state, added_component, removed_component, added_gameobject, removed_gameobject.";
            return false;
        }
        out.hasKind = true;
    }
    if (item.contains("entity") && !item["entity"].is_null()) {
        // a listed entry's entity is an object {id, name, path}
        const Json ref = item["entity"].is_object() && item["entity"].contains("id") ? item["entity"]["id"] : item["entity"];
        out.entity = McpRefs::resolveEntity(scene, ref, error);
        if (out.entity == NULL_ENTITY) {
            error = "entity: " + error;
            return false;
        }
    }
    if (item.contains("component") && !item["component"].is_null()) {
        if (!McpRefs::resolveComponent(item["component"], out.component, error)) return false;
        out.hasComponent = true;
    }
    if (item.contains("properties") && item["properties"].is_array()) {
        for (const Json& name : item["properties"]) {
            if (name.is_string()) out.properties.push_back(name.get<std::string>());
        }
    }
    if (item.contains("state") && !item["state"].is_null()) {
        if (!parseStateBits(item["state"], out.state, error)) return false;
    }
    out.name = item.value("name", std::string());
    if (item.contains("registry_entity") && item["registry_entity"].is_number_integer()) {
        out.registryEntity = item["registry_entity"].get<Entity>();
    }
    const std::string prefab = item.value("prefab", std::string());
    if (!prefab.empty()) out.prefab = FileUtils::pathFromUtf8(prefab).lexically_normal();
    if (!out.hasKind && out.entity == NULL_ENTITY && out.name.empty() && out.registryEntity == NULL_ENTITY) {
        error = "an override selector needs at least entity (or, for removed GameObjects, kind \"removed_gameobject\" with name or registry_entity).";
        return false;
    }
    (void)project;
    return true;
}

bool selectorMatches(const Selector& s, const PrefabOverrideEntry& entry) {
    if (s.hasKind && s.kind != entry.kind) return false;
    if (entry.kind == Kind::RemovedGameObject) {
        // removed GameObjects are not in the scene: matched by name / registry id / prefab
        if (!s.hasKind && s.name.empty() && s.registryEntity == NULL_ENTITY) return false;
        if (s.entity != NULL_ENTITY && s.entity != entry.removal.instanceRoot) return false;
        if (!s.name.empty() && s.name != entry.removal.name) return false;
        if (s.registryEntity != NULL_ENTITY && s.registryEntity != entry.removal.registryEntity) return false;
        if (!s.prefab.empty() && s.prefab != entry.removal.path.lexically_normal()) return false;
        return true;
    }
    if (s.entity != NULL_ENTITY && s.entity != entry.entity) return false;
    if (s.hasComponent) {
        if (entry.kind == Kind::State || entry.kind == Kind::AddedGameObject) return false;
        if (s.component != entry.type) return false;
    }
    if (!s.properties.empty() && entry.kind != Kind::Property) return false;
    if (s.state && entry.kind != Kind::State) return false;
    if (!s.properties.empty() && !entry.properties.count("*")) {
        bool any = false;
        for (const std::string& name : s.properties) any = any || entry.properties.count(name);
        if (!any) return false;
    }
    if (s.state && !(s.state & entry.state)) return false;
    return true;
}

void prefabOverridesTool(const CallPtr& call) {
    PrefabWriteScope prefabWrites(call);
    Project* project = requireProject(call);
    if (!project) return;
    const Json& args = call->args();
    const std::string action = args.value("action", std::string());
    if (action != "apply" && action != "revert") {
        call->fail("action must be apply or revert.");
        return;
    }
    const bool apply = action == "apply";
    SceneProject* scene = sceneArg(call, project);
    if (!scene) return;
    const uint32_t sceneId = scene->id;
    std::string error;

    fs::path target;
    const std::string targetArg = args.value("target", std::string());
    if (!targetArg.empty()) {
        if (!apply) {
            call->fail("target is for apply only (revert always goes back to what the prefab gives).");
            return;
        }
        if (!prefabPathArg(project, targetArg, true, target, error, true)) {
            call->fail("target: " + error);
            return;
        }
    }
    if (apply && !project->canApplyPrefabOverrides(&error)) {
        call->fail("Cannot apply now: " + error);
        return;
    }

    const bool all = args.value("all", false);
    std::vector<Selector> selectors;
    Entity instanceEntity = NULL_ENTITY;
    if (args.contains("entity") && !args["entity"].is_null()) {
        instanceEntity = entityArg(call, scene, "entity");
        if (instanceEntity == NULL_ENTITY) return;
    }
    if (!all) {
        if (args.contains("overrides") && args["overrides"].is_array() && !args["overrides"].empty()) {
            for (const Json& item : args["overrides"]) {
                Selector selector;
                if (!parseSelector(call, project, scene, item, selector, error)) {
                    call->fail("overrides: " + error);
                    return;
                }
                selectors.push_back(selector);
            }
        } else if (instanceEntity != NULL_ENTITY) {
            // shorthand: the top-level entity / kind / component / properties / state
            Selector selector;
            Json item = Json::object();
            for (const char* key : {"kind", "component", "properties", "state", "name", "registry_entity", "prefab"}) {
                if (args.contains(key)) item[key] = args[key];
            }
            item["entity"] = instanceEntity;
            if (!parseSelector(call, project, scene, item, selector, error)) {
                call->fail(error);
                return;
            }
            selectors.push_back(selector);
        } else {
            call->fail("Say which overrides: all=true with entity (the whole instance), or entity (+ component / properties / state / kind) "
                       "for one GameObject's, or overrides=[selectors] as get_prefab_info lists them.");
            return;
        }
    } else if (instanceEntity == NULL_ENTITY) {
        call->fail("all=true needs entity: any GameObject of the instance (its outermost instance is used).");
        return;
    }

    // The outermost instances involved
    std::vector<Entity> roots;
    auto addRoot = [&](Entity entity) -> bool {
        const Entity root = outermostRoot(project, sceneId, entity);
        if (root == NULL_ENTITY) {
            call->fail(McpRefs::entityPath(scene, entity) + " is not part of a prefab instance" +
                       (project->isEditingPrefabAsset(sceneId, entity) ? " (in Prefab Mode the prefab's own GameObjects are the asset: edits there have no overrides)." : "."));
            return false;
        }
        if (std::find(roots.begin(), roots.end(), root) == roots.end()) roots.push_back(root);
        return true;
    };
    if (instanceEntity != NULL_ENTITY && !addRoot(instanceEntity)) return;
    for (const Selector& selector : selectors) {
        if (selector.entity != NULL_ENTITY && !addRoot(selector.entity)) return;
    }
    if (roots.empty()) {
        call->fail("Pass entity (a GameObject of the instance) so the removed GameObjects can be found.");
        return;
    }
    // an apply without target writes each instance's own prefab: inside the project once links are
    // resolved (the write itself goes beneath the project too: the call's write scope)
    if (apply && target.empty()) {
        for (Entity root : roots) {
            PrefabObjectRef ref;
            if (!project->findPrefabObject(sceneId, root, ref) || ref.path.empty()) continue;
            const fs::path file = ref.path.is_absolute() ? ref.path : project->getProjectPath() / ref.path;
            if (const std::string escape = McpRefs::writeEscape(project, file); !escape.empty()) {
                call->fail(escape);
                return;
            }
        }
    }

    const uint64_t since = Out::lastLogId();
    CommandHistory* history = CommandHandle::get(sceneId);

    // all without a target: the Overrides dropdown's Revert All / Apply All
    if (all && target.empty()) {
        std::vector<std::unique_ptr<Command>> steps;
        size_t count = 0;
        Json done = Json::array();
        for (Entity root : roots) {
            const std::vector<PrefabOverrideEntry> entries = project->getPrefabOverrideEntries(sceneId, root);
            count += entries.size();
            for (const PrefabOverrideEntry& entry : entries) done.push_back(entryJson(project, scene, entry, false, false));
            PrefabOverridesCmd* command = apply ? PrefabOverridesCmd::applyAll(project, sceneId, root) : PrefabOverridesCmd::revertAll(project, sceneId, root);
            if (command) steps.emplace_back(command);
        }
        if (count == 0) {
            call->ok("The instance has no overrides.", Json{{"scene_id", sceneId}, {"count", 0}});
            return;
        }
        if (steps.empty() || !runCommand(history, new PrefabOverridesCmd(project, sceneId, std::move(steps)))) {
            const std::string why = problemsSince(since);
            call->fail(std::string(apply ? "Apply All" : "Revert All") + " did nothing" + (why.empty() ? std::string(".") : ": " + why));
            return;
        }
        size_t remaining = 0;
        for (Entity root : roots) remaining += project->getPrefabOverrideEntries(sceneId, root).size();
        Json rootList = Json::array();
        for (Entity root : roots) rootList.push_back(McpRefs::entityJson(scene, root));
        if (prefabWrites.refused()) return;
        call->ok(std::string(apply ? "Applied all " : "Reverted all ") + std::to_string(count) + " override(s)" +
                 (apply ? " to the instance's own prefab (nested instances' overrides become its overrides of them)" : "") +
                 "; " + std::to_string(remaining) + " left (overrides referencing GameObjects outside the instance, and added GameObjects holding an instance of the prefab, cannot be applied). "
                 "One undo step." + (apply ? " Save the scene to write the prefab files." : ""),
                 Json{{"scene_id", sceneId}, {"action", action}, {"instances", rootList}, {"operations", done}, {"remaining", remaining}});
        return;
    }

    // Matching entries
    struct Pick {
        PrefabOverrideEntry entry;
        fs::path target;
    };
    std::vector<Pick> picks;
    std::vector<std::string> skipped;
    std::vector<PrefabOverrideEntry> everything;
    for (Entity root : roots) {
        const std::vector<PrefabOverrideEntry> entries = project->getPrefabOverrideEntries(sceneId, root);
        everything.insert(everything.end(), entries.begin(), entries.end());
    }
    std::vector<bool> used(selectors.size(), false);
    for (const PrefabOverrideEntry& source : everything) {
        PrefabOverrideEntry entry = source;
        bool matched = all;
        std::set<std::string> names;
        uint8_t stateBits = 0;
        bool whole = false;
        for (size_t s = 0; s < selectors.size(); s++) {
            if (!selectorMatches(selectors[s], entry)) continue;
            matched = true;
            used[s] = true;
            if (entry.kind == Kind::Property) {
                if (selectors[s].properties.empty() || entry.properties.count("*")) whole = true;
                for (const std::string& name : selectors[s].properties) {
                    if (entry.properties.count(name)) names.insert(name);
                }
            }
            if (entry.kind == Kind::State) stateBits |= selectors[s].state ? (selectors[s].state & entry.state) : entry.state;
        }
        if (!matched) continue;
        if (entry.kind == Kind::Property && !all && !whole) entry.properties = names;
        if (entry.kind == Kind::State && !all) entry.state = stateBits;

        Pick pick{entry, {}};
        if (apply) {
            const std::vector<fs::path> targets = entryTargets(project, sceneId, entry);
            if (!target.empty()) {
                if (std::find(targets.begin(), targets.end(), target) == targets.end()) {
                    std::string valid;
                    for (const fs::path& path : targets) valid += " " + generic(path);
                    if (all) {
                        skipped.push_back(entrySummary(scene, entry));
                        continue;
                    }
                    call->fail("target " + generic(target) + " is not an apply target of " + entrySummary(scene, entry) + ". Its targets:" + valid + ".");
                    return;
                }
                pick.target = target;
            } else if (!targets.empty()) {
                pick.target = targets.front();
            }
            std::string reason;
            if (!entryCanApply(project, sceneId, entry, pick.target, reason)) {
                if (all) {
                    skipped.push_back(entrySummary(scene, entry) + " (" + reason + ")");
                    continue;
                }
                call->fail("Cannot apply " + entrySummary(scene, entry) + " to " + generic(pick.target) + ": " + reason);
                return;
            }
        }
        picks.push_back(pick);
    }
    for (size_t s = 0; s < selectors.size(); s++) {
        if (used[s]) continue;
        std::string listed;
        size_t n = 0;
        for (const PrefabOverrideEntry& entry : everything) {
            if (n++ >= 25) { listed += " ..."; break; }
            listed += "\n  " + std::string(kindName(entry.kind)) + ": " + entrySummary(scene, entry);
        }
        call->fail("Override selector " + std::to_string(s) + " matches no override of the instance. Its overrides:" +
                   (listed.empty() ? std::string(" (none)") : listed));
        return;
    }
    if (picks.empty()) {
        call->fail(skipped.empty() ? std::string("Nothing to do: the instance has no matching overrides.")
                                   : "Nothing can be applied to " + generic(target) + ".");
        return;
    }

    // Steps in the order the Overrides dropdown uses
    std::vector<std::unique_ptr<Command>> steps;
    Json operations = Json::array();
    if (apply) {
        for (const Pick& pick : picks) {
            if (pick.entry.kind == Kind::AddedGameObject) {
                steps.emplace_back(new AddEntityToPrefabCmd(project, sceneId, pick.entry.entity, pick.entry.parent, pick.target));
            }
        }
        for (const Pick& pick : picks) {
            const PrefabOverrideEntry& e = pick.entry;
            const std::vector<std::string> names = e.properties.count("*") ? std::vector<std::string>() : std::vector<std::string>(e.properties.begin(), e.properties.end());
            switch (e.kind) {
                case Kind::State: steps.emplace_back(new PrefabOverrideCmd(project, sceneId, e.entity, Operation::ApplyState, ComponentType::PrefabComponent, {}, e.state, pick.target)); break;
                case Kind::Property: steps.emplace_back(new PrefabOverrideCmd(project, sceneId, e.entity, Operation::ApplyProperty, e.type, names, 0, pick.target)); break;
                case Kind::AddedComponent: steps.emplace_back(new PrefabOverrideCmd(project, sceneId, e.entity, Operation::ApplyAddedComponent, e.type, {}, 0, pick.target)); break;
                case Kind::RemovedComponent: steps.emplace_back(new PrefabOverrideCmd(project, sceneId, e.entity, Operation::ApplyRemovedComponent, e.type, {}, 0, pick.target)); break;
                default: break;
            }
        }
        for (const Pick& pick : picks) {
            if (pick.entry.kind == Kind::RemovedGameObject) {
                const PrefabRemovalInfo& r = pick.entry.removal;
                steps.emplace_back(new ApplyPrefabRemovalCmd(project, sceneId, r.path, r.instanceRoot, r.registryEntity, pick.target));
            }
        }
    } else {
        std::vector<Entity> added;
        for (const Pick& pick : picks) {
            if (pick.entry.kind == Kind::AddedGameObject) added.push_back(pick.entry.entity);
        }
        if (!added.empty()) steps.emplace_back(new DeleteEntityCmd(project, sceneId, added));
        for (const Pick& pick : picks) {
            if (pick.entry.kind == Kind::RemovedGameObject) {
                const PrefabRemovalInfo& r = pick.entry.removal;
                steps.emplace_back(new RevertPrefabRemovalCmd(project, sceneId, r.path, r.instanceRoot, r.registryEntity));
            }
        }
        for (const Pick& pick : picks) {
            const PrefabOverrideEntry& e = pick.entry;
            const std::vector<std::string> names = e.properties.count("*") ? std::vector<std::string>() : std::vector<std::string>(e.properties.begin(), e.properties.end());
            switch (e.kind) {
                case Kind::State: steps.emplace_back(new PrefabOverrideCmd(project, sceneId, e.entity, Operation::RevertState, ComponentType::PrefabComponent, {}, e.state)); break;
                case Kind::Property: steps.emplace_back(new PrefabOverrideCmd(project, sceneId, e.entity, Operation::RevertProperty, e.type, names)); break;
                case Kind::AddedComponent: steps.emplace_back(new PrefabOverrideCmd(project, sceneId, e.entity, Operation::RevertAddedComponent, e.type)); break;
                case Kind::RemovedComponent: steps.emplace_back(new PrefabOverrideCmd(project, sceneId, e.entity, Operation::RevertRemovedComponent, e.type)); break;
                default: break;
            }
        }
    }
    for (const Pick& pick : picks) {
        Json item = entryJson(project, scene, pick.entry, false, false);
        if (apply) item["target"] = generic(pick.target);
        operations.push_back(item);
    }

    if (!runCommand(history, new PrefabOverridesCmd(project, sceneId, std::move(steps)))) {
        const std::string why = problemsSince(since);
        call->fail(std::string(apply ? "Apply" : "Revert") + " did nothing" + (why.empty() ? std::string(".") : ": " + why));
        return;
    }
    size_t remaining = 0;
    for (Entity root : roots) {
        if (scene->scene->isEntityCreated(root)) remaining += project->getPrefabOverrideEntries(sceneId, root).size();
    }
    std::string text = std::string(apply ? "Applied " : "Reverted ") + std::to_string(picks.size()) + " override(s):";
    for (const Pick& pick : picks) text += "\n  " + entrySummary(scene, pick.entry) + (apply ? " -> " + generic(pick.target) : std::string());
    if (!skipped.empty()) text += "\nSkipped " + std::to_string(skipped.size()) + ".";
    text += "\nOne undo step; " + std::to_string(remaining) + " override(s) left." + (apply ? " Save the scene to write the prefab files." : "");
    Json data = {{"scene_id", sceneId}, {"action", action}, {"operations", operations}, {"remaining", remaining}};
    if (!skipped.empty()) data["skipped"] = skipped;
    if (prefabWrites.refused()) return;
    call->ok(text, data);
}

// ---- prefab_mode

int unsavedChoice(const std::string& name) {
    if (name == "save") return 1;
    if (name == "discard") return 2;
    if (name == "cancel") return 3;
    return -1;
}

void prefabModeTool(const CallPtr& call) {
    Project* project = requireProject(call);
    if (!project) return;
    const Json& args = call->args();
    const std::string action = args.value("action", std::string("status"));
    const std::string unsaved = args.value("unsaved", std::string("save"));
    const int choice = unsavedChoice(unsaved);
    if (choice < 0) {
        call->fail("unsaved must be save, discard or cancel.");
        return;
    }
    if (args.contains("auto_save") && args["auto_save"].is_boolean()) project->setPrefabAutoSave(args["auto_save"].get<bool>());
    if (args.contains("context") && args["context"].is_string()) {
        const std::string context = args["context"].get<std::string>();
        if (context == "normal") project->setPrefabContextVisibility(PrefabContextVisibility::Normal);
        else if (context == "gray") project->setPrefabContextVisibility(PrefabContextVisibility::Gray);
        else if (context == "hidden") project->setPrefabContextVisibility(PrefabContextVisibility::Hidden);
        else {
            call->fail("context must be normal, gray or hidden.");
            return;
        }
    }

    // Closes the stages above `keep` with the agent's choice; false (call failed) when cancelled
    auto closeAbove = [&](size_t keep) -> bool {
        if (project->getPrefabStageCount() <= keep) return true;
        std::vector<std::string> modified;
        for (size_t i = keep; i < project->getPrefabStageCount(); i++) {
            PrefabStage* stage = project->getPrefabStageAt(i);
            if (stage && project->hasUnsavedPrefabStage(stage->sceneProject.id)) modified.push_back(generic(stage->assetPath));
        }
        if (!modified.empty() && choice == 3) {
            std::string list;
            for (const std::string& path : modified) list += " " + path;
            call->fail("Prefab Mode has unsaved changes in:" + list + ". Pass unsaved=save or unsaved=discard.");
            return false;
        }
        bool closed = false;
        bool answered = false;
        const uint64_t since = Out::lastLogId();
        project->closePrefabStages(keep, [&](bool ok) { closed = ok; answered = true; }, choice);
        if (!answered || !closed) {
            const std::string why = problemsSince(since);
            call->fail("Could not leave the prefab stage(s)" + (why.empty() ? std::string(".") : ": " + why));
            return false;
        }
        return true;
    };

    if (action == "status") {
        Json data = prefabModeJson(project);
        call->ok(data["active"].get<bool>() ? std::to_string(data["stages"].size()) + " prefab stage(s) open; the shown one is scene " +
                 std::to_string(data.value("shown_stage_scene_id", 0u)) + "." : "Not in Prefab Mode.", data);
        return;
    }

    if (action == "save") {
        uint32_t stageId = 0;
        if (args.contains("scene") && !args["scene"].is_null()) {
            std::string error;
            SceneProject* scene = McpRefs::resolveScene(project, args["scene"], error);
            if (!scene) { call->fail(error); return; }
            stageId = scene->id;
        } else if (PrefabStage* top = project->getActivePrefabStage()) {
            stageId = top->sceneProject.id;
        }
        PrefabStage* stage = stageId ? project->getPrefabStage(stageId) : nullptr;
        if (!stage) {
            call->fail("Not a prefab stage. Open one with prefab_mode action=open; scene must be a stage's scene id.");
            return;
        }
        const bool allowDataLoss = call->args().value("allow_data_loss", false);
        if (!allowDataLoss && project->isPrefabStageDegraded(stageId)) {
            const Prefab* prefab = project->getPrefab(stage->assetPath);
            failDegraded(*call, generic(stage->assetPath), prefab ? prefab->loadReport : LoadReport());
            return;
        }
        const uint64_t since = Out::lastLogId();
        if (!project->savePrefabStage(stageId, allowDataLoss ? DegradedSavePolicy::SaveAnyway : DegradedSavePolicy::Refuse)) {
            const std::string why = problemsSince(since);
            call->fail("Saving the prefab failed" + (why.empty() ? std::string(".") : ": " + why));
            return;
        }
        call->ok("Saved " + generic(stage->assetPath) + "; its instances in loaded scenes follow (keeping their overrides).",
                 stageJson(project, *stage));
        return;
    }

    if (action == "close") {
        const size_t keep = static_cast<size_t>(std::max(0, args.value("depth", 0)));
        if (project->getPrefabStageCount() == 0) {
            call->ok("Not in Prefab Mode.", prefabModeJson(project));
            return;
        }
        if (!closeAbove(keep)) return;
        Json data = prefabModeJson(project);
        call->ok(keep == 0 ? "Left Prefab Mode; scene " + std::to_string(project->getSelectedSceneId()) + " is selected."
                           : "Closed the stages above depth " + std::to_string(keep) + ".", data);
        return;
    }

    if (action != "open") {
        call->fail("action must be open, close, save or status.");
        return;
    }

    fs::path relative;
    std::string error;
    const std::string prefabArg = args.value("prefab", std::string());
    uint32_t contextSceneId = NULL_PROJECT_SCENE;
    Entity contextRoot = NULL_ENTITY;
    bool inContext = false;
    if (args.contains("entity") && !args["entity"].is_null()) {
        SceneProject* scene = sceneArg(call, project);
        if (!scene) return;
        const Entity entity = entityArg(call, scene, "entity");
        if (entity == NULL_ENTITY) return;
        // An instance root opens its own prefab, a member the prefab it belongs to, around its instance
        fs::path path;
        Entity root = NULL_ENTITY;
        if (project->isPrefabInstanceRoot(scene->id, entity)) {
            path = rootPrefabPath(project, scene, entity);
            root = entity;
        } else {
            path = project->findPrefabPathFor(scene->id, entity);
            const Prefab* prefab = path.empty() ? nullptr : project->getPrefab(path);
            root = prefab ? prefab->getRootEntity(scene->id, entity) : NULL_ENTITY;
        }
        if (path.empty() || root == NULL_ENTITY) {
            call->fail(McpRefs::entityPath(scene, entity) + " is not part of a (live) prefab instance; pass prefab=<path> to open an asset.");
            return;
        }
        if (!prefabArg.empty()) {
            // another level of that instance (a variant's base, the nested prefab...)
            if (!prefabPathArg(project, prefabArg, true, relative, error)) { call->fail("prefab: " + error); return; }
        } else {
            relative = path;
        }
        contextSceneId = scene->id;
        contextRoot = root;
        inContext = args.value("in_context", true);
    } else {
        if (!prefabPathArg(project, prefabArg, true, relative, error)) {
            call->fail("Pass prefab (a .prefab path, opened in isolation) or entity (a GameObject of an instance, opened in context). " + error);
            return;
        }
    }
    if (!project->loadPrefabAsset(relative)) {
        call->fail("The prefab \"" + generic(relative) + "\" could not be read.");
        return;
    }
    // From an instance in a stage the new stage goes on top of that one, else the stack is replaced
    const size_t keep = contextSceneId != NULL_PROJECT_SCENE ? project->getPrefabStageDepth(contextSceneId) : 0;
    // (Project::openPrefabStage's own test: the shown stage opened the same way is only shown;
    // anything else closes the stages above `keep` first, which must not ask)
    bool alreadyShown = false;
    if (project->getPrefabStageCount() == keep + 1) {
        PrefabStage* top = project->getActivePrefabStage();
        const Entity wantedRoot = inContext ? contextRoot : NULL_ENTITY;
        const uint32_t wantedScene = wantedRoot != NULL_ENTITY ? contextSceneId : NULL_PROJECT_SCENE;
        alreadyShown = top && top->assetPath.lexically_normal() == relative.lexically_normal() &&
                       top->contextSceneId == wantedScene && top->contextRoot == wantedRoot;
    }
    if (!alreadyShown && !closeAbove(keep)) return;

    const uint64_t since = Out::lastLogId();
    project->openPrefabStage(relative, contextSceneId, contextRoot, inContext);
    PrefabStage* stage = project->getActivePrefabStage();
    if (!stage || stage->assetPath.lexically_normal() != relative.lexically_normal()) {
        const std::string why = problemsSince(since);
        call->fail("Prefab Mode did not open " + generic(relative) + (why.empty() ? std::string(".") : ": " + why));
        return;
    }
    Json data = stageJson(project, *stage);
    data["auto_save"] = project->isPrefabAutoSave();
    const uint32_t stageId = stage->sceneProject.id;
    call->ok("Prefab Mode: " + generic(relative) + " is open as scene " + std::to_string(stageId) +
             (stage->inContext() ? " (in context)" : " (in isolation)") + ". Edit it with any scene/GameObject tool passing scene=" +
             std::to_string(stageId) + " (paths start at the prefab root \"" + McpRefs::entityPath(&stage->sceneProject, stage->root) +
             "\"); those edits change the asset, not overrides. " +
             (project->isPrefabAutoSave() ? "Auto Save writes the asset ~0.35 s after each edit; " : "Auto Save is off: save with action=save; ") +
             "undo with undo scene=" + std::to_string(stageId) + ". Leave with action=close.", data);
}

// list_prefabs
void listPrefabsTool(const CallPtr& call) {
    Project* project = requireProject(call);
    if (!project) return;
    const Json& args = call->args();
    const std::string filter = lower(args.value("filter", std::string()));
    const bool withUsage = args.value("usage", true);
    std::map<std::string, PrefabUsage> usages;
    if (withUsage) usages = project->collectPrefabUsage();

    std::vector<fs::path> files = project->findPrefabFiles();
    std::vector<fs::path> relatives;
    for (const fs::path& file : files) {
        fs::path relative = file.is_absolute() ? project->normalizeToProjectRelative(file) : file;
        relatives.push_back(relative.lexically_normal());
    }
    std::sort(relatives.begin(), relatives.end());
    Json list = Json::array();
    std::string text;
    for (const fs::path& relative : relatives) {
        if (!filter.empty() && lower(generic(relative)).find(filter) == std::string::npos) continue;
        const std::string guid = project->getPrefabGuid(relative);
        Json item = {{"path", generic(relative)}, {"guid", guid}};
        const fs::path base = project->getPrefabFileBase(relative);
        if (!base.empty()) item["base"] = generic(base);
        if (project->isStandalonePrefab(relative)) item["standalone"] = true;
        if (withUsage) {
            if (const PrefabUsage* usage = findUsage(usages, guid, relative)) {
                item["instances"] = usage->totalInstances();
                if (!usage->nestedIn.empty()) {
                    Json nestedIn = Json::array();
                    for (const fs::path& path : usage->nestedIn) nestedIn.push_back(generic(path));
                    item["nested_in"] = nestedIn;
                }
            } else {
                item["instances"] = 0;
            }
        }
        text += "\n" + generic(relative) + (base.empty() ? std::string() : " (variant of " + generic(base) + ")") +
                (withUsage ? ", " + std::to_string(item["instances"].get<size_t>()) + " instance(s)" : std::string());
        list.push_back(item);
    }
    Json missing = Json::array();
    for (const auto& [key, usage] : usages) {
        std::error_code ec;
        if (usage.totalMissing() > 0 && !fs::exists(project->getProjectPath() / usage.path, ec)) {
            missing.push_back({{"guid", usage.guid}, {"last_path", generic(usage.path)}, {"missing_instances", usage.totalMissing()}});
        }
    }
    Json data = {{"prefabs", list}, {"count", list.size()}};
    if (!missing.empty()) data["missing"] = missing;
    call->ok(std::to_string(list.size()) + " prefab(s)" + (missing.empty() ? std::string() : ", " + std::to_string(missing.size()) + " missing") +
             ":" + (text.empty() ? std::string(" (none)") : text), data);
}

// set_standalone_prefab
void setStandalonePrefabTool(const CallPtr& call) {
    Project* project = requireProject(call);
    if (!project) return;
    const Json& args = call->args();
    fs::path relative;
    std::string error;
    if (!prefabPathArg(project, args.value("prefab", std::string()), true, relative, error, true)) {
        call->fail("prefab: " + error);
        return;
    }
    const bool standalone = args.value("standalone", true);
    std::vector<fs::path> paths = project->getStandalonePrefabs();
    auto it = std::find_if(paths.begin(), paths.end(), [&](const fs::path& p) { return p.lexically_normal() == relative; });
    const bool listed = it != paths.end();
    if (standalone == listed) {
        call->ok(listed ? "Already standalone." : "Not standalone.", Json{{"prefab", generic(relative)}, {"standalone", listed}});
        return;
    }
    if (standalone) paths.push_back(relative);
    else paths.erase(it);
    project->setStandalonePrefabs(std::move(paths));
    project->saveProjectFile();
    call->ok(standalone ? "Listed " + generic(relative) + " as standalone: it is built (and loadable by path from scripts) without a scene instance."
                        : "Removed " + generic(relative) + " from the standalone prefabs.",
             Json{{"prefab", generic(relative)}, {"standalone", standalone}});
}

} // namespace

void registerPrefabTools(McpRegistry& registry) {
    using namespace schema;

    const Json prefabRef = assetPath("");

    {
        ToolDef tool;
        tool.name = "create_prefab";
        tool.title = "Create Prefab";
        tool.description =
            "Create Prefab: saves the GameObject (with children) as a new .prefab (never overwrites) and makes "
            "it the instance root (same id). Several entities: one each, <directory>/<name>.prefab. An instance "
            "root becomes an original prefab (variants: create_prefab_variant); an instance member is refused "
            "(unpack_prefab first).";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef()},
            {"entities", entities()},
            {"path", assetPath("One root only.")},
            {"directory", withDefault(assetPath("Folder used when path is omitted."), "prefabs")}
        });
        tool.allowedWhilePlaying = false;
        tool.handler = createPrefabTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "create_prefab_variant";
        tool.title = "Create Prefab Variant";
        tool.description =
            "Unity's Prefab Variant: a prefab on top of a base (base edits reach it unless overridden). From entity "
            "(outermost instance root): it becomes an instance of the variant, which takes its overrides and added "
            "GameObjects. From base: the asset only (undo scope=project).";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef("")},
            {"base", assetPath("Base .prefab (instead of entity).")},
            {"path", assetPath("Default \"<base> Variant.prefab\" next to base.")}
        });
        tool.allowedWhilePlaying = false;
        tool.handler = createPrefabVariantTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "instantiate_prefab";
        tool.title = "Instantiate Prefab";
        tool.description =
            "Place a prefab (or variant) instance in a scene or prefab stage and select it; name and local placement "
            "default to the prefab root's. Under another instance's GameObject it is an added-GameObject override "
            "(nest_in_parent_prefab: part of that prefab).";
        tool.inputSchema = object({
            {"prefab", prefabRef},
            {"scene", sceneRef()},
            {"parent", entityRef()},
            {"position", vec3("Local position.")},
            {"name", string("")},
            {"nest_in_parent_prefab", withDefault(boolean(""), false)}
        }, {"prefab"});
        tool.allowedWhilePlaying = false;
        tool.handler = instantiatePrefabTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "replace_prefab";
        tool.title = "Replace Prefab";
        tool.description =
            "Replace Prefab Asset: an outermost (or added) instance root or a Missing Prefab root becomes an "
            "instance of another prefab, keeping id, parent, name, pose. Overrides move to GameObjects matched "
            "by name path (name), sibling-index path (hierarchy) or root only (none); dropped ones are logged.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef("")},
            {"entities", entities()},
            {"prefab", assetPath("New .prefab.")},
            {"match", withDefault(stringEnum("", {"name", "hierarchy", "none"}), "name")},
            {"keep_overrides", withDefault(boolean("false: keep only default overrides."), true)},
            {"rename_root", withDefault(boolean("Rename a root named after the old prefab."), true)}
        }, {"prefab"});
        tool.annotations.destructive = true;
        tool.allowedWhilePlaying = false;
        tool.handler = replacePrefabTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "unpack_prefab";
        tool.title = "Unpack Prefab";
        tool.description =
            "Unpack: the outermost instance holding entity stops following its prefab (GameObjects, ids, values "
            "stay). One level by default (nested instances stay; a variant instance becomes its base's); "
            "completely: plain GameObjects all the way down. Missing Prefab roots become plain.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef("Any GameObject of the instance.")},
            {"entities", entities()},
            {"completely", withDefault(boolean(""), false)}
        });
        tool.annotations.destructive = true;
        tool.allowedWhilePlaying = false;
        tool.handler = unpackPrefabTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "add_to_prefab";
        tool.title = "Add GameObject to Prefab";
        tool.description =
            "Apply an added GameObject: moves it (under parent) into that instance's prefab, so every instance "
            "gets it. target: one of parent's apply_targets (an outer prefab gets it as an override of its "
            "nested instance). Written when the scene is saved.";
        tool.inputSchema = object({
            {"allow_data_loss", boolean("")},
            {"scene", sceneRef()},
            {"entity", entityRef("")},
            {"parent", entityRef("Instance GameObject (default: current parent).")},
            {"target", assetPath("Default: parent's own prefab.")}
        }, {"entity"});
        tool.allowedWhilePlaying = false;
        tool.handler = addToPrefabTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "remove_from_prefab";
        tool.title = "Remove GameObject from Prefab";
        tool.description =
            "Remove a GameObject (with children) from its prefab asset: other instances lose it, here it stays plain "
            "(a nested instance root leaves the outer prefab). For this instance only, delete it. Written when the "
            "scene is saved.";
        tool.inputSchema = object({
            {"allow_data_loss", boolean("")},
            {"scene", sceneRef()},
            {"entity", entityRef("Instance member (not the root).")}
        }, {"entity"});
        tool.annotations.destructive = true;
        tool.allowedWhilePlaying = false;
        tool.handler = removeFromPrefabTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "get_prefab_info";
        tool.title = "Get Prefab Info";
        tool.description =
            "Prefab status of a GameObject or a .prefab. entity: missing_prefab, owner, nesting (outermost "
            "first), instance_root, prefab, apply_targets (innermost first), the outermost instance's overrides "
            "(pass entries to prefab_overrides as is). prefab: path, guid, variant chain, variants, nested, "
            "standalone, open_in_prefab_mode, usage, contents.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef()},
            {"prefab", assetPath("Instead of entity.")},
            {"only_entity", withDefault(boolean("Only this GameObject's overrides."), false)},
            {"values", withDefault(boolean("Add current vs prefab values."), false)},
            {"usage", withDefault(boolean("Reads closed scene files."), true)},
            {"contents", withDefault(boolean("List the asset's GameObjects."), false)},
            {"limit", withDefault(integer("Max overrides/contents.", 1, 5000), 200)}
        });
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = getPrefabInfoTool;
        registry.addTool(std::move(tool));
    }
    {
        const Json properties = array(string(""), "");
        const Json state = array(stringEnum("", STATE_NAMES), "");
        const Json removedName = string("Removed GameObject: name in prefab.");
        const Json removedId = integer("Removed GameObject: file id in prefab.");
        ToolDef tool;
        tool.name = "prefab_overrides";
        tool.title = "Apply / Revert Prefab Overrides";
        tool.description =
            "Apply (into the asset; instances follow) or revert instance overrides, chosen by all=true + entity "
            "(Apply/Revert All of its outermost instance), entity [+ kind, component, properties, state], or "
            "overrides (get_prefab_info.overrides entries; removed GameObjects by name or registry_entity). "
            "target: one of the entry's apply_targets (default the innermost; an outer one applies it as an "
            "override there). Written when a scene using the prefab is saved.";
        tool.inputSchema = object({
            {"allow_data_loss", boolean("")},
            {"action", stringEnum("", {"apply", "revert"})},
            {"scene", sceneRef()},
            {"entity", entityRef()},
            {"all", withDefault(boolean(""), false)},
            {"kind", stringEnum("", KIND_NAMES)},
            {"component", componentName()},
            {"properties", properties},
            {"state", state},
            {"name", removedName},
            {"registry_entity", removedId},
            {"overrides", array(anyObject(""), "get_prefab_info.overrides entries.")},
            {"target", assetPath("apply: prefab to apply to.")}
        }, {"action"});
        tool.annotations.destructive = true;
        tool.allowedWhilePlaying = false;
        tool.handler = prefabOverridesTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "prefab_mode";
        tool.title = "Prefab Mode";
        tool.description =
            "Prefab Mode. open: prefab (isolation) or entity (its prefab in context; stacks inside a stage); "
            "returns the stage's scene_id for the scene/GameObject/component tools and undo (paths from the "
            "prefab root). Edits change the asset; nested instances record overrides. auto_save (default on) "
            "writes ~0.35 s after an edit; save writes now. close keeps depth stages. status lists stages.";
        tool.inputSchema = object({
            {"allow_data_loss", boolean("")},
            {"action", withDefault(stringEnum("", {"open", "close", "save", "status"}), "status")},
            {"prefab", assetPath("open: .prefab; with entity: another level of its chain.")},
            {"scene", sceneRef("open: entity's scene; save: stage (default: shown).")},
            {"entity", entityRef("")},
            {"in_context", withDefault(boolean(""), true)},
            {"depth", withDefault(integer("close: stages kept.", 0, 64), 0)},
            {"unsaved", withDefault(stringEnum("cancel: error.", {"save", "discard", "cancel"}), "save")},
            {"auto_save", boolean("")},
            {"context", stringEnum("Scene shown behind the stage.", {"normal", "gray", "hidden"})}
        });
        tool.annotations.destructive = true;
        tool.handler = prefabModeTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "list_prefabs";
        tool.title = "List Prefabs";
        tool.description =
            "Every .prefab: path, guid, base (variants), standalone, instance count across scenes, prefabs nesting "
            "it; plus Missing Prefabs (GUIDs without a file).";
        tool.inputSchema = object({
            {"filter", string("Path substring (case-insensitive).")},
            {"usage", withDefault(boolean("Reads closed scene files."), true)}
        });
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = listPrefabsTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "set_standalone_prefab";
        tool.title = "Set Standalone Prefab";
        tool.description =
            "Add/remove a prefab in the project's standalone list: a prefab no scene places is built only if listed "
            "(or referenced by a script property), so scripts can PrefabAsset.load it by path. Saves project.yaml "
            "(not undoable).";
        tool.inputSchema = object({
            {"prefab", prefabRef},
            {"standalone", withDefault(boolean("false: remove."), true)}
        }, {"prefab"});
        tool.annotations.idempotent = true;
        tool.handler = setStandalonePrefabTool;
        registry.addTool(std::move(tool));
    }
}

}
