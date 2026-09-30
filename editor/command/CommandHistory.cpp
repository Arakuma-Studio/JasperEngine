// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CommandHistory.h"
#include <stdio.h>

using namespace jasperengine;

std::function<void(size_t, bool)> editor::CommandHistory::onSceneModified = nullptr;

editor::CommandHistory::CommandHistory(size_t sceneId): sceneId(sceneId){
}

editor::CommandHistory::~CommandHistory(){
    clear();
}

void editor::CommandHistory::addCommand(editor::Command* cmd){
    if (cmd->execute()){
        if (index < list.size()){
            for (auto it = list.begin() + index; it != list.end(); ++it) {
                delete *it;
            }

            list.erase(list.begin() + index, list.end());
        }

        if (list.size() > undoFloor() && list.back()->canMerge() && cmd->canMerge()){
            if (cmd->mergeWith(list.back())){
                list.pop_back();
            }
        }

        list.push_back(cmd);
        index = list.size();

        if (onSceneModified){
            onSceneModified(sceneId, cmd->affectsStructure());
        }
    }else{
        delete cmd;
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