// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SelectionCmd.h"

#include "Project.h"
#include "command/CommandNames.h"
#include "command/PlayChangeReview.h"

using namespace jasperengine;

editor::SelectionCmd::SelectionCmd(Project* project, uint32_t sceneId, SelectionState before, SelectionState after, uint64_t gesture):
    project(project), sceneId(sceneId), before(std::move(before)), after(std::move(after)), gesture(gesture){
}

bool editor::SelectionCmd::execute(){
    if (!project->getScene(sceneId)){
        return false;
    }
    project->setSelection(sceneId, after, SelectionCause::Restore);
    return true;
}

void editor::SelectionCmd::undo(){
    if (project->getScene(sceneId)){
        project->setSelection(sceneId, before, SelectionCause::Restore);
    }
}

bool editor::SelectionCmd::mergeWith(Command* otherCommand){
    SelectionCmd* otherCmd = dynamic_cast<SelectionCmd*>(otherCommand);
    if (otherCmd && otherCmd->sceneId == sceneId && otherCmd->gesture == gesture){
        before = otherCmd->before;
        return true;
    }
    return false;
}

std::string editor::SelectionCmd::describe() const{
    if (after.entities.empty()){
        return "Deselect All";
    }
    return "Select " + CommandNames::entities(project, sceneId, after.entities);
}

void editor::SelectionCmd::collectKeepCommands(KeepContext& context) const{
    context.nothingToKeep();
}
