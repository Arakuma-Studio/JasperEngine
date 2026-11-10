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
        // Applied when its document was last saved (markSaved): its undo leaves the document
        // unlike its file, whatever modified flag the step puts back. A merged or grouped step's
        // when any part's is
        bool savedApplied = false;
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
        void notifyModified(bool structural, bool selectionOnly = false) const;

        static void releaseCommand(std::unique_ptr<Command> cmd, size_t bytes);
        static void releaseNow(std::unique_ptr<Command> cmd, size_t bytes);
        static void releaseEntries(std::vector<UndoEntry>& entries);

        // the last undo()/redo() a step refused: why ("" when the last call did not refuse, or
        // the step gave no reason), and which step (kept after the next call, for the Edit menu)
        std::string refusal;
        std::string undoProblemText;
        std::string refusedText;
        unsigned long long refusedSerial = 0;
        bool refusedUndo = false;
        // a step's serial when it enters the list (Command::serial)
        static void stamp(Command* cmd);

    public:
        // Called after any command apply/undo/redo with the scene id and affectsStructure().
        static std::function<void(size_t, bool)> onSceneModified;
        // Called after an undo of a step that was applied when the history's document was last
        // saved (markSaved), with the scene id: the scene now differs from its file, whatever
        // modified flag the step put back
        static std::function<void(size_t)> onUndoPastSave;

        // The clock of the document writes, over every document: one compares the stamp of its
        // last write with the one it had at a capture (PrefabUndoState). Any thread; never the
        // same twice.
        static uint64_t stampSave();
        // The history's document was written as it is now (main thread): the applied steps are
        // in the file, the redo tail is not, and the newest applied step stops merging with the
        // next one
        void markSaved();
        // Called before an undo() or redo() step, with the history's scene id (the Pose Animation
        // window puts a posed character's rest bones back: the step runs on the rest, which the
        // pose saves again afterwards)
        static std::function<void(size_t)> beforeStep;

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

        // The editor sets this: commands the history drops (a no-op, a cancelled drag) are freed
        // at flushReleased(), once no widget is active, since the widget that pushed one may
        // still hold its pointer (to end its merge chain). Unset, they are freed at once.
        static bool deferReleases;
        static void flushReleased();

        // Runs before a command executes in a history (the editor opens the active widget's
        // InteractiveEdit there: the first value a slider pushes is part of its drag)
        static std::function<void(CommandHistory& history)> beforeRecord;

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

        // Asked before the scene's history changes, so a tool previewing edits live (the
        // Physics Placement tool's running gesture) can settle them first: the command then
        // records the true values. Returning false drops an undo or a redo; a command is always
        // added (unless beforeMutation refuses it). Re-entrant: the tool's own commit comes
        // through here too.
        enum class Change{
            Add,
            Undo,
            Redo
        };
        static std::function<bool(size_t, Change)> beforeChange;

        explicit CommandHistory(size_t sceneId = 0);
        ~CommandHistory();

        CommandHistory(const CommandHistory&) = delete;
        CommandHistory& operator=(const CommandHistory&) = delete;

        size_t getSceneId() const { return sceneId; }

        // Executes the command and records it (merged into the step before when both merge).
        // Takes ownership. False when it did not run (refused or failed; the command is freed).
        // A command that changed nothing (isNoOp) is undone and freed: true, but no step.
        bool addCommand(Command* cmd);
        bool addCommandNoMerge(Command* cmd);
        // A step the editor takes by itself (a model reloaded because its file changed on disk):
        // undone like any other, but placed at the current position without ending the redo tail
        // that a user edit would drop. In Play it goes below the play floor, so Stop keeps it.
        // Its id is the newest, so a history's ids increase along it except around such steps.
        bool insertCommand(Command* cmd);

        // Undo/redo one step. Undo with an interactive edit open cancels it instead. False when
        // the history did not move, also when the command refused (Command::canUndo / canRedo:
        // the position stays, lastRefusal() says why, the console too) or a redo's execute
        // failed (Command::failure). Never moves on a refusal, so a loop
        // "while (position() > mark && undo())" ends at a refused step. Each step runs in the
        // write scope it was made in (Command::writeRoot).
        bool undo();
        bool redo();
        // Undo until `id` is the newest applied step (0: everything undoable), or redo until
        // `id` has been applied. False when the id is not here or a step was refused.
        bool undoTo(uint64_t id);
        bool redoTo(uint64_t id);
        // Why the last undo() / redo() was refused by its step (Command::undoRefusal / redoRefusal);
        // "" when that call was not refused or the step gave no reason (a child scene cycle)
        const std::string& lastRefusal() const { return refusal; }
        // The last undo() / redo() was taken but a file it had to put back or write could not be
        // (Command::takeUndoProblem, or a write the write scope refused in it; also said in the
        // console): why ("" when it went through)
        const std::string& lastUndoProblem() const { return undoProblemText; }
        // The step undo() / redo() would take now refused the last time it was tried: why (a
        // generic text when it gave none), "" else
        std::string undoRefusedLastTry() const;
        std::string redoRefusedLastTry() const;

        bool canUndo() const;
        bool canRedo() const;

        // Frees every step
        void clear();
        // The steps `matches` names leave the history, wherever they are in it (their content is
        // gone: a file reloaded under them, T6 R77). The others keep their order and stay
        // undoable; they must not depend on the steps dropped. Returns how many went.
        size_t dropSteps(const std::function<bool(Command&)>& matches);
        // Frees the redo tail (UndoTimeline: a step of the partner history ends it)
        void freeRedo();
        // Why the last push/undo/redo was refused (beforeMutation)
        static const std::string& lastBusyRefusal();

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
        // True for a step of the running play session (above the play floor: an inserted
        // edit-time step below it has a newer id than the session's first)
        bool isPlayStep(uint64_t id) const;
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
        // The step undo() would revert / redo() would apply (nullptr when none): who made the
        // newest step (the Sprite Normal Map window's undo routing compares its own)
        const Command* undoCommand() const { return index > 0 && index <= list.size() ? list[index - 1].cmd.get() : nullptr; }
        const Command* redoCommand() const { return index < list.size() ? list[index].cmd.get() : nullptr; }
        // ... their serials (Command::serial; 0 when none): what to keep and compare, never the pointers
        unsigned long long undoSerial() const { const Command* c = undoCommand(); return c ? c->serial() : 0; }
        unsigned long long redoSerial() const { const Command* c = redoCommand(); return c ? c->serial() : 0; }
        // upstream's name for freeRedo(): a call that rolled itself back with undo() leaves
        // nothing to redo
        void dropRedo() { freeRedo(); }

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
        // Grows with every change of any history (a view caches what it read until it moves)
        static uint64_t changeCount();
    };

}
