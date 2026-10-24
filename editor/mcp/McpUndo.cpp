// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "McpUndo.h"

#include "McpCall.h"
#include "command/CommandGroup.h"
#include "command/CommandNames.h"
#include "command/EntityEdit.h"

#include <algorithm>
#include <chrono>

namespace jasperengine::editor::mcp {

namespace {

    // The GameObjects a step edits, from the commands that say (EntityEdit), in first-edit order
    void collectEdited(const Command* cmd, Project*& project, uint32_t& sceneId, std::vector<Entity>& entities) {
        if (const CommandGroup* group = dynamic_cast<const CommandGroup*>(cmd)) {
            for (const std::unique_ptr<Command>& child : group->getCommands()) {
                collectEdited(child.get(), project, sceneId, entities);
            }
            return;
        }
        const EntityEdit* edit = dynamic_cast<const EntityEdit*>(cmd);
        if (!edit) {
            return;
        }
        if (!project) {
            project = edit->editedProject();
            sceneId = edit->editedSceneId();
        }
        for (Entity entity : edit->editedEntities()) {
            if (std::find(entities.begin(), entities.end(), entity) == entities.end()) {
                entities.push_back(entity);
            }
        }
    }

}

std::string McpUndo::clientName(const std::string& raw) {
    std::string name;
    for (char c : raw) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F) continue;
        name += c;
    }
    // trim
    const size_t first = name.find_first_not_of(' ');
    const size_t last = name.find_last_not_of(' ');
    name = first == std::string::npos ? std::string() : name.substr(first, last - first + 1);
    if (name.size() > 32) {
        // 32 bytes, never cutting a UTF-8 sequence: back to the start of the last one, kept
        // when it is whole
        size_t start = 32;
        while (start > 0 && (static_cast<unsigned char>(name[start]) & 0xC0) == 0x80) start--;
        name = name.substr(0, start);
    }
    return name.empty() ? std::string("Agent") : name;
}

Json McpUndo::foldCall(const McpCall& call, const std::string& toolTitle) {
    const uint64_t serial = call.serial();
    std::vector<CommandHistory*> touched;
    for (CommandHistory* history : CommandHistory::all()) {
        const std::vector<const UndoEntry*> entries = history->entries();
        const size_t applied = history->appliedCount();
        size_t run = 0;
        while (run < applied && entries[applied - 1 - run]->info.origin.call == serial) {
            run++;
        }
        if (run == 0) {
            continue;
        }
        // a step of someone else between the call's steps: each keeps its own name
        size_t total = 0;
        for (const UndoEntry* entry : entries) {
            if (entry->info.origin.call == serial) total++;
        }
        if (total != run) {
            continue;
        }
        if (run > 1) {
            Project* project = nullptr;
            uint32_t sceneId = 0;
            std::vector<Entity> edited;
            for (size_t i = applied - run; i < applied; i++) {
                collectEdited(entries[i]->cmd.get(), project, sceneId, edited);
            }
            std::string label = call.getUndoOrigin().client + ": " + toolTitle;
            if (!edited.empty()) {
                label += " (" + CommandNames::entities(project, sceneId, edited) + ")";
            }
            if (!history->groupSince(history->position() - run, label)) {
                continue;
            }
        }
        touched.push_back(history);
    }
    if (touched.empty()) {
        return nullptr;
    }
    if (touched.size() > 1) {
        UndoTimeline::link(touched);
    }
    const UndoEntry* newest = nullptr;
    for (CommandHistory* history : touched) {
        const UndoEntry* top = history->top();
        if (top && (!newest || top->info.id > newest->info.id)) newest = top;
    }
    if (!newest) {
        return nullptr;
    }
    return Json{{"id", newest->info.id}, {"name", newest->name()}};
}

Json McpUndo::entryJson(const UndoTimeline::EntryView& view) {
    const EntryInfo& info = view.entry->info;
    Json origin = {{"kind", undoOriginKindName(info.origin.kind)}};
    if (!info.origin.client.empty()) origin["client"] = info.origin.client;
    if (!info.origin.tool.empty()) origin["tool"] = info.origin.tool;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(info.time.time_since_epoch()).count();
    Json item = {
        {"id", info.id},
        {"name", view.entry->name()},
        {"type", view.entry->cmd->typeName()},
        {"history", view.project ? Json("project") : Json(view.sceneId)},
        {"origin", origin},
        {"time", ms},
        {"bytes", info.bytes},
        {"applied", view.applied}
    };
    if (info.linkId != 0) item["link_id"] = info.linkId;
    if (!view.editable) item["editable"] = false;
    return item;
}

Json McpUndo::stepJson(const UndoTimeline::Step& step) {
    return Json{{"id", step.id}, {"name", step.name}, {"history", step.project ? Json("project") : Json(step.sceneId)}};
}

}
