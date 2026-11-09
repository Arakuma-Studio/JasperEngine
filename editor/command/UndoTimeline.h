// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CommandHistory.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor{

    // The one Ctrl+Z timeline (S24 R17-R21). For the selected scene x it is the union of the
    // scene's history S_x (CommandHandle) and the project history P (asset files), ordered by
    // entry id: undo takes the newer of their two top steps, redo the older of their next
    // ones, with the steps linked to it in any history. A view with no stack of its own, so
    // the per-scene histories, the play floor and mark/groupSince work unchanged.
    namespace UndoTimeline{

        struct EntryView{
            const UndoEntry* entry = nullptr;
            CommandHistory* history = nullptr;
            uint32_t sceneId = 0;       // the scene of a scene history (0 for the project's)
            bool project = false;       // a step of P
            bool applied = false;
            // false: an edit-time step while the scene plays (neither undone nor redone then)
            bool editable = true;

            explicit operator bool() const { return entry != nullptr; }
        };

        // A step undo/redo/jumpTo took
        struct Step{
            uint64_t id = 0;
            std::string name;
            bool project = false;
            uint32_t sceneId = 0;
        };

        // Where Ctrl+Z / Ctrl+Y and Edit > Undo/Redo go (R21)
        enum class Target{
            TextField,      // ImGui's own text undo
            CodeEditor,
            Timeline
        };
        enum class FocusedWindow{
            None,
            Scene,
            Game,
            Hierarchy,
            Inspector,
            Resources,
            Console,
            Code,
            Other
        };
        // Only a text field being edited and the focused Code editor keep their own undo:
        // which other window had focus never picks the history
        Target resolveTarget(bool textInput, bool codeEditorFocused, FocusedWindow lastFocusedWindow);

        // --- Set up by Project (tests set their own) ---
        void setProjectHistory(CommandHistory* history);
        // The history of the Inspector's .material rows (CommandHandle::Redirect), whose scene is
        // not the selected one: a drag there is the timeline's too, as Ctrl+Z in it cancels it
        void setAssetEditHistory(CommandHistory* history);
        CommandHistory* getProjectHistory();
        // The selected scene: the redo tail an asset edit frees with P's (R20)
        extern std::function<uint32_t()> selectedScene;
        // A history's name for messages: "Scene 'Level2'", "Project"
        extern std::function<std::string(uint32_t sceneId)> historyName;
        // Warnings (a linked step partly undone): the Console unless set
        extern std::function<void(const std::string&)> warn;
        // Any history changed (a step pushed, undone, redone, trimmed, renamed): the Edit menu
        // labels and the Undo History panel refresh (an S09 subscription when that lands)
        extern std::function<void()> onChanged;
        // An undo or redo touched P: the Project window refreshes its listing
        extern std::function<void()> onProjectChanged;

        // The scene's history when it exists (no new one is made)
        CommandHistory* sceneHistory(uint32_t sceneId);

        // The step undo/redo would take next in T(sceneId)
        EntryView nextUndo(uint32_t sceneId);
        EntryView nextRedo(uint32_t sceneId);
        bool canUndo(uint32_t sceneId);
        bool canRedo(uint32_t sceneId);

        // One timeline step (with its linked steps). False with `why` when there is none or a
        // history refused. `steps`: what was undone/redone, in order.
        bool undo(uint32_t sceneId, std::string* why = nullptr, std::vector<Step>* steps = nullptr);
        bool redo(uint32_t sceneId, std::string* why = nullptr, std::vector<Step>* steps = nullptr);
        // Steps until `entryId` is the newest applied step of T(sceneId) (undo) or has been
        // applied (redo); 0 undoes everything. Stops at the first refused step, with why.
        bool jumpTo(uint32_t sceneId, uint64_t entryId, std::string* why = nullptr, std::vector<Step>* steps = nullptr);

        // T(sceneId) oldest first: applied steps, then the redo tails (still by id)
        std::vector<EntryView> entries(uint32_t sceneId, bool includeSelection);
        EntryView find(uint32_t sceneId, uint64_t entryId);

        // "Undo Move Player" / "Undo" (and Redo)
        std::string undoLabel(uint32_t sceneId);
        std::string redoLabel(uint32_t sceneId);

        // Frees the redo tails a new step in `history` ends (R20): P's for a scene step; the
        // selected scene's for a step of P. Other scenes are never touched.
        void freePartnerRedo(CommandHistory& history);

        // Links the newest applied step of each history to one id (a batch, an agent call)
        uint64_t link(const std::vector<CommandHistory*>& histories);

    }

}
