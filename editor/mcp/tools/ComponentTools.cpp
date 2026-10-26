// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// W1: the Inspector: get_entity, components (add, remove, reset, copy/paste values),
// set_properties (every property type), set_transform, list_component_types
#include "McpTools.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpValues.h"

#include "Out.h"
#include "Project.h"
#include "Stream.h"
#include "command/type/AddComponentCmd.h"
#include "command/type/ComponentValuesCmd.h"
#include "command/type/MeshChangeCmd.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/ObjectTransformCmd.h"
#include "command/type/RemoveComponentCmd.h"
#include "component/MeshComponent.h"
#include "registry/TagManager.h"
#include "subsystem/MeshSystem.h"
#include "util/EntityClipboard.h"
#include "util/FileUtils.h"
#include "util/Prefab.h"
#include "window/dialog/ComponentAddDialog.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <set>

namespace jasperengine::editor::mcp {

namespace {

using namespace schema;

constexpr size_t ELIDE_CHARS = 400;

SceneProject* sceneArg(const CallPtr& call) {
    std::string error;
    const Json& args = call->args();
    SceneProject* scene = McpRefs::resolveScene(call->ctx().project(), args.contains("scene") ? args["scene"] : Json(), error);
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

Json componentsArgSchema(const std::string& description) {
    Json schema = {{"type", {"string", "array"}}, {"items", {{"type", "string"}}}};
    if (!description.empty()) schema["description"] = description;
    return schema;
}

bool wildcardMatch(const std::string& pattern, const std::string& text) {
    size_t p = 0, t = 0, star = std::string::npos, mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
            p++;
            t++;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = t;
        } else if (star != std::string::npos) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') p++;
    return p == pattern.size();
}

bool componentListArg(const Json& value, std::vector<ComponentType>& out, std::string& error) {
    out.clear();
    Json list = value.is_array() ? value : Json::array({value});
    if (list.empty()) {
        error = "component is empty.";
        return false;
    }
    for (const Json& item : list) {
        ComponentType type;
        if (!McpRefs::resolveComponent(item, type, error)) return false;
        if (std::find(out.begin(), out.end(), type) == out.end()) out.push_back(type);
    }
    return true;
}

// Elides long strings in YAML-derived values (mesh buffers, embedded textures)
void elideLongStrings(Json& value) {
    if (value.is_string() && value.get<std::string>().size() > 2000) {
        value = "<" + std::to_string(value.get<std::string>().size()) + " chars elided>";
    } else if (value.is_array() || value.is_object()) {
        for (auto& item : value) elideLongStrings(item);
    }
}

std::string layerName(int layer) {
    const std::string name = TagManager::layerToName(layer);
    return name.empty() ? "Layer " + std::to_string(layer) : name;
}

// ------------------------------------------------------------------ get_entity

void getEntityTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    SceneProject* scene = sceneArg(call);
    if (!scene) return;
    if (args.value("runtime", false)) scene = McpRefs::runtimeScene(project, scene);
    std::string error;
    const Entity entity = McpRefs::resolveEntity(scene, args.contains("entity") ? args["entity"] : Json(), error);
    if (entity == NULL_ENTITY) {
        call->fail(error);
        return;
    }
    Scene* registry = scene->scene;
    if (scene->playState == ScenePlayState::STOPPED) McpValues::refreshTransforms(scene);
    const std::string detail = args.value("detail", std::string("values"));
    if (detail != "summary" && detail != "values" && detail != "full" && detail != "yaml") {
        call->fail("detail must be summary, values, full or yaml.");
        return;
    }

    std::vector<ComponentType> filter;
    if (args.contains("components") && !args["components"].is_null()) {
        if (!componentListArg(args["components"], filter, error)) {
            call->fail(error);
            return;
        }
    }
    std::vector<std::string> propertyGlobs;
    if (args.contains("properties") && args["properties"].is_array()) {
        for (const Json& item : args["properties"]) {
            if (item.is_string()) propertyGlobs.push_back(item.get<std::string>());
        }
    }

    Json data = McpRefs::entityJson(scene, entity);
    data["scene_id"] = scene->id;
    data["active"] = registry->isEntityActive(entity);
    data["active_in_hierarchy"] = registry->isEntityActiveInHierarchy(entity);
    data["tag"] = registry->getEntityTag(entity);
    data["layer"] = registry->getEntityLayer(entity);
    data["layer_name"] = layerName(registry->getEntityLayer(entity));
    Transform* transform = registry->findComponent<Transform>(entity);
    data["parent"] = McpValues::entityToJson(scene, transform ? transform->parent : NULL_ENTITY);
    {
        Json children = Json::array();
        size_t count = 0;
        auto transforms = registry->getComponentArray<Transform>();
        const size_t size = transforms ? transforms->size() : 0;
        for (size_t i = 0; transform && i < size; i++) {
            if (transforms->getComponentFromIndex(i).parent != entity) continue;
            const Entity child = transforms->getEntity(i);
            if (count++ < 50) children.push_back(Json{{"id", child}, {"name", registry->getEntityName(child)}});
        }
        data["children"] = children;
        data["child_count"] = count;
    }
    const std::vector<Entity> selected = project->getSelectedEntities(scene->id);
    data["selected"] = std::find(selected.begin(), selected.end(), entity) != selected.end();
    if (scene->mainCamera == entity) data["main_camera"] = true;
    if (transform) {
        data["world"] = {
            {"position", McpValues::toJson(transform->worldPosition)},
            {"rotation", McpValues::toJson(transform->worldRotation)},
            {"scale", McpValues::toJson(transform->worldScale)}
        };
    }

    // prefab instance membership and overrides
    Prefab::ObjectOverrides overrides;
    bool hasOverrides = false;
    {
        PrefabObjectRef ref;
        if (project->findPrefabObject(scene->id, entity, ref) && ref.instance) {
            Json prefab = {
                {"path", FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(ref.path))},
                {"instance_root", McpRefs::entityJson(scene, ref.instance->rootEntity)},
                {"is_root", ref.isRoot}
            };
            hasOverrides = project->getPrefabEditOverrides(scene->id, entity, overrides);
            if (hasOverrides && !overrides.empty()) {
                Json props = Json::object();
                for (const auto& [type, names] : overrides.properties) {
                    props[McpRefs::componentName(type)] = Json(std::vector<std::string>(names.begin(), names.end()));
                }
                Json state = Json::array();
                if (overrides.state & PrefabObjectOverrides::StateName) state.push_back("name");
                if (overrides.state & PrefabObjectOverrides::StateActive) state.push_back("active");
                if (overrides.state & PrefabObjectOverrides::StateTag) state.push_back("tag");
                if (overrides.state & PrefabObjectOverrides::StateLayer) state.push_back("layer");
                Json added = Json::array();
                for (ComponentType type : overrides.addedComponents) added.push_back(McpRefs::componentName(type));
                Json removed = Json::array();
                for (ComponentType type : overrides.removedComponents) removed.push_back(McpRefs::componentName(type));
                prefab["overrides"] = {{"properties", props}, {"state", state}, {"added_components", added}, {"removed_components", removed}};
            }
            data["prefab"] = prefab;
        } else if (project->isEditingPrefabAsset(scene->id, entity)) {
            data["prefab"] = {{"editing_asset", true}};
        }
    }

    Json components = Json::object();
    std::string text = data["path"].get<std::string>() + " [" + std::to_string(entity) + "]" +
                       (data["active"].get<bool>() ? "" : " (inactive)") + " components:";
    size_t elided = 0;
    // a mesh's submesh array is fixed-size: only the used submeshes are its properties
    unsigned int usedSubmeshes = 0;
    if (MeshComponent* mesh = registry->findComponent<MeshComponent>(entity)) usedSubmeshes = mesh->numSubmeshes;
    auto unusedSubmesh = [usedSubmeshes](ComponentType type, const std::string& name) {
        if (type != ComponentType::MeshComponent || name.compare(0, 10, "submeshes[") != 0) return false;
        return std::strtoul(name.c_str() + 10, nullptr, 10) >= usedSubmeshes;
    };
    // the GUIDs of the asset fields read ("<Component>.<property>" -> GUID, S04)
    Json assetGuids = Json::object();
    for (ComponentType type : Catalog::findComponents(registry, entity)) {
        if (!filter.empty() && std::find(filter.begin(), filter.end(), type) == filter.end()) continue;
        const std::string name = McpRefs::componentName(type);
        text += " " + name;
        if (detail == "summary") {
            components[name] = Json::object();
            continue;
        }
        if (detail == "yaml") {
            Json yaml = McpValues::yamlToJson(EntityClipboard::encodeComponent(registry, entity, type));
            elideLongStrings(yaml);
            components[name] = yaml;
            continue;
        }
        McpValues::ReadContext ctx;
        ctx.project = project;
        ctx.scene = scene;
        ctx.entity = entity;
        ctx.component = type;
        std::set<std::string> overridden;
        auto overrideIt = overrides.properties.find(type);
        if (hasOverrides && overrideIt != overrides.properties.end()) overridden = overrideIt->second;
        Json props = Json::object();
        for (const auto& [propName, property] : Catalog::findEntityProperties(registry, entity, type)) {
            bool explicitlyAsked = false;
            if (!propertyGlobs.empty()) {
                bool match = false;
                for (const std::string& glob : propertyGlobs) {
                    if (wildcardMatch(glob, propName)) {
                        match = true;
                        explicitlyAsked = glob.find_first_of("*?") == std::string::npos;
                        break;
                    }
                }
                if (!match) continue;
            }
            if (!property.ref && property.type != PropertyType::Custom) continue;
            if (unusedSubmesh(type, propName)) continue;
            Json value = McpValues::readProperty(ctx, propName, property);
            std::string guid;
            if (property.ref && property.type == PropertyType::Asset) {
                guid = static_cast<AssetRef*>(property.ref)->guid.str();
            } else if (property.ref && property.type == PropertyType::Texture) {
                guid = static_cast<Texture*>(property.ref)->getPathGuid(0).str();
            } else if (property.ref && property.type == PropertyType::Shader) {
                guid = Stream::shaderRefGuids(*static_cast<ShaderRef*>(property.ref));
            }
            if (!guid.empty()) assetGuids[name + "." + propName] = guid;
            if (detail == "values") {
                if (property.type == PropertyType::Custom && !explicitlyAsked && value.dump().size() > ELIDE_CHARS) {
                    props[propName] = Json{{"$elided", std::to_string(value.is_array() ? value.size() : value.dump().size()) +
                                                       (value.is_array() ? " items" : " chars") +
                                                       "; pass properties: [\"" + propName + "\"] to read it"}};
                    elided++;
                } else {
                    props[propName] = value;
                }
                continue;
            }
            Json entry = {{"type", McpValues::propertyTypeName(property.type)}, {"value", value}};
            if (!guid.empty()) entry["guid"] = guid;
            if (property.def && property.type != PropertyType::Custom) {
                entry["default"] = McpValues::readProperty(ctx, propName, property, true);
            }
            if (property.type == PropertyType::Enum) {
                if (const McpValues::EnumOptions* options = McpValues::enumOptions(type, propName)) {
                    entry["options"] = McpValues::enumNames(*options);
                }
            } else if (property.type == PropertyType::Ease) {
                entry["options"] = McpValues::enumNames(McpValues::easeOptions());
            }
            if (overridden.count(propName) || overridden.count(PrefabObjectOverrides::WHOLE_COMPONENT)) entry["overridden"] = true;
            props[propName] = entry;
        }
        // explicitly asked names that are not Catalog properties: the component encoding's keys
        for (const std::string& glob : propertyGlobs) {
            if (glob.find_first_of("*?") != std::string::npos || props.contains(glob)) continue;
            bool found = false;
            Json value = McpValues::readYamlProperty(ctx, glob, found);
            if (found) props[glob] = detail == "full" ? Json{{"type", "custom"}, {"value", value}} : value;
        }
        components[name] = props;
    }
    data["components"] = components;
    // a fluid's runtime state (read only; FluidStats, the matching preset)
    if (filter.empty() || std::find(filter.begin(), filter.end(), ComponentType::FluidSimulationComponent) != filter.end()) {
        const Json fluid = McpValues::fluidStatsToJson(registry, entity);
        if (!fluid.is_null()) {
            data["fluid_stats"] = fluid;
            text += ". fluid_stats: " + fluid.dump();
        }
    }
    if (!assetGuids.empty()) data["asset_guids"] = assetGuids;
    // what its draws read of the scene (phase 3) in their current variants, and the cost
    const Json sceneInputs = McpValues::entitySceneInputsToJson(registry, entity);
    if (!sceneInputs.empty()) {
        uint32_t reads = 0;
        bool error = false;
        for (const Json& entry : sceneInputs) {
            for (const char* key : {"reads", "extra_pass_reads"}) {
                if (!entry.contains(key)) continue;
                for (const Json& name : entry[key]) reads |= name == "scenedepth" ? ShaderRequire_SceneDepth : ShaderRequire_SceneColor;
            }
            error = error || entry.contains("error");
        }
        data["scene_inputs"] = sceneInputs;
        data["scene_inputs_cost"] = Catalog::getSceneInputsCost(reads);
        text += ". Reads the scene " + Catalog::getSceneInputsLabel(reads) + " (" + Catalog::getSceneInputsCost(reads, true) + ")" +
                (error ? "; ERROR: a reader draws in the opaque queue (scene_inputs[].error, .fix)" : "");
    }
    if (elided) data["note"] = std::to_string(elided) + " large custom value(s) elided; name them in properties to read them.";
    call->ok(text, data);
}

// ------------------------------------------------------------------ set_properties

Json readBack(Project* project, SceneProject* scene, Entity entity, const std::vector<McpValues::Change>& changes) {
    Json out = Json::object();
    for (const McpValues::Change& change : changes) {
        McpValues::ReadContext ctx;
        ctx.project = project;
        ctx.scene = scene;
        ctx.entity = entity;
        ctx.component = change.component;
        const std::string component = McpRefs::componentName(change.component);
        if (!out.contains(component)) out[component] = Json::object();
        const PropertyData property = Catalog::findProperty(scene->scene, entity, change.component, change.property);
        Json value;
        if (property.ref || property.type == PropertyType::Custom) {
            value = McpValues::readProperty(ctx, change.property, property);
        } else {
            bool found = false;
            value = McpValues::readYamlProperty(ctx, change.property, found);
        }
        if (value.dump().size() > 4 * ELIDE_CHARS) value = Json{{"$elided", "read it with get_entity"}};
        out[component][change.property] = value;
    }
    return out;
}

void setPropertiesTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    SceneProject* scene = sceneArg(call);
    if (!scene) return;
    std::vector<Entity> entities;
    std::string error;
    if (!McpRefs::resolveEntityArgs(scene, args, entities, error)) {
        call->fail(error);
        return;
    }
    std::vector<McpValues::Change> changes;
    if (args.contains("changes")) {
        if (!McpValues::parseChanges(args["changes"], changes, error)) {
            call->fail(error);
            return;
        }
    }
    if (args.contains("values")) {
        std::vector<McpValues::Change> more;
        if (!McpValues::parseChanges(args["values"], more, error)) {
            call->fail(error);
            return;
        }
        changes.insert(changes.end(), more.begin(), more.end());
    }
    if (changes.empty()) {
        call->fail("Pass changes: [{component, property, value}] or values: {component: {property: value}}.");
        return;
    }
    auto* multi = new MultiPropertyCmd();
    if (!McpValues::appendChangeCommands(project, scene, entities, changes, *multi, error)) {
        delete multi;
        call->fail(error);
        return;
    }
    const uint32_t sceneId = scene->id;
    if (!runSceneCommand(sceneId, multi, error)) {
        call->fail(error);
        return;
    }
    SceneProject* current = project->getScene(sceneId);
    Json data = {{"scene_id", sceneId}, {"entities", Json::array()}};
    for (Entity entity : entities) data["entities"].push_back(McpRefs::entityJson(current, entity));
    data["values"] = readBack(project, current, entities[0], changes);
    call->ok("Set " + std::to_string(changes.size()) + " propert" + (changes.size() == 1 ? "y" : "ies") + " on " +
             std::to_string(entities.size()) + " GameObject(s) (one undo step). New values: " + data["values"].dump(), data);
}

// ------------------------------------------------------------------ set_transform

void setTransformTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    SceneProject* scene = sceneArg(call);
    if (!scene) return;
    std::vector<Entity> entities;
    std::string error;
    if (!McpRefs::resolveEntityArgs(scene, args, entities, error)) {
        call->fail(error);
        return;
    }
    const std::string space = args.value("space", std::string("local"));
    if (space != "local" && space != "world") {
        call->fail("space must be local or world.");
        return;
    }
    const bool relative = args.value("relative", false);
    if (!args.contains("position") && !args.contains("rotation") && !args.contains("scale")) {
        call->fail("Pass position, rotation and/or scale.");
        return;
    }
    Scene* registry = scene->scene;
    const uint32_t sceneId = scene->id;
    if (space == "world" && scene->playState == ScenePlayState::STOPPED) McpValues::refreshTransforms(scene);
    auto* multi = new MultiPropertyCmd();
    for (Entity entity : entities) {
        Transform* transform = registry->findComponent<Transform>(entity);
        if (!transform) {
            delete multi;
            call->fail("\"" + registry->getEntityName(entity) + "\" has no Transform.");
            return;
        }
        const bool world = space == "world";
        Vector3 position = world ? transform->worldPosition : transform->position;
        Quaternion rotation = world ? transform->worldRotation : transform->rotation;
        Vector3 scale = world ? transform->worldScale : transform->scale;
        if (args.contains("position")) {
            Vector3 value = relative ? Vector3::ZERO : position;
            if (!McpValues::parseVector3(args["position"], value)) {
                delete multi;
                call->fail("position must be [x,y,z] (or {x,y,z}).");
                return;
            }
            position = relative ? position + value : value;
        }
        if (args.contains("rotation")) {
            Quaternion value = relative ? Quaternion::IDENTITY : rotation;
            if (!McpValues::parseQuaternion(args["rotation"], value)) {
                delete multi;
                call->fail("rotation must be {\"euler\":[x,y,z]} degrees, [x,y,z,w] or {\"quat\":[...]}.");
                return;
            }
            rotation = relative ? value * rotation : value;
        }
        if (args.contains("scale")) {
            Vector3 value = relative ? Vector3(1.0f, 1.0f, 1.0f) : scale;
            if (args["scale"].is_number()) {
                const float uniform = args["scale"].get<float>();
                value = Vector3(uniform, uniform, uniform);
            } else if (!McpValues::parseVector3(args["scale"], value)) {
                delete multi;
                call->fail("scale must be [x,y,z] or one number (uniform).");
                return;
            }
            scale = relative ? Vector3(scale.x * value.x, scale.y * value.y, scale.z * value.z) : value;
        }
        if (world && transform->parent != NULL_ENTITY) {
            const Matrix4 worldMatrix = Matrix4::translateMatrix(position) * rotation.getRotationMatrix() * Matrix4::scaleMatrix(scale);
            const Matrix4 local = registry->getComponent<Transform>(transform->parent).modelMatrix.inverse() * worldMatrix;
            multi->addCommand(std::make_unique<ObjectTransformCmd>(project, sceneId, entity, local));
        } else {
            multi->addCommand(std::make_unique<ObjectTransformCmd>(project, sceneId, entity, position, rotation, scale));
        }
    }
    if (!runSceneCommand(sceneId, multi, error)) {
        call->fail(error);
        return;
    }
    SceneProject* current = project->getScene(sceneId);
    Json list = Json::array();
    for (Entity entity : entities) {
        Transform& t = current->scene->getComponent<Transform>(entity);
        Json item = McpRefs::entityJson(current, entity);
        item["local"] = {{"position", McpValues::toJson(t.position)}, {"rotation", McpValues::toJson(t.rotation)}, {"scale", McpValues::toJson(t.scale)}};
        list.push_back(item);
    }
    call->ok("Transform set on " + std::to_string(entities.size()) + " GameObject(s) (one undo step). Local: " +
             list[0]["local"].dump(), Json{{"scene_id", sceneId}, {"entities", list}});
}

// ------------------------------------------------------------------ add / remove / reset

void addComponentTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    SceneProject* scene = sceneArg(call);
    if (!scene) return;
    std::vector<Entity> entities;
    std::string error;
    if (!McpRefs::resolveEntityArgs(scene, args, entities, error)) {
        call->fail(error);
        return;
    }
    std::vector<ComponentType> types;
    if (!args.contains("component") || !componentListArg(args["component"], types, error)) {
        call->fail(args.contains("component") ? error : "Pass component (a name or an array of names).");
        return;
    }
    for (ComponentType type : types) {
        if (!ComponentAddDialog::isListed(type)) {
            call->fail("A " + Catalog::getComponentDisplayName(type) + " (" + McpRefs::componentName(type) +
                       ") cannot be added with Add Component (it is made by the editor, e.g. by create_entity or a prefab).");
            return;
        }
    }
    for (ComponentType type : types) {
        const ComponentType other = Catalog::getConflictingComponent(type);
        if (other != ComponentType::Transform && std::find(types.begin(), types.end(), other) != types.end()) {
            call->fail("A " + Catalog::getComponentDisplayName(type) + " and a " + Catalog::getComponentDisplayName(other) +
                       " cannot share a GameObject: add one of them.");
            return;
        }
        for (Entity entity : entities) {
            const std::string conflict = Catalog::getComponentConflict(scene->scene, entity, type);
            if (!conflict.empty()) {
                call->fail(conflict, Json{{"entity", entity}});
                return;
            }
        }
    }
    std::vector<McpValues::Change> changes;
    if (args.contains("values") && !args["values"].is_null()) {
        if (!McpValues::parseChanges(args["values"], changes, error)) {
            call->fail("values: " + error);
            return;
        }
    }
    const uint32_t sceneId = scene->id;
    auto* steps = new McpStepsCmd();
    Json added = Json::array();
    Json skipped = Json::array();
    for (Entity entity : entities) {
        for (ComponentType type : types) {
            if (McpRefs::hasComponent(scene, entity, type)) {
                skipped.push_back(Json{{"entity", entity}, {"component", McpRefs::componentName(type)}});
                continue;
            }
            added.push_back(Json{{"entity", entity}, {"component", McpRefs::componentName(type)}});
            steps->addStep([project, sceneId, entity, type](std::string& why) -> Command* {
                why = "Could not add " + McpRefs::componentName(type) + ".";
                return new AddComponentCmd(project, sceneId, entity, type);
            });
        }
    }
    if (added.empty() && changes.empty()) {
        delete steps;
        call->fail("Every GameObject has that component already (set its values with set_properties).",
                   Json{{"skipped", skipped}});
        return;
    }
    if (!changes.empty()) {
        steps->addStep([project, sceneId, entities, changes](std::string& why) -> Command* {
            auto* multi = new MultiPropertyCmd();
            if (!McpValues::appendChangeCommands(project, project->getScene(sceneId), entities, changes, *multi, why)) {
                delete multi;
                return nullptr;
            }
            return multi;
        });
    }
    if (!runSceneCommand(sceneId, steps, error)) {
        call->fail(error);
        return;
    }
    call->ok("Added " + std::to_string(added.size()) + " component(s)" +
             (skipped.empty() ? "" : ", " + std::to_string(skipped.size()) + " already there") + " (one undo step).",
             Json{{"scene_id", sceneId}, {"added", added}, {"skipped", skipped}});
}

void removeComponentTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    SceneProject* scene = sceneArg(call);
    if (!scene) return;
    std::vector<Entity> entities;
    std::string error;
    if (!McpRefs::resolveEntityArgs(scene, args, entities, error)) {
        call->fail(error);
        return;
    }
    std::vector<ComponentType> types;
    if (!args.contains("component") || !componentListArg(args["component"], types, error)) {
        call->fail(args.contains("component") ? error : "Pass component (a name or an array of names).");
        return;
    }
    const uint32_t sceneId = scene->id;
    auto* multi = new MultiPropertyCmd();
    Json removed = Json::array();
    for (Entity entity : entities) {
        for (ComponentType type : types) {
            if (!McpRefs::hasComponent(scene, entity, type)) {
                delete multi;
                call->fail("\"" + scene->scene->getEntityName(entity) + "\" has no " + McpRefs::componentName(type) + " component.");
                return;
            }
            multi->addCommand(std::make_unique<RemoveComponentCmd>(project, sceneId, entity, type));
            removed.push_back(Json{{"entity", entity}, {"component", McpRefs::componentName(type)}});
        }
    }
    if (!runSceneCommand(sceneId, multi, error)) {
        call->fail(error);
        return;
    }
    call->ok("Removed " + std::to_string(removed.size()) + " component(s) (one undo step).", Json{{"scene_id", sceneId}, {"removed", removed}});
}

void resetComponentTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    SceneProject* scene = sceneArg(call);
    if (!scene) return;
    std::vector<Entity> entities;
    std::string error;
    if (!McpRefs::resolveEntityArgs(scene, args, entities, error)) {
        call->fail(error);
        return;
    }
    ComponentType type;
    if (!McpRefs::resolveComponent(args.contains("component") ? args["component"] : Json(), type, error)) {
        call->fail(error);
        return;
    }
    const uint32_t sceneId = scene->id;
    std::string reason;
    Command* cmd = ComponentValuesCmd::buildReset(project, sceneId, entities, type, &reason);
    if (!cmd) {
        call->fail("Cannot reset: " + reason + ".");
        return;
    }
    if (!runSceneCommand(sceneId, cmd, error, "The reset did not apply.")) {
        call->fail(error);
        return;
    }
    call->ok("Reset " + McpRefs::componentName(type) + " to its default values on " + std::to_string(entities.size()) +
             " GameObject(s) (one undo step).", Json{{"scene_id", sceneId}, {"component", McpRefs::componentName(type)}});
}

// ------------------------------------------------------------------ copy / paste component

void copyComponentTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    SceneProject* scene = sceneArg(call);
    if (!scene) return;
    std::string error;
    const Entity entity = McpRefs::resolveEntity(scene, args.contains("entity") ? args["entity"] : Json(), error);
    if (entity == NULL_ENTITY) {
        call->fail(error);
        return;
    }
    ComponentType type;
    if (!McpRefs::resolveComponent(args.contains("component") ? args["component"] : Json(), type, error)) {
        call->fail(error);
        return;
    }
    if (!McpRefs::hasComponent(scene, entity, type)) {
        call->fail("\"" + scene->scene->getEntityName(entity) + "\" has no " + McpRefs::componentName(type) + " component.");
        return;
    }
    if (!EntityClipboard::copyComponent(project, scene->id, entity, type)) {
        call->fail("The component could not be copied.");
        return;
    }
    EntityClipboard::finishPendingWrite();
    call->ok("Copied " + McpRefs::componentName(type) + " of \"" + scene->scene->getEntityName(entity) +
             "\" (paste_component pastes its values, or as a new component).",
             Json{{"component", McpRefs::componentName(type)}, {"entity", McpRefs::entityJson(scene, entity)}});
}

void pasteComponentTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    SceneProject* scene = sceneArg(call);
    if (!scene) return;
    std::vector<Entity> entities;
    std::string error;
    if (!McpRefs::resolveEntityArgs(scene, args, entities, error)) {
        call->fail(error);
        return;
    }
    std::shared_ptr<const ComponentClip> clip = EntityClipboard::readComponent(project);
    if (!clip) {
        call->fail("The clipboard holds no component of this project: copy_component first.");
        return;
    }
    const uint32_t sceneId = scene->id;
    std::string reason;
    const bool asNew = args.value("as_new", false);
    Command* cmd = asNew ? ComponentValuesCmd::buildPasteAsNew(project, sceneId, entities, clip, &reason)
                         : ComponentValuesCmd::buildPasteValues(project, sceneId, entities, clip->type, clip, &reason);
    if (!cmd) {
        call->fail("Cannot paste the " + McpRefs::componentName(clip->type) + ": " + reason +
                   (asNew ? "." : " (as_new=true adds it as a new component)."));
        return;
    }
    if (!runSceneCommand(sceneId, cmd, error, "The paste did not apply.")) {
        call->fail(error);
        return;
    }
    call->ok("Pasted " + McpRefs::componentName(clip->type) + (asNew ? " as a new component" : " values") + " on " +
             std::to_string(entities.size()) + " GameObject(s) (one undo step).",
             Json{{"scene_id", sceneId}, {"component", McpRefs::componentName(clip->type)}});
}

// ------------------------------------------------------------------ set_mesh_geometry

Json vec3Json(const Vector3& v) {
    return Json::array({v.x, v.y, v.z});
}

void setMeshGeometryTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    SceneProject* scene = sceneArg(call);
    if (!scene) return;
    std::string error;
    const Entity entity = McpRefs::resolveEntity(scene, args.contains("entity") ? args["entity"] : Json(), error);
    if (entity == NULL_ENTITY) {
        call->fail(error);
        return;
    }
    const std::string shape = args.value("shape", "");
    static const std::vector<std::string> shapes = {"plane", "box", "sphere", "cylinder", "capsule", "torus", "wall"};
    if (std::find(shapes.begin(), shapes.end(), shape) == shapes.end()) {
        call->fail("shape must be one of plane, box, sphere, cylinder, capsule, torus, wall.");
        return;
    }
    Scene* sc = scene->scene;
    if (!sc->findComponent<MeshComponent>(entity)) {
        call->fail("The GameObject has no mesh component.");
        return;
    }
    // Geometry rebuilt from a source on load would throw the primitive away
    const Signature signature = sc->getSignature(entity);
    if (Stream::isModelBackedMesh(entity, sc, signature)) {
        call->fail("The mesh comes from a model file (rebuilt from it on load); use set_model instead.");
        return;
    }
    if (Stream::isGeneratedMesh(sc, signature)) {
        call->fail("The mesh is generated by its sprite, tilemap, mesh polygon or terrain component.");
        return;
    }
    std::shared_ptr<MeshSystem> meshSystem = sc->getSystem<MeshSystem>();
    if (!meshSystem) {
        call->fail("The scene has no mesh system.");
        return;
    }
    auto size = [&](const char* key, float fallback) {
        const float v = args.contains(key) && args[key].is_number() ? args[key].get<float>() : fallback;
        return std::isfinite(v) ? std::clamp(v, 0.01f, 10000.0f) : fallback;
    };
    auto count = [&](const char* key, unsigned int fallback, unsigned int minimum) {
        const long long v = args.contains(key) && args[key].is_number_integer() ? args[key].get<long long>() : fallback;
        return static_cast<unsigned int>(std::clamp<long long>(v, minimum, 512));
    };
    MeshComponent mesh = sc->getComponent<MeshComponent>(entity);
    const float radius = size("radius", 1.0f);
    if (shape == "plane") {
        meshSystem->createPlane(mesh, size("width", 10.0f), size("depth", 10.0f), count("tiles", 1, 1));
    } else if (shape == "wall") {
        meshSystem->createWall(mesh, size("width", 10.0f), size("height", 10.0f), count("tiles", 1, 1));
    } else if (shape == "box") {
        meshSystem->createBox(mesh, size("width", 1.0f), size("height", 1.0f), size("depth", 1.0f), count("tiles", 1, 1));
    } else if (shape == "sphere") {
        meshSystem->createSphere(mesh, radius, count("slices", 36, 3), count("stacks", 18, 3));
    } else if (shape == "cylinder") {
        meshSystem->createCylinder(mesh, radius, size("top_radius", radius), size("height", 2.0f), count("slices", 36, 3),
                                   count("stacks", 18, 1));
    } else if (shape == "capsule") {
        meshSystem->createCapsule(mesh, radius, size("top_radius", radius), size("height", 2.0f), count("slices", 36, 3),
                                  count("stacks", 18, 1));
    } else {
        meshSystem->createTorus(mesh, radius, size("ring_radius", 0.5f), count("sides", 36, 3), count("rings", 16, 3));
    }
    const uint32_t sceneId = scene->id;
    if (!runSceneCommand(sceneId, new MeshChangeCmd(project, sceneId, entity, mesh), error, "Could not change the mesh.")) {
        call->fail(error);
        return;
    }
    SceneProject* current = project->getScene(sceneId);
    Json data = {{"entity", McpRefs::entityJson(current, entity)}, {"shape", shape}};
    if (MeshComponent* now = current && current->scene ? current->scene->findComponent<MeshComponent>(entity) : nullptr) {
        data["local_size"] = vec3Json(now->aabb.getSize());
        data["local_center"] = vec3Json(now->aabb.getCenter());
    }
    call->ok("Set the mesh of " + data["entity"].value("path", std::string()) + " to a " + shape + ".", data);
}


void listComponentTypesTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    std::string error;
    if (args.contains("component") && !args["component"].is_null()) {
        ComponentType type;
        if (!McpRefs::resolveComponent(args["component"], type, error)) {
            call->fail(error);
            return;
        }
        McpValues::ReadContext ctx;
        ctx.project = project;
        ctx.component = type;
        ctx.hasComponent = false;
        Json props = Json::object();
        for (const auto& [name, property] : Catalog::getProperties(type, nullptr)) {
            Json entry = {{"type", McpValues::propertyTypeName(property.type)}};
            if (property.def && property.type != PropertyType::Custom) entry["default"] = McpValues::readProperty(ctx, name, property, true);
            if (property.type == PropertyType::Enum) {
                if (const McpValues::EnumOptions* options = McpValues::enumOptions(type, name)) entry["options"] = McpValues::enumNames(*options);
            } else if (property.type == PropertyType::Ease) {
                entry["options"] = McpValues::enumNames(McpValues::easeOptions());
            }
            props[name] = entry;
        }
        call->ok(McpRefs::componentName(type) + " (" + Catalog::getComponentDisplayName(type) + "): " +
                 std::to_string(props.size()) + " properties (indexed ones like submeshes[0].x or colliders[0].x exist per element; "
                 "see get_entity).",
                 Json{{"component", McpRefs::componentName(type)}, {"display_name", Catalog::getComponentDisplayName(type)},
                      {"addable", ComponentAddDialog::isListed(type)}, {"properties", props}});
        return;
    }
    Json list = Json::array();
    std::string text;
    for (int i = 0; i < ComponentTypeCount; i++) {
        const ComponentType type = static_cast<ComponentType>(i);
        const std::string name = McpRefs::componentName(type);
        if (name.empty()) continue;
        list.push_back({{"name", name}, {"display_name", Catalog::getComponentDisplayName(type)}, {"addable", ComponentAddDialog::isListed(type)}});
        text += (text.empty() ? "" : ", ") + name;
    }
    call->ok("Component types: " + text + ". Pass component to list one's properties.", Json{{"components", list}});
}

} // namespace

void registerComponentTools(McpRegistry& registry) {

    {
        ToolDef tool;
        tool.name = "get_entity";
        tool.title = "Get Entity";
        tool.description =
            "A GameObject as the Inspector shows it (id, name, path, active, tag, layer, parent, children, world "
            "transform, prefab) and its components. detail: summary (names), values ({component: {property: "
            "value}}), full (+ type, default, enum options, overridden flag), yaml (scene-file encoding). Filter "
            "by components and properties (globs: \"submeshes[0].*\"); big custom values are elided unless named.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef()},
            {"detail", withDefault(stringEnum("", {"summary", "values", "full", "yaml"}), "values")},
            {"components", componentsArgSchema("")},
            {"properties", array(string(""), "Only these properties (names or globs).")},
            {"runtime", withDefault(boolean("While playing: read the running scene."), false)}
        }, {"entity"});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = getEntityTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "set_properties";
        tool.title = "Set Properties";
        tool.description =
            "Set component properties on GameObjects in one undo step (prefab overrides recorded); returns the "
            "values read back. Colors also \"#RRGGBB[AA]\"; enums/eases by name; textures/fonts/models by asset path "
            "(texture {\"camera\": <entity>} = render texture); material: fields object or a .material path (links "
            "it); GameObject refs or null; Prefab script properties a .prefab path; custom values (colliders, "
            "frames, scripts...) as get_entity returns them. Lists: {component, property, insert: value, index?} / "
            "{..., remove: index}. Paths index (\"submeshes[0].material.baseColorFactor\").";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef()},
            {"entities", entities()},
            {"changes", array(anyObject("{component, property, value|insert|remove, index?}"), "")},
            {"values", anyObject("{component: {property: value}}")}
        });
        tool.handler = setPropertiesTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "set_transform";
        tool.title = "Set Transform";
        tool.description =
            "Move/rotate/scale GameObjects. relative=true adds position, applies rotation on top and multiplies "
            "scale.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef()},
            {"entities", entities()},
            {"position", vec3("")},
            {"rotation", quat()},
            {"scale", {{"type", {"array", "number"}}, {"items", {{"type", "number"}}}, {"description", "[x,y,z] or uniform number."}}},
            {"space", withDefault(stringEnum("local = relative to the parent.", {"local", "world"}), "local")},
            {"relative", withDefault(boolean(""), false)}
        });
        tool.handler = setTransformTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "add_component";
        tool.title = "Add Component";
        tool.description =
            "Add components to GameObjects, optionally with initial values (as set_properties); ones having it are "
            "skipped. Names: list_component_types. Colliders: edit_colliders.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef()},
            {"entities", entities()},
            {"component", componentsArgSchema("")},
            {"values", anyObject("{component: {property: value}}")}
        }, {"component"});
        tool.handler = addComponentTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "remove_component";
        tool.title = "Remove Component";
        tool.description = "Remove components from GameObjects.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef()},
            {"entities", entities()},
            {"component", componentsArgSchema("")}
        }, {"component"});
        tool.annotations.destructive = true;
        tool.handler = removeComponentTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "reset_component";
        tool.title = "Reset Component";
        tool.description = "Inspector > Reset: a component's values back to defaults (a script component resets its scripts' fields).";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef()},
            {"entities", entities()},
            {"component", componentName()}
        }, {"component"});
        tool.handler = resetComponentTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "copy_component";
        tool.activity = ToolActivity::Control;  // busy policy (McpServer::busyRefusal)
        tool.title = "Copy Component";
        tool.description = "Inspector > Copy Component: one component's values to the clipboard, for paste_component.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef()},
            {"component", componentName()}
        }, {"entity", "component"});
        tool.handler = copyComponentTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "paste_component";
        tool.title = "Paste Component";
        tool.description =
            "Paste the copied component onto GameObjects: over their component of that type (Paste Component Values), "
            "or as_new adds it (Paste Component As New).";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef()},
            {"entities", entities()},
            {"as_new", withDefault(boolean(""), false)}
        });
        tool.handler = pasteComponentTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "list_component_types";
        tool.title = "List Component Types";
        tool.description =
            "Component names (with Unity display names, addable flag). With component: its properties with type, "
            "default and enum options.";
        tool.inputSchema = object({{"component", componentName("Describe this one.")}});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = listComponentTypesTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "set_mesh_geometry";
        tool.title = "Set Mesh Geometry";
        tool.description =
            "Replace a mesh's geometry with a generated primitive (the Inspector's shape fields). Sizes: plane "
            "width/depth, wall width/height, box width/height/depth, sphere radius, cylinder/capsule "
            "radius/top_radius/height, torus radius/ring_radius; segments tiles, slices/stacks, sides/rings. "
            "Omitted values take defaults, not current ones. Not for models, sprites, tilemaps, polygons, "
            "terrains.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef()},
            {"shape", stringEnum("", {"plane", "box", "sphere", "cylinder", "capsule", "torus", "wall"})},
            {"width", number("")},
            {"height", number("")},
            {"depth", number("")},
            {"radius", number("Base radius for cylinder/capsule.")},
            {"top_radius", number("Default: radius.")},
            {"ring_radius", number("")},
            {"tiles", integer("")},
            {"slices", integer("")},
            {"stacks", integer("")},
            {"sides", integer("")},
            {"rings", integer("")}
        }, {"entity", "shape"});
        tool.allowedWhilePlaying = false;
        tool.handler = setMeshGeometryTool;
        registry.addTool(std::move(tool));
    }
}

}
