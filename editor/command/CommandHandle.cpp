// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CommandHandle.h"

using namespace jasperengine;

std::map<size_t, editor::CommandHistory*>  editor::CommandHandle::historys;


editor::CommandHistory* editor::CommandHandle::get(size_t sceneId){
    if (!historys.count(sceneId)){
        historys[sceneId] = new CommandHistory(sceneId);
    }
    return historys[sceneId];
}

editor::CommandHistory* editor::CommandHandle::find(size_t sceneId){
    auto it = historys.find(sceneId);
    return it != historys.end() ? it->second : nullptr;
}

void editor::CommandHandle::remove(size_t sceneId){
    auto it = historys.find(sceneId);
    if (it != historys.end()){
        CommandHistory* history = it->second;
        historys.erase(it);
        delete history;
    }
}
