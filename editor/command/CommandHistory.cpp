// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CommandHistory.h"
#include "util/SafeWrite.h"
#include <optional>
#include <stdio.h>

using namespace jasperengine;

namespace {

// Commands already applied, undone and redone as one step
class CommandGroup : public jasperengine::editor::Command {
private:
    std::vector<jasperengine::editor::Command*> commands;
    std::string failureText;

public:
    explicit CommandGroup(std::vector<jasperengine::editor::Command*> commands): commands(std::move(commands)) {
        setNoMerge();
    }

    ~CommandGroup() override {
        for (jasperengine::editor::Command* cmd : commands) delete cmd;
    }

    // every step done again; false when one of them failed (CommandHistory::redo then reports
    // it when a step gave a reason)
    bool execute() override {
        bool all = true;
        failureText.clear();
        for (jasperengine::editor::Command* cmd : commands) {
            if (!cmd->execute()) {
                all = false;
                const std::string why = cmd->failure();
                if (!why.empty() && failureText.empty()) failureText = why;
            }
        }
        return all;
    }

    std::string failure() const override { return failureText; }

    void undo() override {
        for (auto it = commands.rbegin(); it != commands.rend(); ++it) {
            (*it)->undo();
            const std::string problem = (*it)->takeUndoProblem();
            if (!problem.empty()) undoProblem += (undoProblem.empty() ? "" : "; ") + problem;
        }
    }

    std::vector<Touched> touchedPaths(bool undo) const override {
        std::vector<Touched> all;
        for (const jasperengine::editor::Command* cmd : commands) {
            std::vector<Touched> some = cmd->touchedPaths(undo);
            all.insert(all.end(), some.begin(), some.end());
        }
        return all;
    }

    bool canUndo() const override {
        for (const jasperengine::editor::Command* cmd : commands) {
            if (!cmd->canUndo()) return false;
        }
        return true;
    }

    bool canRedo() const override {
        for (const jasperengine::editor::Command* cmd : commands) {
            if (!cmd->canRedo()) return false;
        }
        return true;
    }

    std::string undoRefusal() const override {
        for (const jasperengine::editor::Command* cmd : commands) {
            std::string why = cmd->undoRefusal();
            if (!why.empty()) return why;
        }
        return std::string();
    }

    std::string redoRefusal() const override {
        for (const jasperengine::editor::Command* cmd : commands) {
            std::string why = cmd->redoRefusal();
            if (!why.empty()) return why;
        }
        return std::string();
    }

    bool affectsStructure() const override {
        for (const jasperengine::editor::Command* cmd : commands) {
            if (cmd->affectsStructure()) return true;
        }
        return false;
    }
};

}

namespace {

// A step's execute / undo runs in the write scope it was made in (Command::writeRoot): an MCP-made
// step stays beneath the project also when the GUI undoes it
class StepScope {
public:
    explicit StepScope(const jasperengine::editor::Command* cmd) {
        if (cmd && !cmd->writeRoot().empty()) scope.emplace(cmd->writeRoot());
    }

private:
    std::optional<jasperengine::editor::safewrite::Scope> scope;
};

}

std::function<void(size_t, bool)> editor::CommandHistory::onSceneModified = nullptr;
std::function<void(size_t)> editor::CommandHistory::beforeStep = nullptr;
std::function<bool(size_t, editor::CommandHistory::Change)> editor::CommandHistory::beforeChange = nullptr;

editor::CommandHistory::CommandHistory(size_t sceneId): sceneId(sceneId){
}

void editor::CommandHistory::stamp(Command* cmd){
    static unsigned long long next = 0;
    cmd->historySerial = ++next;
}

editor::CommandHistory::~CommandHistory(){
    clear();
}

bool editor::CommandHistory::addCommand(editor::Command* cmd){
    // before execute(), which reads the values the command replaces
    if (beforeChange){
        beforeChange(sceneId, Change::Add);
    }
    bool executed = false;
    {
        StepScope scope(cmd);
        executed = cmd->execute();
    }
    if (executed){
        if (index < list.size()){
            for (auto it = list.begin() + index; it != list.end(); ++it) {
                delete *it;
            }

            list.erase(list.begin() + index, list.end());
        }

        if (list.size() > undoFloor() && list.back()->canMerge() && cmd->canMerge()){
            if (cmd->mergeWith(list.back())){
                delete list.back();
                list.pop_back();
            }
        }

        stamp(cmd);
        list.push_back(cmd);
        index = list.size();

        if (onSceneModified){
            onSceneModified(sceneId, cmd->affectsStructure());
        }
        return true;
    }else{
        delete cmd;
        return false;
    }
}

void editor::CommandHistory::addCommandNoMerge(Command* cmd){
    cmd->setNoMerge();
    addCommand(cmd);
}

bool editor::CommandHistory::insertCommand(Command* cmd){
    // before execute(), as in addCommand: a running Physics Placement gesture settles first
    if (beforeChange){
        beforeChange(sceneId, Change::Add);
    }
    bool executed = false;
    {
        StepScope scope(cmd);
        executed = cmd->execute();
    }
    if (!executed){
        delete cmd;
        return false;
    }
    cmd->setNoMerge();

    // The scene Stop restores was captured with this step applied, so it is an edit-time step
    const size_t position = inPlaySession ? playFloor : index;
    stamp(cmd);
    list.insert(list.begin() + position, cmd);
    if (inPlaySession){
        playFloor++;
    }
    index++;

    if (onSceneModified){
        onSceneModified(sceneId, cmd->affectsStructure());
    }
    return true;
}

bool editor::CommandHistory::undo(){
    refusal.clear();
    undoProblemText.clear();
    if (beforeChange && !beforeChange(sceneId, Change::Undo)){
        return false;
    }
    // a step that refuses (Command::canUndo) keeps the history where it is
    if (index > undoFloor()){
        StepScope scope(list[index-1]);
        if (!list[index-1]->canUndo()){
            refusal = list[index-1]->undoRefusal();
            refusedText = refusal;
            refusedSerial = list[index-1]->serial();
            refusedUndo = true;
            safewrite::warn("Undo not applied: " + (refusal.empty() ? std::string("the step can't be undone now.") : refusal));
            return false;
        }
    }
    if (index > undoFloor()){
        refusedSerial = 0;
        if (beforeStep){
            beforeStep(sceneId);
        }
        const bool structural = list[index-1]->affectsStructure();
        const unsigned long long failuresBefore = safewrite::scopedFailureCount();
        {
            StepScope scope(list[index-1]);
            list[index-1]->undo();
        }
        // a file it could not put back after its check passed (a link planted since; a write the
        // write scope refused inside it): said
        undoProblemText = list[index-1]->takeUndoProblem();
        if (undoProblemText.empty() && safewrite::scopedFailureCount() != failuresBefore) undoProblemText = safewrite::lastScopedError();
        if (!undoProblemText.empty()) safewrite::warn("Undo incomplete: " + undoProblemText);
        index--;

        if (onSceneModified){
            onSceneModified(sceneId, structural);
        }

        #ifdef _DEBUG
        printf("DEBUG: undo (%zu from %zu)\n", index, list.size());
        #endif
        return true;
    }
    return false;
}

bool editor::CommandHistory::redo(){
    refusal.clear();
    undoProblemText.clear();
    if (beforeChange && !beforeChange(sceneId, Change::Redo)){
        return false;
    }
    if (index < list.size()){
        StepScope scope(list[index]);
        if (!list[index]->canRedo()){
            refusal = list[index]->redoRefusal();
            refusedText = refusal;
            refusedSerial = list[index]->serial();
            refusedUndo = false;
            safewrite::warn("Redo not applied: " + (refusal.empty() ? std::string("the step can't be redone now.") : refusal));
            return false;
        }
    }
    if (index < list.size()){
        refusedSerial = 0;
        if (beforeStep){
            beforeStep(sceneId);
        }
        bool executed = false;
        const unsigned long long failuresBefore = safewrite::scopedFailureCount();
        {
            StepScope scope(list[index]);
            executed = list[index]->execute();
        }
        // a step that could not be done again and says why (its failure, or a write the write
        // scope refused in it) is not taken; one that just found nothing to do (the GameObjects it
        // changes are gone) is passed over as before
        if (!executed){
            std::string why = list[index]->failure();
            if (why.empty() && safewrite::scopedFailureCount() != failuresBefore) why = safewrite::lastScopedError();
            if (!why.empty()){
                refusal = why;
                refusedText = refusal;
                refusedSerial = list[index]->serial();
                refusedUndo = false;
                safewrite::warn("Redo not applied: " + refusal);
                return false;
            }
        }
        // done, but a write the write scope refused inside it: said
        if (executed && safewrite::scopedFailureCount() != failuresBefore){
            undoProblemText = safewrite::lastScopedError();
            safewrite::warn("Redo incomplete: " + undoProblemText);
        }
        index++;

        if (onSceneModified){
            onSceneModified(sceneId, list[index-1]->affectsStructure());
        }

        #ifdef _DEBUG
        printf("DEBUG: redo (%zu from %zu)\n", index, list.size());
        #endif
        return true;
    }
    return false;
}

std::string editor::CommandHistory::undoRefusedLastTry() const{
    if (!refusedUndo || refusedSerial == 0 || refusedSerial != undoSerial()) return std::string();
    return refusedText.empty() ? std::string("the step can't be undone now") : refusedText;
}

std::string editor::CommandHistory::redoRefusedLastTry() const{
    if (refusedUndo || refusedSerial == 0 || refusedSerial != redoSerial()) return std::string();
    return refusedText.empty() ? std::string("the step can't be redone now") : refusedText;
}

bool editor::CommandHistory::canUndo() const{
    return index > undoFloor();
}

bool editor::CommandHistory::canRedo() const{
    return index < list.size();
}

void editor::CommandHistory::clear(){
    for (Command* cmd : list){
        delete cmd;
    }
    for (Command* cmd : editRedo){
        delete cmd;
    }
    list.clear();
    editRedo.clear();
    index = 0;
    playFloor = 0;
    inPlaySession = false;
}

void editor::CommandHistory::dropRedo(){
    for (auto it = list.begin() + index; it != list.end(); ++it){
        delete *it;
    }
    list.erase(list.begin() + index, list.end());
}

size_t editor::CommandHistory::mark(){
    if (index > undoFloor()){
        list[index-1]->setNoMerge();
    }
    return index;
}

bool editor::CommandHistory::groupSince(size_t mark){
    // Undone past the mark, a redo tail in the way, or a play floor moved: leave it alone
    if (mark < undoFloor() || mark > index || index != list.size()){
        return false;
    }
    if (index - mark < 2){
        return true;
    }
    std::vector<Command*> commands(list.begin() + mark, list.end());
    list.erase(list.begin() + mark, list.end());
    Command* group = new CommandGroup(std::move(commands));
    stamp(group);
    list.push_back(group);
    index = list.size();
    return true;
}

void editor::CommandHistory::beginPlaySession(){
    if (inPlaySession){
        return;
    }
    inPlaySession = true;
    playFloor = index;
    editRedo.assign(list.begin() + index, list.end());
    list.erase(list.begin() + index, list.end());
}

void editor::CommandHistory::endPlaySession(bool rolledBack){
    if (!inPlaySession){
        return;
    }
    inPlaySession = false;

    if (!rolledBack && list.size() > playFloor){
        // play-time commands stay, and like any new command they end the old redo tail
        for (Command* cmd : editRedo){
            delete cmd;
        }
        editRedo.clear();
        return;
    }

    for (auto it = list.begin() + playFloor; it != list.end(); ++it){
        delete *it;
    }
    list.erase(list.begin() + playFloor, list.end());
    list.insert(list.end(), editRedo.begin(), editRedo.end());
    editRedo.clear();
    index = playFloor;
}
