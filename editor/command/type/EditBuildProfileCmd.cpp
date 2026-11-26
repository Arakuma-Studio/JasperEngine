// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "EditBuildProfileCmd.h"

#include "buildprofile/BuildProfile.h"
#include "io/AtomicFile.h"
#include "util/FileUtils.h"

#include <utility>

using namespace jasperengine;

namespace fs = std::filesystem;

editor::EditBuildProfileCmd::EditBuildProfileCmd(std::string stepName, std::vector<FileChange> changes)
    : stepName(std::move(stepName)), changes(std::move(changes)) {
}

bool editor::EditBuildProfileCmd::apply(bool forward){
    lastFailure.clear();
    // Forward runs the changes in order and undo in reverse, so two changes naming one file (a
    // rename that only changes the letter case, on a case-insensitive filesystem) leave it as
    // the step meant: undo removes the new name first, then writes the old one
    const size_t count = changes.size();
    auto at = [&](size_t k) -> const FileChange& { return changes[forward ? k : count - 1 - k]; };
    // each file as the step leaves it; a failure puts the ones before it back
    for (size_t i = 0; i < count; i++){
        const std::optional<std::string>& wanted = forward ? at(i).after : at(i).before;
        std::string why;
        if (wanted){
            const io::WriteResult written = io::writeFileAtomic(at(i).path, *wanted);
            if (!written.ok()) why = io::describeWriteError(written);
        } else {
            std::error_code ec;
            fs::remove(at(i).path, ec);
            if (ec) why = "cannot remove " + FileUtils::pathToGenericUtf8(at(i).path) + ": " + ec.message();
        }
        if (why.empty()) continue;
        lastFailure = why;
        if (failureSink) *failureSink = why;
        for (size_t j = i; j-- > 0;){
            const std::optional<std::string>& previous = forward ? at(j).before : at(j).after;
            std::error_code ec;
            if (previous) io::writeFileAtomic(at(j).path, *previous);
            else fs::remove(at(j).path, ec);
        }
        buildprofiles::touch();
        return false;
    }
    if (activeName){
        std::string why;
        if (!buildprofiles::setActiveName(activeName->projectPath, forward ? activeName->after : activeName->before, why)){
            lastFailure = why;
            if (failureSink) *failureSink = why;
        }
    }
    buildprofiles::touch();
    return true;
}

void editor::EditBuildProfileCmd::setActiveNameChange(std::filesystem::path projectPath, std::string before, std::string after){
    activeName = ActiveNameChange{std::move(projectPath), std::move(before), std::move(after)};
}

bool editor::EditBuildProfileCmd::execute(){
    return apply(true);
}

void editor::EditBuildProfileCmd::undo(){
    if (!apply(false)) undoProblem = lastFailure;
}

bool editor::EditBuildProfileCmd::mergeWith(Command* previousCommand){
    auto* previous = dynamic_cast<EditBuildProfileCmd*>(previousCommand);
    if (!previous || mergeKey.empty() || previous->mergeKey != mergeKey || previous->changes.size() != changes.size()) return false;
    for (size_t i = 0; i < changes.size(); i++)
        if (previous->changes[i].path != changes[i].path) return false;
    // the merged step undoes to what the files held before the first edit
    for (size_t i = 0; i < changes.size(); i++) previous->changes[i].after = changes[i].after;
    previous->stepName = stepName;
    return true;
}

bool editor::EditBuildProfileCmd::isNoOp() const{
    if (activeName && activeName->before != activeName->after) return false;
    for (const FileChange& change : changes)
        if (change.before != change.after) return false;
    return true;
}

size_t editor::EditBuildProfileCmd::memoryBytes() const{
    size_t bytes = sizeof(*this) + stepName.size();
    for (const FileChange& change : changes)
        bytes += sizeof(change) + (change.before ? change.before->size() : 0) + (change.after ? change.after->size() : 0);
    return bytes;
}

std::vector<editor::Command::Touched> editor::EditBuildProfileCmd::touchedPaths(bool undo) const{
    std::vector<Touched> touched;
    for (const FileChange& change : changes){
        const std::optional<std::string>& wanted = undo ? change.before : change.after;
        touched.push_back({change.path, !wanted.has_value()});
    }
    return touched;
}
