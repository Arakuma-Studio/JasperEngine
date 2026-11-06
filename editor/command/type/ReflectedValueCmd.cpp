// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "command/type/ReflectedValueCmd.h"

#include "Catalog.h"
#include "Project.h"
#include "command/type/PropertyCmd.h"

using namespace jasperengine;
using namespace jasperengine::editor;

ReflectedValueCmd::ReflectedValueCmd(Project* project, uint32_t sceneId, std::vector<Entity> entities, ComponentType type, const std::string& path,
                                     Op op)
        : project(project), sceneId(sceneId), type(type), path(path), op(op), entities(std::move(entities)) {
    // an alias edits the field: recorded and merged under its own name
    if (const reflect::ComponentDesc* desc = reflect::find(type)) {
        const std::string canonical = reflect::canonicalPath(*desc, nullptr, path);
        if (!canonical.empty()) this->path = canonical;
    }
    if (SceneProject* sceneProject = project->getScene(sceneId)) wasModified = sceneProject->isModified;
}

ReflectedValueCmd::ReflectedValueCmd(Project* project, uint32_t sceneId, std::vector<Entity> entities, ComponentType type, const std::string& path,
                                     reflect::Value value)
        : ReflectedValueCmd(project, sceneId, std::move(entities), type, path, Op::Set) {
    this->value = std::move(value);
}

ReflectedValueCmd* ReflectedValueCmd::insert(Project* project, uint32_t sceneId, std::vector<Entity> entities, ComponentType type,
                                             const std::string& path, size_t index, reflect::Value element) {
    auto* cmd = new ReflectedValueCmd(project, sceneId, std::move(entities), type, path, Op::Insert);
    cmd->index = index;
    cmd->value = std::move(element);
    return cmd;
}

ReflectedValueCmd* ReflectedValueCmd::remove(Project* project, uint32_t sceneId, std::vector<Entity> entities, ComponentType type,
                                             const std::string& path, size_t index) {
    auto* cmd = new ReflectedValueCmd(project, sceneId, std::move(entities), type, path, Op::Remove);
    cmd->index = index;
    return cmd;
}

ReflectedValueCmd* ReflectedValueCmd::move(Project* project, uint32_t sceneId, std::vector<Entity> entities, ComponentType type,
                                           const std::string& path, size_t from, size_t to) {
    auto* cmd = new ReflectedValueCmd(project, sceneId, std::move(entities), type, path, Op::Move);
    cmd->index = from;
    cmd->to = to;
    return cmd;
}

namespace {

    bool sameValue(const reflect::TypeOps* ops, const void* a, const void* b) {
        // a type that cannot compare itself counts as changed
        return ops->equals && ops->equals(a, b, -1.0f);
    }

}

bool ReflectedValueCmd::execute() {
    SceneProject* sceneProject = project->getScene(sceneId);
    const reflect::ComponentDesc* desc = reflect::find(type);
    if (!sceneProject || !desc) return false;
    Scene* scene = sceneProject->scene;
    bool changed = false;
    for (Entity entity : entities) {
        EntityValue& entry = values[entity];
        entry.applied = false;
        entry.changed = false;
        void* comp = desc->find(scene, entity);
        // (a set writes: one past the last element appends it)
        const reflect::FieldRef target = comp ? reflect::resolve(*desc, comp, path, nullptr, op == Op::Set) : reflect::FieldRef();
        if (!target) continue;
        const bool arrayOp = op != Op::Set;
        if (arrayOp && (target.element || target.field->kind != reflect::Kind::Array || !target.field->arrayOps)) continue;
        if (op == Op::Set && (!value.ops() || value.ops()->size != target.ops->size)) continue;
        entry.applied = true;
        entry.oldValue = reflect::Value(target.ops, target.ref);
        prefabUndo.capture(project, sceneId, entity);

        uint64_t propertyFields = 0;
        uint64_t* overrideMask = Catalog::getSubmeshOverrideMask(scene, entity, type, path, propertyFields);
        if (overrideMask) entry.oldOverrideFields = *overrideMask;

        const reflect::ArrayOps* array = target.field->arrayOps;
        switch (op) {
            case Op::Set:
                target.ops->copy(target.ref, value.get());
                break;
            case Op::Insert:
                array->insert(target.ref, std::min(index, array->size(target.ref)), value.get());
                break;
            case Op::Remove:
                if (index < array->size(target.ref)) array->erase(target.ref, index);
                break;
            case Op::Move:
                array->move(target.ref, index, to);
                break;
        }
        if (sameValue(target.ops, entry.oldValue.get(), target.ref)) continue;
        entry.changed = true;
        if (overrideMask) *overrideMask |= propertyFields;

        Catalog::markChanged(scene, entity, type);
        Catalog::updateEntity(scene, entity, Catalog::findProperty(scene, entity, type, path).updateFlags);
        project->recordPrefabEdit(sceneId, entity, type, {path});
        changed = true;
    }
    if (changed) sceneProject->isModified = true;
    if (onValueChanged) onValueChanged();
    return true;
}

void ReflectedValueCmd::undo() {
    SceneProject* sceneProject = project->getScene(sceneId);
    const reflect::ComponentDesc* desc = reflect::find(type);
    if (!sceneProject || !desc) return;
    Scene* scene = sceneProject->scene;
    for (auto& [entity, entry] : values) {
        if (!entry.applied) continue;
        void* comp = desc->find(scene, entity);
        const reflect::FieldRef target = comp ? reflect::resolve(*desc, comp, path) : reflect::FieldRef();
        if (!target || !entry.oldValue.ops() || entry.oldValue.ops()->size != target.ops->size) continue;
        target.ops->copy(target.ref, entry.oldValue.get());
        uint64_t propertyFields = 0;
        if (uint64_t* overrideMask = Catalog::getSubmeshOverrideMask(scene, entity, type, path, propertyFields)) {
            *overrideMask = entry.oldOverrideFields;
        }
        if (!entry.changed) continue;
        Catalog::markChanged(scene, entity, type);
        Catalog::updateEntity(scene, entity, Catalog::findProperty(scene, entity, type, path).updateFlags);
    }
    prefabUndo.restore(project, sceneId);
    sceneProject->isModified = wasModified;
    if (onValueChanged) onValueChanged();
}

bool ReflectedValueCmd::mergeWith(Command* otherCommand) {
    // a drag: the newer value with the older command's old values (what one undo restores)
    auto* other = dynamic_cast<ReflectedValueCmd*>(otherCommand);
    if (!other || op != Op::Set || other->op != Op::Set || sceneId != other->sceneId || type != other->type || path != other->path) {
        return false;
    }
    for (auto& [entity, older] : other->values) {
        auto it = values.find(entity);
        if (it == values.end()) {
            values[entity] = std::move(older);
            entities.push_back(entity);
            continue;
        }
        it->second.oldValue = std::move(older.oldValue);
        it->second.oldOverrideFields = older.oldOverrideFields;
        it->second.applied = it->second.applied || older.applied;
        it->second.changed = it->second.changed || older.changed;
    }
    prefabUndo.mergeOlder(other->prefabUndo);
    wasModified = wasModified && other->wasModified;
    if (!onValueChanged) onValueChanged = other->onValueChanged;
    return true;
}

bool ReflectedValueCmd::affectsStructure() const {
    return !isNonStructuralProperty(type, path);
}

std::string ReflectedValueCmd::describe() const {
    const reflect::ComponentDesc* desc = reflect::find(type);
    const std::string component = desc ? desc->displayName : Catalog::getComponentDisplayName(type);
    std::string text;
    switch (op) {
        case Op::Set: text = "Set " + component + " " + editedLabel(); break;
        case Op::Insert: text = "Add " + component + " " + editedLabel() + " Element"; break;
        case Op::Remove: text = "Remove " + component + " " + editedLabel() + " Element"; break;
        case Op::Move: text = "Move " + component + " " + editedLabel() + " Element"; break;
    }
    if (entities.size() > 1) text += " (" + std::to_string(entities.size()) + " objects)";
    return text;
}

bool ReflectedValueCmd::isNoOp() const {
    for (const auto& [entity, entry] : values) {
        if (entry.applied && entry.changed) return false;
    }
    return true;
}

size_t ReflectedValueCmd::memoryBytes() const {
    size_t bytes = sizeof(*this) + path.size();
    if (value.ops()) bytes += value.ops()->size;
    for (const auto& [entity, entry] : values) bytes += sizeof(entry) + (entry.oldValue.ops() ? entry.oldValue.ops()->size : 0);
    return bytes;
}

std::vector<Entity> ReflectedValueCmd::editedEntities() const {
    return entities;
}

std::string ReflectedValueCmd::editedLabel() const {
    const reflect::ComponentDesc* desc = reflect::find(type);
    const reflect::FieldDesc* field = desc ? reflect::findField(*desc, path) : nullptr;
    return field ? reflect::label(*field) : CommandNames::property(path);
}
