// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "McpJson.h"
#include "command/UndoTimeline.h"

#include <cstdint>
#include <string>

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

        // One step for get_undo_history and the undo/redo results
        Json entryJson(const UndoTimeline::EntryView& view);
        Json stepJson(const UndoTimeline::Step& step);

    }

}
