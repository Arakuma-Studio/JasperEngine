// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "RenameFileCmd.h"

#include "util/SafeWrite.h"

#include "util/FileUtils.h"

#include "util/Util.h"
#include "asset/AssetOps.h"
#include "Out.h"
#include "vat/VatAssetFiles.h"
#include "vat/VatDocument.h"

#include <fstream>
#include <iterator>

using namespace jasperengine;

namespace {

    std::string readText(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

}

editor::RenameFileCmd::RenameFileCmd(Project* project, std::string oldName, std::string newName, std::string directory){
    this->project = project;
    this->oldFilename = fs::path(oldName).filename();
    this->newFilename = fs::path(newName).filename();
    this->directory = fs::path(directory);
}

bool editor::RenameFileCmd::renameFile(const fs::path& sourceFs, const fs::path& destFs){
    if (!project) {
        return false;
    }
    const AssetOps::Result result = AssetOps::move(*project, sourceFs, destFs);
    if (!result.ok) {
        Out::error("Couldn't rename \"%s\": %s", sourceFs.filename().string().c_str(), result.error.c_str());
        return false;
    }
    for (const AssetOps::TextReference& broken : result.textReferencesBroken) {
        Out::warning("%s:%u still refers to %s", broken.referrer.c_str(), broken.line, broken.target.c_str());
    }
    return true;
}

std::vector<editor::Command::Touched> editor::RenameFileCmd::touchedPaths(bool) const{
    std::vector<Touched> out = {{directory / oldFilename, true}, {directory / newFilename, true}};
    if (!pairOldName.empty()) {
        out.push_back({directory / pairOldName, true});
        out.push_back({directory / pairNewName, true});
    }
    // the renamed .vat's mesh field, written back
    if (!meshFieldFile.empty()) out.push_back({meshFieldFile, false});
    return out;
}

bool editor::RenameFileCmd::execute(){
    const fs::path sourceFs = directory / oldFilename;
    const fs::path destFs = directory / newFilename;

    // a Vertex Animation's other file goes too; its new name must be free
    if (!pairDecided) {
        pairDecided = true;
        fs::path pairOld, pairNew;
        if (vat::renamedPair(sourceFs, destFs, pairOld, pairNew)) {
            pairOldName = pairOld.filename();
            pairNewName = pairNew.filename();
        }
    }
    const bool withPair = !pairOldName.empty();
    std::error_code ec;
    // a pair member keeps its extension (the other file couldn't follow)
    const std::string refusal = vat::pairRenameRefusal(sourceFs, destFs, oldFilename.string());
    if (!refusal.empty()) {
        Out::error("Couldn't rename \"%s\": %s", oldFilename.string().c_str(), refusal.c_str());
        return false;
    }
    if (withPair && fs::exists(directory / pairNewName, ec) && fs::exists(directory / pairOldName, ec)) {
        Out::error("Couldn't rename \"%s\": %s already exists (a Vertex Animation's .vat and .vat.glb are renamed together)",
                   oldFilename.string().c_str(), pairNewName.string().c_str());
        return false;
    }

    if (!renameFile(sourceFs, destFs)) {
        return false;
    }
    // the pair is renamed whole or not at all: the other file failing puts the first one back
    if (withPair && !renameFile(directory / pairOldName, directory / pairNewName)) {
        Out::error("\"%s\" keeps its name too", oldFilename.string().c_str());
        renameFile(destFs, sourceFs);
        return false;
    }

    // the renamed .vat names its renamed mesh (the payload is the same: no rebake)
    meshFieldFile.clear();
    if (withPair) {
        const fs::path vatFile = Util::isVertexAnimationFile(destFs.string()) ? destFs : directory / pairNewName;
        const std::string text = readText(vatFile);
        std::string updated = text;
        if (vat::setMeshField(updated, vat::pairPathOf(vatFile).filename().generic_string()) &&
            FileUtils::writeFileReplacing(vatFile, updated)) {
            meshFieldFile = vatFile;
            meshFieldBefore = text;
            meshFieldAfter = updated;
            // the database learns the new size and time (same GUID), the documents reload
            project->importPrefabFiles(vatFile);
            VatDocument::filesChanged({vatFile}, project);
        }
    }

    // clips previewing the renamed model (or taken from it) name it again, in this step
    clipHintEdits = remapClipHints(project, sourceFs, destFs);
    return true;
}

void editor::RenameFileCmd::undo(){
    const fs::path sourceFs = directory / newFilename;
    const fs::path destFs = directory / oldFilename;
    restoreClipHints(project, clipHintEdits);
    clipHintEdits.clear();
    if (!meshFieldFile.empty()) {
        // edited since (an Inspector edit undone separately keeps its own text)
        if (readText(meshFieldFile) == meshFieldAfter) {
            FileUtils::writeFileReplacing(meshFieldFile, meshFieldBefore);
        }
        meshFieldFile.clear();
    }
    renameFile(sourceFs, destFs);
    if (!pairOldName.empty()) {
        renameFile(directory / pairNewName, directory / pairOldName);
    }
    if (project && !pairOldName.empty()) {
        const fs::path vatFile = Util::isVertexAnimationFile(destFs.string()) ? destFs : directory / pairOldName;
        project->importPrefabFiles(vatFile);
        VatDocument::filesChanged({vatFile}, project);
    }
}
