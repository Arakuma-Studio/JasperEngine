// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ModelClipSettingsFile.h"

#include "Project.h"
#include "command/CommandHistory.h"
#include "command/type/WriteFilesCmd.h"
#include "util/FileUtils.h"
#include "util/ProjectChecks.h"
#include "util/ProjectFileWrites.h"

#include <memory>
#include <system_error>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace{

    fs::path sidecarOfModel(const fs::path& model){
        return FileUtils::pathFromUtf8(anim::modelClipSettingsPath(FileUtils::pathToUtf8(model.lexically_normal())));
    }

}

bool editor::readModelClipSettingsFile(const fs::path& model, anim::ModelClipSettings& out, std::string& error, std::string* text){
    out = anim::ModelClipSettings();
    if (text) text->clear();
    const fs::path sidecar = sidecarOfModel(model);
    std::error_code ec;
    if (!fs::exists(sidecar, ec)) return true;
    std::string content;
    if (!fs::is_regular_file(sidecar, ec) || !ProjectChecks::readFile(sidecar, content)){
        error = "cannot read " + FileUtils::pathToGenericUtf8(sidecar);
        return false;
    }
    std::vector<std::string> warnings;
    std::string parseError;
    if (!anim::parseModelClipSettings(content, out, warnings, &parseError)){
        error = sidecar.filename().string() + ": " + parseError + " (fix or delete the file first: writing it would lose its settings)";
        return false;
    }
    if (text) *text = std::move(content);
    return true;
}

bool editor::writeModelClipSettingsFile(Project* project, const fs::path& model, const anim::ModelClipSettings& settings,
                                        const std::string& undoName, std::string& error, bool* changed){
    if (changed) *changed = false;
    if (!project){
        error = "no project is open";
        return false;
    }
    anim::ModelClipSettings stored;
    std::string before;
    if (!readModelClipSettingsFile(model, stored, error, &before)) return false;
    const fs::path sidecar = sidecarOfModel(model);
    const std::string text = anim::writeModelClipSettings(settings);
    std::error_code ec;
    if (fs::exists(sidecar, ec) && text == before) return true;
    auto* command = new WriteFilesCmd({{sidecar, text}}, [project](const std::vector<fs::path>& paths){
        ProjectFileWrites::filesWritten(project, paths);
    });
    command->setName(undoName);
    command->setNoMerge();
    auto failure = std::make_shared<std::string>();
    command->reportFailureTo(failure);
    // addCommand runs execute() and deletes the command when it fails (nothing changed then)
    if (!project->getProjectCommandHistory()->addCommand(command)){
        error = "cannot write " + sidecar.filename().string() + (failure->empty() ? std::string() : ": " + *failure);
        return false;
    }
    if (changed) *changed = true;
    return true;
}
