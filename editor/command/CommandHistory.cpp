// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CommandHistory.h"

#include "CommandGroup.h"
#include "CommandNames.h"
#include "UndoTimeline.h"
#include "Out.h"
#include "util/BackgroundRelease.h"
#include "util/SafeWrite.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <optional>

using namespace jasperengine;

namespace {

    std::atomic<uint64_t> nextEntryId{1};
    // CommandHistory::stampSave
    std::atomic<uint64_t> saveClock{0};
    std::atomic<uint64_t> nextLinkId{1};
    uint64_t nextSerial = 1;
    int applyingDepth = 0;
    bool budgetWarned = false;
    std::string refusedWhy;

    struct Released{
        std::unique_ptr<editor::Command> cmd;
        size_t bytes = 0;
    };
    std::vector<Released>& releasedCommands(){
        static auto* released = new std::vector<Released>();
        return *released;
    }

    std::vector<editor::CommandHistory*>& liveHistories(){
        // Never destroyed: histories may be freed during static destruction
        static auto* histories = new std::vector<editor::CommandHistory*>();
        return *histories;
    }

    // A command running through a history: selection changes belong to its step
    struct Applying{
        Applying(){ applyingDepth++; }
        ~Applying(){ applyingDepth--; }
    };

    // A step's execute, undo and redo run in the write scope it was made in (Command::writeRoots,
    // util/SafeWrite.h): an MCP call's step undone from the Edit menu stays beneath the project
    class StepScope {
    public:
        explicit StepScope(const editor::Command* cmd) {
            if (cmd && !cmd->writeRoots().empty()) scope.emplace(cmd->writeRoots());
        }

    private:
        std::optional<editor::safewrite::Scope> scope;
    };

    uint64_t changes = 0;

    void notifyChanged(){
        changes++;
        if (editor::UndoTimeline::onChanged){
            editor::UndoTimeline::onChanged();
        }
    }

}

std::function<void(size_t, bool)> editor::CommandHistory::onSceneModified = nullptr;
std::function<void(size_t)> editor::CommandHistory::onUndoPastSave = nullptr;
std::function<void(size_t)> editor::CommandHistory::beforeStep = nullptr;
std::function<bool(std::string&)> editor::CommandHistory::beforeMutation = nullptr;
std::function<bool(size_t, editor::CommandHistory::Change)> editor::CommandHistory::beforeChange = nullptr;
editor::CommandHistory::SelectionHooks editor::CommandHistory::selectionHooks;
std::function<void(std::unique_ptr<editor::Command>)> editor::CommandHistory::offThreadRelease = nullptr;
std::function<void(editor::CommandHistory&)> editor::CommandHistory::beforeRecord = nullptr;
bool editor::CommandHistory::deferReleases = false;
size_t editor::CommandHistory::maxEntries = editor::CommandHistory::DEFAULT_MAX_ENTRIES;
size_t editor::CommandHistory::memoryLimit = editor::CommandHistory::DEFAULT_MEMORY_LIMIT;

// ------------------------------------------------------------------ InteractiveEdit

editor::CommandHistory* editor::InteractiveEdit::live() const{
    if (!history || serial == 0){
        return nullptr;
    }
    const std::vector<CommandHistory*>& histories = liveHistories();
    if (std::find(histories.begin(), histories.end(), history) == histories.end()){
        return nullptr;
    }
    if (!history->interactive.open || history->interactive.serial != serial){
        return nullptr;
    }
    return history;
}

editor::InteractiveEdit::InteractiveEdit(InteractiveEdit&& other) noexcept: history(other.history), serial(other.serial){
    other.history = nullptr;
    other.serial = 0;
}

editor::InteractiveEdit& editor::InteractiveEdit::operator=(InteractiveEdit&& other) noexcept{
    if (this != &other){
        commit();
        history = other.history;
        serial = other.serial;
        other.history = nullptr;
        other.serial = 0;
    }
    return *this;
}

editor::InteractiveEdit::~InteractiveEdit(){
    if (CommandHistory* open = live()){
        open->commitInteractive(serial, true);
    }
}

void editor::InteractiveEdit::commit(){
    if (CommandHistory* open = live()){
        open->commitInteractive(serial);
    }
    history = nullptr;
    serial = 0;
}

void editor::InteractiveEdit::cancel(){
    if (CommandHistory* open = live()){
        open->cancelInteractive(serial);
    }
    history = nullptr;
    serial = 0;
}

bool editor::InteractiveEdit::isOpen() const{
    return live() != nullptr;
}

// ------------------------------------------------------------------ UndoGroupScope

editor::UndoGroupScope::UndoGroupScope(CommandHistory* history): history(history){
    if (history){
        serial = nextSerial++;
        history->trimHolds.push_back({serial, history->position()});
    }
}

editor::UndoGroupScope::~UndoGroupScope(){
    const std::vector<CommandHistory*>& histories = liveHistories();
    if (!history || std::find(histories.begin(), histories.end(), history) == histories.end()){
        return;
    }
    auto& holds = history->trimHolds;
    holds.erase(std::remove_if(holds.begin(), holds.end(), [this](const CommandHistory::TrimHold& hold){
        return hold.serial == serial;
    }), holds.end());
    if (holds.empty()){
        // what waited for the scope goes now
        history->trim(0);
    }
}

// ------------------------------------------------------------------ CommandHistory

editor::CommandHistory::CommandHistory(size_t sceneId): sceneId(sceneId){
    liveHistories().push_back(this);
}

editor::CommandHistory::~CommandHistory(){
    // the links other histories hold to these steps go (R23)
    std::vector<uint64_t> links;
    for (const UndoEntry& entry : list){
        if (entry.info.linkId != 0) links.push_back(entry.info.linkId);
    }
    clear();
    std::vector<CommandHistory*>& histories = liveHistories();
    histories.erase(std::remove(histories.begin(), histories.end(), this), histories.end());
    for (uint64_t link : links){
        for (CommandHistory* other : histories){
            other->unlink(link);
        }
    }
    notifyChanged();
}

const std::vector<editor::CommandHistory*>& editor::CommandHistory::all(){
    return liveHistories();
}

size_t editor::CommandHistory::totalBytes(){
    size_t bytes = 0;
    for (const CommandHistory* history : liveHistories()){
        bytes += history->heldBytes;
    }
    return bytes;
}

bool editor::CommandHistory::isApplying(){
    return applyingDepth > 0;
}

uint64_t editor::CommandHistory::stampSave(){
    return ++saveClock;
}

void editor::CommandHistory::markSaved(){
    for (size_t i = 0; i < list.size(); i++){
        list[i].info.savedApplied = i < index;
    }
    // what Play set aside is not applied
    for (UndoEntry& entry : editRedo){
        entry.info.savedApplied = false;
    }
    if (index > 0){
        list[index - 1].cmd->setNoMerge();
    }
}

uint64_t editor::CommandHistory::peekNextId(){
    return nextEntryId.load();
}

uint64_t editor::CommandHistory::changeCount(){
    return changes;
}

uint64_t editor::CommandHistory::newLinkId(){
    return nextLinkId++;
}

void editor::CommandHistory::commitInteractiveEdits(const CommandHistory* except){
    // copied: a commit may free a history's commands, never a history
    const std::vector<CommandHistory*> histories = liveHistories();
    for (CommandHistory* history : histories){
        if (history != except && history->interactive.open){
            history->commitInteractive();
        }
    }
}

bool editor::CommandHistory::anyInteractiveOpen(){
    for (const CommandHistory* history : liveHistories()){
        if (history->interactive.open) return true;
    }
    return false;
}

bool editor::CommandHistory::beforeMutating(){
    commitInteractiveEdits(this);
    refusedWhy.clear();
    if (!beforeMutation){
        return true;
    }
    std::string why;
    if (beforeMutation(why)){
        return true;
    }
    refusedWhy = why.empty() ? std::string("the editor is busy") : why;
    Out::warning("Busy: %s", refusedWhy.c_str());
    return false;
}

const std::string& editor::CommandHistory::lastBusyRefusal(){
    return refusedWhy;
}

void editor::CommandHistory::releaseCommand(std::unique_ptr<Command> cmd, size_t bytes){
    if (!cmd){
        return;
    }
    if (deferReleases){
        releasedCommands().push_back({std::move(cmd), bytes});
        return;
    }
    releaseNow(std::move(cmd), bytes);
}

void editor::CommandHistory::flushReleased(){
    std::vector<Released> released;
    released.swap(releasedCommands());
    for (Released& item : released){
        releaseNow(std::move(item.cmd), item.bytes);
    }
}

void editor::CommandHistory::releaseNow(std::unique_ptr<Command> cmd, size_t bytes){
    if (bytes >= OFF_THREAD_RELEASE_BYTES && cmd->releaseOffThread()){
        if (offThreadRelease){
            offThreadRelease(std::move(cmd));
        }else{
            BackgroundRelease::release(std::move(cmd));
        }
        return;
    }
    cmd.reset();
}

void editor::CommandHistory::releaseEntries(std::vector<UndoEntry>& entries){
    for (UndoEntry& entry : entries){
        releaseCommand(std::move(entry.cmd), entry.info.bytes);
    }
    entries.clear();
}

void editor::CommandHistory::recount(){
    size_t bytes = 0;
    for (const UndoEntry& entry : list) bytes += entry.info.bytes;
    for (const UndoEntry& entry : editRedo) bytes += entry.info.bytes;
    for (const UndoEntry& entry : interactive.redoAside) bytes += entry.info.bytes;
    heldBytes = bytes;
}

void editor::CommandHistory::captureName(UndoEntry& entry) const{
    std::string name = entry.cmd->describe();
    const UndoOrigin& origin = entry.info.origin;
    if (origin.kind == UndoOriginKind::Agent && !origin.client.empty()){
        const std::string prefix = origin.client + ": ";
        if (name.compare(0, prefix.size(), prefix) != 0){
            name = prefix + name;
        }
    }
    entry.cmd->label = CommandNames::clip(name);
}

void editor::CommandHistory::notifyModified(bool structural, bool selectionOnly) const{
    // a selection change modifies no scene (and starts no prefab stage Auto Save)
    if (onSceneModified && !selectionOnly){
        onSceneModified(sceneId, structural);
    }
}

void editor::CommandHistory::restoreSelection(const UndoEntry& entry, bool after) const{
    if (!entry.info.hasSelection || !selectionHooks.restore){
        return;
    }
    Applying applying;
    selectionHooks.restore(sceneId, after ? entry.info.selectionAfter : entry.info.selectionBefore);
}

void editor::CommandHistory::cutRedoTail(){
    if (index >= list.size()){
        return;
    }
    std::vector<UndoEntry> tail;
    tail.reserve(list.size() - index);
    for (auto it = list.begin() + index; it != list.end(); ++it){
        tail.push_back(std::move(*it));
    }
    list.erase(list.begin() + index, list.end());
    releaseEntries(tail);
}

bool editor::CommandHistory::addCommand(Command* raw){
    std::unique_ptr<Command> cmd(raw);
    if (!cmd){
        return false;
    }
    if (!beforeMutating()){
        releaseCommand(std::move(cmd), 0);
        return false;
    }
    // before execute(), which reads the values the command replaces: a running Physics
    // Placement gesture settles (and commits its own step) first
    if (beforeChange){
        beforeChange(sceneId, Change::Add);
    }

    if (beforeRecord && !interactive.open){
        beforeRecord(*this);
    }

    SelectionState before;
    const bool hasSelection = selectionHooks.read && selectionHooks.read(sceneId, before);

    bool executed;
    {
        Applying applying;
        StepScope scope(cmd.get());
        executed = cmd->execute();
    }
    if (!executed){
        const size_t bytes = cmd->memoryBytes();
        releaseCommand(std::move(cmd), bytes);
        return false;
    }

    // Changed nothing: no step, and the redo tail stays
    if (cmd->isNoOp()){
        {
            Applying applying;
            StepScope scope(cmd.get());
            cmd->undo();
            if (hasSelection && selectionHooks.restore){
                selectionHooks.restore(sceneId, before);
            }
        }
        const size_t bytes = cmd->memoryBytes();
        releaseCommand(std::move(cmd), bytes);
        return true;
    }

    recordEntry(std::move(cmd), before, hasSelection);
    return true;
}

void editor::CommandHistory::recordEntry(std::unique_ptr<Command> cmd, const SelectionState& before, bool hasSelection){
    cutRedoTail();
    // an interactive edit frees the partner's redo tail at its commit, a cancel keeps it
    if (!interactive.open){
        UndoTimeline::freePartnerRedo(*this);
    }

    const UndoOrigin& origin = UndoOriginScope::current();
    const bool structural = cmd->affectsStructure();

    UndoEntry entry;
    entry.info.origin = origin;
    entry.info.hasSelection = hasSelection;
    entry.info.selectionBefore = before;
    entry.info.selectionOnly = std::strcmp(cmd->typeName(), "SelectionCmd") == 0;

    bool merged = false;
    if (list.size() > undoFloor() && list.back().cmd->canMerge() && cmd->canMerge() &&
        list.back().info.origin.sameSource(origin) && cmd->mergeWith(list.back().cmd.get())){
        // the newer command took over the older one's state: one step with the older id
        UndoEntry older = std::move(list.back());
        list.pop_back();
        entry.info.id = older.info.id;
        entry.info.savedApplied = older.info.savedApplied;
        entry.info.linkId = older.info.linkId;
        entry.info.selectionBefore = older.info.selectionBefore;
        entry.info.hasSelection = older.info.hasSelection;
        releaseCommand(std::move(older.cmd), older.info.bytes);
        merged = true;
    }else{
        entry.info.id = nextEntryId++;
    }

    stamp(cmd.get());
    entry.cmd = std::move(cmd);

    // A merge back to where it started changed nothing
    if (merged && entry.cmd->isNoOp()){
        {
            Applying applying;
            entry.cmd->undo();
        }
        restoreSelection(entry, false);
        const size_t bytes = entry.cmd->memoryBytes();
        releaseCommand(std::move(entry.cmd), bytes);
        index = list.size();
        recount();
        notifyModified(structural, entry.info.selectionOnly);
        notifyChanged();
        return;
    }

    entry.info.time = std::chrono::system_clock::now();
    entry.info.bytes = entry.cmd->memoryBytes();
    if (hasSelection && selectionHooks.read){
        selectionHooks.read(sceneId, entry.info.selectionAfter);
    }
    captureName(entry);
    const uint64_t id = entry.info.id;
    const bool selectionOnly = entry.info.selectionOnly;

    list.push_back(std::move(entry));
    index = list.size();
    recount();

    notifyModified(structural, selectionOnly);
    trim(id);
    notifyChanged();
}

bool editor::CommandHistory::addCommandNoMerge(Command* cmd){
    cmd->setNoMerge();
    return addCommand(cmd);
}

bool editor::CommandHistory::insertCommand(Command* raw){
    std::unique_ptr<Command> cmd(raw);
    if (!cmd){
        return false;
    }
    if (!beforeMutating()){
        releaseCommand(std::move(cmd), 0);
        return false;
    }
    // before execute(), as in addCommand: a running Physics Placement gesture settles first
    if (beforeChange){
        beforeChange(sceneId, Change::Add);
    }

    bool executed;
    {
        Applying applying;
        StepScope scope(cmd.get());
        executed = cmd->execute();
    }
    if (!executed){
        const size_t bytes = cmd->memoryBytes();
        releaseCommand(std::move(cmd), bytes);
        return false;
    }
    cmd->setNoMerge();
    stamp(cmd.get());

    // The scene Stop restores was captured with this step applied, so in Play it is an
    // edit-time step: below the floor. An open drag keeps collecting above it.
    size_t position = this->position();
    if (inPlaySession){
        position = playFloor;
    }else if (interactive.open){
        position = interactive.start;
    }
    const size_t local = position - trimmedCount;

    UndoEntry entry;
    entry.info.id = nextEntryId++;
    entry.info.origin = UndoOriginScope::current();
    entry.info.time = std::chrono::system_clock::now();
    entry.info.bytes = cmd->memoryBytes();
    const bool structural = cmd->affectsStructure();
    entry.cmd = std::move(cmd);
    captureName(entry);
    const uint64_t id = entry.info.id;
    list.insert(list.begin() + local, std::move(entry));
    index++;
    if (inPlaySession){
        playFloor++;
    }
    if (interactive.open && interactive.start >= position){
        interactive.start++;
    }
    for (TrimHold& hold : trimHolds){
        if (hold.position >= position) hold.position++;
    }
    recount();

    notifyModified(structural);
    trim(id);
    notifyChanged();
    return true;
}

bool editor::CommandHistory::undo(){
    refusal.clear();
    undoProblemText.clear();
    // a Physics Placement gesture still running is cancelled instead
    if (beforeChange && !beforeChange(sceneId, Change::Undo)){
        return false;
    }
    // Ctrl+Z in a drag puts the values back and undoes nothing more
    if (interactive.open){
        return cancelInteractive();
    }
    if (!canUndo() || !beforeMutating()){
        return false;
    }
    UndoEntry& entry = list[index - 1];
    // undoing it now would break the project (a child scene cycle; a file it changes resolving
    // outside its write root through a link): the history stays, lastRefusal() says why
    {
        StepScope scope(entry.cmd.get());
        if (!entry.cmd->canUndo()){
            refusal = entry.cmd->undoRefusal();
            refusedText = refusal;
            refusedSerial = entry.cmd->serial();
            refusedUndo = true;
            safewrite::warn("Undo not applied: " + (refusal.empty() ? std::string("the step can't be undone now.") : refusal));
            return false;
        }
    }
    refusedSerial = 0;
    if (beforeStep){
        beforeStep(sceneId);
    }
    const bool structural = entry.cmd->affectsStructure();
    const unsigned long long failuresBefore = safewrite::scopedFailureCount();
    {
        Applying applying;
        StepScope scope(entry.cmd.get());
        entry.cmd->undo();
    }
    // a file it could not put back after its check passed (a link planted since; a write the
    // write scope refused inside it): said
    undoProblemText = entry.cmd->takeUndoProblem();
    if (undoProblemText.empty()) undoProblemText = safewrite::scopedFailureSince(failuresBefore);
    if (!undoProblemText.empty()) safewrite::warn("Undo incomplete: " + undoProblemText);
    index--;
    // the undo put back the modified flag it found, which a save since has made stale
    if (entry.info.savedApplied && onUndoPastSave){
        onUndoPastSave(sceneId);
    }
    restoreSelection(entry, false);
    notifyModified(structural, entry.info.selectionOnly);

    #ifdef _DEBUG
    printf("DEBUG: undo (%zu from %zu)\n", index, list.size());
    #endif
    notifyChanged();
    return true;
}

bool editor::CommandHistory::redo(){
    refusal.clear();
    undoProblemText.clear();
    if (beforeChange && !beforeChange(sceneId, Change::Redo)){
        return false;
    }
    if (interactive.open){
        commitInteractive();
    }
    if (!canRedo() || !beforeMutating()){
        return false;
    }
    UndoEntry& entry = list[index];
    {
        StepScope scope(entry.cmd.get());
        if (!entry.cmd->canRedo()){
            refusal = entry.cmd->redoRefusal();
            refusedText = refusal;
            refusedSerial = entry.cmd->serial();
            refusedUndo = false;
            safewrite::warn("Redo not applied: " + (refusal.empty() ? std::string("the step can't be redone now.") : refusal));
            return false;
        }
    }
    refusedSerial = 0;
    if (beforeStep){
        beforeStep(sceneId);
    }
    bool executed = false;
    const unsigned long long failuresBefore = safewrite::scopedFailureCount();
    {
        Applying applying;
        StepScope scope(entry.cmd.get());
        executed = entry.cmd->execute();
    }
    // a step that could not be done again and says why (its failure, or a write the write
    // scope refused in it) is not taken; one that just found nothing to do (the GameObjects it
    // changes are gone) is passed over as before
    if (!executed){
        std::string why = entry.cmd->failure();
        if (why.empty()) why = safewrite::scopedFailureSince(failuresBefore);
        if (!why.empty()){
            refusal = why;
            refusedText = refusal;
            refusedSerial = entry.cmd->serial();
            refusedUndo = false;
            safewrite::warn("Redo not applied: " + refusal);
            return false;
        }
    }
    // done, but a write the write scope refused inside it: said
    if (executed && safewrite::scopedFailureCount() != failuresBefore){
        undoProblemText = safewrite::scopedFailureSince(failuresBefore);
        safewrite::warn("Redo incomplete: " + undoProblemText);
    }
    index++;
    restoreSelection(entry, true);
    notifyModified(entry.cmd->affectsStructure(), entry.info.selectionOnly);

    #ifdef _DEBUG
    printf("DEBUG: redo (%zu from %zu)\n", index, list.size());
    #endif
    notifyChanged();
    return true;
}

std::string editor::CommandHistory::undoRefusedLastTry() const{
    if (!refusedUndo || refusedSerial == 0 || refusedSerial != undoSerial()) return std::string();
    return refusedText.empty() ? std::string("the step can't be undone now") : refusedText;
}

std::string editor::CommandHistory::redoRefusedLastTry() const{
    if (refusedUndo || refusedSerial == 0 || refusedSerial != redoSerial()) return std::string();
    return refusedText.empty() ? std::string("the step can't be redone now") : refusedText;
}

void editor::CommandHistory::stamp(Command* cmd){
    // a step's identity for the editor's run: a freed command's address can come back, its serial can't
    static unsigned long long next = 0;
    if (cmd) cmd->historySerial = ++next;
}

bool editor::CommandHistory::undoTo(uint64_t id){
    if (id != 0 && (!find(id) || !isApplied(id))){
        return false;
    }
    while (const UndoEntry* entry = top()){
        if (entry->info.id == id){
            return true;
        }
        if (!undo()){
            return false;
        }
    }
    return id == 0;
}

bool editor::CommandHistory::redoTo(uint64_t id){
    if (!find(id)){
        return false;
    }
    while (!isApplied(id)){
        if (!redo()){
            return false;
        }
    }
    return true;
}

bool editor::CommandHistory::canUndo() const{
    return interactive.open || index > undoFloor();
}

bool editor::CommandHistory::canRedo() const{
    return !interactive.open && index < list.size();
}

void editor::CommandHistory::clear(){
    interactive = Interactive{};  // its redo tail is freed below with the rest
    std::vector<UndoEntry> all;
    all.reserve(list.size() + editRedo.size());
    for (UndoEntry& entry : list) all.push_back(std::move(entry));
    for (UndoEntry& entry : editRedo) all.push_back(std::move(entry));
    list.clear();
    editRedo.clear();
    releaseEntries(all);
    index = 0;
    trimmedCount = 0;
    playFloor = 0;
    playStartId = 0;
    inPlaySession = false;
    trimHolds.clear();
    heldBytes = 0;
    notifyChanged();
}

size_t editor::CommandHistory::dropSteps(const std::function<bool(Command&)>& matches){
    std::vector<UndoEntry> dropped;
    for (size_t i = list.size(); i-- > 0;){
        if (!matches(*list[i].cmd)){
            continue;
        }
        const size_t position = i + trimmedCount;
        for (TrimHold& hold : trimHolds){
            if (hold.position > position) hold.position--;
        }
        if (playFloor > position) playFloor--;
        if (interactive.open && interactive.start > position) interactive.start--;
        if (index > i) index--;
        dropped.push_back(std::move(list[i]));
        list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
    }
    for (std::vector<UndoEntry>* aside : {&editRedo, &interactive.redoAside}){
        for (size_t i = aside->size(); i-- > 0;){
            if (matches(*(*aside)[i].cmd)){
                dropped.push_back(std::move((*aside)[i]));
                aside->erase(aside->begin() + static_cast<std::ptrdiff_t>(i));
            }
        }
    }
    const size_t count = dropped.size();
    if (count > 0){
        releaseEntries(dropped);
        recount();
        notifyChanged();
    }
    return count;
}

void editor::CommandHistory::freeRedo(){
    if (index < list.size()){
        cutRedoTail();
        recount();
        notifyChanged();
    }
}

const editor::UndoEntry* editor::CommandHistory::top() const{
    if (index > undoFloor()){
        return &list[index - 1];
    }
    return nullptr;
}

const editor::UndoEntry* editor::CommandHistory::nextRedo() const{
    if (!interactive.open && index < list.size()){
        return &list[index];
    }
    return nullptr;
}

std::vector<const editor::UndoEntry*> editor::CommandHistory::entries() const{
    std::vector<const UndoEntry*> result;
    result.reserve(list.size());
    for (const UndoEntry& entry : list){
        result.push_back(&entry);
    }
    return result;
}

const editor::UndoEntry* editor::CommandHistory::find(uint64_t id) const{
    // (not a binary search: an inserted step's id is newer than the steps after it)
    for (const UndoEntry& entry : list){
        if (entry.info.id == id) return &entry;
    }
    return nullptr;
}

bool editor::CommandHistory::isPlayStep(uint64_t id) const{
    if (!inPlaySession){
        return false;
    }
    for (size_t i = playFloor - trimmedCount; i < list.size(); i++){
        if (list[i].info.id == id) return true;
    }
    return false;
}

bool editor::CommandHistory::isApplied(uint64_t id) const{
    for (size_t i = 0; i < index; i++){
        if (list[i].info.id == id) return true;
    }
    return false;
}

void editor::CommandHistory::setTopLink(uint64_t linkId){
    if (index > 0){
        list[index - 1].info.linkId = linkId;
    }
}

void editor::CommandHistory::unlink(uint64_t linkId){
    if (linkId == 0){
        return;
    }
    for (UndoEntry& entry : list){
        if (entry.info.linkId == linkId) entry.info.linkId = 0;
    }
    for (UndoEntry& entry : editRedo){
        if (entry.info.linkId == linkId) entry.info.linkId = 0;
    }
}

void editor::CommandHistory::renameTop(const std::string& name){
    if (index > 0 && !name.empty()){
        list[index - 1].cmd->label = CommandNames::clip(name);
        notifyChanged();
    }
}

size_t editor::CommandHistory::mark(){
    if (index > undoFloor()){
        list[index - 1].cmd->setNoMerge();
    }
    return position();
}

void editor::CommandHistory::fold(size_t localStart, const std::string& label, bool keepSingle){
    // the steps from localStart to the end become one: the oldest id, the newest time
    if (keepSingle && list.size() - localStart == 1){
        if (!label.empty()){
            list.back().cmd->label = CommandNames::clip(label);
        }
        list.back().cmd->setNoMerge();
        return;
    }
    UndoEntry entry;
    const UndoEntry& first = list[localStart];
    const UndoEntry& last = list.back();
    entry.info = first.info;
    entry.info.time = last.info.time;
    entry.info.selectionAfter = last.info.selectionAfter;
    entry.info.selectionOnly = true;
    entry.info.bytes = 0;
    std::vector<std::unique_ptr<Command>> commands;
    for (auto it = list.begin() + localStart; it != list.end(); ++it){
        entry.info.bytes += it->info.bytes;
        entry.info.selectionOnly = entry.info.selectionOnly && it->info.selectionOnly;
        entry.info.savedApplied = entry.info.savedApplied || it->info.savedApplied;
        if (it->info.linkId != 0 && entry.info.linkId == 0) entry.info.linkId = it->info.linkId;
        commands.push_back(std::move(it->cmd));
    }
    list.erase(list.begin() + localStart, list.end());
    entry.cmd = std::make_unique<CommandGroup>(std::move(commands), label);
    stamp(entry.cmd.get());
    entry.info.bytes += sizeof(CommandGroup);
    entry.cmd->label = CommandNames::clip(entry.cmd->describe());
    list.push_back(std::move(entry));
    index = list.size();
}

bool editor::CommandHistory::groupSince(size_t mark, const std::string& label){
    // Trimmed, undone past the mark, a redo tail in the way, or a play floor moved
    if (mark < trimmedCount){
        return false;
    }
    const size_t local = mark - trimmedCount;
    if (local < undoFloor() || local > index || index != list.size() || interactive.open){
        return false;
    }
    if (index - local < 2){
        if (index - local == 1 && !label.empty()){
            fold(local, label, true);
            notifyChanged();
        }
        return true;
    }
    fold(local, label, false);
    recount();
    notifyChanged();
    return true;
}

// ------------------------------------------------------------------ trimming

bool editor::CommandHistory::trimmable(size_t localIndex, uint64_t keepId) const{
    if (localIndex >= index){
        return false;       // the redo tail stays
    }
    const UndoEntry& entry = list[localIndex];
    if (entry.info.id == keepId){
        return false;       // the step just pushed
    }
    const size_t absolute = trimmedCount + localIndex;
    if (inPlaySession && absolute >= playFloor){
        return false;       // Play steps go with the session
    }
    if (interactive.open && absolute >= interactive.start){
        return false;
    }
    for (const TrimHold& hold : trimHolds){
        if (absolute >= hold.position) return false;
    }
    return true;
}

void editor::CommandHistory::trimFront(){
    UndoEntry entry = std::move(list.front());
    list.erase(list.begin());
    index--;
    trimmedCount++;
    heldBytes -= std::min(heldBytes, entry.info.bytes);
    releaseCommand(std::move(entry.cmd), entry.info.bytes);
}

void editor::CommandHistory::trim(uint64_t keepId){
    bool trimmed = false;
    // Steps per history
    while (index > maxEntries && trimmable(0, keepId)){
        if (!trimmed && !beforeMutating()){
            return;
        }
        trimFront();
        trimmed = true;
    }

    // Bytes over all histories: the oldest step of any history goes first
    if (memoryLimit != 0){
        while (totalBytes() > memoryLimit){
            CommandHistory* oldest = nullptr;
            for (CommandHistory* history : liveHistories()){
                if (history->list.empty() || !history->trimmable(0, keepId)){
                    continue;
                }
                if (!oldest || history->list.front().info.id < oldest->list.front().info.id){
                    oldest = history;
                }
            }
            if (!oldest){
                if (!budgetWarned){
                    budgetWarned = true;
                    Out::warning("Undo history is over its memory limit (%zu MB) with nothing more it may free; "
                                 "the steps of Play mode and of open edits go when they end.", memoryLimit / (1024 * 1024));
                }
                break;
            }
            if (!trimmed && !beforeMutating()){
                return;
            }
            oldest->trimFront();
            trimmed = true;
        }
    }

    if (trimmed){
        notifyChanged();
    }
}

// ------------------------------------------------------------------ interactive edits

editor::InteractiveEdit editor::CommandHistory::beginInteractive(std::string name){
    commitInteractiveEdits(this);
    if (interactive.open){
        commitInteractive();
    }
    mark();
    interactive.open = true;
    interactive.serial = nextSerial++;
    interactive.name = std::move(name);
    interactive.start = position();
    // the redo tail is set aside, as a play session does: a cancel brings it back
    for (auto it = list.begin() + index; it != list.end(); ++it){
        interactive.redoAside.push_back(std::move(*it));
    }
    list.erase(list.begin() + index, list.end());
    notifyChanged();
    return InteractiveEdit(this, interactive.serial);
}

void editor::CommandHistory::commitInteractive(){
    if (interactive.open){
        commitInteractive(interactive.serial);
    }
}

bool editor::CommandHistory::cancelInteractive(){
    if (!interactive.open){
        return false;
    }
    cancelInteractive(interactive.serial);
    return true;
}

void editor::CommandHistory::commitInteractive(uint64_t serial, bool warnForgotten){
    if (!interactive.open || interactive.serial != serial){
        return;
    }
    if (warnForgotten){
        #ifdef _DEBUG
        printf("DEBUG: interactive edit \"%s\" destroyed open, committed\n", interactive.name.c_str());
        #endif
    }
    Interactive edit = std::move(interactive);
    interactive = Interactive{};

    const size_t local = edit.start - trimmedCount;
    bool kept = false;
    if (index > local){
        fold(local, list.size() - local == 1 ? std::string() : edit.name, true);
        UndoEntry& entry = list.back();
        if (entry.cmd->isNoOp()){
            // the drag came back where it started
            const bool structural = entry.cmd->affectsStructure();
            {
                Applying applying;
                entry.cmd->undo();
            }
            restoreSelection(entry, false);
            releaseCommand(std::move(entry.cmd), entry.info.bytes);
            list.pop_back();
            index = list.size();
            notifyModified(structural);
        }else{
            kept = true;
        }
    }

    if (kept){
        releaseEntries(edit.redoAside);
        UndoTimeline::freePartnerRedo(*this);
    }else{
        for (UndoEntry& entry : edit.redoAside){
            list.push_back(std::move(entry));
        }
    }
    recount();
    if (kept){
        trim(list[index - 1].info.id);
    }
    notifyChanged();
}

void editor::CommandHistory::cancelInteractive(uint64_t serial){
    if (!interactive.open || interactive.serial != serial){
        return;
    }
    Interactive edit = std::move(interactive);
    interactive = Interactive{};

    const size_t local = edit.start - trimmedCount;
    std::vector<UndoEntry> collected;
    bool structural = false;
    bool pastSave = false;
    {
        Applying applying;
        while (list.size() > local){
            UndoEntry entry = std::move(list.back());
            list.pop_back();
            structural = structural || entry.cmd->affectsStructure();
            pastSave = pastSave || entry.info.savedApplied;
            {
                StepScope scope(entry.cmd.get());
                entry.cmd->undo();
            }
            if (entry.info.hasSelection && selectionHooks.restore){
                selectionHooks.restore(sceneId, entry.info.selectionBefore);
            }
            collected.push_back(std::move(entry));
        }
    }
    // collected is newest first: freed in that order
    for (UndoEntry& entry : collected){
        releaseCommand(std::move(entry.cmd), entry.info.bytes);
    }
    for (UndoEntry& entry : edit.redoAside){
        list.push_back(std::move(entry));
    }
    index = local;
    recount();
    if (pastSave && onUndoPastSave){
        onUndoPastSave(sceneId);
    }
    if (!collected.empty()){
        notifyModified(structural);
    }
    notifyChanged();
}

// ------------------------------------------------------------------ play sessions

void editor::CommandHistory::beginPlaySession(){
    if (inPlaySession){
        return;
    }
    commitInteractive();
    inPlaySession = true;
    playFloor = position();
    playStartId = nextEntryId.load();
    budgetWarned = false;
    for (auto it = list.begin() + index; it != list.end(); ++it){
        editRedo.push_back(std::move(*it));
    }
    list.erase(list.begin() + index, list.end());
    notifyChanged();
}

void editor::CommandHistory::endPlaySession(bool rolledBack){
    if (!inPlaySession){
        return;
    }
    commitInteractive();
    inPlaySession = false;
    const size_t floor = playFloor - trimmedCount;

    if (!rolledBack && list.size() > floor){
        // play-time commands stay, and like any new command they end the old redo tail
        releaseEntries(editRedo);
        recount();
        notifyChanged();
        return;
    }

    std::vector<UndoEntry> played;
    for (auto it = list.begin() + floor; it != list.end(); ++it){
        played.push_back(std::move(*it));
    }
    list.erase(list.begin() + floor, list.end());
    releaseEntries(played);
    for (UndoEntry& entry : editRedo){
        list.push_back(std::move(entry));
    }
    editRedo.clear();
    index = floor;
    recount();
    notifyChanged();
}
