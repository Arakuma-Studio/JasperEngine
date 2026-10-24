// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "command/CommandNames.h"
#include "command/EntityEdit.h"
#include "command/PlayChangeReview.h"
#include "command/PrefabEditUndo.h"
#include "Scene.h"
#include "math/Vector3.h"
#include "ecs/Entity.h"
#include "component/Transform.h"
#include "Catalog.h"
#include <functional>
#include <type_traits>


namespace jasperengine::editor{


    template<typename T, typename = void>
    struct is_iterable_container : std::false_type {};

    template<typename T>
    struct is_iterable_container<T, std::void_t<decltype(std::declval<T>().begin()), decltype(std::declval<T>().end()), typename T::value_type>> : std::true_type {};

    template<typename T, typename = void>
    struct is_equality_comparable : std::false_type {};

    template<typename T>
    struct is_equality_comparable<T, std::void_t<decltype(std::declval<const T&>() == std::declval<const T&>())>> : std::true_type {};

    template<typename T, typename = void>
    struct safe_has_equality_operator : std::false_type {};

    template<typename T>
    struct safe_has_equality_operator<T, std::enable_if_t<!is_iterable_container<T>::value>>
        : is_equality_comparable<T> {};

    template<typename T>
    struct safe_has_equality_operator<T, std::enable_if_t<is_iterable_container<T>::value>>
        : is_equality_comparable<typename T::value_type> {};

    template<typename T>
    inline constexpr bool has_equality_operator_v = safe_has_equality_operator<T>::value;


    template<typename T>
    struct PropertyCmdValue{
        T oldValue;
        T newValue;
        uint32_t oldOverrideFields = 0; // submesh properties only, see getSubmeshOverrideMask
        bool applied = false;           // the entity had the property when it ran
    };

    // Component types the Hierarchy tree reads nothing from: it caches names, signature
    // icons, hierarchy and prefab/lock state, not component values. Opt-out list, so
    // anything unlisted stays structural.
    inline bool isNonStructuralProperty(ComponentType type, const std::string& propertyName){
        switch (type){
            // Geometry and sub-selection payloads, dragged continuously in the viewport.
            case ComponentType::InstancedMeshComponent:
            case ComponentType::TilemapComponent:
            case ComponentType::LineRendererComponent:
            case ComponentType::PointsComponent:
            case ComponentType::PolygonComponent:
            case ComponentType::MeshPolygonComponent:
            case ComponentType::ShadowCaster2DComponent:
            case ComponentType::TranslateTracksComponent:
            case ComponentType::RotateTracksComponent:
            case ComponentType::ScaleTracksComponent:
            // Sizes written by the UI resize drag.
            case ComponentType::RectTransformComponent:
            case ComponentType::SpriteComponent:
            case ComponentType::TextComponent:
                return true;
            // Document placement and input; filename and render settings reload the document.
            case ComponentType::UIDocumentComponent:
                return propertyName == "sortOrder" || propertyName == "enabled" || propertyName == "interactive" || propertyName == "pixelRatio";
            // Transform.parent drives tree order and lock state.
            case ComponentType::Transform:
                return propertyName == "position" || propertyName == "rotation" || propertyName == "scale";
            default:
                return false;
        }
    }

    template<typename T>
    class PropertyCmd: public Command, public EntityEdit{

    private:
        Project* project;
        uint32_t sceneId;
        ComponentType type;
        std::string propertyName;
        bool wasModified;
        std::function<void()> onValueChanged;

        std::map<Entity,PropertyCmdValue<T>> values;
        // Unity: a value edited on a prefab instance's GameObject is an override of it
        PrefabEditUndo prefabUndo;

    public:

        PropertyCmd(Project* project, uint32_t sceneId, Entity entity, ComponentType type, std::string propertyName, T newValue, std::function<void()> onValueChanged = nullptr){
            this->project = project;
            this->sceneId = sceneId;
            this->type = type;
            this->propertyName = propertyName;
            this->onValueChanged = onValueChanged;

            this->values[entity].newValue = newValue;
            this->wasModified = project->getScene(sceneId)->isModified;
        }

        bool execute() override{
            SceneProject* sceneProject = project->getScene(sceneId);
            if (!sceneProject){
                return false;
            }
            bool changed = false;
            for (auto& [entity, value] : values){
                PropertyData prop = Catalog::findProperty(sceneProject->scene, entity, type, propertyName);
                T* valueRef = static_cast<T*>(prop.ref);
                // script properties are per entity, a selection may not share all of them
                if (!valueRef){
                    value.applied = false;
                    continue;
                }

                value.applied = true;
                value.oldValue = T(*valueRef);
                prefabUndo.capture(project, sceneId, entity);
                *valueRef = value.newValue;

                // Read before the no-op check below: a command merging into this one adopts this
                // mask, and an entity already at the new value still carries its earlier edits.
                uint32_t propertyFields = 0;
                uint32_t* overrideMask = Catalog::getSubmeshOverrideMask(sceneProject->scene, entity, type, propertyName, propertyFields);
                if (overrideMask){
                    value.oldOverrideFields = *overrideMask;
                }

                if constexpr (has_equality_operator_v<T>) {
                    if (value.oldValue == value.newValue){
                        continue;
                    }
                }

                if (overrideMask){
                    *overrideMask |= propertyFields;
                }

                Catalog::markChanged(sceneProject->scene, entity, type);
                Catalog::updateEntity(sceneProject->scene, entity, prop.updateFlags);

                project->recordPrefabEdit(sceneId, entity, type, {propertyName});
                changed = true;
            }

            // a value set to what it was is no edit (the history drops the command)
            if (changed){
                sceneProject->isModified = true;
            }

            if (onValueChanged) {
                onValueChanged();
            }

            return true;
        }

        void undo() override{
            SceneProject* sceneProject = project->getScene(sceneId);
            if (!sceneProject){
                return;
            }
            for (auto const& [entity, value] : values){
                PropertyData prop = Catalog::findProperty(sceneProject->scene, entity, type, propertyName);
                T* valueRef = static_cast<T*>(prop.ref);
                if (!valueRef){
                    continue;
                }

                *valueRef = value.oldValue;

                // Mirrors execute: restored whether or not the value moved.
                uint32_t propertyFields = 0;
                if (uint32_t* overrideMask = Catalog::getSubmeshOverrideMask(sceneProject->scene, entity, type, propertyName, propertyFields)){
                    *overrideMask = value.oldOverrideFields;
                }

                if constexpr (has_equality_operator_v<T>) {
                    if (value.oldValue == value.newValue){
                        continue;
                    }
                }

                Catalog::markChanged(sceneProject->scene, entity, type);
                Catalog::updateEntity(sceneProject->scene, entity, prop.updateFlags);
            }
            prefabUndo.restore(project, sceneId);

            sceneProject->isModified = wasModified;

            if (onValueChanged) {
                onValueChanged();
            }
        }

        bool affectsStructure() const override{
            return !isNonStructuralProperty(type, propertyName);
        }

        std::string describe() const override{
            return "Set " + editedLabel() + " (" + CommandNames::entities(project, sceneId, editedEntities()) + ")";
        }

        const char* typeName() const override{ return "PropertyCmd"; }

        // Every entity it reached kept its value (a value type without == always counts as an edit)
        bool isNoOp() const override{
            for (auto const& [entity, value] : values){
                if (!value.applied){
                    continue;
                }
                if constexpr (has_equality_operator_v<T>) {
                    if (!(value.oldValue == value.newValue)){
                        return false;
                    }
                }else{
                    return false;
                }
            }
            return true;
        }

        std::vector<Entity> editedEntities() const override{
            std::vector<Entity> entities;
            entities.reserve(values.size());
            for (auto const& [entity, value] : values){
                entities.push_back(entity);
            }
            return entities;
        }

        std::string editedLabel() const override{
            return CommandNames::property(propertyName);
        }

        Project* editedProject() const override{ return project; }
        uint32_t editedSceneId() const override{ return sceneId; }

        // A fresh PropertyCmd per GameObject, with the value it has now
        void collectKeepCommands(KeepContext& context) const override{
            SceneProject* sceneProject = project->getScene(sceneId);
            if (!sceneProject){
                return;
            }
            for (auto const& [entity, value] : values){
                if (!value.applied){
                    continue;
                }
                PropertyData prop = Catalog::findProperty(sceneProject->scene, entity, type, propertyName);
                if (!prop.ref){
                    continue;
                }
                const T current = *static_cast<T*>(prop.ref);
                KeepRow row;
                row.kind = KeepRow::Kind::Property;
                row.entity = entity;
                row.hasComponent = true;
                row.component = type;
                row.property = propertyName;
                row.playValue = KeepContext::text(current);
                Project* target = project;
                const uint32_t id = sceneId;
                const ComponentType component = type;
                const std::string name = propertyName;
                const Entity edited = entity;
                row.factory = [target, id, edited, component, name, current]() -> std::unique_ptr<Command> {
                    return std::make_unique<PropertyCmd<T>>(target, id, edited, component, name, current);
                };
                row.readEditValue = [target, id, edited, component, name]() -> std::string {
                    SceneProject* restored = target->getScene(id);
                    PropertyData now = restored ? Catalog::findProperty(restored->scene, edited, component, name) : PropertyData{};
                    return now.ref ? KeepContext::text(*static_cast<T*>(now.ref)) : std::string("(none)");
                };
                context.add(std::move(row));
            }
        }

        uint32_t getSceneId() const{ return sceneId; }
        ComponentType getComponentType() const{ return type; }
        const std::string& getPropertyName() const{ return propertyName; }

        bool mergeWith(editor::Command* otherCommand) override{
            PropertyCmd* otherCmd = dynamic_cast<PropertyCmd*>(otherCommand);
            if (otherCmd != nullptr){
                if (sceneId == otherCmd->sceneId && propertyName == otherCmd->propertyName){
                    for (auto const& [otherEntity, otherValue] : otherCmd->values){
                        if (values.find(otherEntity) != values.end()) {
                            // The older command holds the state a single undo has to restore.
                            values[otherEntity].oldValue = otherValue.oldValue;
                            values[otherEntity].oldOverrideFields = otherValue.oldOverrideFields;
                            values[otherEntity].applied = values[otherEntity].applied || otherValue.applied;
                        }else{
                            values[otherEntity] = otherValue;
                        }
                    }
                    prefabUndo.mergeOlder(otherCmd->prefabUndo);
                    wasModified = wasModified && otherCmd->wasModified;
                    // Keep the most recent callback
                    if (otherCmd->onValueChanged) {
                        onValueChanged = otherCmd->onValueChanged;
                    }
                    return true;
                }
            }

            return false;
        }

    };

}
