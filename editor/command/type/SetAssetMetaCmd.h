// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"

#include "yaml-cpp/yaml.h"

#include <filesystem>
#include <string>

namespace jasperengine::editor{

    class Project;

    // Sets one key of an asset's metadata sidecar (<asset>.meta, YAML: import settings, the
    // asset's GUID and labels) as a step of the project history (S24 R22; S14 import settings
    // and S04 asset identity write through it). `key` is a dotted path ("import.compression");
    // a null value removes the key. Undo writes the old value back, removing the key (and the
    // sidecar, when it did not exist) if there was none.
    class SetAssetMetaCmd: public Command{
    private:
        Project* project;
        std::filesystem::path asset;    // project-relative
        std::string key;
        YAML::Node newValue;
        YAML::Node oldValue;            // null: the key was absent
        bool fileExisted = false;
        bool captured = false;

        std::filesystem::path metaPath() const;
        bool write(const YAML::Node& value, bool removeFileIfEmpty);

    public:
        SetAssetMetaCmd(Project* project, const std::filesystem::path& asset, std::string key, YAML::Node value);

        // The sidecar of an asset ("textures/a.png" -> "textures/a.png.meta")
        static std::filesystem::path metaPathOf(const std::filesystem::path& absoluteAsset);
        // The key's value in an asset's sidecar (null when absent)
        static YAML::Node read(const std::filesystem::path& absoluteAsset, const std::string& key);

        bool execute() override;
        void undo() override;
        bool affectsStructure() const override { return false; }

        // "Set Import Compression (a.png)"
        std::string describe() const override;
        const char* typeName() const override { return "SetAssetMetaCmd"; }
        bool isNoOp() const override;
        size_t memoryBytes() const override;
    };

}
