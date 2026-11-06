// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AssetTypes.h"

#include <algorithm>
#include <cctype>

using namespace jasperengine;
namespace fs = std::filesystem;

namespace {

    struct Row {
        editor::AssetTypeInfo info;
        std::vector<std::string> extensions;
    };

    using editor::AssetIdentity;
    using editor::AssetPathBase;

    // docs/specs/S04-asset-identity.md §6.1
    const std::vector<Row>& rows() {
        static const std::vector<Row> table = {
            {{AssetType::Prefab, AssetIdentity::InFile, AssetPathBase::Project, false, false}, {".prefab"}},
            {{AssetType::Scene, AssetIdentity::InFile, AssetPathBase::Project, false, false}, {".scene"}},
            {{AssetType::Material, AssetIdentity::InFile, AssetPathBase::Project, false, false}, {".material"}},
            {{AssetType::Texture, AssetIdentity::Sidecar, AssetPathBase::Assets, false, false},
                {".png", ".jpg", ".jpeg", ".bmp", ".tga", ".gif", ".hdr", ".psd", ".pic", ".pnm", ".svg"}},
            {{AssetType::Model, AssetIdentity::Sidecar, AssetPathBase::Assets, true, false}, {".gltf", ".glb", ".obj", ".fbx"}},
            {{AssetType::Audio, AssetIdentity::Sidecar, AssetPathBase::Assets, false, false}, {".wav", ".ogg", ".mp3", ".flac"}},
            {{AssetType::Font, AssetIdentity::Sidecar, AssetPathBase::Assets, false, false}, {".ttf", ".otf", ".ttc"}},
            {{AssetType::UIDocument, AssetIdentity::Sidecar, AssetPathBase::Assets, false, false}, {".html", ".htm", ".rml"}},
            {{AssetType::Stylesheet, AssetIdentity::Sidecar, AssetPathBase::Assets, false, false}, {".css", ".rcss"}},
            {{AssetType::Shader, AssetIdentity::Sidecar, AssetPathBase::Project, false, false}, {".vert", ".frag", ".glsl"}},
            {{AssetType::Script, AssetIdentity::Sidecar, AssetPathBase::Project, false, false},
                {".lua", ".cpp", ".cc", ".cxx", ".h", ".hh", ".hpp", ".hxx"}},
            {{AssetType::Data, AssetIdentity::Sidecar, AssetPathBase::Assets, false, true}, {".json", ".txt", ".csv", ".xml", ".bytes", ".inputactions"}},
            {{AssetType::ColorLut, AssetIdentity::Sidecar, AssetPathBase::Assets, false, false}, {".cube"}},
            {{AssetType::ParticleEffect, AssetIdentity::Sidecar, AssetPathBase::Assets, false, false}, {".particles"}},
            // animation clips keep their GUID in their own JSON ("guid", AnimationClipIO), as the
            // engine's YAML assets do; pose animations have a meta like other data files
            {{AssetType::AnimationClip, AssetIdentity::InFile, AssetPathBase::Assets, false, false}, {".anim"}},
            {{AssetType::PoseAnimation, AssetIdentity::Sidecar, AssetPathBase::Assets, false, false}, {".poseanim"}},
            // Vertex Animations as clips (their JSON "guid"); the generated "<x>.vat.glb" beside one
            // is a model with a meta like any other
            {{AssetType::VertexAnimation, AssetIdentity::InFile, AssetPathBase::Assets, false, false}, {".vat"}},
            {{AssetType::NavMesh, AssetIdentity::Sidecar, AssetPathBase::Assets, false, false}, {".navmesh"}},
            {{AssetType::NavGrid, AssetIdentity::Sidecar, AssetPathBase::Assets, false, false}, {".navgrid"}},
            // the Animator's assets (S21) keep their GUID in their JSON like clips
            {{AssetType::AnimatorController, AssetIdentity::InFile, AssetPathBase::Assets, false, false}, {".controller"}},
            {{AssetType::AvatarMask, AssetIdentity::InFile, AssetPathBase::Assets, false, false}, {".mask"}},
            {{AssetType::AnimationMontage, AssetIdentity::InFile, AssetPathBase::Assets, false, false}, {".montage"}},
            // Sound Events (S37): the file holds no GUID, its .meta does
            {{AssetType::SoundEvent, AssetIdentity::Sidecar, AssetPathBase::Assets, false, false}, {".soundevent"}},
            // Title Art documents (S80) keep their GUID in their JSON; their baked PNGs are Textures
            {{AssetType::TitleArt, AssetIdentity::InFile, AssetPathBase::Assets, false, false}, {".titleart"}},
        };
        return table;
    }

    std::string assetTypeExtension(const fs::path& path) {
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return ext;
    }

    bool isBelow(const fs::path& projectRelative, const fs::path& directory) {
        if (directory.empty() || directory == ".") {
            return true;
        }
        const fs::path relative = projectRelative.lexically_normal().lexically_relative(directory.lexically_normal());
        return !relative.empty() && *relative.begin() != "..";
    }

}

const editor::AssetTypeInfo* editor::AssetTypes::find(const fs::path& projectRelative, const fs::path& assetsDir) {
    if (projectRelative.empty() || isMetaFile(projectRelative) || isExcludedFile(projectRelative)) {
        return nullptr;
    }
    const std::string ext = assetTypeExtension(projectRelative);
    if (ext.empty()) {
        return nullptr;
    }
    for (const Row& row : rows()) {
        if (std::find(row.extensions.begin(), row.extensions.end(), ext) == row.extensions.end()) {
            continue;
        }
        if (row.info.assetsDirOnly && !isBelow(projectRelative, assetsDir)) {
            return nullptr;
        }
        return &row.info;
    }
    return nullptr;
}

const editor::AssetTypeInfo& editor::AssetTypes::info(AssetType type) {
    for (const Row& row : rows()) {
        if (row.info.type == type) {
            return row.info;
        }
    }
    static const AssetTypeInfo unknown{AssetType::Unknown, AssetIdentity::Sidecar, AssetPathBase::Project, false, false};
    return unknown;
}

bool editor::AssetTypes::isMetaFile(const fs::path& path) {
    return assetTypeExtension(path) == metaExtension;
}

fs::path editor::AssetTypes::metaPathFor(const fs::path& assetPath) {
    fs::path meta = assetPath;
    meta += metaExtension;
    return meta;
}

fs::path editor::AssetTypes::assetPathForMeta(const fs::path& metaPath) {
    fs::path asset = metaPath;
    asset.replace_extension();
    return asset;
}

bool editor::AssetTypes::isSkippedDirectory(const std::string& name) {
    return name.rfind('.', 0) == 0 || name == "build";
}

bool editor::AssetTypes::isExcludedFile(const fs::path& projectRelative) {
    const fs::path normal = projectRelative.lexically_normal();
    const std::string name = normal.filename().string();
    if (name.rfind('.', 0) == 0) {
        return true; // hidden files, the S03 atomic-write temporaries among them
    }
    // In the project root only: what the editor generates or owns there
    if (normal.has_parent_path() && normal.parent_path() != ".") {
        return false;
    }
    return name == "project.yaml" || name == "CMakeLists.txt" || name == "ProjectBuild.cmake" ||
           name == "compile_commands.json";
}

std::vector<std::string> editor::AssetTypes::extensions(AssetType type) {
    for (const Row& row : rows()) {
        if (row.info.type == type) {
            return row.extensions;
        }
    }
    return {};
}
