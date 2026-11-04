// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CommandHandle.h"

#include "PlayChangeReview.h"

using namespace jasperengine;

std::map<size_t, editor::CommandHistory*>  editor::CommandHandle::historys;
bool editor::CommandHandle::redirecting = false;
size_t editor::CommandHandle::redirectFrom = 0;
size_t editor::CommandHandle::redirectTo = 0;


editor::CommandHandle::Redirect::Redirect(size_t from, size_t to){
    redirecting = true;
    redirectFrom = from;
    redirectTo = to;
}

editor::CommandHandle::Redirect::~Redirect(){
    redirecting = false;
}

editor::CommandHistory* editor::CommandHandle::get(size_t sceneId){
    if (redirecting && sceneId == redirectFrom) sceneId = redirectTo;
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
    // a pending Play Mode Changes review goes with its scene (S24 R30)
    PlayChangeReview::get().discard((uint32_t)sceneId);
    auto it = historys.find(sceneId);
    if (it != historys.end()){
        CommandHistory* history = it->second;
        historys.erase(it);
        delete history;
    }
}
