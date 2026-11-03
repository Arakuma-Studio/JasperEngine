// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "UndoOrigin.h"

#include <utility>

using namespace jasperengine;

namespace {

    editor::UndoOrigin& currentOrigin(){
        static editor::UndoOrigin origin;
        return origin;
    }

}

const char* editor::undoOriginKindName(UndoOriginKind kind){
    switch (kind){
        case UndoOriginKind::User: return "user";
        case UndoOriginKind::Agent: return "agent";
        case UndoOriginKind::PlayReview: return "play_review";
        case UndoOriginKind::System: return "system";
    }
    return "user";
}

editor::UndoOriginScope::UndoOriginScope(UndoOrigin origin): previous(currentOrigin()){
    currentOrigin() = std::move(origin);
}

editor::UndoOriginScope::~UndoOriginScope(){
    currentOrigin() = std::move(previous);
}

const editor::UndoOrigin& editor::UndoOriginScope::current(){
    return currentOrigin();
}
