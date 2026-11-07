// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CopyFileCmd.h"
#include "util/FileUtils.h"
#include "io/AtomicFile.h"
#include "util/SafeWrite.h"

#include "util/Util.h"
#include "asset/AssetOps.h"
#include "asset/AssetFileWrite.h"
#include "project/SceneFileIdentity.h"
#include "Out.h"
#include "vat/VatAssetFiles.h"
#include "vat/VatDocument.h"

#include <algorithm>
#include <fstream>
#include <iterator>

using namespace jasperengine;

// A Vertex Animation moves and copies as a pair (vertex animation spec 2.1): the .vat or the
// .vat.glb brings the other file of its directory along (same name: the copied .vat still names
// its mesh, and gets a GUID of its own)
void editor::CopyFileCmd::addVatPairs(){
    const size_t count = files.size();
    for (size_t i = 0; i < count; i++) {
        const FileCopyData fdata = files[i];
        const fs::path pair = vat::existingPairOf(fdata.sourceDirectory / fdata.filename);
        if (pair.empty()) {
            continue;
        }
        const bool listed = std::any_of(files.begin(), files.end(), [&](const FileCopyData& other) {
            return (other.sourceDirectory / other.filename).lexically_normal() == pair.lexically_normal();
        });
        if (!listed) {
            FileCopyData added = fdata;
            added.filename = pair.filename();
            files.push_back(added);
        }
    }
}

editor::CopyFileCmd::CopyFileCmd(Project* project, std::vector<std::string> sourceFiles, std::string currentDirectory, std::string targetDirectory, bool copy){
    this->project = project;
    for (const auto& sourceFile : sourceFiles) {
        FileCopyData fdata;
        fdata.filename = fs::path(sourceFile).filename();
        fdata.sourceDirectory = fs::path(currentDirectory);
        fdata.targetDirectory = fs::path(targetDirectory);

        this->files.push_back(fdata);
    }
    this->copy = copy;
    addVatPairs();
}

editor::CopyFileCmd::CopyFileCmd(Project* project, std::vector<std::string> sourcePaths, std::string targetDirectory, bool copy){
    this->project = project;
    for (const auto& sourcePath : sourcePaths) {
        FileCopyData fdata;
        fdata.filename = fs::path(sourcePath).filename();
        fdata.sourceDirectory = fs::path(sourcePath).remove_filename();
        fdata.targetDirectory = fs::path(targetDirectory);

        this->files.push_back(fdata);
    }
    this->copy = copy;
    addVatPairs();
}

editor::CopyFileCmd::CopyFileCmd(Project* project, std::vector<FileCopyData> entries, bool copy){
    this->project = project;
    this->files = std::move(entries);
    this->copy = copy;
    addVatPairs();
}

namespace {

    // source's bytes replace target's atomically: copied next to it, then published (S03)
    bool replaceFile(const fs::path& source, const fs::path& target, std::string& error) {
        std::error_code ec;
        // inside a write scope (util/SafeWrite.h): one copy beneath its root, never through a
        // link planted on the way
        if (const editor::FileUtils::ScopedOps* ops = editor::FileUtils::scopedOps()) {
            const int scoped = ops->copy(source, target, true, ec);
            if (scoped == 0) {
                error = editor::safewrite::lastScopedError().empty() ? ec.message() : editor::safewrite::lastScopedError();
                return false;
            }
            if (scoped == 1) return true;
            ec.clear();
        }
        const fs::path temp = io::makeTemporarySiblingPath(target);
        if (!fs::copy_file(source, temp, ec) || ec) {
            fs::remove(temp, ec);
            error = ec ? ec.message() : "copy failed";
            return false;
        }
        const io::WriteResult published = io::publishFile(temp, target);
        if (!published.ok()) {
            error = io::describeWriteError(published);
            return false;
        }
        return true;
    }

    // Unity's duplicate naming: "hero.png" -> "hero 1.png", "hero 2.png", ... A Vertex
    // Animation's file is named with its pair: "walk.vat" and "walk.vat.glb" -> "walk 1.vat" and
    // "walk 1.vat.glb", both free, so the copies are a pair again (vertex animation spec 2.1)
    // Unity numbers on: the copy of "hero 1" is "hero 2", not "hero 1 1" (the base, and the first
    // number to try)
    std::pair<std::string, int> numberedBase(const std::string& stem) {
        const size_t space = stem.find_last_of(' ');
        if (space == std::string::npos || space == 0 || space + 1 >= stem.size() || stem.size() - space - 1 > 9) {
            return {stem, 1};
        }
        for (size_t i = space + 1; i < stem.size(); i++) {
            if (stem[i] < '0' || stem[i] > '9') return {stem, 1};
        }
        return {stem.substr(0, space), std::stoi(stem.substr(space + 1)) + 1};
    }

    fs::path freeCopyName(const fs::path& target) {
        std::error_code ec;
        const fs::path pair = editor::vat::pairPathOf(target);
        if (!pair.empty()) {
            const bool isMesh = editor::vat::isVatMeshFile(editor::FileUtils::pathToUtf8(target));
            const fs::path vatFile = isMesh ? pair : target;
            if (!fs::exists(vatFile, ec) && !fs::exists(editor::vat::pairPathOf(vatFile), ec)) {
                return target;
            }
            const auto [stem, first] = numberedBase(vatFile.stem().string());
            for (int counter = first;; counter++) {
                const fs::path candidate = vatFile.parent_path() / (stem + " " + std::to_string(counter) + vatFile.extension().string());
                const fs::path candidateMesh = editor::vat::pairPathOf(candidate);
                if (!fs::exists(candidate, ec) && !fs::exists(candidateMesh, ec)) {
                    return isMesh ? candidateMesh : candidate;
                }
            }
        }
        if (!fs::exists(target, ec)) {
            return target;
        }
        // a folder's name is all name: "my.folder" -> "my.folder 1"
        const bool directory = fs::is_directory(target, ec);
        const auto [stem, first] = numberedBase(directory ? target.filename().string() : target.stem().string());
        const std::string extension = directory ? std::string() : target.extension().string();
        for (int counter = first;; counter++) {
            const fs::path candidate = target.parent_path() / (stem + " " + std::to_string(counter) + extension);
            if (!fs::exists(candidate, ec)) {
                return candidate;
            }
        }
    }

}

fs::path editor::CopyFileCmd::freeName(const fs::path& target){
    return freeCopyName(target);
}

std::vector<editor::Command::Touched> editor::CopyFileCmd::touchedPaths(bool undo) const{
    std::vector<Touched> out;
    for (const auto& fdata : files) {
        const fs::path source = fdata.sourceDirectory / fdata.filename;
        const fs::path destination = fdata.destination.empty() ? fdata.targetDirectory / fdata.filename : fdata.destination;
        // a copy: its destination made (redo) or removed (undo); a move: both entries renamed;
        // an overwrite's backup (in the project's trash) is written back on the undo
        if (!copy) out.push_back({source, true});
        out.push_back({destination, true});
        if (undo && !fdata.backup.empty()) out.push_back({fdata.backup, true});
    }
    return out;
}

fs::path editor::CopyFileCmd::destinationFor(const FileCopyData& fdata) const{
    const fs::path source = (fdata.sourceDirectory / fdata.filename).lexically_normal();
    const fs::path target = (fdata.targetDirectory / fdata.filename).lexically_normal();
    if (!copy && source == target) {
        return fs::path();
    }
    return freeCopyName(target);
}

std::string editor::CopyFileCmd::refusal() const{
    for (const FileCopyData& fdata : files) {
        const fs::path source = fdata.sourceDirectory / fdata.filename;
        const fs::path target = fdata.targetDirectory / fdata.filename;
        std::string why = copy ? AssetOps::copyRefusal(source, target) : AssetOps::moveRefusal(source, target);
        if (!why.empty()) {
            return why;
        }
    }
    return std::string();
}

bool editor::CopyFileCmd::fail(std::string error){
    lastError = std::move(error);
    if (report) report->error = lastError;
    return false;
}

void editor::CopyFileCmd::beginReport(){
    lastError.clear();
    if (report) *report = Report();
    for (FileCopyData& fdata : files) {
        fdata.went = false;
    }
}

bool editor::CopyFileCmd::isNoOp() const{
    return std::none_of(files.begin(), files.end(), [](const FileCopyData& fdata) { return fdata.went; });
}

std::string editor::CopyFileCmd::describe() const{
    std::vector<fs::path> names;
    for (const FileCopyData& file : files) {
        names.push_back(file.filename);
    }
    const char* verb = duplicate ? "Duplicate " : copy ? "Copy " : "Move ";
    std::string name = std::string(verb) + (files.size() == 1 ? "File " : "") + CommandNames::files(names);
    // a move that took a free name says so
    if (!copy && files.size() == 1 && !files[0].destination.empty() && files[0].destination.filename() != files[0].filename) {
        name += " as " + FileUtils::pathToUtf8(files[0].destination.filename());
    }
    return name;
}

// Moves and copies go through AssetOps: files and metas together, GUIDs kept by moves and
// new (the same again on a redo) for copies, references refreshed in memory
// Without a project (file operations of a folder that is none: the tests of the write scope),
// the files alone: copied or moved under a free name, in the write scope beneath its root
bool editor::CopyFileCmd::executeFiles(){
    beginReport();
    if (std::string why = refusal(); !why.empty()) {
        Out::error("%s", why.c_str());
        return fail(why);
    }
    for (size_t i = 0; i < files.size(); i++) {
        FileCopyData& fdata = files[i];
        const fs::path source = fdata.sourceDirectory / fdata.filename;
        std::error_code ec;
        if (!fs::exists(source, ec)) {
            if (report) report->gone++;
            continue;
        }
        if (fdata.destination.empty()) fdata.destination = destinationFor(fdata);
        if (fdata.destination.empty()) {
            continue;   // moved into the folder it is in
        }
        bool done;
        if (copy) {
            done = FileUtils::copyPath(source, fdata.destination, false, ec);
        } else {
            done = fs::exists(fdata.destination, ec) ? false : FileUtils::renamePath(source, fdata.destination, ec);
        }
        if (!done) {
            const std::string why = !safewrite::lastScopedError().empty() ? safewrite::lastScopedError()
                                  : ec ? ec.message() : fdata.destination.generic_string() + " already exists";
            Out::error("Couldn't %s \"%s\": %s", copy ? "copy" : "move", fdata.filename.string().c_str(), why.c_str());
            for (size_t back = i; back-- > 0;) {
                const FileCopyData& earlier = files[back];
                if (!earlier.went) continue;
                std::error_code ignored;
                if (copy) FileUtils::removeAll(earlier.destination, ignored);
                else FileUtils::renamePath(earlier.destination, earlier.sourceDirectory / earlier.filename, ignored);
            }
            return fail("Couldn't " + std::string(copy ? "copy " : "move ") + FileUtils::pathToUtf8(fdata.filename) + ": " + why);
        }
        fdata.went = true;
        if (report) report->landed.push_back(fdata.destination);
    }
    return true;
}

void editor::CopyFileCmd::undoFiles(){
    for (auto it = files.rbegin(); it != files.rend(); ++it) {
        const FileCopyData& fdata = *it;
        std::error_code ec;
        if (!fdata.went || fdata.destination.empty() || !fs::exists(fdata.destination, ec)) {
            continue;
        }
        const bool done = copy ? FileUtils::removeAll(fdata.destination, ec)
                               : FileUtils::renamePath(fdata.destination, fdata.sourceDirectory / fdata.filename, ec);
        if (!done) {
            Out::error("Couldn't undo the %s of \"%s\": %s", copy ? "copy" : "move", fdata.filename.string().c_str(),
                       safewrite::lastScopedError().empty() ? ec.message().c_str() : safewrite::lastScopedError().c_str());
        }
    }
}

bool editor::CopyFileCmd::execute(){
    if (!project) {
        return executeFiles();
    }
    beginReport();
    // a folder into itself (or a copy making a second project.yaml) refuses the whole step
    if (std::string why = refusal(); !why.empty()) {
        Out::error("%s", why.c_str());
        return fail(why);
    }
    // what went (moved or copied): the Vertex Animation meshes among it name their textures again
    std::vector<std::pair<fs::path, fs::path>> went;
    for (size_t i = 0; i < files.size(); i++) {
        FileCopyData& fdata = files[i];
        const fs::path source = fdata.sourceDirectory / fdata.filename;
        std::error_code ec;
        if (!fs::exists(source, ec)) {
            if (report) report->gone++;
            continue;
        }
        AssetOps::Result result;
        const fs::path target = fdata.targetDirectory / fdata.filename;
        if (copy && overwrite && fs::is_regular_file(target, ec) && fs::is_regular_file(source, ec)) {
            // replaced in place: the file keeps its meta and GUID, so its references still find it
            fdata.destination = target;
            fdata.backup = AssetOps::uniqueTrashPath(project->getProjectPath() / ".trash", target);
            // the editor's own .trash (the deny-by-default guard of an MCP call's scope)
            safewrite::EditorInternal ownTrash(fdata.backup.parent_path());
            FileUtils::createDirectories(fdata.backup.parent_path(), ec);
            // a scene replaced in place stays the scene it was: its id and name, as its GUID
            uint32_t targetSceneId = 0;
            std::string targetSceneName;
            bool keepSceneIdentity = false;
            if (Util::isSceneFile(FileUtils::pathToUtf8(target))) {
                std::string text;
                keepSceneIdentity = assetio::readFile(target, text) && sceneid::readSceneHeader(text, targetSceneId, targetSceneName);
            }
            // the backup path is new (uniqueTrashPath): nothing is overwritten
            std::string error;
            if (!FileUtils::copyPath(target, fdata.backup, false, ec)) {
                error = ec ? ec.message() : "backup failed";
            } else if (!replaceFile(source, target, error)) {
                FileUtils::removePath(fdata.backup, ec);
            } else if (keepSceneIdentity && !sceneid::rewriteSceneHeader(target, targetSceneId, targetSceneName, &error)) {
                std::string ignored;
                replaceFile(fdata.backup, target, ignored);
                FileUtils::removePath(fdata.backup, ec);
            }
            if (!error.empty()) {
                fdata.backup.clear();
                Out::error("Couldn't replace \"%s\": %s", fdata.filename.string().c_str(), error.c_str());
                return fail("Couldn't replace " + FileUtils::pathToUtf8(fdata.filename) + ": " + error);
            }
            project->onAssetFilesChanged(project->getAssetDatabase().importAssets(target));
            went.emplace_back(source, target);
            fdata.went = true;
            if (report) report->landed.push_back(target);
            continue;
        }
        if (fdata.destination.empty()) {
            fdata.destination = destinationFor(fdata);
            // the other file of a Vertex Animation pair in this step takes the same free name
            const fs::path pair = vat::pairPathOf(source);
            for (FileCopyData& other : files) {
                if (!pair.empty() && !fdata.destination.empty() && other.destination.empty() &&
                    (other.sourceDirectory / other.filename).lexically_normal() == pair.lexically_normal()) {
                    other.destination = vat::pairPathOf(fdata.destination);
                }
            }
        }
        if (fdata.destination.empty()) {
            continue;   // moved into the folder it is in: nothing to do
        }
        if (!copy && fdata.destination.filename() != fdata.filename) {
            Out::info("Moved %s as %s (the name was taken)", FileUtils::pathToUtf8(fdata.filename).c_str(),
                      FileUtils::pathToUtf8(fdata.destination.filename()).c_str());
        }
        result = copy ? AssetOps::copy(*project, source, fdata.destination, &copyGuids, &copyScenes)
                      : AssetOps::move(*project, source, fdata.destination);
        if (!result.ok) {
            Out::error("Couldn't %s \"%s\": %s", copy ? "copy" : "move", fdata.filename.string().c_str(), result.error.c_str());
            // what was done goes back, so a failed command leaves nothing half done
            for (size_t back = i; back-- > 0;) {
                const FileCopyData& done = files[back];
                if (!done.went) continue;
                if (copy) {
                    AssetOps::removeCopy(*project, done.destination);
                } else {
                    AssetOps::move(*project, done.destination, done.sourceDirectory / done.filename);
                }
            }
            return fail("Couldn't " + std::string(copy ? "copy " : "move ") + FileUtils::pathToUtf8(fdata.filename) + ": " + result.error);
        }
        fdata.went = true;
        if (report) report->landed.push_back(fdata.destination);
        went.emplace_back(source, fdata.destination);
    }

    // a .vat.glb in another directory names the same texture files (or the copies made with it);
    // a move's undo writes the old URIs back
    vatUriMoves.clear();
    if (!copy) {
        vatUriMoves = went;
    }
    vatUriEdits = vat::rebaseMovedVatMeshes(went);
    // a .vat copied or moved under a free name ("walk 1.vat") names the mesh that went with it;
    // a move's undo writes the old text back
    vatMeshFieldEdits.clear();
    for (const FileCopyData& fdata : files) {
        const std::string destination = FileUtils::pathToUtf8(fdata.destination);
        if (!fdata.went || !Util::isVertexAnimationFile(destination) || fdata.destination.filename() == fdata.filename) {
            continue;
        }
        std::ifstream file(fdata.destination, std::ios::binary);
        const std::string original((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();
        std::string text = original;
        if (vat::setMeshField(text, vat::pairPathOf(fdata.destination).filename().generic_string()) &&
            FileUtils::writeFileReplacing(fdata.destination, text)) {
            if (!copy) vatMeshFieldEdits.emplace_back(fdata.destination, original);
            project->importPrefabFiles(fdata.destination);
            VatDocument::filesChanged({fdata.destination}, project);
        }
    }

    // clips previewing a moved model (or taken from it) name it again, in this step (every file
    // first: a clip moved in the same step is at its final place when its hint is rewritten)
    if (!copy) {
        for (const FileCopyData& fdata : files) {
            if (!fdata.went) {
                continue;
            }
            std::vector<ClipHintEdit> edits = remapClipHints(project, fdata.sourceDirectory / fdata.filename, fdata.destination);
            clipHintEdits.insert(clipHintEdits.end(), edits.begin(), edits.end());
        }
    }
    return true;
}

void editor::CopyFileCmd::undo(){
    if (!project) {
        undoFiles();
        return;
    }
    restoreClipHints(project, clipHintEdits);
    clipHintEdits.clear();
    // a moved .vat that took a free name names its old mesh again before it goes back
    for (const auto& [path, original] : vatMeshFieldEdits) {
        if (FileUtils::writeFileReplacing(path, original)) {
            VatDocument::filesChanged({path}, project);
        }
    }
    vatMeshFieldEdits.clear();
    for (auto it = files.rbegin(); it != files.rend(); ++it) {
        FileCopyData& fdata = *it;
        std::error_code ec;
        if (!fdata.went || fdata.destination.empty() || !fs::exists(fdata.destination, ec)) {
            continue;
        }
        if (!fdata.backup.empty()) {
            // the replaced content comes back, the file kept its identity throughout
            std::string error;
            safewrite::EditorInternal ownTrash(fdata.backup.parent_path());   // read back out of .trash
            if (!replaceFile(fdata.backup, fdata.destination, error)) {
                Out::error("Couldn't restore \"%s\": %s", fdata.filename.string().c_str(), error.c_str());
                continue;
            }
            FileUtils::removePath(fdata.backup, ec);
            fdata.backup.clear();
            project->onAssetFilesChanged(project->getAssetDatabase().importAssets(fdata.destination));
            continue;
        }
        // instances of a copied prefab made since become Missing Prefabs
        const AssetOps::Result result = copy ? AssetOps::removeCopy(*project, fdata.destination)
                                             : AssetOps::move(*project, fdata.destination, fdata.sourceDirectory / fdata.filename);
        if (!result.ok) {
            Out::error("Couldn't undo the %s of \"%s\": %s", copy ? "copy" : "move", fdata.filename.string().c_str(), result.error.c_str());
        }
    }

    // the moved meshes back where they were name their textures as before (copies are gone)
    if (!copy) {
        vat::restoreVatMeshUris(vatUriEdits, vatUriMoves);
    }
    vatUriEdits.clear();
    vatUriMoves.clear();
}
