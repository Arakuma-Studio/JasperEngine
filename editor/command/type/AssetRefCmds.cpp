// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AssetRefCmds.h"

#include "Project.h"
#include "asset/AssetTypes.h"

using namespace jasperengine;
using namespace jasperengine::editor;

AddFormerGuidCmd::AddFormerGuidCmd(Project* project, std::filesystem::path assetPath, const Guid& formerGuid,
                                   std::shared_ptr<AssetRefCmdReport> report)
    : project(project), assetPath(std::move(assetPath)), formerGuid(formerGuid),
      report(report ? std::move(report) : std::make_shared<AssetRefCmdReport>()) {
}

bool AddFormerGuidCmd::execute() {
    report->error.clear();
    report->filesWritten.clear();
    if (!project->getAssetDatabase().addFormerGuid(assetPath, formerGuid, &report->error)) {
        return false;
    }
    // the asset's identity is its meta, or the guid lines of a scene, prefab or material
    const std::string relative = project->normalizeToProjectRelative(assetPath).generic_string();
    const AssetTypeInfo* info = AssetTypes::find(relative, project->getAssetRoots().assetsDir);
    report->filesWritten.push_back(info && info->identity == AssetIdentity::Sidecar
        ? AssetTypes::metaPathFor(relative).generic_string() : relative);
    // references to the former GUID resolve to the asset now: the loaded ones take its path
    project->refreshAssetHints();
    return true;
}

void AddFormerGuidCmd::undo() {
    std::string ignored;
    project->getAssetDatabase().removeFormerGuid(assetPath, formerGuid, &ignored);
    project->refreshAssetHints();
}

RetargetAssetRefsCmd::RetargetAssetRefsCmd(Project* project, std::vector<AssetRefTarget> targets, AssetRef newRef,
                                           std::shared_ptr<AssetRefCmdReport> report)
    : project(project), targets(std::move(targets)), newRef(std::move(newRef)),
      report(report ? std::move(report) : std::make_shared<AssetRefCmdReport>()) {
}

bool RetargetAssetRefsCmd::execute() {
    report->error.clear();
    report->filesWritten.clear();
    undoData.clear();
    return AssetRefEditor::apply(*project, targets, newRef, undoData, report->filesWritten, report->error);
}

void RetargetAssetRefsCmd::undo() {
    AssetRefEditor::revert(*project, undoData);
    undoData.clear();
}
