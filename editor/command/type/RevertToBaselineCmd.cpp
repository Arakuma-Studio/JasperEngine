// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "RevertToBaselineCmd.h"

#include "Catalog.h"
#include "Project.h"
#include "Stream.h"
#include "command/type/ComponentValuesCmd.h"
#include "command/type/DeleteEntityCmd.h"
#include "command/type/EntityNameCmd.h"
#include "command/type/EntityStateCmd.h"
#include "command/type/MoveEntityOrderCmd.h"
#include "command/type/PrefabOverrideCmd.h"
#include "command/type/ReflectedValueCmd.h"
#include "command/type/RemoveComponentCmd.h"
#include "command/type/SetMainCameraCmd.h"
#include "reflect/Reflect.h"
#include "util/EntityClipboard.h"
#include "util/ProjectUtils.h"
#include "util/WorldTransforms.h"
#include "util/YamlNumbers.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace scene = jasperengine::editor::vcs::scene;

namespace {

    // The GameObjects below `parent` (NULL_ENTITY: the roots) the scene tracks, in Transform order
    std::vector<Entity> childrenInOrder(SceneProject* sceneProject, Entity parent) {
        std::vector<Entity> children;
        Scene* scene = sceneProject->scene;
        auto transforms = scene->getComponentArray<Transform>();
        for (size_t i = 0; i < transforms->size(); i++) {
            const Entity entity = transforms->getEntity(i);
            if (transforms->getComponentFromIndex(i).parent != parent) continue;
            if (std::find(sceneProject->entities.begin(), sceneProject->entities.end(), entity) == sceneProject->entities.end()) continue;
            children.push_back(entity);
        }
        return children;
    }

    // The command that puts `entity` at `index` below `parent` (null when it is there already)
    Command* moveTo(Project* project, uint32_t sceneId, Entity entity, Entity parent, int index) {
        SceneProject* sceneProject = project->getScene(sceneId);
        std::vector<Entity> siblings = childrenInOrder(sceneProject, parent);
        const auto at = std::find(siblings.begin(), siblings.end(), entity);
        const bool sameParent = at != siblings.end();
        if (sameParent && at - siblings.begin() == index) return nullptr;
        if (sameParent) siblings.erase(at);
        if (index < static_cast<int>(siblings.size())) {
            return new MoveEntityOrderCmd(project, sceneId, entity, siblings[static_cast<size_t>(std::max(index, 0))], InsertionType::BEFORE);
        }
        if (!siblings.empty()) return new MoveEntityOrderCmd(project, sceneId, entity, siblings.back(), InsertionType::AFTER);
        if (parent != NULL_ENTITY) return new MoveEntityOrderCmd(project, sceneId, entity, parent, InsertionType::INTO);
        return nullptr;
    }

    // A removed GameObject made again from the baseline's node: the first execute decodes it (its
    // own id when free), puts it below its parent at its index; then a delete of it is the undo
    // and that delete's undo the redo, so later steps keep finding the same ids
    class RecreateStep : public Command {
    public:
        RecreateStep(Project* project, uint32_t sceneId, YAML::Node node, Entity parent, int index)
            : project(project), sceneId(sceneId), node(std::move(node)), parent(parent), index(index) {}

        bool execute() override {
            if (remover) {
                remover->undo();
                return true;
            }
            SceneProject* sceneProject = project->getScene(sceneId);
            if (!sceneProject || !sceneProject->scene) return false;
            // its world values once it is decoded and back in its place (the baseline holds none)
            WorldTransformScope worldTransforms(sceneProject->scene);
            std::unordered_map<Entity, Entity> remap;
            const std::vector<Entity> created = Stream::decodeEntity(node, sceneProject->scene, &sceneProject->entities, project, sceneProject,
                                                                     NULL_ENTITY, true, false, &remap);
            if (created.empty()) return false;
            const Entity root = created.front();
            Scene* scene = sceneProject->scene;
            if (scene->findComponent<Transform>(root)) {
                scene->addEntityChild(parent, root, true);
                ProjectUtils::sortEntitiesByTransformOrder(scene, sceneProject->entities);
                if (std::unique_ptr<Command> move{moveTo(project, sceneId, root, parent, index)}) move->execute();
            }
            sceneProject->isModified = true;
            sceneProject->structureVersion++;
            remover = std::make_unique<DeleteEntityCmd>(project, sceneId, root, true);
            return true;
        }

        void undo() override {
            if (remover) remover->execute();
        }

        std::string describe() const override { return "Recreate GameObject"; }
        const char* typeName() const override { return "RecreateStep"; }
        size_t memoryBytes() const override { return sizeof(*this) + (remover ? remover->memoryBytes() : 0); }

    private:
        Project* project;
        uint32_t sceneId;
        YAML::Node node;
        Entity parent;
        int index;
        std::unique_ptr<DeleteEntityCmd> remover;
    };

    // The scene's settings block (scene:) with one value of the baseline's (or all of it)
    class SceneSettingsStep : public Command {
    public:
        SceneSettingsStep(Project* project, uint32_t sceneId, YAML::Node after): project(project), sceneId(sceneId), after(std::move(after)) {}

        bool execute() override {
            SceneProject* sceneProject = project->getScene(sceneId);
            if (!sceneProject || !sceneProject->scene) return false;
            before = Stream::encodeScene(sceneProject->scene);
            wasModified = sceneProject->isModified;
            Stream::decodeScene(sceneProject->scene, after);
            sceneProject->isModified = true;
            sceneProject->needUpdateRender = true;
            return true;
        }

        void undo() override {
            SceneProject* sceneProject = project->getScene(sceneId);
            if (!sceneProject || !sceneProject->scene) return;
            Stream::decodeScene(sceneProject->scene, before);
            sceneProject->isModified = wasModified;
            sceneProject->needUpdateRender = true;
        }

        std::string describe() const override { return "Scene Settings"; }
        const char* typeName() const override { return "SceneSettingsStep"; }
        bool affectsStructure() const override { return false; }

    private:
        Project* project;
        uint32_t sceneId;
        YAML::Node after;
        YAML::Node before;
        bool wasModified = false;
    };

    std::shared_ptr<ComponentClip> clipOf(Project* project, uint32_t sceneId, Entity entity, ComponentType type, YAML::Node value) {
        auto clip = std::make_shared<ComponentClip>();
        clip->project = EntityClipboard::projectKey(project);
        clip->sceneId = sceneId;
        clip->type = type;
        clip->entity = entity;
        clip->value = std::move(value);
        return clip;
    }

    std::string topField(const std::string& path) {
        const size_t end = path.find_first_of(".[");
        return end == std::string::npos ? path : path.substr(0, end);
    }

    struct Builder {
        Project* project;
        uint32_t sceneId;
        const scene::SceneYamlModel& baseline;
        std::vector<std::unique_ptr<Command>>& steps;
        std::string& why;

        SceneProject* sceneProject() const { return project->getScene(sceneId); }

        bool fail(std::string reason) {
            if (why.empty()) why = std::move(reason);
            return false;
        }

        bool add(Command* command, const std::string& reason) {
            if (!command) return fail(reason);
            steps.emplace_back(command);
            return true;
        }

        bool entityExists(Entity entity) const {
            SceneProject* s = sceneProject();
            return s && s->scene && s->scene->isEntityCreated(entity);
        }

        // ---- a GameObject's own values (name, active, tag, layer)
        bool gameObjectProperty(const scene::ObjectDiff& object, const scene::PropertyDiff& property) {
            const Entity entity = static_cast<Entity>(object.id);
            const std::string& field = property.path;
            if (object.prefabMember && !property.inBase) {
                uint8_t bit = field == "name" ? PrefabObjectOverrides::StateName : field == "active" ? PrefabObjectOverrides::StateActive :
                              field == "tag" ? PrefabObjectOverrides::StateTag : field == "layer" ? PrefabObjectOverrides::StateLayer : 0;
                if (!bit) return fail(field + " is not a GameObject value");
                return add(new PrefabOverrideCmd(project, sceneId, entity, PrefabOverrideCmd::Operation::RevertState, ComponentType::Transform, {}, bit),
                           "the override cannot be reverted");
            }
            const YAML::Node value = property.baseNode;
            if (field == "name") return add(new EntityNameCmd(project, sceneId, entity, property.inBase ? value.as<std::string>() : std::string("GameObject")), "");
            if (field == "active") return add(EntityStateCmd::active(project, sceneId, {entity}, !property.inBase || YamlNumbers::as<bool>(value)), "");
            if (field == "tag") return add(EntityStateCmd::tag(project, sceneId, {entity}, property.inBase ? value.as<std::string>() : std::string(DEFAULT_ENTITY_TAG)), "");
            if (field == "layer") return add(EntityStateCmd::layer(project, sceneId, {entity}, property.inBase ? YamlNumbers::as<int>(value) : 0), "");
            return fail(field + " is kept for a newer editor: no command edits it");
        }

        // ---- one property of a component
        bool property(const scene::ObjectDiff& object, const scene::ComponentDiff& component, const scene::PropertyDiff& property) {
            if (object.settings) return settingsProperty(component, property);
            if (component.key == scene::kGameObjectKey) return gameObjectProperty(object, property);
            if (component.key == scene::kPrefabInstanceKey || component.key == "prefabOverrides") {
                return fail("the prefab instance's bookkeeping follows its overrides: revert those instead");
            }
            const Entity entity = static_cast<Entity>(object.id);
            const reflect::ComponentDesc* desc = reflect::findByKey(component.key);
            if (!desc) return fail("no component is called " + component.key);
            SceneProject* s = sceneProject();
            void* live = desc->find(s->scene, entity);
            if (!live) return fail(object.name + " has no " + desc->displayName + " now: revert the component");
            // a prefab member's value the baseline did not override: the prefab's, as Revert in
            // the Inspector gives it
            if (object.prefabMember && !property.inBase) {
                return add(new PrefabOverrideCmd(project, sceneId, entity, PrefabOverrideCmd::Operation::RevertProperty, desc->type,
                                                 {topField(property.path)}),
                           "");
            }
            const reflect::FieldRef field = reflect::resolve(*desc, live, property.path);
            if (field && !(field.field->flags & reflect::Derived)) {
                // the baseline's component, decoded by its table into a temporary one
                reflect::Value temporary(desc->body.ops, desc->body.defaults());
                reflect::decodeInto(desc->type, component.baseNode, temporary.get(), nullptr);
                const reflect::FieldRef baseField = reflect::resolve(*desc, temporary.get(), property.path);
                if (!baseField) return fail(std::string("the baseline's ") + desc->displayName + " has no " + property.path);
                return add(new ReflectedValueCmd(project, sceneId, {entity}, desc->type, property.path, reflect::Value(baseField.ops, baseField.ref)), "");
            }
            // a value no field holds (a script's property): spliced into the component, pasted
            YAML::Node spliced = YAML::Clone(EntityClipboard::encodeComponent(s->scene, entity, desc->type));
            if (!scene::setValueAt(spliced, property.path, property.inBase ? property.baseNode : YAML::Node(YAML::NodeType::Undefined))) {
                return fail(property.path + " is not a value of " + desc->displayName);
            }
            std::string reason;
            Command* paste = ComponentValuesCmd::buildPasteValues(project, sceneId, {entity}, desc->type,
                                                                  clipOf(project, sceneId, entity, desc->type, spliced), &reason);
            return add(paste, reason);
        }

        // ---- a whole component
        bool componentWhole(const scene::ObjectDiff& object, const scene::ComponentDiff& component) {
            if (object.settings) return settingsComponent(component);
            if (component.key == scene::kGameObjectKey) {
                for (const scene::PropertyDiff& p : component.properties) {
                    if (!gameObjectProperty(object, p)) return false;
                }
                return true;
            }
            if (component.key == scene::kPrefabInstanceKey || component.key == "prefabOverrides") {
                return fail("the prefab instance's bookkeeping follows its overrides: revert those instead");
            }
            const Entity entity = static_cast<Entity>(object.id);
            const reflect::ComponentDesc* desc = reflect::findByKey(component.key);
            if (!desc) return fail("no component is called " + component.key);
            std::string reason;
            if (object.prefabMember) {
                if (!component.baseNode.IsDefined()) {
                    // no override of it in the baseline: every property back to the prefab's
                    return add(new PrefabOverrideCmd(project, sceneId, entity, PrefabOverrideCmd::Operation::RevertProperty, desc->type, {}), "");
                }
                return add(ComponentValuesCmd::buildPasteValues(project, sceneId, {entity}, desc->type,
                                                                clipOf(project, sceneId, entity, desc->type, YAML::Clone(component.baseNode)), &reason),
                           reason);
            }
            switch (component.state) {
                case scene::ChangeState::Added:
                    return add(new RemoveComponentCmd(project, sceneId, entity, desc->type), "");
                case scene::ChangeState::Removed:
                    return add(ComponentValuesCmd::buildPasteAsNew(project, sceneId, {entity},
                                                                   clipOf(project, sceneId, entity, desc->type, YAML::Clone(component.baseNode)), &reason),
                               reason);
                case scene::ChangeState::Modified:
                    return add(ComponentValuesCmd::buildPasteValues(project, sceneId, {entity}, desc->type,
                                                                    clipOf(project, sceneId, entity, desc->type, YAML::Clone(component.baseNode)), &reason),
                               reason);
                case scene::ChangeState::Unchanged:
                    return true;
            }
            return true;
        }

        // ---- Scene Settings
        bool settingsProperty(const scene::ComponentDiff& component, const scene::PropertyDiff& property) {
            SceneProject* s = sceneProject();
            if (component.key == "scene") {
                YAML::Node after = YAML::Clone(Stream::encodeScene(s->scene));
                if (!scene::setValueAt(after, property.path, property.inBase ? property.baseNode : YAML::Node(YAML::NodeType::Undefined))) {
                    return fail(property.path + " is not a scene setting");
                }
                return add(new SceneSettingsStep(project, sceneId, after), "");
            }
            return settingsComponent(component);
        }

        bool settingsComponent(const scene::ComponentDiff& component) {
            if (component.key == "scene") {
                if (!component.baseNode.IsDefined()) return fail("the baseline has no scene settings");
                return add(new SceneSettingsStep(project, sceneId, YAML::Clone(component.baseNode)), "");
            }
            if (component.key == "mainCamera") {
                uint64_t camera = 0;
                if (component.baseNode.IsDefined() && !scene::parseId(component.baseNode, camera)) return fail("the baseline's main camera is no id");
                if (camera != 0 && !entityExists(static_cast<Entity>(camera))) return fail("the baseline's main camera is not in the scene");
                return add(new SetMainCameraCmd(project, sceneId, static_cast<Entity>(camera)), "");
            }
            return fail(component.key + " is not reverted from here (Project Settings, the scene's own windows)");
        }

        // ---- a whole GameObject
        bool gameObject(const scene::ObjectDiff& object) {
            if (object.settings) {
                for (const scene::ComponentDiff& component : object.components) {
                    if (!settingsComponent(component)) return false;
                }
                return true;
            }
            if (object.prefabMember && (object.state == scene::ChangeState::Added || object.state == scene::ChangeState::Removed)) {
                return fail("a prefab GameObject removed from or added back to its instance: use the instance's Overrides dropdown");
            }
            const Entity entity = static_cast<Entity>(object.id);
            switch (object.state) {
                case scene::ChangeState::Added:
                    if (!entityExists(entity)) return fail(object.name + " is not in the scene");
                    return add(new DeleteEntityCmd(project, sceneId, entity, true), "");
                case scene::ChangeState::Removed: {
                    if (entityExists(entity) && sceneProject()->scene->getEntityName(entity) == object.name) {
                        return fail(object.name + " is in the scene already");
                    }
                    // its children that are gone too come back with it
                    const SceneProject* s = sceneProject();
                    YAML::Node node = YAML::Clone(baseline.entityNode(object.id, [s](uint64_t child) {
                        return !s->scene->isEntityCreated(static_cast<Entity>(child));
                    }));
                    if (!node.IsDefined()) return fail("the baseline has no " + object.name);
                    const scene::SceneObject* base = baseline.find(object.id);
                    Entity parent = base ? static_cast<Entity>(base->parentId) : NULL_ENTITY;
                    if (parent != NULL_ENTITY && !entityExists(parent)) parent = NULL_ENTITY;
                    return add(new RecreateStep(project, sceneId, node, parent, base ? base->siblingIndex : 0), "");
                }
                case scene::ChangeState::Modified:
                case scene::ChangeState::Unchanged:
                    break;
            }
            if (!entityExists(entity)) return fail(object.name + " is not in the scene");
            if (object.reparented || object.reordered) {
                // one move to its baseline parent, at its index there
                const Entity parent = static_cast<Entity>(object.baseParent);
                if (parent != NULL_ENTITY && !entityExists(parent)) return fail("its baseline parent is not in the scene");
                if (Command* move = moveTo(project, sceneId, entity, parent, object.baseIndex)) steps.emplace_back(move);
            }
            for (const scene::ComponentDiff& component : object.components) {
                if (component.key == scene::kPrefabInstanceKey || component.key == "prefabOverrides") continue;
                if (!componentWhole(object, component)) return false;
            }
            if (steps.empty()) return fail("nothing of " + object.name + " differs from the baseline");
            return true;
        }
    };

}

RevertToBaselineCmd::RevertToBaselineCmd(std::string label, std::vector<std::unique_ptr<Command>> steps)
        : label(std::move(label)), steps(std::move(steps)) {
}

RevertToBaselineCmd* RevertToBaselineCmd::build(Project* project, uint32_t sceneId, const scene::SceneYamlModel& baseline,
                                                const scene::ObjectDiff& object, const scene::ComponentDiff* component,
                                                const scene::PropertyDiff* property, std::string* why) {
    std::string reason;
    std::vector<std::unique_ptr<Command>> steps;
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    if (!sceneProject || !sceneProject->scene) {
        if (why) *why = "the scene is not open";
        return nullptr;
    }
    Builder builder{project, sceneId, baseline, steps, reason};
    bool built = false;
    std::string subject;
    if (component && property) {
        built = builder.property(object, *component, *property);
        subject = scene::propertyPath(*component, *property);
    } else if (component) {
        built = builder.componentWhole(object, *component);
        subject = component->key;
    } else {
        built = builder.gameObject(object);
        subject = object.name.empty() ? std::to_string(object.id) : object.name;
    }
    if (!built || steps.empty()) {
        if (why) *why = reason.empty() ? std::string("nothing to revert") : reason;
        return nullptr;
    }
    return new RevertToBaselineCmd("Revert " + subject + " to Baseline", std::move(steps));
}

bool RevertToBaselineCmd::execute() {
    for (size_t i = 0; i < steps.size(); i++) {
        if (steps[i]->execute()) continue;
        // a step that cannot run leaves nothing half done
        while (i-- > 0) steps[i]->undo();
        return false;
    }
    return true;
}

void RevertToBaselineCmd::undo() {
    for (auto it = steps.rbegin(); it != steps.rend(); ++it) (*it)->undo();
}

bool RevertToBaselineCmd::canUndo() const {
    return std::all_of(steps.begin(), steps.end(), [](const std::unique_ptr<Command>& step) { return step->canUndo(); });
}

bool RevertToBaselineCmd::canRedo() const {
    return std::all_of(steps.begin(), steps.end(), [](const std::unique_ptr<Command>& step) { return step->canRedo(); });
}

size_t RevertToBaselineCmd::memoryBytes() const {
    size_t bytes = sizeof(*this) + label.size();
    for (const std::unique_ptr<Command>& step : steps) bytes += step->memoryBytes();
    return bytes;
}
