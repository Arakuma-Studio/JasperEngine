// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "BuildReport.h"

#include "io/AtomicFile.h"
#include "texture/CookedTextures.h"
#include "util/FileUtils.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    std::string lowerExtension(const std::string& path){
        std::string extension = fs::path(path).extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        return extension;
    }

    std::mutex& lastMutex(){
        static std::mutex mutex;
        return mutex;
    }

    fs::path& lastReport(){
        static fs::path path;
        return path;
    }

}

const std::vector<std::string>& BuildReport::categoryNames(){
    static const std::vector<std::string> names = {"models", "textures", "audio", "scripts", "scenes", "engine", "other"};
    return names;
}

std::string BuildReport::categoryOf(const std::string& path){
    const std::string extension = lowerExtension(path);
    const std::string cooked = std::string("assets/") + CookedTextures::kCookedDir + "/";
    if (path.rfind(cooked + "models/", 0) == 0 || path.find("/" + std::string(CookedTextures::kCookedDir) + "/models/") != std::string::npos)
        return "models";
    if (path.rfind(cooked, 0) == 0 || path.find("/" + std::string(CookedTextures::kCookedDir) + "/") != std::string::npos)
        return "textures";
    static const std::set<std::string> models = {".glb", ".gltf", ".fbx", ".obj", ".bin", ".vat"};
    static const std::set<std::string> textures = {".png", ".jpg", ".jpeg", ".tga", ".bmp", ".psd", ".gif", ".hdr", ".exr", ".ktx2", ".svg", ".webp", ".jlut"};
    static const std::set<std::string> audio = {".wav", ".ogg", ".mp3", ".flac", ".opus"};
    static const std::set<std::string> scripts = {".lua", ".luac"};
    static const std::set<std::string> scenes = {".scene", ".prefab", ".jscn", ".jpfb"};
    static const std::set<std::string> engine = {".exe", ".dll", ".so", ".dylib", ".wasm", ".js", ".a", ".apk", ".aab", ".pdb"};
    if (models.count(extension)) return "models";
    if (textures.count(extension)) return "textures";
    if (audio.count(extension)) return "audio";
    if (scripts.count(extension) || path.rfind("lua/", 0) == 0) return "scripts";
    if (scenes.count(extension) || path.rfind("scenes/", 0) == 0 || path.rfind("prefabs/", 0) == 0) return "scenes";
    const std::string name = fs::path(path).filename().string();
    if (engine.count(extension) || name.rfind("jasperengine-player", 0) == 0 || name.find(".so.") != std::string::npos ||
        path.find(".framework/") != std::string::npos || path.find(".app/Contents/MacOS/") != std::string::npos)
        return "engine";
    return "other";
}

void BuildReport::addFile(const BuildReportFile& file){
    totalBytes += file.bytes;
    categories[file.category] += file.bytes;
    largest.push_back(file);
    std::sort(largest.begin(), largest.end(), [](const BuildReportFile& a, const BuildReportFile& b){
        return a.bytes != b.bytes ? a.bytes > b.bytes : a.path < b.path;
    });
    if (largest.size() > kLargest)
        largest.resize(kLargest);
}

void BuildReport::addTree(const fs::path& root, const std::map<std::string, std::pair<std::string, uint64_t>>& cooked,
                          const std::vector<std::string>& only, const std::string& prefix){
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)){
        if (!it->is_regular_file(ec))
            continue;
        BuildReportFile file;
        file.path = prefix + FileUtils::pathToUtf8(it->path().lexically_relative(root));
        if (file.path == kFileName)
            continue;
        file.category = categoryOf(file.path);
        if (!only.empty() && std::find(only.begin(), only.end(), file.category) == only.end())
            continue;
        file.bytes = (uint64_t)it->file_size(ec);
        auto source = cooked.find(file.path);
        if (source != cooked.end()){
            file.source = source->second.first;
            file.sourceBytes = source->second.second;
        }
        addFile(file);
    }
}

std::map<std::string, std::pair<std::string, uint64_t>> BuildReport::cookedTextures(const fs::path& assetsDir, const fs::path& sourceAssets){
    std::map<std::string, std::pair<std::string, uint64_t>> out;
    std::error_code ec;
    const fs::path dir = assetsDir / CookedTextures::kCookedDir;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)){
        if (!it->is_directory(ec) || it->path().filename() == "models")
            continue;
        std::ifstream in(it->path() / "index.json", std::ios::binary);
        if (!in)
            continue;
        std::stringstream text;
        text << in.rdbuf();
        const Json index = Json::parse(text.str(), nullptr, false);
        if (!index.is_object() || !index.contains("textures") || !index["textures"].is_object())
            continue;
        for (const auto& [source, value] : index["textures"].items()){
            if (!value.is_object() || !value.contains("file"))
                continue;
            const std::string file = std::string("assets/") + CookedTextures::kCookedDir + "/" + it->path().filename().string() + "/" +
                                     value.value("file", std::string());
            out[file] = {source, (uint64_t)fs::file_size(sourceAssets / FileUtils::pathFromUtf8(source), ec)};
        }
    }
    return out;
}

Json BuildReport::toJson() const{
    Json categoryJson = Json::object();
    for (const std::string& name : categoryNames())
        categoryJson[name] = categories.count(name) ? categories.at(name) : 0;
    Json files = Json::array();
    for (const BuildReportFile& file : largest){
        Json entry = {{"path", file.path}, {"category", file.category}, {"bytes", file.bytes}};
        if (!file.source.empty()){
            entry["source"] = file.source;
            entry["source_bytes"] = file.sourceBytes;
        }
        files.push_back(entry);
    }
    return {{"version", 1}, {"platform", platform}, {"method", method}, {"total_bytes", totalBytes}, {"categories", categoryJson}, {"largest", files}};
}

bool BuildReport::fromJson(const Json& json, BuildReport& out){
    if (!json.is_object() || json.value("version", 0) != 1)
        return false;
    out = BuildReport();
    out.platform = json.value("platform", std::string());
    out.method = json.value("method", std::string());
    out.totalBytes = json.value("total_bytes", (uint64_t)0);
    if (json.contains("categories") && json["categories"].is_object())
        for (const auto& [name, bytes] : json["categories"].items())
            if (bytes.is_number_unsigned()) out.categories[name] = bytes.get<uint64_t>();
    if (json.contains("largest") && json["largest"].is_array()){
        for (const Json& entry : json["largest"]){
            BuildReportFile file;
            file.path = entry.value("path", std::string());
            file.category = entry.value("category", std::string());
            file.bytes = entry.value("bytes", (uint64_t)0);
            file.source = entry.value("source", std::string());
            file.sourceBytes = entry.value("source_bytes", (uint64_t)0);
            out.largest.push_back(file);
        }
    }
    return true;
}

bool BuildReport::write(const fs::path& file, std::string& error) const{
    io::AtomicWriteOptions options;
    options.durability = io::Durability::Cache;
    const io::WriteResult result = io::writeFileAtomic(file, toJson().dump(2) + "\n", options);
    if (!result.ok()){
        error = result.message;
        return false;
    }
    return true;
}

bool BuildReport::load(const fs::path& file, BuildReport& out, std::string& error){
    std::ifstream in(file, std::ios::binary);
    if (!in){
        error = "no " + std::string(kFileName) + " at " + FileUtils::pathToUtf8(file.parent_path());
        return false;
    }
    std::stringstream text;
    text << in.rdbuf();
    if (!fromJson(Json::parse(text.str(), nullptr, false), out)){
        error = FileUtils::pathToUtf8(file) + " is not a build report";
        return false;
    }
    return true;
}

std::string BuildReport::formatBytes(uint64_t bytes){
    char text[32];
    if (bytes >= (1ull << 30)) std::snprintf(text, sizeof(text), "%.2f GB", bytes / 1073741824.0);
    else if (bytes >= (1ull << 20)) std::snprintf(text, sizeof(text), "%.1f MB", bytes / 1048576.0);
    else if (bytes >= 1024) std::snprintf(text, sizeof(text), "%.1f KB", bytes / 1024.0);
    else std::snprintf(text, sizeof(text), "%llu B", (unsigned long long)bytes);
    return text;
}

std::string BuildReport::summary() const{
    std::string out = "Build Report (" + platform + "): " + formatBytes(totalBytes);
    std::string parts;
    for (const std::string& name : categoryNames()){
        auto it = categories.find(name);
        if (it == categories.end() || it->second == 0)
            continue;
        parts += (parts.empty() ? "" : ", ") + name + " " + formatBytes(it->second);
    }
    return parts.empty() ? out : out + "; " + parts;
}

void BuildReport::setLast(const fs::path& file){
    std::lock_guard<std::mutex> lock(lastMutex());
    lastReport() = file;
}

fs::path BuildReport::last(){
    std::lock_guard<std::mutex> lock(lastMutex());
    return lastReport();
}
