// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CreateClipCmd.h"

using namespace jasperengine;
using namespace jasperengine::editor;

CreateClipCmd::CreateClipCmd(Project* project, uint32_t sceneId, std::vector<File> files, std::vector<Command*> sceneCommands):
        project(project), sceneId(sceneId), files(std::move(files)){
    for (Command* command : sceneCommands){
        this->sceneCommands.emplace_back(command);
    }
}

CreateClipCmd::~CreateClipCmd(){
}

bool CreateClipCmd::execute(){
    // TODO(T2d): write the files, run the scene commands (all or nothing)
    failure = "Creating clips is not implemented yet.";
    return false;
}

void CreateClipCmd::undo(){
    // TODO(T2d)
}

bool CreateClipCmd::affectsStructure() const{
    for (const std::unique_ptr<Command>& command : sceneCommands){
        if (command->affectsStructure()) return true;
    }
    return false;
}
