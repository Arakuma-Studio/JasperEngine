// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The one table that decides which files are assets with an identity (a GUID), where the
// identity is stored and which root their references are relative to. Nothing else
// decides tracking (docs/specs/S04-asset-identity.md §6.1).

#pragma once

#include "asset/AssetType.h"

#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {

    // Engine-authored YAML assets keep a top-level `guid:` key in the file (an .anim clip: the
    // top-level "guid" member of its JSON); every other asset gets a `<file name>.meta` sidecar
    // next to it
    enum class AssetIdentity { InFile, Sidecar };

    // The root a reference's path hint is relative to. Each reference field keeps the root it
    // has always used (Lua script entries use the Lua directory)
    enum class AssetPathBase { Project, Assets, Lua };

    struct AssetTypeInfo {
        AssetType type;
        AssetIdentity identity;
        AssetPathBase base;
        bool hasSubAssets;
        // data files are assets only inside the assets directory
        bool assetsDirOnly;
    };

    class AssetTypes {
    public:
        static constexpr const char* metaExtension = ".meta";

        // The type of a project file, nullptr when it is not a tracked asset.
        // projectRelative is the path relative to the project root (generic or native),
        // assetsDir the project's assets directory relative to the project root ("" = root).
        static const AssetTypeInfo* find(const std::filesystem::path& projectRelative, const std::filesystem::path& assetsDir);
        static const AssetTypeInfo& info(AssetType type);

        static bool isMetaFile(const std::filesystem::path& path);
        // "hero.png" -> "hero.png.meta"
        static std::filesystem::path metaPathFor(const std::filesystem::path& assetPath);
        // "hero.png.meta" -> "hero.png"
        static std::filesystem::path assetPathForMeta(const std::filesystem::path& metaPath);

        // A directory below the project the asset scan never enters: names starting with '.'
        // (.jasperengine, .trash, .git) and build/, the set Project::findProjectFiles skips
        static bool isSkippedDirectory(const std::string& name);
        // A directory that is one asset (T4's Name.terrainworld): the scan takes it whole and does not
        // enter it, so its files have no .meta
        static bool isFolderAsset(const std::string& name);
        // The file an in-file identity lives in: the asset, or a folder asset's manifest.yaml
        static std::filesystem::path identityFileOf(const std::filesystem::path& assetPath);
        // Files with a tracked extension that are not assets: project.yaml and the files the
        // editor generates in the project root (CMakeLists.txt, ProjectBuild.cmake, ...)
        static bool isExcludedFile(const std::filesystem::path& projectRelative);

        // The extensions of a type, for docs and filters
        static std::vector<std::string> extensions(AssetType type);
    };

}
