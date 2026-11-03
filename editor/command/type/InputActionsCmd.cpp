// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "InputActionsCmd.h"

#include "Out.h"
#include "command/type/InputActionsEditCmd.h"
#include "input/InputActions.h"
#include "io/AtomicFile.h"
#include "io/FileData.h"

#include <fstream>
#include <iterator>
#include <system_error>

using namespace jasperengine;

namespace {

    // a free name in the assets directory for the asset the command makes
    std::filesystem::path newAssetPath(editor::Project* project){
        const std::filesystem::path assets = project->getAssetsPath();
        for (int n = 1;; n++){
            const std::string name = n == 1 ? "InputManager.inputactions" : "InputManager " + std::to_string(n) + ".inputactions";
            std::error_code ec;
            if (!std::filesystem::exists(assets / name, ec)){
                std::filesystem::path relative = (assets / name).lexically_relative(project->getProjectPath());
                return relative.empty() ? assets / name : relative;
            }
        }
    }

}

editor::InputActionsCmd::InputActionsCmd(Project* project, const std::vector<InputManagerAction>& actions)
    : project(project), after(actions) {
    beforeAsset = project->getInputActionsAsset();
    InputActionAssetDesc asset;
    std::string error;
    if (!beforeAsset.empty() && project->readInputActionsAsset(beforeText) && InputActionAssetDesc::parse(beforeText, asset, error)){
        afterAsset = beforeAsset;
    }else{
        // no asset (or one that cannot be read, which stays as it is): a new one
        afterAsset = newAssetPath(project);
        beforeText.clear();
        asset = InputActionAssetDesc();
        created = true;
    }
    InputManagerFormat::setInAsset(asset, actions);
    afterText = asset.save();
    label = created ? "Create Input Manager Actions" : "Edit Input Manager Actions";
}

bool editor::InputActionsCmd::apply(bool undoing){
    const std::filesystem::path& asset = undoing ? beforeAsset : afterAsset;
    if (!project->getProjectPath().empty()){
        const std::filesystem::path file = project->getProjectPath() / afterAsset;
        if (undoing && created){
            std::error_code ec;
            std::filesystem::remove(file, ec);
            if (ec) Out::error("Could not remove %s: %s", file.generic_string().c_str(), ec.message().c_str());
        }else{
            std::error_code ec;
            std::filesystem::create_directories(file.parent_path(), ec);
            const io::WriteResult result = io::writeFileAtomic(file, undoing ? beforeText : afterText);
            if (!result.ok()){
                Out::error("Could not write %s: %s", file.generic_string().c_str(), result.message.c_str());
                return false;
            }
        }
    }
    project->setInputActionsAsset(asset);
    // the standalone build's main.cpp loads the asset
    project->markGeneratedSourcesDirty();

    // a running Play takes the edit at once (its maps keep their state)
    if (project->getPlaySessionSceneId() != NULL_PROJECT_SCENE){
        if (InputActions::isLoaded()){
            std::error_code ec;
            const std::filesystem::path loaded = FileData::getSystemPath(InputActions::getAssetPath());
            const std::filesystem::path file = project->getProjectPath() / afterAsset;
            if (std::filesystem::equivalent(loaded, file, ec) || loaded == file)
                InputActions::reloadText(undoing ? (created ? InputManagerFormat::defaultsAsset().save() : beforeText) : afterText);
        }else if (InputActions::usesBuiltInDefaults()){
            InputActions::setManagerActions(undoing ? InputManagerFormat::defaults() : after);
        }
    }
    if (project->getProjectPath().empty())
        return true;
    return project->saveProjectFile();
}

bool editor::InputActionsCmd::execute(){
    return apply(false);
}

void editor::InputActionsCmd::undo(){
    apply(true);
}

std::vector<editor::Command::Touched> editor::InputActionsCmd::touchedPaths(bool) const{
    if (project->getProjectPath().empty())
        return {};
    return {{project->getProjectPath() / "project.yaml", false}, {project->getProjectPath() / afterAsset, created}};
}
