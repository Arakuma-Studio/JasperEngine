// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/dialogue-graphs.md R1, R3, R5, R11, R12, R21, R22, R26, D6)
// - Purpose: what the editor adds to the dialogue runtime, whatever edits a .dialogue (the Dialogue
//   window, edit_graph, the Project window's Create): the kind's edit hooks (a source text edit turns
//   the translations of that key needs_review in the same undo step; a new file gets its namespace
//   from its stem, the project's default source language and a GUID), the lint's project context
//   (Project Settings > Dialogue and the project's dialogues' entries and jumps, read again when a
//   file changes), and Play's start (the variable store emptied, the project's defaults set).
// - The editor's own writes of a dialogue reach running conversations (Programs::fileChanged) and
//   the editor's Localization (the dialogue's texts are its table).
// - Thread-safety: main thread.

#pragma once

#include "dialogue/DialogueKind.h"
#include "dialogue/DialogueValue.h"
#include "graph/Graph.h"

#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::editor {

    class Command;
    class Project;
    struct DialogueSettings;

    namespace dialogues {

        // the hooks, the lint context and the change listener, for this project (App, the editor tests)
        void install(Project* project);
        void uninstall();

        // Project Settings > Dialogue > Variables as values (an unparsable number is 0)
        std::vector<std::pair<std::string, jasperengine::dialogue::Value>> projectDefaults(const DialogueSettings& settings);
        // je-dialogue.json for an export; "" when the project declares no variable
        std::string settingsFileText(const DialogueSettings& settings);
        // Play starts: an empty store with the project's defaults
        void preparePlay(Project* project);

        // the step that turns the translations of the source texts `before` -> `after` changed
        // needs_review; null when none changes
        std::unique_ptr<Command> reviewStep(Project* project, const std::filesystem::path& file, const std::string& before, const std::string& after);
        // a new dialogue at `file`: its namespace (from the stem, made unique), source language and
        // GUID; `starter` adds an entry `start` wired to an empty line (Create > Dialogue)
        void prepareNew(Project* project, const std::filesystem::path& file, graph::Graph& graph, bool starter);
        // the lint's context now (Project Settings > Dialogue, the project's dialogues)
        std::shared_ptr<const jasperengine::dialogue::LintContext> lintContext(Project* project);
        // the entries of a project dialogue (Start Entry's choices); empty when it does not load
        std::vector<std::string> entriesOf(Project* project, const std::string& path);
    }

}
