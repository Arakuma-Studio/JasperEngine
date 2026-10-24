// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "util/FbxImporter.h"

#include "util/FileUtils.h"
#include "util/GlbBuilder.h"
#include "util/Util.h"

#include "ufbx.h"
#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace jasperengine::editor {

namespace {

// Bump when the output changes, to redo existing imports
constexpr int IMPORTER_VERSION = 1;
constexpr const char* IMPORT_EXTRAS_KEY = "jasperengineFbxImport";

constexpr size_t MAX_UFBX_WARNINGS = 10;

struct FileStamp {
    uintmax_t size = 0;
    fs::file_time_type time;
    std::string hash;
};

struct Dependency {
    std::string path; // relative to the FBX when possible
    std::string hash;
    bool linked = false; // referenced by URI, not embedded
};

// A texture the FBX names and where it was found, empty when nowhere
struct TextureEntry {
    GlbBuilder::TextureRef texture;
    std::string path;
};

struct ImportInfo {
    int version = 0;
    std::vector<Dependency> files; // the FBX first
    std::vector<TextureEntry> textures;
};

struct CachedInfo {
    uintmax_t size = 0;
    fs::file_time_type time;
    ImportInfo info;
};

std::mutex importMutex;
std::mutex cacheMutex;
std::unordered_map<std::string, FileStamp> fileStamps;
std::unordered_map<std::string, CachedInfo> importInfos;

// FNV-1a, since mtimes do not survive a git clone
std::string hashContents(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    uint64_t hash = 1469598103934665603ull;
    std::vector<char> chunk(1 << 20);
    while (file) {
        file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const std::streamsize count = file.gcount();
        for (std::streamsize i = 0; i < count; i++) {
            hash ^= static_cast<unsigned char>(chunk[static_cast<size_t>(i)]);
            hash *= 1099511628211ull;
        }
    }
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
    return text;
}

// Cached until the file's size or mtime changes
std::string hashFile(const fs::path& path) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(path, ec);
    if (ec) {
        return {};
    }
    const fs::file_time_type time = fs::last_write_time(path, ec);
    if (ec) {
        return {};
    }

    const std::string key = FileUtils::pathToGenericUtf8(path.lexically_normal());
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        auto it = fileStamps.find(key);
        if (it != fileStamps.end() && it->second.size == size && it->second.time == time) {
            return it->second.hash;
        }
    }

    std::string hash = hashContents(path);
    if (!hash.empty()) {
        std::lock_guard<std::mutex> lock(cacheMutex);
        fileStamps[key] = {size, time, hash};
    }
    return hash;
}

bool readGlbJson(const fs::path& path, Json& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    // magic, version, length, then the first chunk's length and type
    uint32_t header[5];
    if (!file.read(reinterpret_cast<char*>(header), sizeof(header))) {
        return false;
    }
    if (header[0] != 0x46546C67u || header[1] != 2 || header[4] != 0x4E4F534Au) {
        return false;
    }
    std::string text(header[3], '\0');
    if (!file.read(text.data(), static_cast<std::streamsize>(text.size()))) {
        return false;
    }
    out = Json::parse(text, nullptr, false);
    return !out.is_discarded();
}

ImportInfo readImportInfo(const fs::path& glbPath) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(glbPath, ec);
    if (ec) {
        return {};
    }
    const fs::file_time_type time = fs::last_write_time(glbPath, ec);
    if (ec) {
        return {};
    }

    const std::string key = FileUtils::pathToGenericUtf8(glbPath.lexically_normal());
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        auto it = importInfos.find(key);
        if (it != importInfos.end() && it->second.size == size && it->second.time == time) {
            return it->second.info;
        }
    }

    ImportInfo info;
    Json doc;
    if (readGlbJson(glbPath, doc)) {
        // A file without the marker, or a damaged one, reads as no import
        try {
            const Json& data = doc.at("asset").at("extras").at(IMPORT_EXTRAS_KEY);
            info.version = data.value("version", 0);
            for (const Json& entry : data.value("files", Json::array())) {
                Dependency dependency;
                dependency.path = entry.at("path").get<std::string>();
                dependency.hash = entry.value("hash", "");
                dependency.linked = entry.value("linked", false);
                info.files.push_back(dependency);
            }
            for (const Json& entry : data.value("textures", Json::array())) {
                info.textures.push_back({{entry.value("filename", ""), entry.value("relative", ""), entry.value("absolute", "")},
                    entry.value("path", "")});
            }
        } catch (const Json::exception&) {
            info = ImportInfo();
        }
    }

    std::lock_guard<std::mutex> lock(cacheMutex);
    importInfos[key] = {size, time, info};
    return info;
}

// The marker of a loaded model
ImportInfo readImportInfo(const tinygltf::Model& model) {
    ImportInfo info;
    auto text = [](const tinygltf::Value& object, const char* key) {
        return object.Has(key) && object.Get(key).IsString() ? object.Get(key).Get<std::string>() : std::string();
    };
    if (!model.asset.extras.Has(IMPORT_EXTRAS_KEY) || !model.asset.extras.Get(IMPORT_EXTRAS_KEY).Has("files")) {
        return info;
    }
    const tinygltf::Value& data = model.asset.extras.Get(IMPORT_EXTRAS_KEY);
    if (data.Has("version")) {
        info.version = data.Get("version").GetNumberAsInt();
    }
    const tinygltf::Value& files = data.Get("files");
    for (size_t i = 0; i < files.ArrayLen(); i++) {
        const tinygltf::Value& entry = files.Get(i);
        Dependency dependency;
        dependency.path = text(entry, "path");
        dependency.hash = text(entry, "hash");
        dependency.linked = entry.Has("linked") && entry.Get("linked").IsBool() && entry.Get("linked").Get<bool>();
        info.files.push_back(dependency);
    }
    return info;
}

// Written aside and renamed, so a reader never sees half a file
bool writeFile(const fs::path& path, const std::vector<unsigned char>& bytes, std::string& error) {
    fs::path tempPath = path;
    tempPath += ".tmp";
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    {
        std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
        if (!out || !out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
            error = "Could not write " + FileUtils::pathToUtf8(tempPath);
            out.close();
            fs::remove(tempPath, ec);
            return false;
        }
    }
    // Windows cannot replace a file that a reader or virus scan briefly holds
    for (int attempt = 0; attempt < 10; attempt++) {
        ec.clear();
        fs::rename(tempPath, path, ec);
        if (!ec) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    error = "Could not write " + FileUtils::pathToUtf8(path) + ": " + ec.message();
    fs::remove(tempPath, ec);
    return false;
}

fs::path resolveDependency(const fs::path& fbxPath, const std::string& path) {
    fs::path file = FileUtils::pathFromUtf8(path);
    if (file.is_relative()) {
        file = fbxPath.parent_path() / file;
    }
    return file.lexically_normal();
}

std::string makeSourceId(int version, const std::string& hash) {
    return hash.empty() ? std::string() : std::to_string(version) + "-" + hash;
}

// The marker that ties a .glb to the files it was imported from
tinygltf::Value describeImport(const GlbBuilder& builder, const fs::path& fbxPath, const fs::path& assetsRoot) {
    const fs::path fbxDir = fbxPath.parent_path().lexically_normal();
    // Relative to the FBX when part of the project
    auto pathOf = [&](const fs::path& path) {
        const fs::path normal = path.lexically_normal();
        const bool relative = Util::isInsidePath(normal, fbxDir) || Util::isInsidePath(normal, assetsRoot);
        return FileUtils::pathToGenericUtf8(relative ? normal.lexically_relative(fbxDir) : normal);
    };
    auto describe = [&](const fs::path& path) {
        tinygltf::Value::Object entry;
        entry["path"] = tinygltf::Value(pathOf(path));
        entry["hash"] = tinygltf::Value(hashFile(path.lexically_normal()));
        if (builder.isLinked(path)) {
            entry["linked"] = tinygltf::Value(true);
        }
        return tinygltf::Value(entry);
    };

    tinygltf::Value::Array files = {describe(fbxPath)};
    for (const fs::path& dependency : builder.getDependencies()) {
        files.push_back(describe(dependency));
    }
    tinygltf::Value::Array textures;
    for (const GlbBuilder::TextureLookup& lookup : builder.getTextureLookups()) {
        textures.emplace_back(tinygltf::Value::Object{{"filename", tinygltf::Value(lookup.texture.filename)},
            {"relative", tinygltf::Value(lookup.texture.relative)}, {"absolute", tinygltf::Value(lookup.texture.absolute)},
            {"path", tinygltf::Value(lookup.found.empty() ? std::string() : pathOf(lookup.found))}});
    }
    tinygltf::Value::Object marker;
    marker["version"] = tinygltf::Value(IMPORTER_VERSION);
    marker["files"] = tinygltf::Value(files);
    if (!textures.empty()) {
        marker["textures"] = tinygltf::Value(textures);
    }
    return tinygltf::Value(tinygltf::Value::Object{{IMPORT_EXTRAS_KEY, tinygltf::Value(marker)}});
}

} // namespace

bool FbxImporter::isFbxFile(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".fbx";
}

FbxImporter::State FbxImporter::getState(const fs::path& fbxPath, const fs::path& glbPath, const fs::path& assetsRoot) {
    std::error_code ec;
    if (!fs::exists(glbPath, ec)) {
        return State::Missing;
    }

    const ImportInfo info = readImportInfo(glbPath);
    if (info.files.empty() || info.version != IMPORTER_VERSION) {
        return State::Stale;
    }
    for (const Dependency& file : info.files) {
        const std::string hash = hashFile(resolveDependency(fbxPath, file.path));
        // A deleted file keeps its last import: a reimport could only drop it
        if (!hash.empty() && hash != file.hash) {
            return State::Stale;
        }
    }
    // A texture added since, or a copy the search now prefers
    for (const TextureEntry& entry : info.textures) {
        const fs::path found = GlbBuilder::findTextureFile(entry.texture, fbxPath, assetsRoot);
        if (!found.empty() && (entry.path.empty() || found != resolveDependency(fbxPath, entry.path))) {
            return State::Stale;
        }
    }
    return State::UpToDate;
}

std::string FbxImporter::getSourceId(const fs::path& fbxPath) {
    return makeSourceId(IMPORTER_VERSION, hashFile(fbxPath));
}

std::string FbxImporter::getImportId(const tinygltf::Model& model) {
    const ImportInfo info = readImportInfo(model);
    return info.files.empty() ? std::string() : makeSourceId(info.version, info.files.front().hash);
}

bool FbxImporter::isCurrentImport(const tinygltf::Model& model, const fs::path& glbPath) {
    const ImportInfo loaded = readImportInfo(model);
    const ImportInfo current = readImportInfo(glbPath);
    return !loaded.files.empty() && loaded.version == current.version && loaded.files.size() == current.files.size() &&
        std::equal(loaded.files.begin(), loaded.files.end(), current.files.begin(), [](const Dependency& a, const Dependency& b) {
            return a.path == b.path && a.hash == b.hash && a.linked == b.linked;
        });
}

std::vector<fs::path> FbxImporter::findMissingLinkedFiles(const fs::path& fbxPath, const fs::path& glbPath) {
    std::vector<fs::path> missing;
    for (const Dependency& file : readImportInfo(glbPath).files) {
        std::error_code ec;
        const fs::path path = resolveDependency(fbxPath, file.path);
        if (file.linked && !fs::exists(path, ec)) {
            missing.push_back(path);
        }
    }
    return missing;
}

FbxImporter::Result FbxImporter::import(const fs::path& fbxPath, const fs::path& glbPath, const fs::path& assetsRoot, bool force) {
    std::lock_guard<std::mutex> lock(importMutex);

    Result result;
    // Another thread may have imported it while this one waited for the lock
    if (!force && getState(fbxPath, glbPath, assetsRoot) == State::UpToDate) {
        result.success = true;
        return result;
    }

    ufbx_load_opts options = {};
    options.target_axes = ufbx_axes_right_handed_y_up;
    options.target_unit_meters = 1.0;
    options.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
    options.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_MODIFY_GEOMETRY;
    options.inherit_mode_handling = UFBX_INHERIT_MODE_HANDLING_COMPENSATE;
    options.pivot_handling = UFBX_PIVOT_HANDLING_ADJUST_TO_PIVOT;
    options.geometry_transform_helper_name.data = "GeometryTransformHelper";
    options.geometry_transform_helper_name.length = SIZE_MAX;
    options.scale_helper_name.data = "ScaleHelper";
    options.scale_helper_name.length = SIZE_MAX;
    options.target_camera_axes = ufbx_axes_right_handed_y_up;
    options.target_light_axes = ufbx_axes_right_handed_y_up;
    options.node_depth_limit = 512;
    options.clean_skin_weights = true;
    options.generate_missing_normals = true;
    options.use_blender_pbr_material = true;

    ufbx_error error;
    ufbx_scene* scene = ufbx_load_file(FileUtils::pathToUtf8(fbxPath).c_str(), &options, &error);
    if (!scene) {
        char message[1024];
        ufbx_format_error(message, sizeof(message), &error);
        result.error = message;
        return result;
    }
    std::unique_ptr<ufbx_scene, decltype(&ufbx_free_scene)> sceneGuard(scene, &ufbx_free_scene);

    for (const ufbx_warning& warning : scene->metadata.warnings) {
        if (result.warnings.size() >= MAX_UFBX_WARNINGS) {
            break;
        }
        result.warnings.emplace_back(warning.description.data, warning.description.length);
    }

    std::vector<unsigned char> glb;
    try {
        GlbBuilder builder(scene, fbxPath, assetsRoot);
        const bool built = builder.build(result.error);
        result.warnings.insert(result.warnings.end(), builder.getWarnings().begin(), builder.getWarnings().end());
        if (!built) {
            return result;
        }
        glb = builder.write(describeImport(builder, fbxPath, assetsRoot));
    } catch (const std::exception& e) {
        result.error = e.what();
        return result;
    }
    if (glb.empty()) {
        result.error = "the glTF data could not be encoded";
        return result;
    }

    if (!writeFile(glbPath, glb, result.error)) {
        return result;
    }
    // A coarse mtime could match the old file's
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        importInfos.erase(FileUtils::pathToGenericUtf8(glbPath.lexically_normal()));
    }

    result.success = true;
    return result;
}

}
