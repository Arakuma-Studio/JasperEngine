// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "UnityProject.h"

#include <fstream>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor::unity;
namespace fs = std::filesystem;

namespace {

    const char* const BUILTIN_MESHES_GUID = "0000000000000000e000000000000000";
    const char* const BUILTIN_RESOURCES_GUID = "0000000000000000f000000000000000";

    // Unity's built-in assets by fileID. These ids are Unity's well known ones (its built-in
    // resource table); they cannot be checked against the game's repository, which only
    // references them, so they are kept here in one place and an id not in it is reported,
    // never guessed.
    struct BuiltinEntry {
        const char* guid;
        int64_t fileID;
        BuiltinAsset asset;
    };

    const BuiltinEntry BUILTINS[] = {
        {BUILTIN_MESHES_GUID, 10202, {BuiltinAsset::Mesh, "Cube", "cube"}},
        {BUILTIN_MESHES_GUID, 10206, {BuiltinAsset::Mesh, "Cylinder", "cylinder"}},
        {BUILTIN_MESHES_GUID, 10207, {BuiltinAsset::Mesh, "Sphere", "sphere"}},
        {BUILTIN_MESHES_GUID, 10208, {BuiltinAsset::Mesh, "Capsule", "capsule"}},
        {BUILTIN_MESHES_GUID, 10209, {BuiltinAsset::Mesh, "Plane", "plane"}},
        {BUILTIN_MESHES_GUID, 10210, {BuiltinAsset::Mesh, "Quad", "quad"}},
        {BUILTIN_RESOURCES_GUID, 10301, {BuiltinAsset::Material, "Default-Particle", ""}},
        {BUILTIN_RESOURCES_GUID, 10306, {BuiltinAsset::Material, "Default-Line", ""}},
        {BUILTIN_RESOURCES_GUID, 10308, {BuiltinAsset::Material, "Default-ParticleSystem", ""}},
        {BUILTIN_RESOURCES_GUID, 10300, {BuiltinAsset::Texture, "Default-Particle", ""}},
        {BUILTIN_RESOURCES_GUID, 200, {BuiltinAsset::Shader, "Legacy Shaders/Particles/Additive", ""}},
        {BUILTIN_RESOURCES_GUID, 10720, {BuiltinAsset::Shader, "Mobile/Particles/Additive", ""}},
        {BUILTIN_RESOURCES_GUID, 10721, {BuiltinAsset::Shader, "Mobile/Particles/Alpha Blended", ""}},
    };

}

fs::path Project::findRoot(const fs::path& file){
    std::error_code ec;
    fs::path dir = fs::absolute(file, ec).parent_path();
    while (!dir.empty()){
        if (fs::is_directory(dir / "Assets", ec) && fs::is_directory(dir / "ProjectSettings", ec)) return dir;
        const fs::path parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return {};
}

bool Project::open(const fs::path& root, std::string& error){
    std::error_code ec;
    projectRoot = fs::absolute(root, ec).lexically_normal();
    guids.clear();
    const fs::path assets = projectRoot / "Assets";
    if (!fs::is_directory(assets, ec)){
        error = "not a Unity project (no Assets/): " + projectRoot.string();
        return false;
    }
    // each .meta's first lines name its guid
    for (fs::recursive_directory_iterator it(assets, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)){
        if (ec) break;
        const fs::path& meta = it->path();
        if (meta.extension() != ".meta") continue;
        std::ifstream in(meta);
        std::string line;
        for (int n = 0; n < 4 && std::getline(in, line); n++){
            if (line.compare(0, 6, "guid: ") == 0){
                std::string guid = line.substr(6);
                while (!guid.empty() && (guid.back() == '\r' || guid.back() == ' ')) guid.pop_back();
                guids[guid] = meta.parent_path() / meta.stem();
                break;
            }
        }
    }
    return true;
}

fs::path Project::pathOf(const std::string& guid) const{
    auto it = guids.find(guid);
    return it == guids.end() ? fs::path() : it->second;
}

std::string Project::relative(const fs::path& path) const{
    return path.lexically_relative(projectRoot).generic_string();
}

YAML::Node Project::metaOf(const fs::path& asset) const{
    std::error_code ec;
    const fs::path meta = asset.string() + ".meta";
    if (!fs::exists(meta, ec)) return YAML::Node();
    try {
        return YAML::LoadFile(meta.string());
    } catch (const YAML::Exception&) {
        return YAML::Node();
    }
}

bool Project::isBuiltinGuid(const std::string& guid){
    return guid == BUILTIN_MESHES_GUID || guid == BUILTIN_RESOURCES_GUID;
}

const BuiltinAsset* Project::builtin(const Ref& ref){
    for (const BuiltinEntry& entry : BUILTINS){
        if (ref.guid == entry.guid && ref.fileID == entry.fileID) return &entry.asset;
    }
    return nullptr;
}
