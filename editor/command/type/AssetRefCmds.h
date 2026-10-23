// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Missing Assets actions (S04 R16), on the project history: Retarget All (a former GUID
// added to the chosen asset, so every reference to the missing one resolves to it, no
// referrer rewritten), Retarget Selected and Clear (the chosen fields rewritten).

#pragma once

#include "command/Command.h"
#include "asset/AssetRefEditor.h"
#include "asset/Guid.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    // What a command did, for the caller (MCP): it outlives a command the history drops
    struct AssetRefCmdReport {
        std::string error;
        std::vector<std::string> filesWritten;
    };

    class AddFormerGuidCmd : public Command {
    public:
        AddFormerGuidCmd(Project* project, std::filesystem::path assetPath, const Guid& formerGuid,
                         std::shared_ptr<AssetRefCmdReport> report = nullptr);
        bool execute() override;
        void undo() override;
        bool affectsStructure() const override { return false; }

    private:
        Project* project;
        std::filesystem::path assetPath;
        Guid formerGuid;
        std::shared_ptr<AssetRefCmdReport> report;
    };

    class RetargetAssetRefsCmd : public Command {
    public:
        // newRef: project-relative path + GUID; empty clears the fields
        RetargetAssetRefsCmd(Project* project, std::vector<AssetRefTarget> targets, AssetRef newRef,
                             std::shared_ptr<AssetRefCmdReport> report = nullptr);
        bool execute() override;
        void undo() override;
        bool affectsStructure() const override { return false; }

    private:
        Project* project;
        std::vector<AssetRefTarget> targets;
        AssetRef newRef;
        std::vector<AssetRefEditor::Undo> undoData;
        std::shared_ptr<AssetRefCmdReport> report;
    };

}
