// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PlayChangeReview.h"
#include "component/Transform.h"

#include "Project.h"
#include "command/CommandGroup.h"
#include "command/CommandHandle.h"
#include "command/UndoOrigin.h"
#include "command/type/AddComponentCmd.h"
#include "command/type/ComponentValuesCmd.h"
#include "command/type/DeleteEntityCmd.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/PasteEntitiesCmd.h"
#include "command/type/RemoveComponentCmd.h"
#include "util/EntityClipboard.h"

#include <algorithm>
#include <cstdio>

using namespace jasperengine;

bool editor::PlayChangeReview::enabled = true;

// ------------------------------------------------------------------ KeepContext

std::string editor::KeepContext::number(double value){
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.4g", value);
    return buffer;
}

bool editor::KeepContext::madeInPlay(Entity entity) const{
    if (!sceneProject){
        return false;
    }
    return !sceneProject->playStateEntities.count(entity) &&
        std::find(sceneProject->entities.begin(), sceneProject->entities.end(), entity) != sceneProject->entities.end();
}

bool editor::KeepContext::runtimeOnly(Entity entity) const{
    if (!sceneProject || entity == NULL_ENTITY){
        return false;
    }
    return !sceneProject->playStateEntities.count(entity) &&
        std::find(sceneProject->entities.begin(), sceneProject->entities.end(), entity) == sceneProject->entities.end();
}

std::string editor::KeepContext::name(Entity entity) const{
    if (!sceneProject || !sceneProject->scene || !sceneProject->scene->isEntityCreated(entity)){
        return "GameObject";
    }
    const std::string entityName = sceneProject->scene->getEntityName(entity);
    return entityName.empty() ? std::string("GameObject") : entityName;
}

void editor::KeepContext::add(KeepRow row){
    row.id = nextId++;
    row.source = source;
    if (row.gameObject.empty() && row.entity != NULL_ENTITY){
        row.gameObject = name(row.entity);
    }
    if (row.kind != KeepRow::Kind::Unsupported && runtimeOnly(row.entity)){
        row.keepable = false;
        row.reason = "Created by a script during Play";
        row.factory = nullptr;
    }
    row.madeInPlay = row.entity != NULL_ENTITY && madeInPlay(row.entity);

    auto sameEntity = [&row](const KeepRow& other){
        return other.entity == row.entity && other.kind != KeepRow::Kind::Unsupported;
    };
    if (row.kind == KeepRow::Kind::Created){
        // the snapshot carries every later edit of it
        rows.erase(std::remove_if(rows.begin(), rows.end(), sameEntity), rows.end());
    }else if (row.kind == KeepRow::Kind::Deleted){
        const bool created = std::any_of(rows.begin(), rows.end(), [&row](const KeepRow& other){
            return other.entity == row.entity && other.kind == KeepRow::Kind::Created;
        });
        rows.erase(std::remove_if(rows.begin(), rows.end(), sameEntity), rows.end());
        if (created || row.madeInPlay){
            return;     // made and deleted while playing: nothing to keep
        }
    }else{
        // a later edit of a GameObject made while playing is in its Created snapshot
        const bool created = std::any_of(rows.begin(), rows.end(), [&row](const KeepRow& other){
            return other.entity == row.entity && other.kind == KeepRow::Kind::Created;
        });
        if (created){
            return;
        }
        rows.erase(std::remove_if(rows.begin(), rows.end(), [&row](const KeepRow& other){
            return other.sameTarget(row);
        }), rows.end());
    }
    rows.push_back(std::move(row));
}

void editor::KeepContext::collect(const Command& cmd, const std::string& stepName){
    const std::string previous = source;
    source = stepName;
    const uint64_t before = nextId;
    handled = false;
    cmd.collectKeepCommands(*this);
    if (nextId == before && !handled){
        KeepRow row;
        row.kind = KeepRow::Kind::Unsupported;
        row.property = stepName + ": can't be kept";
        row.keepable = false;
        row.reason = "This kind of step can't be kept after Play";
        add(std::move(row));
    }
    source = previous;
    handled = false;
}

void editor::KeepContext::addComponentValues(Entity entity, ComponentType type, bool added){
    Scene* scene = sceneProject->scene;
    if (!scene->isEntityCreated(entity) || !scene->getSignature(entity).test(Catalog::getComponentId(scene, type))){
        return;
    }
    auto clip = std::make_shared<ComponentClip>();
    clip->project = EntityClipboard::projectKey(project);
    clip->sceneId = sceneId;
    clip->type = type;
    clip->entity = entity;
    clip->value = EntityClipboard::encodeComponent(scene, entity, type);
    // its references to the scene's GameObjects stay (they are there after Stop too)
    for (Entity other : sceneProject->entities){
        if (scene->isEntityCreated(other)) clip->externalNames[other] = scene->getEntityName(other);
    }

    KeepRow row;
    row.kind = added ? KeepRow::Kind::AddComponent : KeepRow::Kind::Component;
    row.entity = entity;
    row.hasComponent = true;
    row.component = type;
    row.playValue = added ? std::string("Added") : std::string("Values at Stop");
    Project* target = project;
    const uint32_t id = sceneId;
    row.factory = [target, id, entity, type, clip, added]() -> std::unique_ptr<Command> {
        auto values = std::make_unique<ComponentValuesCmd>(target, id, std::vector<Entity>{entity}, type, clip);
        if (!added){
            return values;
        }
        auto multi = std::make_unique<MultiPropertyCmd>();
        multi->addCommand(std::make_unique<AddComponentCmd>(target, id, entity, type));
        multi->addCommand(std::move(values));
        return multi;
    };
    if (added){
        row.readEditValue = []() { return std::string("(none)"); };
    }else{
        // the keys the play values differ in, read once the scene is back
        row.readEditValue = [target, id, entity, type, clip]() -> std::string {
            SceneProject* restored = target->getScene(id);
            if (!restored || !restored->scene->isEntityCreated(entity) ||
                !restored->scene->getSignature(entity).test(Catalog::getComponentId(restored->scene, type))){
                return std::string();
            }
            const YAML::Node now = EntityClipboard::encodeComponent(restored->scene, entity, type);
            std::string keys;
            if (clip->value.IsMap()){
                for (const auto& pair : clip->value){
                    const std::string key = pair.first.as<std::string>();
                    if (!now[key] || YAML::Dump(now[key]) != YAML::Dump(pair.second)){
                        if (!keys.empty()) keys += ", ";
                        keys += key;
                    }
                }
            }
            return keys;
        };
    }
    add(std::move(row));
}

void editor::KeepContext::addCreated(Entity root){
    Scene* scene = sceneProject->scene;
    if (!scene->isEntityCreated(root)){
        return;
    }
    std::shared_ptr<GameObjectsClip> clip = EntityClipboard::snapshotGameObjects(project, sceneId, {root});
    KeepRow row;
    row.kind = KeepRow::Kind::Created;
    row.entity = root;
    row.playValue = "Created";
    if (Transform* transform = scene->findComponent<Transform>(root)){
        row.parent = transform->parent;
    }
    if (!clip){
        row.keepable = false;
        row.reason = "It can't be copied (a part of a model or a control)";
        add(std::move(row));
        return;
    }
    Project* target = project;
    const uint32_t id = sceneId;
    const Entity parent = row.parent;
    row.factory = [target, id, clip, parent]() -> std::unique_ptr<Command> {
        return std::make_unique<PasteEntitiesCmd>(target, id, clip,
            parent != NULL_ENTITY ? EntityClipboard::Placement::Into : EntityClipboard::Placement::Root, parent);
    };
    add(std::move(row));
}

void editor::KeepContext::addDeleted(Entity entity, const std::string& gameObject){
    KeepRow row;
    row.kind = KeepRow::Kind::Deleted;
    row.entity = entity;
    row.gameObject = gameObject;
    row.playValue = "Deleted";
    Project* target = project;
    const uint32_t id = sceneId;
    row.factory = [target, id, entity]() -> std::unique_ptr<Command> {
        return std::make_unique<DeleteEntityCmd>(target, id, entity);
    };
    row.readEditValue = []() { return std::string("Exists"); };
    // gone from the play-time scene: neither a script's nor a command's GameObject now
    const bool authored = sceneProject->playStateEntities.count(entity) != 0;
    add(std::move(row));
    if (!authored && !rows.empty() && rows.back().entity == entity && rows.back().kind == KeepRow::Kind::Deleted){
        rows.pop_back();
    }
}

void editor::KeepContext::addRemovedComponent(Entity entity, ComponentType type){
    KeepRow row;
    row.kind = KeepRow::Kind::RemoveComponent;
    row.entity = entity;
    row.hasComponent = true;
    row.component = type;
    row.playValue = "Removed";
    Project* target = project;
    const uint32_t id = sceneId;
    row.factory = [target, id, entity, type]() -> std::unique_ptr<Command> {
        return std::make_unique<RemoveComponentCmd>(target, id, entity, type);
    };
    row.readEditValue = []() { return std::string("Present"); };
    add(std::move(row));
}

// ------------------------------------------------------------------ PlayChangeReview

editor::PlayChangeReview& editor::PlayChangeReview::get(){
    static PlayChangeReview review;
    return review;
}

void editor::PlayChangeReview::markComponent(uint32_t sceneId, Entity entity, ComponentType type){
    if (!isMarked(sceneId, entity, type)){
        marks[sceneId].push_back({entity, type});
    }
}

void editor::PlayChangeReview::markGameObject(uint32_t sceneId, Entity entity){
    marks[sceneId].push_back({entity, std::nullopt});
}

bool editor::PlayChangeReview::isMarked(uint32_t sceneId, Entity entity, ComponentType type) const{
    auto it = marks.find(sceneId);
    if (it == marks.end()){
        return false;
    }
    return std::any_of(it->second.begin(), it->second.end(), [&](const Mark& mark){
        return mark.entity == entity && (!mark.component || *mark.component == type);
    });
}

void editor::PlayChangeReview::capture(Project* project, SceneProject* sceneProject){
    const uint32_t sceneId = sceneProject->id;
    reviews.erase(sceneId);
    std::vector<Mark> sceneMarks;
    if (auto it = marks.find(sceneId); it != marks.end()){
        sceneMarks = std::move(it->second);
        marks.erase(it);
    }
    if (!enabled){
        return;
    }
    CommandHistory* history = CommandHandle::find(sceneId);
    if (!history || !history->isInPlaySession()){
        return;
    }

    KeepContext context;
    context.project = project;
    context.sceneId = sceneId;
    context.sceneProject = sceneProject;

    // the applied play-time steps, oldest first; selection steps are not edits
    const std::vector<const UndoEntry*> entries = history->entries();
    // (by position: a step the editor inserted below the floor is edit-time, newer id or not)
    const size_t floor = history->getPlayFloor() - history->getTrimmedCount();
    for (size_t i = floor; i < history->appliedCount(); i++){
        const UndoEntry* entry = entries[i];
        if (entry->info.selectionOnly){
            continue;
        }
        context.collect(*entry->cmd, entry->name());
    }

    // Keep Values After Play: what scripts changed, as whole-component rows
    Scene* scene = sceneProject->scene;
    context.source = "Keep Values After Play";
    for (const Mark& mark : sceneMarks){
        if (!scene->isEntityCreated(mark.entity)){
            continue;
        }
        const std::vector<ComponentType> types = mark.component ? std::vector<ComponentType>{*mark.component}
                                                               : Catalog::findComponents(scene, mark.entity);
        for (ComponentType type : types){
            context.addComponentValues(mark.entity, type);
        }
    }

    if (!context.rows.empty()){
        Pending pendingReview;
        pendingReview.sceneId = sceneId;
        pendingReview.sceneName = sceneProject->name;
        pendingReview.rows = std::move(context.rows);
        reviews[sceneId] = std::move(pendingReview);
    }
}

void editor::PlayChangeReview::afterRestore(Project* project, SceneProject* sceneProject){
    auto it = reviews.find(sceneProject->id);
    if (it == reviews.end()){
        return;
    }
    Scene* scene = sceneProject->scene;
    std::vector<KeepRow>& rows = it->second.rows;
    auto exists = [&](Entity entity){
        return entity != NULL_ENTITY && scene->isEntityCreated(entity) &&
            std::find(sceneProject->entities.begin(), sceneProject->entities.end(), entity) != sceneProject->entities.end();
    };
    auto createdRow = [&rows](Entity entity){
        return std::any_of(rows.begin(), rows.end(), [entity](const KeepRow& row){
            return row.kind == KeepRow::Kind::Created && row.entity == entity && row.keepable;
        });
    };
    for (KeepRow& row : rows){
        if (!row.keepable){
            continue;
        }
        switch (row.kind){
            case KeepRow::Kind::Created:
                if (row.parent != NULL_ENTITY && !exists(row.parent) && !createdRow(row.parent)){
                    row.keepable = false;
                    row.reason = "Its parent is gone after Play";
                }
                row.editValue = "(none)";
                break;
            case KeepRow::Kind::Unsupported:
                break;
            default:
                if (!exists(row.entity) && !(row.madeInPlay && createdRow(row.entity))){
                    row.keepable = false;
                    row.reason = "The GameObject is gone after Play";
                }
                break;
        }
        if (row.keepable && row.readEditValue && row.kind != KeepRow::Kind::Created){
            row.editValue = row.readEditValue();
        }
    }
    // a whole-component row whose values are all back where they were changes nothing
    rows.erase(std::remove_if(rows.begin(), rows.end(), [](const KeepRow& row){
        return row.kind == KeepRow::Kind::Component && row.keepable && row.editValue.empty();
    }), rows.end());
    for (KeepRow& row : rows){
        if (row.kind == KeepRow::Kind::Component){
            row.property = row.editValue;
            row.editValue = "Values before Play";
        }
    }
    if (rows.empty()){
        reviews.erase(it);
        return;
    }
    if (onPending){
        onPending(sceneProject->id, rows.size());
    }
    quietStop = false;
}

const editor::PlayChangeReview::Pending* editor::PlayChangeReview::pending(uint32_t sceneId) const{
    auto it = reviews.find(sceneId);
    return it != reviews.end() ? &it->second : nullptr;
}

std::vector<uint32_t> editor::PlayChangeReview::pendingScenes() const{
    std::vector<uint32_t> scenes;
    for (const auto& [sceneId, review] : reviews){
        scenes.push_back(sceneId);
    }
    return scenes;
}

size_t editor::PlayChangeReview::pendingCount(uint32_t sceneId) const{
    const Pending* review = pending(sceneId);
    return review ? review->rows.size() : 0;
}

editor::PlayChangeReview::ApplyResult editor::PlayChangeReview::apply(Project* project, uint32_t sceneId, const std::vector<uint64_t>* ids){
    ApplyResult result;
    auto it = reviews.find(sceneId);
    if (it == reviews.end() || !project->getScene(sceneId)){
        return result;
    }
    // taken out first: the commands below run with no review pending
    Pending review = std::move(it->second);
    reviews.erase(it);

    std::vector<std::unique_ptr<Command>> commands;
    std::vector<uint64_t> rowIds;
    for (const KeepRow& row : review.rows){
        const bool chosen = !ids || std::find(ids->begin(), ids->end(), row.id) != ids->end();
        if (!chosen){
            continue;
        }
        if (!row.keepable || !row.factory){
            result.skipped.emplace_back(row.id, row.reason.empty() ? std::string("It can't be kept") : row.reason);
            continue;
        }
        commands.push_back(row.factory());
        rowIds.push_back(row.id);
    }
    if (commands.empty()){
        return result;
    }

    const size_t chosenCount = commands.size();
    auto* group = new CommandGroup(std::move(commands), "Apply Play Mode Changes (" + std::to_string(chosenCount) + ")", false);
    CommandHistory* history = CommandHandle::get(sceneId);
    UndoOrigin origin;
    origin.kind = UndoOriginKind::PlayReview;
    // the group reports what it dropped before the history may free it
    std::vector<size_t> failed;
    group->setOnFailures([&failed](const std::vector<size_t>& indices){ failed = indices; });
    const uint64_t firstNewId = CommandHistory::peekNextId();
    bool pushed;
    {
        UndoOriginScope scope(origin);
        pushed = history->addCommandNoMerge(group);
    }
    for (size_t i = 0; i < rowIds.size(); i++){
        if (std::find(failed.begin(), failed.end(), i) != failed.end() || !pushed){
            result.skipped.emplace_back(rowIds[i], "Its command did not apply to the restored scene");
        }else{
            result.kept.push_back(rowIds[i]);
        }
    }
    if (!pushed || result.kept.empty()){
        return result;
    }
    result.applied = true;
    const UndoEntry* top = history->top();
    if (!top || top->info.id < firstNewId){
        return result;      // the scene already had those values: no step
    }
    if (result.kept.size() != chosenCount){
        history->renameTop("Apply Play Mode Changes (" + std::to_string(result.kept.size()) + ")");
    }
    result.entryId = top->info.id;
    result.entryName = top->name();
    return result;
}

void editor::PlayChangeReview::discard(uint32_t sceneId){
    reviews.erase(sceneId);
    marks.erase(sceneId);
}

void editor::PlayChangeReview::discardAll(){
    reviews.clear();
    marks.clear();
    quietStop = false;
}
