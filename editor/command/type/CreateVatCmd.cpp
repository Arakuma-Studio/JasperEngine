// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CreateVatCmd.h"

#include "Out.h"
#include "Project.h"
#include "animation/AnimationClipDocument.h"
#include "animation/ClipEditorUtil.h"
#include "command/type/AddComponentCmd.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/EntityStateCmd.h"
#include "command/type/ModelLoadCmd.h"
#include "command/type/PropertyCmd.h"
#include "util/FileUtils.h"
#include "util/SafeWrite.h"
#include "util/Util.h"

#include "pool/VertexAnimationPool.h"

#include <algorithm>
#include <cerrno>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

CreateVatCmd::CreateVatCmd(Project* project, uint32_t sceneId, std::vector<File> files, VatPlacement placement):
        project(project), sceneId(sceneId), files(std::move(files)), placement(std::move(placement)){
}

CreateVatCmd::~CreateVatCmd(){
}

bool CreateVatCmd::fail(const std::string& message){
    // a write the write scope refused during this execute: its reason too (never an older one)
    failureText = message;
    const std::string scoped = safewrite::scopedFailureSince(failureMark);
    if (!scoped.empty() && message.find(scoped) == std::string::npos) failureText += " (" + scoped + ")";
    if (failureSink) *failureSink = message;
    return false;
}

void CreateVatCmd::notifyFiles(Project* project, const std::vector<fs::path>& paths){
    if (!project || paths.empty()) return;
    for (const fs::path& path : paths){
        if (Util::isVertexAnimationFile(FileUtils::pathToUtf8(path))){
            VertexAnimationPool::invalidate(FileUtils::pathToUtf8(path));
        }
    }
    AnimationClipDocument::filesWritten(project, paths);
}

void CreateVatCmd::notify(){
    std::vector<fs::path> paths;
    for (const File& file : files) paths.push_back(file.path);
    notifyFiles(project, paths);
}

void CreateVatCmd::restoreFiles(size_t count, bool checkBytes){
    for (size_t i = count; i-- > 0;){
        if (i >= written.size() || !written[i].done) continue;
        const File& file = files[i];
        written[i].done = false;
        std::error_code ec;
        if (checkBytes){
            std::string current;
            const bool exists = fs::is_regular_file(file.path, ec);
            if (!exists || !clipui::readWholeFile(file.path, current) || current != file.content){
                if (exists){
                    keptFiles.push_back(file.path);
                    Out::warning("Kept '%s': it changed after the bake wrote it, so undo leaves it as it is",
                                 FileUtils::pathToGenericUtf8(file.path).c_str());
                }
                continue;
            }
        }
        if (written[i].existed){
            if (!FileUtils::writeFileReplacing(file.path, written[i].previous)){
                if (checkBytes) undoProblem += (undoProblem.empty() ? "" : "; ") + std::string("could not put back ") + FileUtils::pathToGenericUtf8(file.path);
                Out::warning("Could not put back the previous content of '%s'", FileUtils::pathToGenericUtf8(file.path).c_str());
            }
        }else if (fs::is_regular_file(file.path, ec) || fs::is_symlink(file.path, ec)){
            if (!FileUtils::removePath(file.path, ec) && checkBytes){
                undoProblem += (undoProblem.empty() ? "" : "; ") + std::string("could not remove ") + FileUtils::pathToGenericUtf8(file.path);
            }
        }
    }
    std::vector<fs::path> dirs = createdDirs;
    std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b){ return a.native().size() > b.native().size(); });
    for (const fs::path& dir : dirs){
        std::error_code ec;
        if (fs::is_directory(dir, ec) && fs::is_empty(dir, ec)) FileUtils::removePath(dir, ec);
    }
    createdDirs.clear();
}

bool CreateVatCmd::writeFiles(){
    const std::vector<fs::path> keptByUndo = keptFiles;
    keptFiles.clear();
    written.assign(files.size(), Written());
    createdDirs.clear();
    for (size_t i = 0; i < files.size(); i++){
        const File& file = files[i];
        std::error_code ec;
        const bool exists = fs::is_regular_file(file.path, ec);
        const bool kept = std::find(keptByUndo.begin(), keptByUndo.end(), file.path) != keptByUndo.end();
        const bool recreated = executedOnce && i < existedFirst.size() && !existedFirst[i];
        if ((kept || recreated) && exists){
            std::string current;
            if (clipui::readWholeFile(file.path, current) && current != file.content){
                keptFiles.push_back(file.path);
                Out::warning("Kept '%s': it changed since the bake wrote it, so redo leaves it as it is",
                             FileUtils::pathToGenericUtf8(file.path).c_str());
                continue;
            }
        }
        if (!executedOnce){
            if (existedFirst.size() < files.size()) existedFirst.resize(files.size(), false);
            existedFirst[i] = exists;
        }
        written[i].existed = exists;
        if (exists && !clipui::readWholeFile(file.path, written[i].previous)){
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
            written[i].done = true;
            restoreFiles(i + 1, false);
            return fail("can't write " + FileUtils::pathToGenericUtf8(file.path) + ": " +
                        (code ? std::strerror(code) : std::string("write failed")));
        }
        written[i].done = true;
    }
    return true;
}

bool CreateVatCmd::buildAndRun(){
    // takes `command` (sceneCommands owns it from here: a command that fails is deleted here,
    // never by the caller)
    auto run = [&](Command* command) -> bool{
        sceneCommands.emplace_back(command);
        if (command->execute()) return true;
        sceneCommands.pop_back();
        return false;
    };
    auto rollback = [&](const std::string& message){
        for (size_t j = sceneCommands.size(); j-- > 0;) sceneCommands[j]->undo();
        sceneCommands.clear();
        entity = NULL_ENTITY;
        return fail(message);
    };
    if (placement.mode == VatPlacement::Mode::None) return true;

    auto* create = placement.parent == NULL_ENTITY
                       ? new CreateEntityCmd(project, sceneId, placement.name, EntityCreationType::MODEL)
                       : new CreateEntityCmd(project, sceneId, placement.name, EntityCreationType::MODEL, placement.parent);
    create->setQuiet(true);
    create->addProperty<Vector3>(ComponentType::Transform, "position", placement.position);
    create->addProperty<Quaternion>(ComponentType::Transform, "rotation", placement.rotation);
    create->addProperty<Vector3>(ComponentType::Transform, "scale", placement.scale);
    if (!run(create)){
        return rollback("could not create the GameObject '" + placement.name + "'");
    }
    entity = create->getEntity();
    auto* load = new ModelLoadCmd(project, sceneId, entity, placement.model);
    if (!run(load)){
        return rollback("could not load '" + placement.model + "' into '" + placement.name + "'");
    }
    auto* add = new AddComponentCmd(project, sceneId, entity, ComponentType::VertexAnimatorComponent);
    if (!run(add)){
        return rollback("could not add the Vertex Animator");
    }
    auto* animation = new PropertyCmd<std::string>(project, sceneId, entity, ComponentType::VertexAnimatorComponent, "animation",
                                                   placement.animation);
    if (!run(animation)){
        return rollback("could not set the Vertex Animator's animation");
    }
    if (!placement.guid.empty()){
        auto* guid = new PropertyCmd<std::string>(project, sceneId, entity, ComponentType::VertexAnimatorComponent, "guid", placement.guid);
        if (!run(guid)){
            return rollback("could not set the Vertex Animator's GUID");
        }
    }
    if (placement.mode == VatPlacement::Mode::Replace && placement.source != NULL_ENTITY){
        EntityStateValue inactive;
        inactive.active = false;
        auto* disable = new EntityStateCmd(project, sceneId, {placement.source}, EntityStateField::Active, inactive);
        if (!run(disable)){
            return rollback("could not disable the source GameObject");
        }
    }
    if (placement.mode == VatPlacement::Mode::Crowd){
        if (!placement.crowd){
            return rollback("crowd placement is not available yet");
        }
        std::vector<Command*> commands;
        std::string error;
        if (!placement.crowd(project, sceneId, entity, commands, error)){
            for (Command* command : commands) delete command;
            return rollback(error.empty() ? std::string("the crowd could not be built") : error);
        }
        for (size_t i = 0; i < commands.size(); i++){
            if (!run(commands[i])){
                // commands[i] went with run(); the ones after it never ran
                for (size_t j = i + 1; j < commands.size(); j++) delete commands[j];
                return rollback("the editor refused a crowd change (command " + std::to_string(i + 1) + ")");
            }
        }
    }
    built = true;
    return true;
}

bool CreateVatCmd::execute(){
    failureText.clear();
    failureMark = safewrite::scopedFailureCount();
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    if (!sceneProject || !sceneProject->scene){
        return fail("the scene is not open");
    }
    wasModified = sceneProject->isModified;
    previousSelection = sceneProject->selectedEntities;

    if (!writeFiles()) return false;

    if (!built){
        if (!buildAndRun()){
            restoreFiles(files.size(), false);
            notify();
            sceneProject->isModified = wasModified;
            return false;
        }
    }else{
        for (size_t i = 0; i < sceneCommands.size(); i++){
            if (!sceneCommands[i]->execute()){
                for (size_t j = i; j-- > 0;) sceneCommands[j]->undo();
                restoreFiles(files.size(), false);
                notify();
                sceneProject->isModified = wasModified;
                return fail("the editor refused a scene change of this step (command " + std::to_string(i + 1) + " of " +
                            std::to_string(sceneCommands.size()) + "); nothing was changed");
            }
        }
    }

    sceneProject = project->getScene(sceneId);
    if (sceneProject){
        sceneProject->isModified = true;
        if (entity != NULL_ENTITY) project->replaceSelectedEntities(sceneId, {entity});
    }
    notify();
    executedOnce = true;
    return true;
}

void CreateVatCmd::undo(){
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    if (!sceneProject) return;
    for (size_t i = sceneCommands.size(); i-- > 0;){
        sceneCommands[i]->undo();
    }
    restoreFiles(files.size(), true);
    sceneProject = project->getScene(sceneId);
    if (sceneProject){
        sceneProject->isModified = wasModified;
        if (entity != NULL_ENTITY) project->replaceSelectedEntities(sceneId, previousSelection);
    }
    notify();
}

bool CreateVatCmd::affectsStructure() const{
    return placement.mode != VatPlacement::Mode::None;
}

std::vector<Command::Touched> CreateVatCmd::touchedPaths(bool undo) const{
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
