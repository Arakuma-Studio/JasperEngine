// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "McpJson.h"
#include "command/UndoTimeline.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class Project;
    class UndoGroupScope;
}

namespace jasperengine::editor::mcp {

    class McpCall;

    // Undo attribution of agent edits (S24 R24, R36-R38)
    namespace McpUndo {

        // The client name shown in undo steps: printable, at most 32 characters, "Agent" when empty
        std::string clientName(const std::string& raw);

        // When an attributed call replies: in each history, the call's steps on top become one
        // ("<client>: <title> (<subject>)"), linked across histories, unless a step of someone
        // else lies between them. Returns {id, name} of the call's newest step, null when it made
        // none. Main thread only.
        Json foldCall(const McpCall& call, const std::string& toolTitle);

        // Several calls as one undo step (batch undo_group, an editor script's run, S24 R25):
        // beginGroup marks every history (and keeps the marks from trimming), endGroup makes
        // what each changed since one step named `label`, linked so one Ctrl+Z undoes it all.
        // Returns {scene_ids, complete, link_id?, undo: {id, name}?}. Main thread only.
        struct Group {
            std::map<uint32_t, size_t> sceneMarks;
            size_t projectMark = 0;
            std::vector<std::unique_ptr<UndoGroupScope>> trimHolds;
            Group();
            ~Group();
            Group(Group&&) noexcept;
            Group& operator=(Group&&) noexcept;
        };
        void beginGroup(Group& group, Project* project);
        Json endGroup(Group& group, Project* project, const std::string& label);

        // One step for get_undo_history and the undo/redo results
        Json entryJson(const UndoTimeline::EntryView& view);
        Json stepJson(const UndoTimeline::Step& step);

    }

}
