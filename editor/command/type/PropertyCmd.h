// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "reflect/Reflect.h"
#include "command/Command.h"
#include "command/CommandNames.h"
#include "command/EntityEdit.h"
#include "command/PlayChangeReview.h"
#include "command/PrefabEditUndo.h"
#include "Scene.h"
#include "math/Vector3.h"
#include "ecs/Entity.h"
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
        uint64_t oldOverrideFields = 0; // submesh properties only, see getSubmeshOverrideMask
        bool applied = false;           // the entity had the property when it ran
    };

    // A string set on an asset reference (Asset, Shader properties) is its path: the reference
    // as it was, GUIDs included, comes back on undo
    template<>
    struct PropertyCmdValue<std::string>{
        std::string oldValue;
        std::string newValue;
        uint64_t oldOverrideFields = 0;
        bool applied = false;
        AssetRef oldAsset;
        ShaderRef oldShader;
    };

    // Component types the Hierarchy tree reads nothing from: it caches names, signature
    // icons, hierarchy and prefab/lock state, not component values. Opt-out list, so
    // anything unlisted stays structural.
    // What a component's reflection table says of a property (a field's NonStructural flag, else
    // its component's: geometry and payloads dragged continuously in the viewport, UI sizes, the
    // Transform's position, rotation and scale, a document's placement and input)
    inline bool isNonStructuralProperty(ComponentType type, const std::string& propertyName){
        const reflect::ComponentDesc* desc = reflect::find(type);
        if (!desc){
            return false;
        }
        const reflect::FieldDesc* field = reflect::findField(*desc, propertyName);
        return field ? !reflect::isStructural(*desc, *field) : !desc->structural;
    }

    // What Record reads of a property edit (anim-properties spec 10): the Inspector row builds the
    // command, Record takes its values instead of running it
    class RecordablePropertyCommand{
    public:
        virtual ~RecordablePropertyCommand() {}
        // the new value of each entity as floats of the stored value (float, double, ints and
        // unsigned, bool 0/1, Vector2 to 4); false for other value types
        virtual bool newValueFloats(std::vector<std::pair<Entity, std::vector<float>>>& out) const = 0;
    };

    template<typename T>
    class PropertyCmd: public Command, public EntityEdit, public RecordablePropertyCommand{

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

        bool newValueFloats(std::vector<std::pair<Entity, std::vector<float>>>& out) const override{
            out.clear();
            for (const auto& entry : values){
                const T& value = entry.second.newValue;
                // (sized, then filled: an initializer list assign trips GCC's -Wnonnull here)
                std::vector<float> floats(4, 0.0f);
                size_t width = 0;
                if constexpr (std::is_same_v<T, float> || std::is_same_v<T, double> || std::is_same_v<T, int> || std::is_same_v<T, unsigned int>){
                    floats[0] = (float)value;
                    width = 1;
                }else if constexpr (std::is_same_v<T, bool>){
                    floats[0] = value ? 1.0f : 0.0f;
                    width = 1;
                }else if constexpr (std::is_same_v<T, Vector2>){
                    floats[0] = value.x;
                    floats[1] = value.y;
                    width = 2;
                }else if constexpr (std::is_same_v<T, Vector3>){
                    floats[0] = value.x;
                    floats[1] = value.y;
                    floats[2] = value.z;
                    width = 3;
                }else if constexpr (std::is_same_v<T, Vector4>){
                    floats[0] = value.x;
                    floats[1] = value.y;
                    floats[2] = value.z;
                    floats[3] = value.w;
                    width = 4;
                }else{
                    (void)value;
                    return false;
                }
                floats.resize(width);
                out.push_back({entry.first, floats});
            }
            return !out.empty();
        }

        bool execute() override{
            SceneProject* sceneProject = project->getScene(sceneId);
            if (!sceneProject){
                return false;
            }
            bool changed = false;
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
                // (a write: "tiles[N]" one past the last tile appends it)
                PropertyData prop = Catalog::findProperty(sceneProject->scene, entity, type, propertyName, true);
                // script properties are per entity, a selection may not share all of them
                if (!prop.ref){
                    value.applied = false;
                    continue;
                }

                value.applied = true;
                if constexpr (std::is_same_v<T, std::string>) {
                    // an asset reference set by its path: a new path drops the GUID (S04)
                    if (prop.type == PropertyType::Asset) {
                        AssetRef& ref = *static_cast<AssetRef*>(prop.ref);
                        value.oldAsset = ref;
                        value.oldValue = ref.path;
                        prefabUndo.capture(project, sceneId, entity);
                        ref = value.newValue;
                    } else if (prop.type == PropertyType::Shader) {
                        ShaderRef& ref = *static_cast<ShaderRef*>(prop.ref);
                        value.oldShader = ref;
                        value.oldValue = ref.path;
                        prefabUndo.capture(project, sceneId, entity);
                        ref = value.newValue;
                    } else {
                        std::string* valueRef = static_cast<std::string*>(prop.ref);
                        value.oldValue = *valueRef;
                        prefabUndo.capture(project, sceneId, entity);
                        *valueRef = value.newValue;
                    }
                } else {
                    T* valueRef = static_cast<T*>(prop.ref);
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
                }

                // Read before the no-op check below: a command merging into this one adopts this
                // mask, and an entity already at the new value still carries its earlier edits.
                uint64_t propertyFields = 0;
                uint64_t* overrideMask = Catalog::getSubmeshOverrideMask(sceneProject->scene, entity, type, propertyName, propertyFields);
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
                if (!prop.ref){
                    continue;
                }

                if constexpr (std::is_same_v<T, std::string>) {
                    if (prop.type == PropertyType::Asset) {
                        *static_cast<AssetRef*>(prop.ref) = value.oldAsset;
                    } else if (prop.type == PropertyType::Shader) {
                        *static_cast<ShaderRef*>(prop.ref) = value.oldShader;
                    } else {
                        *static_cast<std::string*>(prop.ref) = value.oldValue;
                    }
                } else {
                    *static_cast<T*>(prop.ref) = value.oldValue;
                }

                // Mirrors execute: restored whether or not the value moved.
                uint64_t propertyFields = 0;
                if (uint64_t* overrideMask = Catalog::getSubmeshOverrideMask(sceneProject->scene, entity, type, propertyName, propertyFields)){
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

            notifyAfterWrite(true);

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
                            if constexpr (std::is_same_v<T, std::string>) {
                                values[otherEntity].oldAsset = otherValue.oldAsset;
                                values[otherEntity].oldShader = otherValue.oldShader;
                            }
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
