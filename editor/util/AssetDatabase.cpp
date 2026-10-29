// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AssetDatabase.h"

#include "Out.h"
#include "util/FileUtils.h"
#include "util/SafeWrite.h"
#include "util/SHA1.h"
#include "util/Util.h"

#include "animation/AnimationClipIO.h"
#include "animation/VertexAnimation.h"
#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <random>
#include <sstream>
#include <unordered_set>

using namespace jasperengine;
namespace fs = std::filesystem;

namespace {

    bool readText(const fs::path& path, std::string& text) {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            return false;
        }
        std::ostringstream buffer;
        buffer << file.rdbuf();
        text = buffer.str();
        return true;
    }

    // It rewrites the user's own files (a GUID put into a scene, prefab or material)
    bool writeText(const fs::path& path, const std::string& text) {
        return editor::FileUtils::writeFileReplacing(path, text);
    }

    // A top-level `guid:` line: a key at column 0, which in block style YAML is a key of
    // the document's own map (a block scalar's lines are indented)
    bool isGuidLine(const std::string& text, size_t begin) {
        return text.compare(begin, 5, "guid:") == 0;
    }

    std::string guidValue(const std::string& text, size_t begin, size_t end) {
        std::string value = text.substr(begin + 5, end - begin - 5);
        const size_t comment = value.find(" #");
        if (comment != std::string::npos) {
            value.erase(comment);
        }
        auto notSpace = [](unsigned char c) { return !std::isspace(c); };
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
        value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front()) {
            value = value.substr(1, value.size() - 2);
        }
        return value;
    }

    // .anim clips and .vat Vertex Animations are JSON: their GUID is the top-level "guid" member
    // (AnimationClipIO reads and replaces it in any JSON object that has a "version" member)
    bool isClipFile(const fs::path& path) {
        return editor::Util::isAnimationClipFile(path.string()) || editor::Util::isVertexAnimationFile(path.string());
    }

    // the JSON asset parses as its own type; a broken or foreign file (a Unity YAML .anim, broken
    // JSON) is left alone: its error must keep its line
    bool parsesAsJsonAsset(const fs::path& path, const std::string& text) {
        if (editor::Util::isVertexAnimationFile(path.string())) {
            return vat::parseVat(text).ok();
        }
        return anim::parseClip(text).ok();
    }

    // for the warnings
    const char* kindOf(const std::string& relative) {
        if (editor::Util::isAnimationClipFile(relative)) return "Animation clip";
        if (editor::Util::isVertexAnimationFile(relative)) return "Vertex Animation";
        return "Prefab";
    }

    bool skippedDirectory(const std::string& name) {
        // '.jasperengine' holds generated files and the cache, '.trash' deleted files, 'build'
        // the build artifacts (the same set Project::findProjectFiles skips)
        return name.rfind('.', 0) == 0 || name == "build";
    }

}

bool editor::AssetDatabase::isTrackedFile(const fs::path& path) {
    return Util::isPrefabFile(path.string()) || Util::isAnimationClipFile(path.string()) ||
           Util::isVertexAnimationFile(path.string());
}

std::string editor::AssetDatabase::generateGuid() {
    static std::mt19937_64 generator = [] {
        std::random_device device;
        std::seed_seq seed{device(), device(), device(), device(), device(), device(), device(), device()};
        return std::mt19937_64(seed);
    }();
    std::ostringstream text;
    text << std::hex;
    for (int part = 0; part < 2; part++) {
        const uint64_t value = generator();
        for (int shift = 60; shift >= 0; shift -= 4) {
            text << ((value >> shift) & 0xF);
        }
    }
    return text.str();
}

std::string editor::AssetDatabase::legacyGuid(const fs::path& relativePath) {
    return SHA1::hash("jasperengine:asset:" + relativePath.lexically_normal().generic_string()).substr(0, 32);
}

bool editor::AssetDatabase::isGuid(const std::string& text) {
    return text.size() == 32 && std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return std::isxdigit(c) != 0;
    });
}

std::string editor::AssetDatabase::readGuid(const fs::path& fullPath) {
    if (isClipFile(fullPath)) {
        std::string text;
        return readText(fullPath, text) ? anim::readClipGuid(text) : std::string();
    }
    std::ifstream file(fullPath, std::ios::binary);
    if (!file) {
        return {};
    }
    // GUIDs normally precede the entity data. Stop reading at that line instead of
    // loading and copying every prefab in full when the asset database opens.
    std::string line;
    while (std::getline(file, line)) {
        if (isGuidLine(line, 0)) {
            size_t valueEnd = line.size();
            if (valueEnd > 0 && line[valueEnd - 1] == '\r') {
                --valueEnd;
            }
            return guidValue(line, 0, valueEnd);
        }
    }
    return {};
}

bool editor::AssetDatabase::writeGuid(const fs::path& fullPath, const std::string& guid) {
    std::string text;
    if (!readText(fullPath, text)) {
        return false;
    }
    if (isClipFile(fullPath)) {
        // the member replaced or inserted after "version", the rest of the text unchanged; a
        // file that is not a clip (a Unity YAML .anim, broken JSON: its error must keep its
        // line) is left alone
        if (!parsesAsJsonAsset(fullPath, text) || !anim::replaceClipGuid(text, guid)) {
            return false;
        }
        if (!writeText(fullPath, text)) {
            Out::warning("Could not write the GUID of %s", fullPath.string().c_str());
            return false;
        }
        return true;
    }
    const std::string newline = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    const std::string line = "guid: " + guid;

    std::string updated;
    bool placed = false;
    for (size_t begin = 0; begin < text.size();) {
        size_t end = text.find('\n', begin);
        if (end == std::string::npos) {
            end = text.size();
        }
        if (isGuidLine(text, begin)) {
            size_t valueEnd = end;
            if (valueEnd > begin && text[valueEnd - 1] == '\r') {
                valueEnd--;
            }
            updated = text.substr(0, begin) + line + text.substr(valueEnd);
            placed = true;
            break;
        }
        begin = end + 1;
    }

    if (!placed) {
        // Before the first line of the document's map: past comments, directives and a
        // document start marker
        size_t insertAt = 0;
        for (size_t begin = 0; begin < text.size();) {
            size_t end = text.find('\n', begin);
            const size_t lineEnd = end == std::string::npos ? text.size() : end;
            const std::string lineText = text.substr(begin, lineEnd - begin);
            const size_t first = lineText.find_first_not_of(" \t\r");
            if (first == std::string::npos || lineText[first] == '#' || lineText[0] == '%' ||
                lineText.compare(0, 3, "---") == 0) {
                insertAt = end == std::string::npos ? text.size() : end + 1;
                begin = insertAt;
                if (end == std::string::npos) break;
                continue;
            }
            break;
        }
        std::string prefix = text.substr(0, insertAt);
        if (!prefix.empty() && prefix.back() != '\n') {
            prefix += newline;
        }
        updated = prefix + line + newline + text.substr(insertAt);
    }

    // Line edits suit the block style the editor writes; anything else (a flow style map)
    // is parsed and written again
    bool verified = false;
    try {
        YAML::Node check = YAML::Load(updated);
        verified = check.IsMap() && check["guid"] && check["guid"].as<std::string>() == guid;
    } catch (const std::exception&) {
        verified = false;
    }
    if (!verified) {
        try {
            YAML::Node node = YAML::Load(text);
            if (!node.IsMap()) {
                return false;
            }
            node["guid"] = guid;
            updated = YAML::Dump(node) + "\n";
        } catch (const std::exception& e) {
            Out::warning("Could not write the GUID of %s: %s", fullPath.string().c_str(), e.what());
            return false;
        }
    }

    if (!writeText(fullPath, updated)) {
        Out::warning("Could not write the GUID of %s", fullPath.string().c_str());
        return false;
    }
    return true;
}

fs::path editor::AssetDatabase::toRelative(const fs::path& path) const {
    if (path.empty()) {
        return {};
    }
    fs::path normalized = path.lexically_normal();
    if (normalized.is_absolute() && !projectPath.empty()) {
        std::error_code ec;
        fs::path relative = fs::relative(normalized, projectPath, ec);
        if (!ec && !relative.empty()) {
            return relative.lexically_normal();
        }
        relative = normalized.lexically_relative(projectPath.lexically_normal());
        if (!relative.empty()) {
            return relative.lexically_normal();
        }
    }
    return normalized;
}

std::string editor::AssetDatabase::key(const fs::path& path) const {
    return toRelative(path).generic_string();
}

bool editor::AssetDatabase::fileExists(const std::string& relative) const {
    std::error_code ec;
    return fs::is_regular_file(projectPath / relative, ec);
}

void editor::AssetDatabase::stat(const std::string& relative, Entry& entry) const {
    std::error_code ec;
    const fs::path full = projectPath / relative;
    entry.writeTime = fs::last_write_time(full, ec);
    if (ec) {
        entry.writeTime = {};
    }
    entry.size = fs::file_size(full, ec);
    if (ec) {
        entry.size = 0;
    }
}

void editor::AssetDatabase::insert(const std::string& relative, const std::string& guid) {
    auto old = byPath.find(relative);
    if (old != byPath.end() && old->second.guid != guid) {
        auto guidIt = byGuid.find(old->second.guid);
        if (guidIt != byGuid.end() && guidIt->second == relative) {
            byGuid.erase(guidIt);
        }
    }
    Entry& entry = byPath[relative];
    entry.guid = guid;
    stat(relative, entry);
    byGuid[guid] = relative;
}

void editor::AssetDatabase::erase(const std::string& relative) {
    auto it = byPath.find(relative);
    if (it == byPath.end()) {
        return;
    }
    auto guidIt = byGuid.find(it->second.guid);
    if (guidIt != byGuid.end() && guidIt->second == relative) {
        byGuid.erase(guidIt);
    }
    byPath.erase(it);
}

std::string editor::AssetDatabase::assignNewGuid(const std::string& relative, const std::string& preferred) {
    std::string guid = preferred;
    while (!isGuid(guid) || byGuid.count(guid)) {
        guid = generateGuid();
    }
    if (!writeFileGuid(relative, guid) && !readOnly) {
        // a clip that does not parse (a Unity YAML .anim, broken JSON) is reported by the clip
        // checks, not here
        std::string text;
        if (!(isClipFile(relative) && readText(projectPath / relative, text) && !parsesAsJsonAsset(relative, text))) {
            Out::warning("The GUID of %s is kept in memory only", relative.c_str());
        }
    }
    return guid;
}

std::vector<std::string> editor::AssetDatabase::collectFiles(const fs::path& fullPath) const {
    std::vector<std::string> files;
    std::error_code ec;
    if (fs::is_regular_file(fullPath, ec)) {
        if (isTrackedFile(fullPath)) {
            files.push_back(key(fullPath));
        }
        return files;
    }
    if (!fs::is_directory(fullPath, ec)) {
        return files;
    }
    for (fs::recursive_directory_iterator it(fullPath, fs::directory_options::skip_permission_denied, ec), end;
         it != end && !ec; it.increment(ec)) {
        const fs::directory_entry& entry = *it;
        std::error_code entryError;
        if (entry.is_directory(entryError)) {
            if (skippedDirectory(entry.path().filename().string())) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (entry.is_regular_file(entryError) && isTrackedFile(entry.path())) {
            files.push_back(key(entry.path()));
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

fs::path editor::AssetDatabase::cacheFile() const {
    return projectPath / ".jasperengine" / "assetdb.yaml";
}

void editor::AssetDatabase::loadCache(std::unordered_map<std::string, std::string>& owners) const {
    std::error_code ec;
    if (!fs::exists(cacheFile(), ec)) {
        return;
    }
    try {
        YAML::Node node = YAML::LoadFile(cacheFile().string());
        for (const YAML::Node& asset : node["assets"]) {
            if (asset["guid"] && asset["path"]) {
                owners[asset["guid"].as<std::string>()] = asset["path"].as<std::string>();
            }
        }
    } catch (const std::exception& e) {
        Out::warning("Ignoring the asset database cache: %s", e.what());
    }
}

bool editor::AssetDatabase::writeFileGuid(const std::string& relative, const std::string& guid) {
    if (readOnly) {
        return false;   // in memory only: a check or a headless analysis changes no project file
    }
    return writeGuid(projectPath / relative, guid);
}

void editor::AssetDatabase::setReadOnly(bool value) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    readOnly = value;
}

void editor::AssetDatabase::saveCache() const {
    if (projectPath.empty() || readOnly) {
        return;
    }
    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << "version" << YAML::Value << 1;
    out << YAML::Key << "assets" << YAML::Value << YAML::BeginSeq;
    for (const auto& [path, entry] : byPath) {
        out << YAML::Flow << YAML::BeginMap;
        out << YAML::Key << "guid" << YAML::Value << entry.guid;
        out << YAML::Key << "path" << YAML::Value << path;
        out << YAML::EndMap;
    }
    out << YAML::EndSeq << YAML::EndMap;

    std::error_code ec;
    // the asset-database cache lives in the editor's internal folder (an MCP call's deny-by-default
    // guard refuses it otherwise: util/SafeWrite.h, EditorInternal)
    safewrite::EditorInternal ownCache(cacheFile().parent_path());
    editor::FileUtils::createDirectories(cacheFile().parent_path(), ec);   // (in a write scope: beneath its root)
    writeText(cacheFile(), std::string(out.c_str()) + "\n");
}

void editor::AssetDatabase::open(const fs::path& path) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    close();
    projectPath = path;

    std::unordered_map<std::string, std::string> owners;
    loadCache(owners);

    struct Found {
        std::string relative;
        std::string guid;
        bool legacy = false;
        Entry stat;
    };
    std::vector<Found> found;
    for (const std::string& relative : collectFiles(projectPath)) {
        Found file;
        file.relative = relative;
        file.guid = readGuid(projectPath / relative);
        if (!isGuid(file.guid)) {
            if (!file.guid.empty()) {
                Out::warning("%s %s has an invalid GUID \"%s\", it gets a new one", kindOf(relative), relative.c_str(), file.guid.c_str());
            } else {
                file.legacy = true;
            }
            file.guid.clear();
        }
        stat(relative, file.stat);
        found.push_back(std::move(file));
    }

    // A GUID found twice stays with the file that had it last session, else the older one
    std::unordered_map<std::string, std::vector<size_t>> byFoundGuid;
    for (size_t i = 0; i < found.size(); i++) {
        if (!found[i].guid.empty()) {
            byFoundGuid[found[i].guid].push_back(i);
        }
    }
    for (auto& [guid, indices] : byFoundGuid) {
        if (indices.size() < 2) {
            continue;
        }
        size_t keeper = indices[0];
        auto owner = owners.find(guid);
        bool ownerFound = false;
        if (owner != owners.end()) {
            for (size_t index : indices) {
                if (found[index].relative == owner->second) {
                    keeper = index;
                    ownerFound = true;
                }
            }
        }
        if (!ownerFound) {
            for (size_t index : indices) {
                const Found& candidate = found[index];
                const Found& best = found[keeper];
                if (candidate.stat.writeTime < best.stat.writeTime ||
                    (candidate.stat.writeTime == best.stat.writeTime && candidate.relative < best.relative)) {
                    keeper = index;
                }
            }
        }
        for (size_t index : indices) {
            if (index != keeper) {
                Out::warning("%s %s has the GUID of %s (a copy?), it gets a new one",
                    kindOf(found[index].relative), found[index].relative.c_str(), found[keeper].relative.c_str());
                found[index].guid.clear();
            }
        }
    }

    for (const Found& file : found) {
        if (!file.guid.empty()) {
            insert(file.relative, file.guid);
        }
    }
    for (const Found& file : found) {
        if (!file.guid.empty()) {
            continue;
        }
        std::string guid = file.legacy ? legacyGuid(file.relative) : std::string();
        guid = assignNewGuid(file.relative, guid);
        insert(file.relative, guid);
    }

    saveCache();
}

void editor::AssetDatabase::close() {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    projectPath.clear();
    byPath.clear();
    byGuid.clear();
}

std::vector<editor::AssetDatabase::Change> editor::AssetDatabase::refresh() {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    std::vector<Change> changes;
    if (!isOpen()) {
        return changes;
    }
    bool dirty = false;

    const std::vector<std::string> files = collectFiles(projectPath);
    const std::unordered_set<std::string> present(files.begin(), files.end());

    std::vector<std::string> fresh;
    for (const std::string& relative : files) {
        auto it = byPath.find(relative);
        if (it == byPath.end()) {
            fresh.push_back(relative);
            continue;
        }
        Entry current;
        stat(relative, current);
        if (current.writeTime == it->second.writeTime && current.size == it->second.size) {
            continue;
        }
        dirty = true;
        const std::string guid = readGuid(projectPath / relative);
        if (guid == it->second.guid) {
            it->second.writeTime = current.writeTime;
            it->second.size = current.size;
            changes.push_back({Change::Type::Modified, it->second.guid, relative, {}});
        } else if (guid.empty()) {
            // Written again without its GUID (by another tool): it is still the same asset
            writeFileGuid(relative, it->second.guid);
            stat(relative, it->second);
            changes.push_back({Change::Type::Modified, it->second.guid, relative, {}});
        } else if (isClipFile(relative) && byGuid.count(guid) && byGuid[guid] != relative && fileExists(byGuid[guid])) {
            // A clip rewritten with another existing clip's GUID (JSON copied from it by an agent):
            // still this asset, its references keep finding it
            writeFileGuid(relative, it->second.guid);
            stat(relative, it->second);
            changes.push_back({Change::Type::Modified, it->second.guid, relative, {}});
        } else {
            // Another asset now (a checkout replaced it)
            changes.push_back({Change::Type::Removed, it->second.guid, relative, {}});
            erase(relative);
            fresh.push_back(relative);
        }
    }

    std::unordered_map<std::string, std::string> gone; // guid -> relative path
    for (auto it = byPath.begin(); it != byPath.end();) {
        if (present.count(it->first)) {
            ++it;
            continue;
        }
        gone[it->second.guid] = it->first;
        auto guidIt = byGuid.find(it->second.guid);
        if (guidIt != byGuid.end() && guidIt->second == it->first) {
            byGuid.erase(guidIt);
        }
        it = byPath.erase(it);
        dirty = true;
    }

    for (const std::string& relative : fresh) {
        dirty = true;
        std::string guid = readGuid(projectPath / relative);
        if (!isGuid(guid)) {
            guid = assignNewGuid(relative, guid.empty() ? legacyGuid(relative) : std::string());
            insert(relative, guid);
            changes.push_back({Change::Type::Added, guid, relative, {}});
            continue;
        }
        if (byGuid.count(guid)) {
            // A copy of a file the database has: the copy gets a new GUID
            guid = assignNewGuid(relative);
            insert(relative, guid);
            changes.push_back({Change::Type::Added, guid, relative, {}});
            continue;
        }
        auto moved = gone.find(guid);
        if (moved != gone.end()) {
            insert(relative, guid);
            changes.push_back({Change::Type::Moved, guid, relative, moved->second});
            gone.erase(moved);
            continue;
        }
        insert(relative, guid);
        changes.push_back({Change::Type::Added, guid, relative, {}});
    }

    for (const auto& [guid, relative] : gone) {
        changes.push_back({Change::Type::Removed, guid, relative, {}});
    }

    if (dirty) {
        saveCache();
    }
    return changes;
}

bool editor::AssetDatabase::isOpen() const {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return !projectPath.empty();
}

bool editor::AssetDatabase::contains(const std::string& guid) const {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return byGuid.count(guid) != 0;
}

fs::path editor::AssetDatabase::getPath(const std::string& guid) const {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    auto it = byGuid.find(guid);
    return it != byGuid.end() ? fs::path(it->second) : fs::path();
}

std::string editor::AssetDatabase::getGuid(const fs::path& path) const {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    auto it = byPath.find(key(path));
    return it != byPath.end() ? it->second.guid : std::string();
}

std::vector<editor::AssetDatabase::Change> editor::AssetDatabase::moveAssets(const fs::path& oldPath, const fs::path& newPath) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    std::vector<Change> changes;
    if (!isOpen()) {
        return changes;
    }
    const std::string oldKey = key(oldPath);
    const std::string newKey = key(newPath);
    if (oldKey.empty() || newKey.empty()) {
        return changes;
    }

    std::vector<std::pair<std::string, std::string>> moved; // old relative, guid
    for (const auto& [relative, entry] : byPath) {
        if (relative == oldKey || relative.rfind(oldKey + "/", 0) == 0) {
            moved.emplace_back(relative, entry.guid);
        }
    }
    for (const auto& [relative, guid] : moved) {
        erase(relative);
    }
    for (const auto& [relative, guid] : moved) {
        const std::string updated = newKey + relative.substr(oldKey.size());
        if (isTrackedFile(updated) && fileExists(updated)) {
            insert(updated, guid);
            changes.push_back({Change::Type::Moved, guid, updated, relative});
        } else {
            changes.push_back({Change::Type::Removed, guid, relative, {}});
        }
    }

    // What became a tracked file with the new name
    std::vector<Change> imported = importAssets(projectPath / newKey);
    changes.insert(changes.end(), imported.begin(), imported.end());

    saveCache();
    return changes;
}

std::vector<editor::AssetDatabase::Change> editor::AssetDatabase::removeAssets(const fs::path& path) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    std::vector<Change> changes;
    if (!isOpen()) {
        return changes;
    }
    const std::string removedKey = key(path);
    if (removedKey.empty()) {
        return changes;
    }
    std::vector<std::string> removed;
    for (const auto& [relative, entry] : byPath) {
        if (relative == removedKey || relative.rfind(removedKey + "/", 0) == 0) {
            removed.push_back(relative);
            changes.push_back({Change::Type::Removed, entry.guid, relative, {}});
        }
    }
    for (const std::string& relative : removed) {
        erase(relative);
    }
    if (!removed.empty()) {
        saveCache();
    }
    return changes;
}

std::vector<editor::AssetDatabase::Change> editor::AssetDatabase::importAssets(const fs::path& path, bool copy,
        std::map<std::string, std::string>* copyGuids) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    std::vector<Change> changes;
    if (!isOpen()) {
        return changes;
    }
    bool dirty = false;
    for (const std::string& relative : collectFiles(path.is_absolute() ? path : projectPath / path)) {
        std::string guid = readGuid(projectPath / relative);

        auto existing = byPath.find(relative);
        if (existing != byPath.end()) {
            if (existing->second.guid == guid && !(copy && copyGuids && copyGuids->count(relative))) {
                // the same asset, a new time and size: the cache (GUID and path only) is unchanged
                stat(relative, existing->second);
                continue;
            }
            // Overwritten by another file: the asset it was is gone
            if (existing->second.guid != guid) {
                changes.push_back({Change::Type::Removed, existing->second.guid, relative, {}});
            }
            erase(relative);
        }
        dirty = true;

        // A redo of a copy gives the copy the GUID it got the first time
        if (copy && copyGuids) {
            auto recorded = copyGuids->find(relative);
            if (recorded != copyGuids->end() && isGuid(recorded->second) && !byGuid.count(recorded->second)) {
                if (guid != recorded->second && !writeFileGuid(relative, recorded->second)) {
                    Out::warning("The GUID of %s is kept in memory only", relative.c_str());
                }
                insert(relative, recorded->second);
                changes.push_back({Change::Type::Added, recorded->second, relative, {}});
                continue;
            }
        }

        if (!isGuid(guid)) {
            guid = assignNewGuid(relative, guid.empty() ? legacyGuid(relative) : std::string());
        } else if (byGuid.count(guid)) {
            const std::string other = byGuid[guid];
            if (fileExists(other)) {
                guid = assignNewGuid(relative); // a copy
            } else {
                // The file the database knew is gone: this is it, moved
                erase(other);
                insert(relative, guid);
                changes.push_back({Change::Type::Moved, guid, relative, other});
                if (copy && copyGuids) {
                    (*copyGuids)[relative] = guid;
                }
                continue;
            }
        }
        insert(relative, guid);
        changes.push_back({Change::Type::Added, guid, relative, {}});
        if (copy && copyGuids) {
            (*copyGuids)[relative] = guid;
        }
    }
    if (dirty) {
        saveCache();
    }
    return changes;
}

void editor::AssetDatabase::registerAsset(const fs::path& path, const std::string& guid) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (!isOpen() || guid.empty()) {
        return;
    }
    const std::string relative = key(path);
    auto other = byGuid.find(guid);
    if (other != byGuid.end() && other->second != relative) {
        erase(other->second);
    }
    insert(relative, guid);
    saveCache();
}
