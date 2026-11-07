// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"

#include "yaml-cpp/yaml.h"

#include <filesystem>
#include <functional>
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
        // the undo step's verb when the key alone does not say what was done
        std::string action;

        std::filesystem::path metaPath() const;
        bool write(const YAML::Node& value, bool removeFileIfEmpty);
        bool writeFile(const YAML::Node& value, bool removeFileIfEmpty);

    public:
        SetAssetMetaCmd(Project* project, const std::filesystem::path& asset, std::string key, YAML::Node value);

        // The sidecar of an asset ("textures/a.png" -> "textures/a.png.meta")
        static std::filesystem::path metaPathOf(const std::filesystem::path& absoluteAsset);
        // The key's value in an asset's sidecar (null when absent)
        static YAML::Node read(const std::filesystem::path& absoluteAsset, const std::string& key);

        // Told after every write, undo included (the texture and model cook queues requeue what
        // an import-settings edit changed): the project, the project-relative asset and the key.
        // Any number of listeners, each under its owner's key (S53 R14): adding one under a key
        // that has one replaces it, and removing a key leaves the others alone.
        using WriteListener = std::function<void(Project* project, const std::filesystem::path& asset, const std::string& key)>;
        static void addWriteListener(const void* owner, WriteListener listener);
        static void removeWriteListener(const void* owner);

        bool execute() override;
        void undo() override;
        bool affectsStructure() const override { return false; }

        // "Set Import Compression (a.png)", or "<action> (a.png)" once setAction gave one
        std::string describe() const override;
        void setAction(std::string name) { action = std::move(name); }
        // the project-relative asset whose sidecar it writes (lock-on-edit, S29 R12)
        const std::filesystem::path& getAsset() const { return asset; }
        const char* typeName() const override { return "SetAssetMetaCmd"; }
        bool isNoOp() const override;
        size_t memoryBytes() const override;
    };

}
