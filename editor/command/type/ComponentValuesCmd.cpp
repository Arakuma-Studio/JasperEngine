// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ComponentValuesCmd.h"
#include "component/ActionComponent.h"
#include "component/AnimationComponent.h"
#include "component/ModelComponent.h"
#include "animation/ClipRecordHooks.h"

#include "Stream.h"
#include "command/CommandNames.h"
#include "command/PlayChangeReview.h"
#include "util/YamlFootprint.h"
#include "command/type/AddComponentCmd.h"
#include "command/type/ModelClearCmd.h"
#include "command/type/ModelLoadCmd.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/PropertyCmd.h"
#include "util/CameraTextureLink.h"
#include "window/dialog/ComponentAddDialog.h"

#include <initializer_list>

using namespace jasperengine;

namespace {

    // Mesh fields the geometry owns. A sprite, tilemap, polygon or terrain rebuilds them and a
    // model loads them from its file, so a paste onto such a mesh keeps them.
    const std::initializer_list<const char*> meshGeometryKeys = {
        "buffer", "indices", "eBuffers", "bonesMatrix", "aabb", "verticesAABB", "worldAABB",
        "normAdjustJoint", "normAdjustWeight", "morphWeights"
    };
    const std::initializer_list<const char*> submeshGeometryKeys = {
        "primitiveType", "vertexCount", "hasTexCoord1", "hasTexCoord2", "hasNormalMap", "hasTangent",
        "hasVertexColor3", "hasVertexColor4", "hasTextureRect", "hasSkinning", "hasMorphTarget",
        "hasMorphNormal", "hasMorphTangent", "normAdjustJoint", "normAdjustWeight"
    };
    // The same for a UI element an image, text or polygon draws
    const std::initializer_list<const char*> uiGeometryKeys = {
        "buffer", "indices", "minBufferCount", "minIndicesCount", "primitiveType", "vertexCount", "aabb", "worldAABB"
    };

    void keepKey(YAML::Node& value, const YAML::Node& current, const char* key) {
        if (current[key]) {
            value[key] = YAML::Clone(current[key]);
        } else {
            value.remove(key);
        }
    }

    // `source` made fit for `entity`: its own structure (child entities, owned tracks,
    // generated geometry) stays. `skip` gets the properties that must then not be remapped.
    YAML::Node prepareValue(Scene* scene, Entity entity, editor::ComponentType type, const YAML::Node& source,
                            std::vector<std::string>& skip) {
        YAML::Node value = YAML::Clone(source);
        if (!value.IsMap()) {
            value = YAML::Node(YAML::NodeType::Map);
        }
        const YAML::Node current = editor::EntityClipboard::encodeComponent(scene, entity, type);

        auto keep = [&](const char* key) {
            keepKey(value, current, key);
            skip.push_back(key);
        };

        switch (type) {
            case editor::ComponentType::ButtonComponent:
                keep("label");
                break;
            case editor::ComponentType::ScrollbarComponent:
                keep("bar");
                break;
            case editor::ComponentType::ProgressbarComponent:
                keep("fill");
                break;
            case editor::ComponentType::OnScreenStickComponent:
                keep("background");
                keep("handle");
                break;
            case editor::ComponentType::PanelComponent:
                keep("headerimage");
                keep("headercontainer");
                keep("headertext");
                break;
            case editor::ComponentType::InputFieldComponent:
                keep("text");
                keep("selection");
                keep("cursor");
                break;
            case editor::ComponentType::ActionComponent:
                // Ownership is the entity's: a pasted target is referenced, never owned, and
                // a target this action owns is not given up
                keepKey(value, current, "ownedTarget");
                if (scene->getComponent<ActionComponent>(entity).ownedTarget) {
                    keep("target");
                }
                break;
            case editor::ComponentType::AnimationComponent:
                keepKey(value, current, "ownedActions");
                if (scene->getComponent<AnimationComponent>(entity).ownedActions) {
                    keep("actions");
                }
                break;
            case editor::ComponentType::MeshComponent: {
                const Signature signature = scene->getSignature(entity);
                const bool modelBacked = editor::Stream::isModelBackedMesh(entity, scene, signature);
                if (!modelBacked && !editor::Stream::isGeneratedMesh(scene, signature)) {
                    break;
                }
                for (const char* key : meshGeometryKeys) {
                    keepKey(value, current, key);
                }
                // As many submeshes as the geometry has, each keeping its geometry fields
                const YAML::Node currentSubmeshes = current["submeshes"];
                const YAML::Node sourceSubmeshes = value["submeshes"];
                YAML::Node submeshes(YAML::NodeType::Sequence);
                for (size_t i = 0; currentSubmeshes && i < currentSubmeshes.size(); i++) {
                    const bool pasted = sourceSubmeshes && sourceSubmeshes.IsSequence() && i < sourceSubmeshes.size();
                    YAML::Node submesh = YAML::Clone(pasted ? sourceSubmeshes[i] : currentSubmeshes[i]);
                    for (const char* key : submeshGeometryKeys) {
                        keepKey(submesh, currentSubmeshes[i], key);
                    }
                    if (modelBacked && pasted) {
                        // The next load of the model file must keep what was pasted
                        const YAML::Node currentOverrides = currentSubmeshes[i]["overrideFields"];
                        uint32_t overrides = currentOverrides ? currentOverrides.as<uint32_t>() : 0;
                        overrides |= SubmeshOverride_Material | SubmeshOverride_FaceCulling | SubmeshOverride_TextureShadow;
                        submesh["overrideFields"] = overrides;
                    }
                    submeshes.push_back(submesh);
                }
                value["submeshes"] = submeshes;
                break;
            }
            case editor::ComponentType::UIComponent:
                if (editor::Stream::isGeneratedUI(scene, scene->getSignature(entity))) {
                    for (const char* key : uiGeometryKeys) {
                        keepKey(value, current, key);
                    }
                }
                break;
            default:
                break;
        }
        return value;
    }

    // Unity's Reset of a script resets its fields; here that is the property values of every
    // script in the component, which keeps its scripts
    YAML::Node scriptDefaults(Scene* scene, Entity entity) {
        YAML::Node value = editor::EntityClipboard::encodeComponent(scene, entity, editor::ComponentType::ScriptComponent);
        YAML::Node scripts = value["scripts"];
        if (scripts && scripts.IsSequence()) {
            for (YAML::Node script : scripts) {
                YAML::Node properties = script["properties"];
                if (!properties || !properties.IsSequence()) {
                    continue;
                }
                for (YAML::Node property : properties) {
                    if (property["defaultValue"]) {
                        property["value"] = YAML::Clone(property["defaultValue"]);
                    } else {
                        property.remove("value");
                    }
                    property.remove("sceneId");
                }
            }
        }
        return value;
    }

    bool hasComponent(Scene* scene, Entity entity, editor::ComponentType type) {
        return scene->isEntityCreated(entity) &&
            scene->getSignature(entity).test(editor::Catalog::getComponentId(scene, type));
    }

    struct ModelValue {
        bool enabled = true;
        std::string filename;
        bool mergeStaticMeshes = false;
        bool animationEntities = true;
    };

    ModelValue readModelValue(const YAML::Node& value) {
        ModelValue model;
        if (value["enabled"]) model.enabled = value["enabled"].as<bool>();
        if (value["filename"]) model.filename = value["filename"].as<std::string>();
        if (value["mergeStaticMeshes"]) model.mergeStaticMeshes = value["mergeStaticMeshes"].as<bool>();
        // written only when false (Stream::encodeModelComponent)
        if (value["animationEntities"]) model.animationEntities = value["animationEntities"].as<bool>();
        return model;
    }

    // A model is its file: applying one loads it (ModelLoadCmd rebuilds the generated
    // children into the GameObject's Transform and Mesh), and applying no file unloads the
    // current one (ModelClearCmd); the other two values are plain properties. `current` is
    // null for a Model component added by the same command. A reset also puts back the
    // references the user can set on an empty Model, its skeleton and animations.
    void addModelCommands(editor::MultiPropertyCmd* multi, editor::Project* project, uint32_t sceneId, Scene* scene, Entity entity,
                          const ModelValue& model, const ModelComponent* current, bool reset = false) {
        const std::string currentFile = current ? current->filename.path : std::string();
        const bool currentMerge = current ? current->mergeStaticMeshes : false;
        const bool currentAnimationEntities = current ? current->animationEntities : true;
        // the loader makes (or skips) the animation entities by the flag (spec 9.R15): set before
        // the load in the same step, and a changed flag on the same file reloads it
        if (!model.filename.empty() && (model.filename != currentFile || model.mergeStaticMeshes != currentMerge ||
                                        model.animationEntities != currentAnimationEntities)) {
            for (editor::ComponentType needed : {editor::ComponentType::Transform, editor::ComponentType::MeshComponent}) {
                if (!hasComponent(scene, entity, needed)) {
                    multi->addCommand(std::make_unique<editor::AddComponentCmd>(project, sceneId, entity, needed));
                }
            }
            if (model.animationEntities != currentAnimationEntities) {
                multi->addPropertyCmd<bool>(project, sceneId, entity, editor::ComponentType::ModelComponent, "animationEntities", model.animationEntities);
            }
            multi->addCommand(std::make_unique<editor::ModelLoadCmd>(project, sceneId, entity, model.filename, model.mergeStaticMeshes));
        } else if (model.filename.empty()) {
            if (current && editor::ModelClearCmd::hasContent(*current)) {
                multi->addCommand(std::make_unique<editor::ModelClearCmd>(project, sceneId, entity));
            }
            multi->addPropertyCmd<bool>(project, sceneId, entity, editor::ComponentType::ModelComponent, "mergeStaticMeshes", model.mergeStaticMeshes);
            multi->addPropertyCmd<bool>(project, sceneId, entity, editor::ComponentType::ModelComponent, "animationEntities", model.animationEntities);
            if (reset) {
                multi->addPropertyCmd<Entity>(project, sceneId, entity, editor::ComponentType::ModelComponent, "skeleton", NULL_ENTITY);
                multi->addPropertyCmd<std::vector<Entity>>(project, sceneId, entity, editor::ComponentType::ModelComponent, "animations", std::vector<Entity>());
            }
        }
        multi->addPropertyCmd<bool>(project, sceneId, entity, editor::ComponentType::ModelComponent, "enabled", model.enabled);
    }

    // A Model with a loaded file takes no file only by being emptied
    bool canEmptyModels(editor::Project* project, uint32_t sceneId, Scene* scene, const std::vector<Entity>& entities, std::string* reason) {
        for (Entity entity : entities) {
            const ModelComponent& model = scene->getComponent<ModelComponent>(entity);
            if (editor::ModelClearCmd::hasContent(model) && !editor::ModelClearCmd::canClear(project, sceneId, entity, reason)) {
                return false;
            }
        }
        return true;
    }

}

editor::ComponentValuesCmd::ComponentValuesCmd(Project* project, uint32_t sceneId, const std::vector<Entity>& entities,
                                               ComponentType type, std::shared_ptr<const ComponentClip> clip){
    this->project = project;
    this->sceneId = sceneId;
    this->type = type;
    this->clip = std::move(clip);
    for (Entity entity : entities) {
        Target target;
        target.entity = entity;
        targets.push_back(target);
    }
}

bool editor::ComponentValuesCmd::execute(){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene){
        return false;
    }
    Scene* scene = sceneProject->scene;

    wasModified = sceneProject->isModified;
    bool changed = false;
    differs = false;

    YAML::Node defaults;
    if (!clip && type != ComponentType::ScriptComponent) {
        defaults = EntityClipboard::encodeDefaultComponent(type);
    }

    // a posed bone's Transform (paste values, reset; Pose window) and previewed channels (clip
    // preview) go back to their rest: recorded and edited on it (animation clips 9.R5/R6)
    for (const Target& target : targets) ClipRecordHooks::beforeComponentWrite(sceneId, target.entity, type, "");
    for (Target& target : targets) {
        target.before = YAML::Node();
        if (!hasComponent(scene, target.entity, type)) {
            continue;
        }

        target.before = EntityClipboard::encodeComponent(scene, target.entity, type);
        prefabUndo.capture(project, sceneId, target.entity);

        std::vector<std::string> skip;
        YAML::Node source = clip ? clip->value :
            (type == ComponentType::ScriptComponent ? scriptDefaults(scene, target.entity) : defaults);
        EntityClipboard::applyComponentValue(scene, target.entity, type, prepareValue(scene, target.entity, type, source, skip));
        Catalog::markChanged(scene, target.entity, type);

        if (clip) {
            // The copied GameObject becomes this one; other references follow the paste rules
            std::unordered_map<Entity, Entity> self{{clip->entity, target.entity}};
            ClipboardRemap remap;
            remap.map = &self;
            remap.externalNames = &clip->externalNames;
            remap.sourceSceneId = clip->sceneId;
            remap.targetSceneId = sceneId;
            remap.sameScene = clip->project == EntityClipboard::projectKey(project) && clip->sceneId == sceneId;
            uint64_t updateFlags = EntityClipboard::remapReferences(scene, target.entity, type, remap, skip);
            if (updateFlags != 0) {
                Catalog::updateEntity(scene, target.entity, updateFlags);
            }
        }

        YAML::Node before(YAML::NodeType::Map);
        before[Catalog::getComponentName(type, true)] = target.before;
        project->recordPrefabComponentEdit(sceneId, target.entity, type, before);
        changed = true;
        // the same values again are no edit (the history then drops the command)
        if (!differs) {
            differs = !yamlEqual(EntityClipboard::encodeComponent(scene, target.entity, type), target.before);
        }
    }

    for (const Target& target : targets) ClipRecordHooks::afterComponentWrite(sceneId, target.entity, type, "");

    if (!changed) {
        return false;
    }

    if (CameraTextureLink::isLinkSourceType(type)) {
        CameraTextureLink::resolve(scene);
    }
    if (differs) {
        sceneProject->isModified = true;
    }
    return true;
}

std::string editor::ComponentValuesCmd::describe() const{
    const std::string subject = " (" + CommandNames::entities(project, sceneId, editedEntities()) + ")";
    if (!clip) {
        return "Reset " + editedLabel() + subject;
    }
    const bool ownValues = targets.size() == 1 && targets.front().entity == clip->entity && clip->sceneId == sceneId &&
        clip->project == EntityClipboard::projectKey(project);
    return (ownValues ? "Set " : "Paste Component Values ") + editedLabel() + subject;
}

size_t editor::ComponentValuesCmd::memoryBytes() const{
    size_t bytes = sizeof(*this) + targets.capacity() * sizeof(Target);
    for (const Target& target : targets) {
        bytes += yamlFootprint(target.before);
    }
    if (clip) {
        bytes += yamlFootprint(clip->value);
    }
    return bytes;
}

std::vector<Entity> editor::ComponentValuesCmd::editedEntities() const{
    std::vector<Entity> entities;
    entities.reserve(targets.size());
    for (const Target& target : targets) {
        entities.push_back(target.entity);
    }
    return entities;
}

std::string editor::ComponentValuesCmd::editedLabel() const{
    return CommandNames::component(type);
}

void editor::ComponentValuesCmd::undo(){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene){
        return;
    }
    Scene* scene = sceneProject->scene;

    for (const Target& target : targets) ClipRecordHooks::beforeComponentWrite(sceneId, target.entity, type, "");
    for (auto it = targets.rbegin(); it != targets.rend(); ++it) {
        if (!it->before.IsMap() || !hasComponent(scene, it->entity, type)) {
            continue;
        }
        EntityClipboard::applyComponentValue(scene, it->entity, type, it->before);
        Catalog::markChanged(scene, it->entity, type);
        if (uint64_t updateFlags = EntityClipboard::bindCameraTextures(scene, it->entity, type)) {
            Catalog::updateEntity(scene, it->entity, updateFlags);
        }

    }
    prefabUndo.restore(project, sceneId);

    if (CameraTextureLink::isLinkSourceType(type)) {
        CameraTextureLink::resolve(scene);
    }
    sceneProject->isModified = wasModified;

    for (const Target& target : targets) ClipRecordHooks::afterComponentWrite(sceneId, target.entity, type, "");
}

bool editor::ComponentValuesCmd::canPasteValues(Project* project, uint32_t sceneId, const std::vector<Entity>& entities,
                                                ComponentType type, const ComponentClip* clip, std::string* reason){
    auto fail = [reason](const std::string& message) {
        if (reason) *reason = message;
        return false;
    };
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene || entities.empty()) {
        return fail("No GameObject selected");
    }
    if (!clip) {
        return fail("Copy a component first");
    }
    if (clip->type != type) {
        return fail("The copied component is a " + Catalog::getComponentDisplayName(clip->type));
    }
    for (Entity entity : entities) {
        if (!hasComponent(sceneProject->scene, entity, type)) {
            return fail("Not every selected GameObject has a " + Catalog::getComponentDisplayName(type));
        }
    }
    if (type == ComponentType::ModelComponent && readModelValue(clip->value).filename.empty()) {
        return canEmptyModels(project, sceneId, sceneProject->scene, entities, reason);
    }
    return true;
}

bool editor::ComponentValuesCmd::canPasteAsNew(Project* project, uint32_t sceneId, const std::vector<Entity>& entities,
                                               const ComponentClip* clip, std::string* reason){
    auto fail = [reason](const std::string& message) {
        if (reason) *reason = message;
        return false;
    };
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene || entities.empty()) {
        return fail("No GameObject selected");
    }
    if (!clip) {
        return fail("Copy a component first");
    }
    if (!ComponentAddDialog::isListed(clip->type)) {
        return fail("A " + Catalog::getComponentDisplayName(clip->type) + " cannot be added");
    }
    for (Entity entity : entities) {
        if (!sceneProject->scene->isEntityCreated(entity)) {
            return fail("No GameObject selected");
        }
        if (hasComponent(sceneProject->scene, entity, clip->type)) {
            return fail("A selected GameObject already has a " + Catalog::getComponentDisplayName(clip->type));
        }
    }
    return true;
}

bool editor::ComponentValuesCmd::canReset(Project* project, uint32_t sceneId, const std::vector<Entity>& entities,
                                          ComponentType type, std::string* reason){
    auto fail = [reason](const std::string& message) {
        if (reason) *reason = message;
        return false;
    };
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene || entities.empty()) {
        return fail("No GameObject selected");
    }
    if (type == ComponentType::PrefabComponent) {
        return fail("The prefab link has no default values");
    }
    for (Entity entity : entities) {
        if (!hasComponent(sceneProject->scene, entity, type)) {
            return fail("Not every selected GameObject has a " + Catalog::getComponentDisplayName(type));
        }
    }
    if (type == ComponentType::ModelComponent) {
        return canEmptyModels(project, sceneId, sceneProject->scene, entities, reason);
    }
    return true;
}

editor::Command* editor::ComponentValuesCmd::buildPasteValues(Project* project, uint32_t sceneId, const std::vector<Entity>& entities,
                                                              ComponentType type, std::shared_ptr<const ComponentClip> clip, std::string* reason){
    if (!canPasteValues(project, sceneId, entities, type, clip.get(), reason)) {
        return nullptr;
    }
    if (type != ComponentType::ModelComponent) {
        return new ComponentValuesCmd(project, sceneId, entities, type, clip);
    }

    Scene* scene = project->getScene(sceneId)->scene;
    const ModelValue model = readModelValue(clip->value);
    MultiPropertyCmd* multi = new MultiPropertyCmd();
    for (Entity entity : entities) {
        addModelCommands(multi, project, sceneId, scene, entity, model, &scene->getComponent<ModelComponent>(entity));
    }
    return multi;
}

editor::Command* editor::ComponentValuesCmd::buildPasteAsNew(Project* project, uint32_t sceneId, const std::vector<Entity>& entities,
                                                             std::shared_ptr<const ComponentClip> clip, std::string* reason){
    if (!canPasteAsNew(project, sceneId, entities, clip.get(), reason)) {
        return nullptr;
    }
    Scene* scene = project->getScene(sceneId)->scene;
    MultiPropertyCmd* multi = new MultiPropertyCmd();

    if (clip->type == ComponentType::ModelComponent) {
        // A model loads into the Transform and Mesh of its GameObject
        const ModelValue model = readModelValue(clip->value);
        for (Entity entity : entities) {
            multi->addCommand(std::make_unique<AddComponentCmd>(project, sceneId, entity, ComponentType::ModelComponent));
            addModelCommands(multi, project, sceneId, scene, entity, model, nullptr);
        }
        return multi;
    }

    for (Entity entity : entities) {
        multi->addCommand(std::make_unique<AddComponentCmd>(project, sceneId, entity, clip->type));
    }
    multi->addCommand(std::make_unique<ComponentValuesCmd>(project, sceneId, entities, clip->type, clip));
    return multi;
}

editor::Command* editor::ComponentValuesCmd::buildReset(Project* project, uint32_t sceneId, const std::vector<Entity>& entities,
                                                        ComponentType type, std::string* reason){
    if (!canReset(project, sceneId, entities, type, reason)) {
        return nullptr;
    }
    if (type != ComponentType::ModelComponent) {
        return new ComponentValuesCmd(project, sceneId, entities, type, nullptr);
    }

    // A loaded model is emptied, then every value goes back to its default
    Scene* scene = project->getScene(sceneId)->scene;
    MultiPropertyCmd* multi = new MultiPropertyCmd();
    for (Entity entity : entities) {
        addModelCommands(multi, project, sceneId, scene, entity, ModelValue(), &scene->getComponent<ModelComponent>(entity), true);
    }
    return multi;
}

void editor::ComponentValuesCmd::collectKeepCommands(KeepContext& context) const{
    for (const Target& target : targets){
        context.addComponentValues(target.entity, type);
    }
}
