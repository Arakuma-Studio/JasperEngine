// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "FogProfileAssetCmd.h"

#include "DocumentFile.h"
#include "FogProfileAssets.h"
#include "Out.h"
#include "Project.h"
#include "asset/AssetTypes.h"
#include "command/CommandNames.h"
#include "render/FogProfile.h"
#include "util/AssetDatabase.h"
#include "util/FileUtils.h"

#include <system_error>

using namespace jasperengine;

namespace fs = std::filesystem;

editor::FogProfileAssetCmd::FogProfileAssetCmd(Project* project, std::string path, std::string before, std::string after, Guid guid)
    : project(project), path(FogProfileLibrary::normalizeKey(path)), before(std::move(before)), after(std::move(after)), guid(guid) {
}

editor::FogProfileAssetCmd* editor::FogProfileAssetCmd::edit(Project* project, const std::string& path, const FogProfileData& before,
                                                             const FogProfileData& after) {
    std::string beforeText = FogProfileIO::write(before);
    std::string afterText = FogProfileIO::write(after);
    if (beforeText == afterText) return nullptr;
    return new FogProfileAssetCmd(project, path, std::move(beforeText), std::move(afterText));
}

editor::FogProfileAssetCmd* editor::FogProfileAssetCmd::create(Project* project, const std::string& path, const FogProfileData& data) {
    return new FogProfileAssetCmd(project, path, std::string(), FogProfileIO::write(data), Guid::parse(AssetDatabase::generateGuid()));
}

std::string editor::FogProfileAssetCmd::freePath(const Project& project, const std::string& folder, const std::string& name) {
    std::string base = folder;
    while (!base.empty() && (base.back() == '/' || base.back() == '\\')) base.pop_back();
    if (!base.empty()) base += "/";
    const std::string stem = name.empty() ? std::string("New Fog Profile") : name;
    std::error_code ec;
    std::string candidate = base + stem + FogProfileIO::EXTENSION;
    for (int i = 1; fs::exists(project.getAssetsPath() / FileUtils::pathFromUtf8(candidate), ec); i++)
        candidate = base + stem + " " + std::to_string(i) + FogProfileIO::EXTENSION;
    return candidate;
}

fs::path editor::FogProfileAssetCmd::file() const {
    return project->getAssetsPath() / FileUtils::pathFromUtf8(path);
}

bool editor::FogProfileAssetCmd::apply(const std::string& text, bool undoing) {
    const fs::path target = file();
    std::error_code ec;
    if (text.empty()) {
        // undoing the creation: the file and its meta go, the asset leaves the database, and
        // every user reads it as missing again (R6)
        const fs::path relative = project->getAssetDatabase().toRelative(target);
        fs::remove(target, ec);
        fs::remove(project->getProjectPath() / AssetTypes::metaPathFor(relative.generic_string()), ec);
        project->getAssetDatabase().removeAssets(target);
        if (createdFolder && fs::is_empty(target.parent_path(), ec)) fs::remove(target.parent_path(), ec);
        fogprofiles::profileChanged(*project, path);
        return true;
    }

    const bool created = !fs::exists(target, ec);
    if (created && !undoing && before.empty()) createdFolder = !fs::exists(target.parent_path(), ec);
    const io::WriteResult written = DocumentFiles::write(target, text);
    if (!written.ok()) {
        Out::error("%s", DocumentFiles::saveFailureMessage(target, written).c_str());
        return false;
    }
    if (created && !guid.isNull()) {
        project->getAssetDatabase().registerAsset(target, guid.str());
    }

    FogProfileData data;
    if (FogProfileIO::parse(text, data)) {
        fogprofiles::profileWritten(*project, path, data);
    } else {
        fogprofiles::profileChanged(*project, path);
    }
    return true;
}

bool editor::FogProfileAssetCmd::execute() {
    return apply(after, false);
}

void editor::FogProfileAssetCmd::undo() {
    if (!apply(before, true)) undoProblem = "Could not write " + path + " back";
}

bool editor::FogProfileAssetCmd::mergeWith(Command* olderCommand) {
    // the history merges a newer command into the step before it: this one takes over its state
    FogProfileAssetCmd* older = dynamic_cast<FogProfileAssetCmd*>(olderCommand);
    if (!older || older->project != project || older->path != path || older->before.empty() || before.empty()) {
        return false;
    }
    before = older->before;
    return true;
}

std::vector<editor::Command::Touched> editor::FogProfileAssetCmd::touchedPaths(bool undo) const {
    std::vector<Touched> out;
    // an undone creation removes the file (the entry) and its meta, anything else writes bytes
    const bool removes = undo && before.empty();
    out.push_back({file(), removes});
    if (removes) {
        const fs::path relative = project->getAssetDatabase().toRelative(file());
        out.push_back({project->getProjectPath() / AssetTypes::metaPathFor(relative.generic_string()), true});
    }
    return out;
}

std::string editor::FogProfileAssetCmd::describe() const {
    return (before.empty() ? "Create Fog Profile " : "Edit Fog Profile ") + CommandNames::file(path);
}
