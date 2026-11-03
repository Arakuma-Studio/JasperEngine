// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CreateClipCmd.h"
#include "command/CommandNames.h"

#include "Out.h"
#include "Project.h"
#include "animation/AnimationClipDocument.h"
#include "animation/AnimationClipIO.h"
#include "animation/ClipEditorUtil.h"
#include "command/type/ClipSceneActions.h"
#include "util/FileUtils.h"
#include "util/SafeWrite.h"

#include <algorithm>
#include <cerrno>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    using clipui::readWholeFile;

}

CreateClipCmd::CreateClipCmd(Project* project, uint32_t sceneId, std::vector<File> files, std::vector<Command*> sceneCommands):
        project(project), sceneId(sceneId), files(std::move(files)){
    for (Command* command : sceneCommands){
        this->sceneCommands.emplace_back(command);
    }
}

CreateClipCmd::~CreateClipCmd(){
}

bool CreateClipCmd::fail(const std::string& message){
    // a write the write scope refused during this execute: its reason too (never an older one)
    failureText = message;
    const std::string scoped = safewrite::scopedFailureSince(failureMark);
    if (!scoped.empty() && message.find(scoped) == std::string::npos) failureText += " (" + scoped + ")";
    if (failureSink) *failureSink = message;
    return false;
}

void CreateClipCmd::notifyFiles(){
    std::vector<fs::path> paths;
    for (const File& file : files) paths.push_back(file.path);
    AnimationClipDocument::filesWritten(project, paths);
}

void CreateClipCmd::restoreFiles(size_t count, bool checkBytes){
    for (size_t i = count; i-- > 0;){
        if (i >= written.size() || !written[i].done) continue;
        const File& file = files[i];
        written[i].done = false;
        std::error_code ec;
        if (checkBytes){
            std::string current;
            const bool exists = fs::is_regular_file(file.path, ec);
            if (!exists || !readWholeFile(file.path, current) || current != file.content){
                // changed (or deleted) since it was written: not this step's to take back (R4)
                if (exists){
                    keptFiles.push_back(file.path);
                    editor::Out::warning("Kept '%s': it changed after it was written, so undo leaves it as it is",
                                         FileUtils::pathToGenericUtf8(file.path).c_str());
                }else{
                    const fs::path moved = movedTo(file);
                    if (!moved.empty()){
                        keptFiles.push_back(moved);
                        editor::Out::warning("Kept '%s': the clip this step wrote moved there, so undo leaves it as it is",
                                             FileUtils::pathToGenericUtf8(moved).c_str());
                    }
                }
                continue;
            }
        }
        if (written[i].existed){
            if (!FileUtils::writeFileReplacing(file.path, written[i].previous)){
                if (checkBytes) undoProblem += (undoProblem.empty() ? "" : "; ") + std::string("could not put back ") + FileUtils::pathToGenericUtf8(file.path);
                editor::Out::warning("Could not put back the previous content of '%s'", FileUtils::pathToGenericUtf8(file.path).c_str());
            }
        }else if (fs::is_regular_file(file.path, ec) || fs::is_symlink(file.path, ec)){
            if (!FileUtils::removePath(file.path, ec) && checkBytes){
                undoProblem += (undoProblem.empty() ? "" : "; ") + std::string("could not remove ") + FileUtils::pathToGenericUtf8(file.path);
            }
        }
    }
    // the directories the write created, deepest first, when empty again
    std::vector<fs::path> dirs = createdDirs;
    std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b){ return a.native().size() > b.native().size(); });
    for (const fs::path& dir : dirs){
        std::error_code ec;
        if (fs::is_directory(dir, ec) && fs::is_empty(dir, ec)) FileUtils::removePath(dir, ec);
    }
    createdDirs.clear();
}

bool CreateClipCmd::execute(){
    failureText.clear();
    failureMark = safewrite::scopedFailureCount();
    // the files the last undo kept because the user changed them: a redo keeps them too
    const std::vector<fs::path> keptByUndo = keptFiles;
    keptFiles.clear();
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    if (!sceneProject || !sceneProject->scene){
        return fail("the scene is not open");
    }
    wasModified = sceneProject->isModified;
    // a redo (or a step built while a preview ran) changes the player's hierarchy from its rest
    ClipSceneActions::stopPreviewsFor(sceneId, {playerRoot});

    // the files, all or nothing; what is there now is what undo gives back
    written.assign(files.size(), Written());
    createdDirs.clear();
    for (size_t i = 0; i < files.size(); i++){
        const File& file = files[i];
        std::error_code ec;
        const bool exists = fs::is_regular_file(file.path, ec);
        const bool kept = std::find(keptByUndo.begin(), keptByUndo.end(), file.path) != keptByUndo.end();
        // a new file this step created, back with other bytes (a project redo recreated it)
        const bool recreated = executedOnce && i < existedFirst.size() && !existedFirst[i];
        if ((kept || recreated) && exists){
            std::string current;
            if (readWholeFile(file.path, current) && current != file.content){
                // not written over: the edits stay, and the next undo leaves the file alone
                keptFiles.push_back(file.path);
                editor::Out::warning("Kept '%s': it changed since this step wrote it, so redo leaves it as it is",
                                     FileUtils::pathToGenericUtf8(file.path).c_str());
                continue;
            }
        }
        if (executedOnce && !exists){
            const fs::path moved = movedTo(file);
            if (!moved.empty()){
                // renamed or moved since: no second file with its GUID at the old path (the
                // entries this step gives back follow the GUID to it, 9.R8)
                keptFiles.push_back(moved);
                editor::Out::warning("Kept '%s': the clip this step wrote moved there, so redo doesn't write it again at '%s'",
                                     FileUtils::pathToGenericUtf8(moved).c_str(), FileUtils::pathToGenericUtf8(file.path).c_str());
                continue;
            }
        }
        if (!executedOnce){
            if (existedFirst.size() < files.size()) existedFirst.resize(files.size(), false);
            existedFirst[i] = exists;
        }
        written[i].existed = exists;
        if (written[i].existed && !readWholeFile(file.path, written[i].previous)){
            restoreFiles(i, false);
            return fail("can't read " + FileUtils::pathToGenericUtf8(file.path) + " (its content is kept for undo); nothing was written");
        }
        const fs::path parent = file.path.parent_path();
        if (!parent.empty() && !fs::exists(parent, ec)){
            std::vector<fs::path> missing;
            for (fs::path dir = parent; !dir.empty() && !fs::exists(dir, ec); dir = dir.parent_path()){
                missing.push_back(dir);
                if (dir.parent_path() == dir) break;
            }
            FileUtils::createDirectories(parent, ec);
            for (const fs::path& dir : missing){
                std::error_code exists;
                if (fs::is_directory(dir, exists)) createdDirs.push_back(dir);
            }
            if (ec){
                restoreFiles(i, false);
                return fail("can't create the directory " + FileUtils::pathToGenericUtf8(parent) + ": " + ec.message());
            }
        }
        errno = 0;
        if (!FileUtils::writeFileReplacing(file.path, file.content)){
            const int code = errno;
            written[i].done = true;     // possibly half written: put back too
            restoreFiles(i + 1, false);
            return fail("can't write " + FileUtils::pathToGenericUtf8(file.path) + ": " +
                        (code ? std::strerror(code) : std::string("write failed")));
        }
        written[i].done = true;
    }

    // the scene commands, in order; one failing takes back the ones before it and the files
    for (size_t i = 0; i < sceneCommands.size(); i++){
        if (!sceneCommands[i]->execute()){
            for (size_t j = i; j-- > 0;) sceneCommands[j]->undo();
            restoreFiles(files.size(), false);
            if (!files.empty()) notifyFiles();
            sceneProject->isModified = wasModified;
            return fail("the editor refused a scene change of this step (command " + std::to_string(i + 1) + " of " +
                        std::to_string(sceneCommands.size()) + "); nothing was changed");
        }
    }

    // a player applies now: legacy-layout models under it reload in the node hierarchy (R3)
    if (playerRoot != NULL_ENTITY){
        if (!reloadsTaken){
            PlayerModelLayout::ensureNodeHierarchyForPlayer(project, sceneId, playerRoot, layoutReloads);
            reloadsTaken = true;
        }else{
            PlayerModelLayout::redoReloads(layoutReloads);
        }
    }

    sceneProject = project->getScene(sceneId);
    if (sceneProject) sceneProject->isModified = true;
    if (!files.empty()) notifyFiles();
    executedOnce = true;
    return true;
}

fs::path CreateClipCmd::movedTo(const File& file) const{
    const std::string guid = anim::readClipGuid(file.content);
    if (!project || !anim::isClipGuid(guid)) return fs::path();
    const fs::path relative = project->getAssetDatabase().getPath(guid);
    if (relative.empty()) return fs::path();
    const fs::path absolute = (project->getProjectPath() / relative).lexically_normal();
    std::error_code ec;
    if (absolute == file.path.lexically_normal() || !fs::is_regular_file(absolute, ec)) return fs::path();
    return absolute;
}

void CreateClipCmd::undo(){
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    if (!sceneProject){
        return;
    }
    ClipSceneActions::stopPreviewsFor(sceneId, {playerRoot});
    // (keptFiles holds what this execute left alone: still kept)
    PlayerModelLayout::undoReloads(layoutReloads);
    for (size_t i = sceneCommands.size(); i-- > 0;){
        sceneCommands[i]->undo();
    }
    restoreFiles(files.size(), true);
    sceneProject = project->getScene(sceneId);
    if (sceneProject) sceneProject->isModified = wasModified;
    if (!files.empty()) notifyFiles();
}

bool CreateClipCmd::affectsStructure() const{
    // a layout reload rebuilds the model's GameObjects
    if (!layoutReloads.empty()) return true;
    for (const std::unique_ptr<Command>& command : sceneCommands){
        if (command->affectsStructure()) return true;
    }
    return false;
}

std::string editor::CreateClipCmd::describe() const{
    std::vector<std::filesystem::path> paths;
    for (const File& file : files) paths.push_back(file.path);
    return files.size() == 1 ? "Create Animation Clip " + CommandNames::file(paths.front())
                             : "Create " + std::to_string(files.size()) + " Animation Clips";
}

std::vector<Command::Touched> CreateClipCmd::touchedPaths(bool undo) const{
    std::vector<Touched> out;
    for (size_t i = 0; i < files.size(); i++){
        // an undo writes a previous file back or removes a new one (the entry); a redo writes
        const bool existed = i < written.size() && written[i].existed;
        out.push_back({files[i].path, undo && !existed});
    }
    if (undo){
        for (const std::filesystem::path& dir : createdDirs) out.push_back({dir, true});
    }
    return out;
}
