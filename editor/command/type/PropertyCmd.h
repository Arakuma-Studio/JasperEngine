// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "command/PrefabEditUndo.h"
#include "Scene.h"
#include "math/Vector3.h"
#include "ecs/Entity.h"
#include "component/Transform.h"
#include "Catalog.h"
#include "animation/ClipRecordHooks.h"
#include <cstdlib>
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
            case ComponentType::LatticeTracksComponent:
            // Lattice points, dragged in the viewport, and its settings; bindings and followers
            // reference other GameObjects but the tree shows nothing of them.
            case ComponentType::LatticeComponent:
            case ComponentType::LatticeModifierComponent:
            case ComponentType::LatticeFollowerComponent:
            // Sizes written by the UI resize drag.
            case ComponentType::RectTransformComponent:
            case ComponentType::SpriteComponent:
            case ComponentType::TextComponent:
                return true;
            // Document placement and input; filename and render settings reload the document.
            case ComponentType::UIDocumentComponent:
                return propertyName == "sortOrder" || propertyName == "enabled" || propertyName == "interactive" || propertyName == "pixelRatio";
            // Nothing of it is shown in the Hierarchy.
            case ComponentType::PoseAnimatorComponent:
            case ComponentType::AnimationPlayerComponent:
            case ComponentType::VertexAnimatorComponent:
            case ComponentType::TrailRendererComponent:
                return true;
            // Transform.parent drives tree order and lock state.
            case ComponentType::Transform:
                return propertyName == "position" || propertyName == "rotation" || propertyName == "scale";
            default:
                return false;
        }
    }

    template<typename T>
    class PropertyCmd: public Command{

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
        // the first execute ran (components left as a clip preview showed them were put back)
        bool executed = false;
        // Vector3 components (bit i: x y z) the caller carried as the Scene view shows them
        // instead of editing them (a Transform row's other fields): with a clip preview they keep
        // their rest. Every other component is a value set on purpose (setShownComponents)
        uint32_t shownComponents = 0;

        // what this write names explicitly for the clip preview's rest (ClipRecordHooks)
        ClipRecordHooks::ExplicitWrite explicitWrite(bool undoing) const{
            ClipRecordHooks::ExplicitWrite write;
            if (undoing || shownComponents == 0){
                // a weight of a mesh's blend shapes is one component of its morph channel
                if (type == ComponentType::MeshComponent && propertyName.compare(0, 13, "morphWeights[") == 0 && !undoing){
                    write.components.push_back((size_t)std::strtoul(propertyName.c_str() + 13, nullptr, 10));
                }else{
                    write.all = true;
                }
                return write;
            }
            for (size_t i = 0; i < 3; i++){
                if (!(shownComponents & (1u << i))) write.components.push_back(i);
            }
            return write;
        }

        void notifyAfterWrite(bool undoing){
            ClipRecordHooks::ExplicitWriteScope scope(explicitWrite(undoing));
            for (const auto& entry : values) ClipRecordHooks::afterComponentWrite(sceneId, entry.first, type, propertyName);
        }

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

        // the Vector3 components (bit 0 x, 1 y, 2 z) the value carries as the Scene view shows
        // them, not edited (an Inspector Transform row's other fields): see shownComponents
        void setShownComponents(uint32_t mask){ shownComponents = mask; }

        bool execute() override{
            SceneProject* sceneProject = project->getScene(sceneId);
            if (!sceneProject){
                return false;
            }
            // A Transform Position / Scale row edits one component and carries the others as the
            // Scene view shows them (setShownComponents): with a clip preview those may be
            // previewed values. Read them before the rest is put back, so a component left as
            // shown keeps its rest (T3c review C1); the edited one is the new rest even when
            // equal to the preview, as is every component of any other named write (FX)
            std::map<Entity, T> shown;
            if constexpr (std::is_same_v<T, Vector3>) {
                if (!executed && shownComponents != 0){
                    for (const auto& entry : values){
                        PropertyData prop = Catalog::findProperty(sceneProject->scene, entry.first, type, propertyName);
                        if (prop.ref) shown[entry.first] = *static_cast<T*>(prop.ref);
                    }
                }
            }
            // a posed bone (Pose window) or a previewed channel (clip preview) goes back to its rest
            // first: the command records and edits the rest (animation clips 9.R5/R6)
            for (const auto& entry : values) ClipRecordHooks::beforeComponentWrite(sceneId, entry.first, type, propertyName);
            for (auto& [entity, value] : values){
                PropertyData prop = Catalog::findProperty(sceneProject->scene, entity, type, propertyName);
                T* valueRef = static_cast<T*>(prop.ref);
                // script properties are per entity, a selection may not share all of them
                if (!valueRef){
                    continue;
                }

                value.oldValue = T(*valueRef);
                if constexpr (std::is_same_v<T, Vector3>) {
                    auto it = shown.find(entity);
                    if (it != shown.end()){
                        const Vector3& seen = it->second;
                        if ((shownComponents & 1u) && value.newValue.x == seen.x && value.oldValue.x != seen.x) value.newValue.x = value.oldValue.x;
                        if ((shownComponents & 2u) && value.newValue.y == seen.y && value.oldValue.y != seen.y) value.newValue.y = value.oldValue.y;
                        if ((shownComponents & 4u) && value.newValue.z == seen.z && value.oldValue.z != seen.z) value.newValue.z = value.oldValue.z;
                    }
                }
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

                Catalog::updateEntity(sceneProject->scene, entity, prop.updateFlags);

                project->recordPrefabEdit(sceneId, entity, type, {propertyName});
            }

            sceneProject->isModified = true;
            executed = true;

            notifyAfterWrite(false);

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
            for (const auto& entry : values) ClipRecordHooks::beforeComponentWrite(sceneId, entry.first, type, propertyName);
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

                Catalog::updateEntity(sceneProject->scene, entity, prop.updateFlags);
            }
            prefabUndo.restore(project, sceneId);

            sceneProject->isModified = wasModified;

            notifyAfterWrite(true);

            if (onValueChanged) {
                onValueChanged();
            }
        }

        bool affectsStructure() const override{
            return !isNonStructuralProperty(type, propertyName);
        }

        bool mergeWith(editor::Command* otherCommand) override{
            PropertyCmd* otherCmd = dynamic_cast<PropertyCmd*>(otherCommand);
            if (otherCmd != nullptr){
                if (sceneId == otherCmd->sceneId && propertyName == otherCmd->propertyName){
                    for (auto const& [otherEntity, otherValue] : otherCmd->values){
                        if (values.find(otherEntity) != values.end()) {
                            // The older command holds the state a single undo has to restore.
                            values[otherEntity].oldValue = otherValue.oldValue;
                            values[otherEntity].oldOverrideFields = otherValue.oldOverrideFields;
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
