// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Command.h"
#include "UndoOrigin.h"
#include "ecs/Entity.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor{

    class CommandHistory;

    // A scene's selection as undo stores it: the GameObjects in order and the active one
    struct SelectionState{
        std::vector<Entity> entities;
        Entity active = NULL_ENTITY;

        bool operator==(const SelectionState& other) const{
            return active == other.active && entities == other.entities;
        }
        bool operator!=(const SelectionState& other) const{ return !(*this == other); }
    };

    // What the history knows about one undo step
    struct EntryInfo{
        // Global and strictly increasing over every history: the timeline's order. A merge
        // keeps the older id.
        uint64_t id = 0;
        // When the step was made, or last merged into
        std::chrono::system_clock::time_point time;
        UndoOrigin origin;
        // Steps of other histories made by the same action (a batch, an agent call): undone
        // and redone together by UndoTimeline. 0: none.
        uint64_t linkId = 0;
        // Command::memoryBytes when pushed or last merged
        size_t bytes = 0;
        // A scene history's step: the selection before and after it ran (R14)
        bool hasSelection = false;
        SelectionState selectionBefore;
        SelectionState selectionAfter;
        // A selection change alone (SelectionCmd)
        bool selectionOnly = false;
    };

    // One undo step of a history
    struct UndoEntry{
        std::unique_ptr<Command> cmd;
        EntryInfo info;

        const std::string& name() const { return cmd->name(); }
    };

    // A drag (gizmo, inspector slider, terrain stroke) as one undo step: the commands pushed
    // while it is open become one entry at commit(); cancel() undoes them and the history is
    // as before beginInteractive (values, overrides, isModified, selection, redo tail).
    // Movable, not copyable. Destroyed while open, it commits: a forgotten close never loses
    // the user's work.
    class InteractiveEdit{
    private:
        friend class CommandHistory;
        CommandHistory* history = nullptr;
        uint64_t serial = 0;

        InteractiveEdit(CommandHistory* history, uint64_t serial): history(history), serial(serial) {}
        CommandHistory* live() const;

    public:
        InteractiveEdit() = default;
        InteractiveEdit(InteractiveEdit&& other) noexcept;
        InteractiveEdit& operator=(InteractiveEdit&& other) noexcept;
        InteractiveEdit(const InteractiveEdit&) = delete;
        InteractiveEdit& operator=(const InteractiveEdit&) = delete;
        ~InteractiveEdit();

        void commit();
        void cancel();
        bool isOpen() const;
    };

    // Keeps trimming away from a history's steps from now until it ends (MCP batch undo
    // groups: their mark must stay groupable)
    class UndoGroupScope{
    private:
        CommandHistory* history = nullptr;
        uint64_t serial = 0;

    public:
        explicit UndoGroupScope(CommandHistory* history);
        ~UndoGroupScope();
        UndoGroupScope(const UndoGroupScope&) = delete;
        UndoGroupScope& operator=(const UndoGroupScope&) = delete;
    };

    class CommandHistory{
        friend class InteractiveEdit;
        friend class UndoGroupScope;

    public:
        using Entry = UndoEntry;

    private:
        std::vector<UndoEntry> list;
        size_t index = 0;           // entries below are applied, from it on the redo tail
        size_t trimmedCount = 0;    // entries trimmed off the front: positions are absolute
        size_t sceneId = 0;
        size_t heldBytes = 0;       // EntryInfo::bytes of everything held (list + set aside)

        // Play mode: the scene is restored from a snapshot on Stop, so commands made while
        // playing can't outlive the session, and edit-time commands can't be undone during it
        bool inPlaySession = false;
        size_t playFloor = 0;               // absolute position of the play session's first step
        uint64_t playStartId = 0;           // the first entry id the session could make
        std::vector<UndoEntry> editRedo;        // edit-time redo tail, set aside while playing

        struct Interactive{
            bool open = false;
            uint64_t serial = 0;
            std::string name;
            size_t start = 0;               // absolute position at begin
            std::vector<UndoEntry> redoAside;   // the redo tail, back on cancel or a no-op commit
        };
        Interactive interactive;

        struct TrimHold{
            uint64_t serial = 0;
            size_t position = 0;            // absolute
        };
        std::vector<TrimHold> trimHolds;

        size_t undoFloor() const { return inPlaySession ? playFloor - trimmedCount : 0; }
        bool beforeMutating();
        void recordEntry(std::unique_ptr<Command> cmd, const SelectionState& before, bool hasSelection);
        void captureName(UndoEntry& entry) const;
        void cutRedoTail();
        void trim(uint64_t keepId);
        bool trimmable(size_t localIndex, uint64_t keepId) const;
        void trimFront();
        void recount();
        void restoreSelection(const UndoEntry& entry, bool after) const;
        void commitInteractive(uint64_t serial, bool warnForgotten = false);
        void cancelInteractive(uint64_t serial);
        void fold(size_t localStart, const std::string& label, bool keepSingle);
        void notifyModified(bool structural) const;

        static void releaseCommand(std::unique_ptr<Command> cmd, size_t bytes);
        static void releaseEntries(std::vector<UndoEntry>& entries);

    public:
        // Called after any command apply/undo/redo with the scene id and affectsStructure().
        static std::function<void(size_t, bool)> onSceneModified;

        // Runs before every push, undo, redo, jump and trim: false refuses it, with the reason
        // ("Busy: <reason>" for the user). Open interactive edits of other histories are
        // committed first, whatever the hook says.
        static std::function<bool(std::string& whyNot)> beforeMutation;

        // A scene history's selection (Project sets these): read() false for a history without
        // one (the project history); restore() puts a stored selection back, leaving out the
        // GameObjects that no longer exist
        struct SelectionHooks{
            std::function<bool(size_t sceneId, SelectionState& state)> read;
            std::function<void(size_t sceneId, const SelectionState& state)> restore;
        };
        static SelectionHooks selectionHooks;

        // Frees a large off-thread command (BackgroundRelease when unset; tests inject a sink)
        static std::function<void(std::unique_ptr<Command>)> offThreadRelease;
        // Commands at least this big with releaseOffThread() are freed off the main thread
        static constexpr size_t OFF_THREAD_RELEASE_BYTES = 256 * 1024;

        // Caps (Editor Settings > Undo): steps per history, and bytes over all histories
        // (0: unlimited). The oldest steps go first; see trim().
        static size_t maxEntries;
        static size_t memoryLimit;
        static constexpr size_t DEFAULT_MAX_ENTRIES = 1000;
        static constexpr size_t DEFAULT_MEMORY_LIMIT = size_t(512) * 1024 * 1024;

        explicit CommandHistory(size_t sceneId = 0);
        ~CommandHistory();

        CommandHistory(const CommandHistory&) = delete;
        CommandHistory& operator=(const CommandHistory&) = delete;

        size_t getSceneId() const { return sceneId; }

        // Executes the command and records it (merged into the step before when both merge).
        // Takes ownership. False when it did not run (refused or failed; the command is freed).
        // A command that changed nothing (isNoOp) is undone and freed: true, but no step.
        bool addCommand(Command* cmd);
        void addCommandNoMerge(Command* cmd);

        // Undo/redo one step. Undo with an interactive edit open cancels it instead.
        bool undo();
        bool redo();
        // Undo until `id` is the newest applied step (0: everything undoable), or redo until
        // `id` has been applied. False when the id is not here or a step was refused.
        bool undoTo(uint64_t id);
        bool redoTo(uint64_t id);

        bool canUndo() const;
        bool canRedo() const;

        // Frees every step
        void clear();
        // Frees the redo tail (UndoTimeline: a step of the partner history ends it)
        void freeRedo();
        // Why the last push/undo/redo was refused (beforeMutation)
        static const std::string& lastRefusal();

        // The newest applied step undo would take (null when none: empty, or at the play floor)
        const UndoEntry* top() const;
        // The step redo would apply (null when none)
        const UndoEntry* nextRedo() const;
        // Every step held, oldest first: applied, then the redo tail
        std::vector<const UndoEntry*> entries() const;
        const UndoEntry* find(uint64_t id) const;
        bool isApplied(uint64_t id) const;
        size_t appliedCount() const { return index; }
        size_t bytes() const { return heldBytes; }
        // The first id of the play session (0 outside Play): older steps are edit-time
        uint64_t getPlayStartId() const { return inPlaySession ? playStartId : 0; }
        bool isInPlaySession() const { return inPlaySession; }
        size_t getPlayFloor() const { return playFloor; }

        // Links the newest applied step to others (UndoTimeline undoes them together)
        void setTopLink(uint64_t linkId);
        // Drops a link from every step holding it
        void unlink(uint64_t linkId);
        // A new link id (global)
        static uint64_t newLinkId();

        // Undo groups (MCP batch): mark() before a run of commands, groupSince(mark) after
        // collapses them into one undo step. mark() also stops the next command merging
        // into the one before it. groupSince is a no-op (false) if the history was undone
        // past the mark, the mark was trimmed, or a play session began or ended in between.
        // `label` names the group (empty: its first step's name, "(+N more)").
        size_t mark();
        bool groupSince(size_t mark, const std::string& label = {});
        // Current position (what mark() returns, without its side effect); absolute, so a
        // position survives trimming
        size_t position() const { return trimmedCount + index; }
        size_t getTrimmedCount() const { return trimmedCount; }
        // Renames the newest applied step (folding an agent call keeps its own name otherwise)
        void renameTop(const std::string& name);

        // Interactive edit (Esc cancels a drag): see InteractiveEdit. A second begin in the
        // same history commits the first.
        InteractiveEdit beginInteractive(std::string name);
        bool hasOpenInteractive() const { return interactive.open; }
        // Commits/cancels the open interactive edit, if any
        void commitInteractive();
        bool cancelInteractive();

        void beginPlaySession();
        // rolledBack: the scene was restored to its pre-play state, so the play-time commands
        // are dropped and the edit redo tail comes back; otherwise the play-time commands stay
        void endPlaySession(bool rolledBack);

        // --- Every live history (the global caps, the timeline, the interactive edits) ---
        static const std::vector<CommandHistory*>& all();
        // Bytes held over all histories
        static size_t totalBytes();
        // Commits the open interactive edit of every history but `except`
        static void commitInteractiveEdits(const CommandHistory* except = nullptr);
        static bool anyInteractiveOpen();
        // True while a command executes, undoes or redoes through a history (a selection
        // change then belongs to that step)
        static bool isApplying();
        // The id the next new step will get
        static uint64_t peekNextId();
    };

}
