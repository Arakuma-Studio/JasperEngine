// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CreatePrefabVariantAssetCmd.h"

#include "Out.h"
#include "util/FileUtils.h"

#include <system_error>

using namespace jasperengine;

editor::CreatePrefabVariantAssetCmd::CreatePrefabVariantAssetCmd(Project* project, const std::filesystem::path& base, const std::filesystem::path& filepath) {
    this->project = project;
    this->base = base.is_absolute() ? project->normalizeToProjectRelative(base) : base.lexically_normal();
    this->filepath = filepath.empty() ? defaultPath(project, this->base) :
        (filepath.is_absolute() ? project->normalizeToProjectRelative(filepath) : filepath.lexically_normal());
}

std::filesystem::path editor::CreatePrefabVariantAssetCmd::defaultPath(const Project* project, const std::filesystem::path& base) {
    const std::filesystem::path directory = base.parent_path();
    const std::string stem = base.stem().string() + " Variant";
    std::filesystem::path candidate = directory / (stem + ".prefab");
    std::error_code ec;
    for (int counter = 1; std::filesystem::exists(project->getProjectPath() / candidate, ec) && counter < 10000; counter++) {
        candidate = directory / (stem + " " + std::to_string(counter) + ".prefab");
    }
    return candidate;
}

bool editor::CreatePrefabVariantAssetCmd::execute() {
    created = project->createPrefabVariantAsset(base, filepath, guid);
    if (created) {
        guid = project->getPrefabGuid(filepath);
    }
    return created;
}

void editor::CreatePrefabVariantAssetCmd::undo() {
    if (!created) {
        return;
    }
    // the file it made, wherever it was moved since
    if (!guid.empty()) {
        const std::filesystem::path current = project->resolvePrefabPath(guid, filepath);
        if (!current.empty()) {
            filepath = current;
        }
    }
    const std::filesystem::path fullPath = project->getProjectPath() / filepath;
    std::error_code ec;
    // the entry itself (in a write scope: beneath its root)
    if (!FileUtils::removePath(fullPath, ec) && std::filesystem::exists(std::filesystem::symlink_status(fullPath, ec)))
        undoProblem = "could not remove " + FileUtils::pathToGenericUtf8(fullPath);
    // like deleting it: an instance made since goes Missing (and comes back with a redo)
    project->onAssetFilesChanged(project->getAssetDatabase().removeAssets(fullPath));
    created = false;
}

std::vector<editor::Command::Touched> editor::CreatePrefabVariantAssetCmd::touchedPaths(bool undo) const {
    // where the variant is now (moved since: its GUID knows), as the undo will find it
    std::filesystem::path current = filepath;
    if (!guid.empty()) {
        const std::filesystem::path moved = project->resolvePrefabPath(guid, filepath);
        if (!moved.empty()) current = moved;
    }
    return {{project->getProjectPath() / current, undo}};
}
