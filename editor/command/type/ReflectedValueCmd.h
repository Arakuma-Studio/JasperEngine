// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// A value set on a field of a component through its reflection table, on several GameObjects as
// one undo step (docs/specs/S08-reflection.md R11): the old values are kept type-erased
// (reflect::Value), the field's update flags run, prefab overrides are recorded under the field's
// canonical path (an alias edits the field), submesh override masks are kept as PropertyCmd keeps
// them, and consecutive sets of one path merge (a drag is one entry). An array field's element
// can be inserted, removed or moved the same way. Scalar Inspector rows keep PropertyCmd<T>;
// arrays, structs and custom values use this.

#include "command/Command.h"
#include "command/EntityEdit.h"
#include "command/PrefabEditUndo.h"
#include "reflect/Reflect.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    class ReflectedValueCmd : public Command, public EntityEdit {
    public:
        enum class Op { Set, Insert, Remove, Move };

        // The field at `path` of each GameObject's component set to `value` (of the field's type)
        ReflectedValueCmd(Project* project, uint32_t sceneId, std::vector<Entity> entities, ComponentType type, const std::string& path,
                          reflect::Value value);

        // An element of the array at `path`: `element` inserted at `index` (past the end: appended)
        static ReflectedValueCmd* insert(Project* project, uint32_t sceneId, std::vector<Entity> entities, ComponentType type,
                                         const std::string& path, size_t index, reflect::Value element);
        // the element at `index` removed
        static ReflectedValueCmd* remove(Project* project, uint32_t sceneId, std::vector<Entity> entities, ComponentType type,
                                         const std::string& path, size_t index);
        // the element at `from` moved to `to`
        static ReflectedValueCmd* move(Project* project, uint32_t sceneId, std::vector<Entity> entities, ComponentType type,
                                       const std::string& path, size_t from, size_t to);

        void setOnValueChanged(std::function<void()> callback) { onValueChanged = std::move(callback); }

        bool execute() override;
        void undo() override;
        bool mergeWith(Command* other) override;
        bool affectsStructure() const override;
        // "Set Light Intensity", "Add Joint Path Points Element", plus " (N objects)" for several
        std::string describe() const override;
        const char* typeName() const override { return "ReflectedValueCmd"; }
        bool isNoOp() const override;
        size_t memoryBytes() const override;

        std::vector<Entity> editedEntities() const override;
        std::string editedLabel() const override;
        Project* editedProject() const override { return project; }
        uint32_t editedSceneId() const override { return sceneId; }

        const std::string& getPath() const { return path; }
        ComponentType getComponentType() const { return type; }

    private:
        ReflectedValueCmd(Project* project, uint32_t sceneId, std::vector<Entity> entities, ComponentType type, const std::string& path,
                          Op op);

        struct EntityValue {
            reflect::Value oldValue;      // the field (or the whole array) before
            uint32_t oldOverrideFields = 0;
            bool applied = false;         // the GameObject had the field when it ran
            bool changed = false;
        };

        Project* project;
        uint32_t sceneId;
        ComponentType type;
        std::string path;                 // canonical (aliases replaced)
        Op op;
        reflect::Value value;             // Set: the value; Insert: the element
        size_t index = 0;                 // Insert / Remove / Move (from)
        size_t to = 0;                    // Move
        std::vector<Entity> entities;
        std::map<Entity, EntityValue> values;
        PrefabEditUndo prefabUndo;
        bool wasModified = false;
        std::function<void()> onValueChanged;
    };

}
