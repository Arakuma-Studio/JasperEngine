// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CommandHistory.h"
#include <stdio.h>

using namespace jasperengine;

namespace {

// Commands already applied, undone and redone as one step
class CommandGroup : public jasperengine::editor::Command {
private:
    std::vector<jasperengine::editor::Command*> commands;

public:
    explicit CommandGroup(std::vector<jasperengine::editor::Command*> commands): commands(std::move(commands)) {
        setNoMerge();
    }

    ~CommandGroup() override {
        for (jasperengine::editor::Command* cmd : commands) delete cmd;
    }

    bool execute() override {
        for (jasperengine::editor::Command* cmd : commands) cmd->execute();
        return true;
    }

    void undo() override {
        for (auto it = commands.rbegin(); it != commands.rend(); ++it) (*it)->undo();
    }

    bool affectsStructure() const override {
        for (const jasperengine::editor::Command* cmd : commands) {
            if (cmd->affectsStructure()) return true;
        }
        return false;
    }
};

}

std::function<void(size_t, bool)> editor::CommandHistory::onSceneModified = nullptr;

editor::CommandHistory::CommandHistory(size_t sceneId): sceneId(sceneId){
}

editor::CommandHistory::~CommandHistory(){
    clear();
}

bool editor::CommandHistory::addCommand(editor::Command* cmd){
    if (cmd->execute()){
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

void editor::CommandHistory::undo(){
    if (index > undoFloor()){
        const bool structural = list[index-1]->affectsStructure();
        list[index-1]->undo();
        index--;

        if (onSceneModified){
            onSceneModified(sceneId, structural);
        }

        #ifdef _DEBUG
        printf("DEBUG: undo (%zu from %zu)\n", index, list.size());
        #endif
    }
}

void editor::CommandHistory::redo(){
    if (index < list.size()){
        index++;
        list[index-1]->execute();

        if (onSceneModified){
            onSceneModified(sceneId, list[index-1]->affectsStructure());
        }

        #ifdef _DEBUG
        printf("DEBUG: redo (%zu from %zu)\n", index, list.size());
        #endif
    }
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
    list.push_back(new CommandGroup(std::move(commands)));
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
