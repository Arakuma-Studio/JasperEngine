// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ShaderListCache.h"

#include "DocumentFile.h"
#include "Stream.h"
#include "io/AtomicFile.h"
#include "util/FileUtils.h"
#include "util/SafeWrite.h"

#include <system_error>
#include <yaml-cpp/yaml.h>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {
    // A new format of this file reads as no file
    constexpr int CACHE_VERSION = 1;
}

const ShaderListCache::Entry* ShaderListCache::find(const std::string& scene) const {
    const auto it = entries.find(scene);
    return it == entries.end() ? nullptr : &it->second;
}

void ShaderListCache::store(const std::string& scene, Entry entry) {
    entries[scene] = std::move(entry);
    dirty = true;
}

void ShaderListCache::updateStamp(const std::string& scene, std::int64_t writeTime, std::uintmax_t size) {
    const auto it = entries.find(scene);
    if (it == entries.end()) return;
    it->second.writeTime = writeTime;
    it->second.size = size;
    dirty = true;
}

void ShaderListCache::clear() {
    entries.clear();
    dirty = false;
}

bool ShaderListCache::load(const std::filesystem::path& file) {
    clear();
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) return false;
    try {
        const YAML::Node root = YAML::LoadFile(FileUtils::pathToUtf8(file));
        if (!root["version"] || root["version"].as<int>() != CACHE_VERSION || !root["scenes"]) return false;
        std::map<std::string, Entry> read;
        for (const auto& item : root["scenes"]) {
            const YAML::Node node = item.second;
            Entry entry;
            entry.sha1 = node["sha1"].as<std::string>();
            entry.writeTime = node["writeTime"].as<std::int64_t>();
            entry.size = node["size"].as<std::uintmax_t>();
            entry.context = node["context"].as<std::string>();
            for (ShaderKey key : Stream::decodeShaderKeys(node["keys"])) entry.keys.insert(key);
            if (node["prefabs"]) entry.prefabs = node["prefabs"].as<std::vector<std::string>>();
            read[item.first.as<std::string>()] = std::move(entry);
        }
        entries = std::move(read);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::map<std::string, ShaderListCache::Entry> ShaderListCache::takeEntriesToWrite() {
    dirty = false;
    return entries;
}

bool ShaderListCache::write(const std::filesystem::path& file, const std::map<std::string, Entry>& entries) {
    YAML::Node root;
    root["version"] = CACHE_VERSION;
    YAML::Node scenes(YAML::NodeType::Map);
    for (const auto& [scene, entry] : entries) {
        YAML::Node node;
        node["sha1"] = entry.sha1;
        node["writeTime"] = entry.writeTime;
        node["size"] = entry.size;
        node["context"] = entry.context;
        node["keys"] = Stream::encodeShaderKeys(entry.keys);
        if (!entry.prefabs.empty()) node["prefabs"] = entry.prefabs;
        scenes[scene] = node;
    }
    root["scenes"] = scenes;

    // the editor's own file in its own folder (util/SafeWrite.h), also during an MCP call
    safewrite::EditorInternal own(file.parent_path());
    std::error_code ec;
    FileUtils::createDirectories(file.parent_path(), ec);
    if (ec) return false;
    return io::writeFileAtomic(file, DocumentFiles::text(root)).ok();
}
