// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AssetDatabase.h"
#include "io/AtomicFile.h"

#include "Out.h"
#include "util/FileUtils.h"
#include "asset/AssetFileWrite.h"
#include "asset/AssetMeta.h"
#include "asset/AssetTypes.h"
#include "asset/ContentHash.h"
#include "asset/ModelSubAssets.h"
#include "util/SafeWrite.h"
#include "util/SHA1.h"
#include "util/Util.h"

#include "json.hpp"
#include "animation/AnimationClipIO.h"
#include "animation/VertexAnimation.h"
#include "animation/AnimatorAssetIO.h"
#include "graph/Graph.h"
#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <random>
#include <set>
#include <sstream>
#include <unordered_set>
#include "recorder/RecorderSettings.h"
#include "ecs/View.h"
#include "profiler/GpuMemory.h"

// One stat per file on Linux (std::filesystem asks twice: time, then size); the time is built
// as libstdc++'s last_write_time builds it, so the cache compares equal either way
#if defined(__linux__) && defined(__GLIBCXX__) && __cplusplus > 201703L
#define JASPER_ASSETDB_POSIX_STAT 1
#include <sys/stat.h>
#endif

using namespace jasperengine;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

    // A cache's text. A file name in another encoding than UTF-8 can't be JSON: its entries are
    // left out (read from disk again next time) instead of the dump throwing, which failed the
    // project's load
    std::string cacheText(Json& root, const char* listKey) {
        try {
            return root.dump() + "\n";
        } catch (const Json::type_error&) {
            Json kept = Json::array();
            for (Json& item : root[listKey]) {
                try {
                    (void)item.dump();
                    kept.push_back(std::move(item));
                } catch (const Json::type_error&) {
                }
            }
            root[listKey] = std::move(kept);
            return root.dump(-1, ' ', false, Json::error_handler_t::replace) + "\n";
        }
    }

    constexpr int databaseCacheVersion = 1;

    // A top-level key line: a key at column 0, which in block style YAML is a key of the
    // document's own map (a block scalar's lines are indented)
    bool isKeyLine(const std::string& text, size_t begin, const char* key) {
        const size_t length = std::char_traits<char>::length(key);
        return text.compare(begin, length, key) == 0 && begin + length < text.size() + 1 &&
               (begin + length == text.size() || text[begin + length] == ':');
    }

    std::string trimmed(std::string value) {
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

    std::string lineValue(const std::string& line, size_t keyLength) {
        std::string value = line.substr(keyLength + 1);
        if (!value.empty() && value.back() == '\r') {
            value.pop_back();
        }
        return trimmed(value);
    }

    // Where the document's own content starts on `line`, npos for a line before it: blank, a
    // comment, a directive or a bare document start marker ("--- {...}" starts at the brace)
    size_t documentContentStart(const std::string& line) {
        size_t start = 0;
        if (line.compare(0, 3, "---") == 0) {
            start = 3;
        } else if (!line.empty() && line[0] == '%') {
            return std::string::npos;
        }
        start = line.find_first_not_of(" \t\r", start);
        if (start == std::string::npos || line[start] == '#') {
            return std::string::npos;
        }
        return start;
    }

    // A document whose content starts as a flow style map (written by another tool): its keys
    // are not lines, so it is parsed. Its top-level `guid` and `formerGuids`; false when the
    // file is not such a document.
    bool flowIdentity(const fs::path& path, std::string& guid, std::vector<std::string>& formerGuids) {
        std::string text;
        if (!editor::assetio::readFile(path, text)) {
            return false;
        }
        bool flow = false;
        for (size_t begin = 0; begin < text.size();) {
            size_t end = text.find('\n', begin);
            end = end == std::string::npos ? text.size() : end;
            const std::string line = text.substr(begin, end - begin);
            const size_t content = documentContentStart(line);
            if (content != std::string::npos) {
                flow = line[content] == '{';
                break;
            }
            begin = end + 1;
        }
        if (!flow) {
            return false;
        }
        try {
            const YAML::Node node = YAML::Load(text);
            if (node.IsMap() && node["guid"] && node["guid"].IsScalar()) {
                guid = node["guid"].Scalar();
            }
            if (node.IsMap() && node["formerGuids"] && node["formerGuids"].IsSequence()) {
                for (const YAML::Node& former : node["formerGuids"]) {
                    if (former.IsScalar()) formerGuids.push_back(former.Scalar());
                }
            }
        } catch (const std::exception&) {
        }
        return true;
    }

    // The line before the document's content starts (blank lines, comments, directives, a
    // bare document start marker); the content's first line reports whether it is a flow map
    bool startsFlowMap(const std::string& line, bool& decided) {
        const size_t content = documentContentStart(line);
        if (content == std::string::npos) {
            return false;
        }
        decided = true;
        return line[content] == '{';
    }

    // "[a, b]" or a block of "  - a" lines -> GUIDs
    void parseGuidList(const std::string& flow, std::vector<Guid>& out) {
        std::string inner = flow;
        if (!inner.empty() && inner.front() == '[') inner.erase(0, 1);
        if (!inner.empty() && inner.back() == ']') inner.pop_back();
        std::stringstream stream(inner);
        std::string item;
        while (std::getline(stream, item, ',')) {
            const Guid guid = Guid::parse(trimmed(item));
            if (!guid.isNull()) {
                out.push_back(guid);
            }
        }
    }

    std::string formerLine(const std::vector<Guid>& formerGuids) {
        std::vector<Guid> sorted = formerGuids;
        std::sort(sorted.begin(), sorted.end());
        sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
        std::string line = "formerGuids: [";
        for (size_t i = 0; i < sorted.size(); i++) {
            line += (i ? ", " : "") + sorted[i].str();
        }
        return line + "]";
    }

    int64_t timeValue(const fs::file_time_type& time) {
        return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count());
    }

    fs::file_time_type timeFrom(int64_t value) {
        return fs::file_time_type(std::chrono::duration_cast<fs::file_time_type::duration>(std::chrono::nanoseconds(value)));
    }

    std::string swapCase(const std::string& text) {
        std::string out = text;
        for (char& c : out) {
            const unsigned char u = static_cast<unsigned char>(c);
            c = std::isupper(u) ? static_cast<char>(std::tolower(u)) : static_cast<char>(std::toupper(u));
        }
        return out;
    }

    // Case-insensitive volumes (Windows, macOS by default) are detected per project: the
    // project folder found under its name with the case of its letters swapped
    bool detectCaseInsensitive(const fs::path& projectPath) {
        const std::string name = projectPath.filename().string();
        const std::string swapped = swapCase(name);
        if (swapped == name) {
#ifdef _WIN32
            return true;
#else
            return false;
#endif
        }
        std::error_code ec;
        const fs::path other = projectPath.parent_path() / swapped;
        return fs::exists(other, ec) && fs::equivalent(projectPath, other, ec);
    }

    std::string lowerDatabaseKey(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    std::vector<editor::AssetDatabase::SubAsset> toDatabaseSubAssets(const std::vector<editor::AssetMeta::SubAsset>& metaEntries) {
        std::vector<editor::AssetDatabase::SubAsset> out;
        for (const editor::AssetMeta::SubAsset& entry : metaEntries) {
            SubAssetKind kind;
            std::string name;
            long index;
            if (!parseSubAssetKey(entry.key, kind, name, index)) {
                continue;
            }
            out.push_back({entry.key, entry.guid, entry.name, kind});
        }
        return out;
    }

    // .anim clips, .vat Vertex Animations and the Animator's .controller / .mask / .montage files are JSON: their GUID is the top-level "guid" member
    // (AnimationClipIO reads and replaces it in any JSON object that has a "version" member)
    bool isClipFile(const fs::path& path) {
        return editor::Util::isAnimationClipFile(path.string()) || editor::Util::isVertexAnimationFile(path.string()) ||
               editor::Util::isAnimatorAssetFile(path.string()) || editor::Util::isProceduralMaterialFile(path.string());
    }

    // the Animator asset text parses (its writer is what the identity rewrite uses)
    bool animatorAssetParses(const std::string& text) {
        const std::string kind = anim::animatorAssetKind(text);
        if (kind == "controller") return anim::parseController(text).ok;
        if (kind == "mask") return anim::parseMask(text).ok;
        if (kind == "montage") return anim::parseMontage(text).ok;
        return false;
    }

    // the Animator asset with "formerGuids" set (an unknown key to its reader, written back) and
    // that GUID: written canonically again; false for a text that is not one
    template<class Result, class Write>
    bool rewriteIdentity(Result parsed, Write write, std::string& text, const Guid& guid, const std::vector<Guid>& formerGuids) {
        if (!parsed.ok) return false;
        auto& unknown = parsed.data.unknownKeys;
        unknown.erase(std::remove_if(unknown.begin(), unknown.end(), [](const auto& key) { return key.first == "formerGuids"; }), unknown.end());
        if (!formerGuids.empty()) {
            std::vector<Guid> sorted = formerGuids;
            std::sort(sorted.begin(), sorted.end());
            sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
            Json list = Json::array();
            for (const Guid& former : sorted) list.push_back(former.str());
            unknown.emplace_back("formerGuids", list.dump());
        }
        parsed.data.guid = guid.str();
        text = write(parsed.data);
        return true;
    }

    bool withAnimatorAssetIdentity(std::string& text, const Guid& guid, const std::vector<Guid>& formerGuids) {
        if (!animatorAssetParses(text)) return false;
        if (formerGuids.empty() && text.find("\"formerGuids\"") == std::string::npos) {
            return anim::replaceClipGuid(text, guid.str());
        }
        const std::string kind = anim::animatorAssetKind(text);
        if (kind == "controller") return rewriteIdentity(anim::parseController(text), static_cast<std::string (*)(const anim::ControllerDef&)>(&anim::writeController), text, guid, formerGuids);
        if (kind == "mask") return rewriteIdentity(anim::parseMask(text), &anim::writeMask, text, guid, formerGuids);
        return rewriteIdentity(anim::parseMontage(text), &anim::writeMontage, text, guid, formerGuids);
    }

    // the JSON asset parses as its own type; a broken or foreign file (a Unity YAML .anim, .mask
    // or .controller, broken JSON) is left alone: its error must keep its line
    bool parsesAsJsonAsset(const fs::path& path, const std::string& text) {
        if (editor::Util::isVertexAnimationFile(path.string())) {
            return vat::parseVat(text).ok();
        }
        if (editor::Util::isAnimatorAssetFile(path.string())) {
            return animatorAssetParses(text);
        }
        if (editor::Util::isProceduralMaterialFile(path.string())) {
            graph::LoadReport report;
            graph::load(text, report);
            return report.ok;
        }
        return anim::parseClip(text).ok();
    }

    // A clip's in-file identity: "guid" and, for an asset that took over others (R8), the
    // "formerGuids" array (an unknown key to AnimationClipIO, which writes it back as it is)
    bool clipIdentity(const std::string& text, Guid& guid, std::vector<Guid>& formerGuids) {
        guid = Guid::parse(anim::readClipGuid(text));
        if (text.find("\"formerGuids\"") != std::string::npos) {
            const Json root = Json::parse(text, nullptr, false);
            if (root.is_object() && root.contains("formerGuids") && root["formerGuids"].is_array()) {
                for (const Json& former : root["formerGuids"]) {
                    const Guid parsed = former.is_string() ? Guid::parse(former.get<std::string>()) : Guid();
                    if (!parsed.isNull()) formerGuids.push_back(parsed);
                }
            }
        }
        return !guid.isNull();
    }

    // `unknown` (a JSON asset's unknown top-level keys) holding `formerGuids` as the list; false
    // when it already did (the text needs no rewrite for them)
    bool setFormerGuids(std::vector<std::pair<std::string, std::string>>& unknown, const std::vector<Guid>& formerGuids) {
        const auto had = std::find_if(unknown.begin(), unknown.end(), [](const auto& key) { return key.first == "formerGuids"; });
        if (formerGuids.empty() && had == unknown.end()) {
            return false;
        }
        if (had != unknown.end()) {
            unknown.erase(had);
        }
        if (!formerGuids.empty()) {
            std::vector<Guid> sorted = formerGuids;
            std::sort(sorted.begin(), sorted.end());
            sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
            Json list = Json::array();
            for (const Guid& former : sorted) list.push_back(former.str());
            unknown.emplace_back("formerGuids", list.dump());
        }
        return true;
    }

    // The JSON asset's text (.anim clip, .vat Vertex Animation) with that identity: only the
    // "guid" member replaced or inserted after "version" (the rest byte-identical) while there
    // are no former GUIDs to write or drop; else the asset written again canonically. False for
    // a text that is not one (a Unity YAML .anim, broken JSON: its error must keep its line).
    bool withJsonAssetIdentity(const fs::path& path, std::string& text, const Guid& guid, const std::vector<Guid>& formerGuids) {
        // a graph file has no "version" member to insert after: written canonically, the identity
        // among its unknown keys, which the graph model keeps
        if (editor::Util::isProceduralMaterialFile(path.string())) {
            graph::LoadReport report;
            graph::Graph file = graph::load(text, report);
            if (!report.ok) {
                return false;
            }
            file.passthrough["guid"] = guid.str();
            file.passthrough.erase("formerGuids");
            if (!formerGuids.empty()) {
                std::vector<Guid> sorted = formerGuids;
                std::sort(sorted.begin(), sorted.end());
                sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
                graph::Json list = graph::Json::array();
                for (const Guid& former : sorted) list.push_back(former.str());
                file.passthrough["formerGuids"] = std::move(list);
            }
            text = graph::save(file);
            return true;
        }
        // the Animator's assets (S21): their own readers and writers
        if (editor::Util::isAnimatorAssetFile(path.string()) || !anim::animatorAssetKind(text).empty()) {
            return withAnimatorAssetIdentity(text, guid, formerGuids);
        }
        if (editor::Util::isVertexAnimationFile(path.string())) {
            vat::VatParseResult parsed = vat::parseVat(text);
            if (!parsed.ok()) {
                return false;
            }
            vat::VatData& data = *parsed.data;
            if (!setFormerGuids(data.unknownKeys, formerGuids)) {
                return anim::replaceClipGuid(text, guid.str());
            }
            data.guid = guid.str();
            text = vat::writeVat(data);
            return true;
        }
        anim::ClipParseResult parsed = anim::parseClip(text);
        if (!parsed.ok()) {
            return false;
        }
        ClipData& data = *parsed.data;
        auto& unknown = data.unknownKeys;
        if (!setFormerGuids(unknown, formerGuids)) {
            return anim::replaceClipGuid(text, guid.str());
        }
        data.guid = guid.str();
        text = anim::writeClip(data);
        return true;
    }

}

// ==================================================================== identity of files

AssetType editor::AssetDatabase::typeOf(const fs::path& path) const {
    const AssetTypeInfo* info = AssetTypes::find(toRelative(path), assetsDir);
    return info ? info->type : AssetType::Unknown;
}

bool editor::AssetDatabase::isTrackedFile(const fs::path& path) const {
    return typeOf(path) != AssetType::Unknown;
}

std::string editor::AssetDatabase::generateGuid() {
    static std::mutex generatorMutex;
    static std::mt19937_64 generator = [] {
        std::random_device device;
        std::seed_seq seed{device(), device(), device(), device(), device(), device(), device(), device()};
        return std::mt19937_64(seed);
    }();
    std::lock_guard<std::mutex> lock(generatorMutex);
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
        return editor::assetio::readFile(fullPath, text) ? anim::readClipGuid(text) : std::string();
    }
    std::ifstream file(fullPath, std::ios::binary);
    if (!file) {
        return {};
    }
    // GUIDs normally precede the entity data. Stop reading at that line instead of
    // loading and copying every prefab in full when the asset database opens.
    std::string line;
    bool decided = false;
    while (std::getline(file, line)) {
        if (isKeyLine(line, 0, "guid")) {
            return lineValue(line, 4);
        }
        if (!decided && startsFlowMap(line, decided)) {
            file.close();
            std::string guid;
            std::vector<std::string> formerGuids;
            flowIdentity(fullPath, guid, formerGuids);
            return guid;
        }
    }
    return {};
}

bool editor::AssetDatabase::readInFileIdentity(const fs::path& fullPath, Guid& guid, std::vector<Guid>& formerGuids) {
    guid = Guid();
    formerGuids.clear();
    if (isClipFile(fullPath)) {
        std::string text;
        return assetio::readFile(fullPath, text) && clipIdentity(text, guid, formerGuids);
    }
    std::ifstream file(fullPath, std::ios::binary);
    if (!file) {
        return false;
    }
    // `formerGuids:` follows `guid:` (both written by writeInFileIdentity and the encoders)
    std::string line;
    bool afterGuid = false;
    bool inBlock = false;
    bool decided = false;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!afterGuid) {
            if (isKeyLine(line, 0, "guid")) {
                guid = Guid::parse(lineValue(line, 4));
                afterGuid = true;
            } else if (!decided && startsFlowMap(line, decided)) {
                file.close();
                std::string flowGuid;
                std::vector<std::string> flowFormer;
                flowIdentity(fullPath, flowGuid, flowFormer);
                guid = Guid::parse(flowGuid);
                for (const std::string& former : flowFormer) {
                    const Guid parsed = Guid::parse(former);
                    if (!parsed.isNull()) formerGuids.push_back(parsed);
                }
                return !guid.isNull();
            }
            continue;
        }
        if (inBlock) {
            const size_t dash = line.find_first_not_of(' ');
            if (dash != std::string::npos && dash > 0 && line[dash] == '-') {
                const Guid former = Guid::parse(trimmed(line.substr(dash + 1)));
                if (!former.isNull()) formerGuids.push_back(former);
                continue;
            }
            break;
        }
        if (isKeyLine(line, 0, "formerGuids")) {
            const std::string value = lineValue(line, 11);
            if (value.empty()) {
                inBlock = true;
                continue;
            }
            parseGuidList(value, formerGuids);
        }
        break;
    }
    return !guid.isNull();
}

bool editor::AssetDatabase::writeInFileIdentity(const fs::path& fullPath, const Guid& guid, const std::vector<Guid>& formerGuids) {
    // The GUID goes into the bytes on disk: a save of another program that lands between the
    // read and the write (an agent editing the clip while the asset refresh adds its GUID) is
    // read again, not overwritten with what was read before it
    constexpr int ATTEMPTS = 5;
    for (int attempt = 0; attempt < ATTEMPTS; attempt++) {
        bool superseded = false;
        const bool ok = writeInFileIdentityOnce(fullPath, guid, formerGuids, &superseded);
        if (!superseded) return ok;
    }
    Out::warning("Could not write the GUID of %s: it kept changing while it was written", fullPath.string().c_str());
    return false;
}

bool editor::AssetDatabase::writeInFileIdentityOnce(const fs::path& fullPath, const Guid& guid, const std::vector<Guid>& formerGuids,
                                                    bool* superseded) {
    const assetio::ReadStamp read = assetio::stampOf(fullPath);
    std::string text;
    if (!read.exists || !assetio::readFile(fullPath, text)) {
        return false;
    }
    if (isClipFile(fullPath)) {
        // a file that is not a clip (a Unity YAML .anim, broken JSON: its error must keep its
        // line) is left alone
        if (!withJsonAssetIdentity(fullPath, text, guid, formerGuids)) {
            return false;
        }
        std::string error;
        if (!assetio::rewriteFile(fullPath, text, assetio::Durability::Document, read, superseded, &error)) {
            if (!*superseded) Out::warning("Could not write the GUID of %s: %s", fullPath.string().c_str(), error.c_str());
            return false;
        }
        return true;
    }
    const std::string newline = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::string identity = "guid: " + guid.str() + newline;
    if (!formerGuids.empty()) {
        identity += formerLine(formerGuids) + newline;
    }

    // The lines of the old identity: the guid line and a formerGuids line (or block) after it
    std::vector<std::pair<size_t, size_t>> lines; // begin, end (past '\n')
    for (size_t begin = 0; begin < text.size();) {
        size_t end = text.find('\n', begin);
        end = end == std::string::npos ? text.size() : end + 1;
        lines.push_back({begin, end});
        begin = end;
    }
    auto lineText = [&](size_t i) { return text.substr(lines[i].first, lines[i].second - lines[i].first); };

    std::string updated;
    bool placed = false;
    for (size_t i = 0; i < lines.size(); i++) {
        const std::string line = lineText(i);
        if (!placed && isKeyLine(line, 0, "guid")) {
            updated += identity;
            placed = true;
            continue;
        }
        if (isKeyLine(line, 0, "formerGuids")) {
            // dropped here: the identity block holds it; its block lines go with it
            size_t next = i + 1;
            while (next < lines.size()) {
                const std::string item = lineText(next);
                const size_t first = item.find_first_not_of(' ');
                if (first == std::string::npos || first == 0 || item[first] != '-') break;
                next++;
            }
            i = next - 1;
            continue;
        }
        updated += line;
    }

    if (!placed) {
        // Before the first line of the document's map: past comments, directives and a
        // document start marker
        size_t insertAt = 0;
        for (const auto& [begin, end] : lines) {
            const std::string line = text.substr(begin, end - begin);
            const size_t first = line.find_first_not_of(" \t\r\n");
            if (first == std::string::npos || line[first] == '#' || line[0] == '%' || line.compare(0, 3, "---") == 0) {
                insertAt = end;
                continue;
            }
            break;
        }
        std::string prefix = updated.substr(0, insertAt);
        if (!prefix.empty() && prefix.back() != '\n') {
            prefix += newline;
        }
        updated = prefix + identity + updated.substr(std::min(insertAt, updated.size()));
    }

    // Line edits suit the block style the editor writes; anything else (a flow style map)
    // is parsed and written again
    bool verified = false;
    try {
        YAML::Node check = YAML::Load(updated);
        verified = check.IsMap() && check["guid"] && Guid::parse(check["guid"].as<std::string>()) == guid;
    } catch (const std::exception&) {
        verified = false;
    }
    if (!verified) {
        try {
            const YAML::Node node = YAML::Load(text);
            if (!node.IsMap()) {
                return false;
            }
            // block style at the top, the identity first: the lines readGuid and
            // readInFileIdentity look for, and the line edits above work on the next write
            // (nested values keep their style)
            YAML::Node rewritten(YAML::NodeType::Map);
            rewritten.SetStyle(YAML::EmitterStyle::Block);
            rewritten.force_insert("guid", guid.str());
            if (!formerGuids.empty()) {
                YAML::Node list(YAML::NodeType::Sequence);
                list.SetStyle(YAML::EmitterStyle::Flow);
                for (const Guid& former : formerGuids) list.push_back(former.str());
                rewritten.force_insert("formerGuids", list);
            }
            for (const auto& entry : node) {
                if (entry.first.IsScalar() && (entry.first.Scalar() == "guid" || entry.first.Scalar() == "formerGuids")) {
                    continue;
                }
                rewritten.force_insert(entry.first, entry.second);
            }
            updated = YAML::Dump(rewritten) + "\n";
        } catch (const std::exception& e) {
            Out::warning("Could not write the GUID of %s: %s", fullPath.string().c_str(), e.what());
            return false;
        }
    }

    std::string error;
    if (!assetio::rewriteFile(fullPath, updated, assetio::Durability::Document, read, superseded, &error)) {
        if (!*superseded) Out::warning("Could not write the GUID of %s: %s", fullPath.string().c_str(), error.c_str());
        return false;
    }
    return true;
}

bool editor::AssetDatabase::writeGuid(const fs::path& fullPath, const std::string& guid) {
    Guid ignored;
    std::vector<Guid> formerGuids;
    readInFileIdentity(fullPath, ignored, formerGuids);
    return writeInFileIdentity(fullPath, Guid::parse(guid), formerGuids);
}

// ==================================================================== keys and entries

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

std::string editor::AssetDatabase::relativeKeyOf(const std::string& relative) const {
    return caseInsensitive ? lowerDatabaseKey(relative) : relative;
}

std::string editor::AssetDatabase::key(const fs::path& path) const {
    return relativeKeyOf(toRelative(path).generic_string());
}

bool editor::AssetDatabase::fileExists(const std::string& relative) const {
    std::error_code ec;
    return fs::is_regular_file(projectPath / relative, ec);
}

void editor::AssetDatabase::statFile(const std::string& relative, fs::file_time_type& time, uint64_t& size) const {
    std::error_code ec;
    const fs::path full = projectPath / relative;
#ifdef JASPER_ASSETDB_POSIX_STAT
    struct stat st;
    if (::stat(full.c_str(), &st) != 0) {
        time = {};
        size = 0;
        return;
    }
    const auto sinceEpoch = std::chrono::seconds(st.st_mtim.tv_sec) + std::chrono::nanoseconds(st.st_mtim.tv_nsec);
    time = fs::file_time_type::clock::from_sys(
        std::chrono::system_clock::time_point(std::chrono::duration_cast<std::chrono::system_clock::duration>(sinceEpoch)));
    size = static_cast<uint64_t>(st.st_size);
    return;
#endif
    time = fs::last_write_time(full, ec);
    if (ec) {
        time = {};
    }
    size = static_cast<uint64_t>(fs::file_size(full, ec));
    if (ec) {
        size = 0;
    }
}

void editor::AssetDatabase::insert(Entry entry) {
    const std::string k = relativeKeyOf(entry.path);
    auto old = byPath.find(k);
    if (old != byPath.end()) {
        auto guidIt = byGuid.find(old->second.guid);
        if (guidIt != byGuid.end() && guidIt->second == k) {
            liveEntries.erase(guidIt->first);
            byGuid.erase(guidIt);
        }
    }
    const Guid guid = entry.guid;
    byGuid[guid] = k;
    Entry& stored = byPath[k];
    stored = std::move(entry);
    liveEntries[guid] = &stored;
    version++;
}

void editor::AssetDatabase::erase(const std::string& relative) {
    const std::string k = relativeKeyOf(relative);
    auto it = byPath.find(k);
    if (it == byPath.end()) {
        return;
    }
    auto guidIt = byGuid.find(it->second.guid);
    if (guidIt != byGuid.end() && guidIt->second == k) {
        liveEntries.erase(guidIt->first);
        byGuid.erase(guidIt);
    }
    byPath.erase(it);
    version++;
}

void editor::AssetDatabase::reindexRedirects() {
    byFormer.clear();
    bySubAsset.clear();
    conflicts.clear();
    for (const auto& [k, entry] : byPath) {
        for (const Guid& former : entry.formerGuids) {
            auto live = byGuid.find(former);
            if (live != byGuid.end()) {
                conflicts.push_back({former, byPath[live->second].path, entry.path});
                continue;
            }
            byFormer[former] = k;
        }
        for (const SubAsset& sub : entry.subAssets) {
            bySubAsset[sub.guid] = {k, sub.key};
        }
    }
    for (const RedirectConflict& conflict : conflicts) {
        Out::warning("GUID %s is the GUID of %s and a former GUID of %s: references resolve to %s",
            conflict.guid.str().c_str(), conflict.livePath.c_str(), conflict.formerOwnerPath.c_str(), conflict.livePath.c_str());
    }
    version++;
}

void editor::AssetDatabase::indexRedirectsOf(const std::string& relative) {
    auto it = byPath.find(relativeKeyOf(relative));
    if (it == byPath.end()) {
        return;
    }
    for (const Guid& former : it->second.formerGuids) {
        if (!byGuid.count(former)) {
            byFormer[former] = it->first;
        }
    }
    for (const SubAsset& sub : it->second.subAssets) {
        bySubAsset[sub.guid] = {it->first, sub.key};
    }
    version++;
}

bool editor::AssetDatabase::guidInUse(const Guid& guid) const {
    return byGuid.count(guid) || byFormer.count(guid) || bySubAsset.count(guid);
}

Guid editor::AssetDatabase::freshGuid(const std::string& preferred) const {
    Guid guid = Guid::parse(preferred);
    while (guid.isNull() || guidInUse(guid)) {
        guid = Guid::parse(generateGuid());
    }
    return guid;
}

// ==================================================================== reading and writing identity

bool editor::AssetDatabase::readIdentity(const std::string& relative, AssetType type, Entry& entry, const Entry* cached) const {
    const AssetTypeInfo& info = AssetTypes::info(type);
    entry.path = relative;
    entry.type = type;
    statFile(relative, entry.writeTime, entry.size);
    const bool fileUnchanged = cached && cached->writeTime == entry.writeTime && cached->size == entry.size;
    if (cached && fileUnchanged) {
        entry.hasContentHash = cached->hasContentHash;
        entry.contentHash = cached->contentHash;
    }

    if (info.identity == AssetIdentity::InFile) {
        if (fileUnchanged && !cached->guid.isNull()) {
            entry.guid = cached->guid;
            entry.formerGuids = cached->formerGuids;
            return true;
        }
        return readInFileIdentity(projectPath / relative, entry.guid, entry.formerGuids);
    }

    const std::string metaRelative = AssetTypes::metaPathFor(relative).generic_string();
    std::error_code ec;
    // a meta open()'s walk found needs no check; any other is asked for (written since the
    // walk, or under another case on a case-insensitive file system)
    const bool walked = walkMetas && walkMetas->count(metaRelative) > 0;
    if (!walked && !fs::is_regular_file(projectPath / metaRelative, ec)) {
        entry.hasMeta = false;
        return false;
    }
    entry.hasMeta = true;
    statFile(metaRelative, entry.metaWriteTime, entry.metaSize);
    if (cached && cached->hasMeta && cached->metaWriteTime == entry.metaWriteTime && cached->metaSize == entry.metaSize && !cached->guid.isNull()) {
        entry.guid = cached->guid;
        entry.formerGuids = cached->formerGuids;
        entry.subAssets = cached->subAssets;
        entry.subAssetIndices = cached->subAssetIndices;
        return true;
    }
    std::string text;
    AssetMeta meta;
    std::string error;
    if (!assetio::readFile(projectPath / metaRelative, text) || !AssetMeta::parse(text, meta, &error)) {
        Out::warning("%s is not a valid meta file (%s): its asset gets a new identity", metaRelative.c_str(), error.c_str());
        return false;
    }
    entry.guid = meta.guid;
    entry.formerGuids = meta.formerGuids;
    entry.subAssets = toDatabaseSubAssets(meta.subAssets);
    // the sub-asset order of the cache, when it still describes these entries
    entry.subAssetIndices.clear();
    if (cached && cached->subAssets.size() == cached->subAssetIndices.size()) {
        for (const SubAsset& sub : entry.subAssets) {
            size_t index = SIZE_MAX;
            for (size_t i = 0; i < cached->subAssets.size(); i++) {
                if (cached->subAssets[i].guid == sub.guid) index = cached->subAssetIndices[i];
            }
            entry.subAssetIndices.push_back(index);
        }
    }
    return true;
}

void editor::AssetDatabase::stampAfterIdentityWrite(Entry& entry, bool written) const {
    statFile(entry.path, entry.writeTime, entry.size);
    if (!written || readOnly || AssetTypes::info(entry.type).identity != AssetIdentity::InFile) {
        return;
    }
    Guid guid;
    std::vector<Guid> formerGuids;
    const bool found = readInFileIdentity(projectPath / entry.path, guid, formerGuids);
    fs::file_time_type time;
    uint64_t size = 0;
    statFile(entry.path, time, size);
    if (!found || guid != entry.guid || time != entry.writeTime || size != entry.size) {
        entry.writeTime = fs::file_time_type{};
        entry.size = 0;
    }
}

bool editor::AssetDatabase::writeIdentity(const Entry& entry) const {
    // read-only: the identity lives in memory (a legacy file's GUID is the one an editor
    // upgrading it writes, R3)
    if (readOnly) {
        return true;
    }
    const AssetTypeInfo& info = AssetTypes::info(entry.type);
    if (info.identity == AssetIdentity::InFile) {
        Guid current;
        std::vector<Guid> currentFormer;
        readInFileIdentity(projectPath / entry.path, current, currentFormer);
        std::vector<Guid> wanted = entry.formerGuids;
        std::sort(wanted.begin(), wanted.end());
        std::sort(currentFormer.begin(), currentFormer.end());
        if (current == entry.guid && currentFormer == wanted) {
            return true;
        }
        return writeInFileIdentity(projectPath / entry.path, entry.guid, entry.formerGuids);
    }

    const fs::path metaPath = projectPath / AssetTypes::metaPathFor(entry.path);
    AssetMeta meta;
    std::string text;
    if (assetio::readFile(metaPath, text) && !AssetMeta::parse(text, meta)) {
        meta = AssetMeta();
    }
    meta.guid = entry.guid;
    meta.formerGuids.clear();
    for (const Guid& former : entry.formerGuids) {
        meta.addFormerGuid(former);
    }
    std::vector<AssetMeta::SubAsset> subAssets;
    for (const SubAsset& sub : entry.subAssets) {
        subAssets.push_back({sub.key, sub.guid, sub.name});
    }
    meta.setSubAssets(std::move(subAssets));
    // A new meta holding only what its path derives (the legacy GUID, sub-asset GUIDs derived
    // from it) is written again identically if a crash loses it: no sync for it, which keeps the
    // first open of a large project written before GUIDs fast (thousands of metas)
    const bool reproducible = text.empty() && entry.formerGuids.empty() &&
                              entry.guid == Guid::parse(legacyGuid(entry.path));
    std::string error;
    if (!assetio::writeFile(metaPath, meta.serialize(),
                            reproducible ? assetio::Durability::Cache : assetio::Durability::Document, &error)) {
        Out::warning("Could not write %s: %s", metaPath.string().c_str(), error.c_str());
        return false;
    }
    return true;
}

bool editor::AssetDatabase::refreshSubAssets(Entry& entry, const Entry* previous) const {
    if (!AssetTypes::info(entry.type).hasSubAssets) {
        const bool had = !entry.subAssets.empty();
        entry.subAssets.clear();
        entry.subAssetIndices.clear();
        return had;
    }
    ModelContents contents;
    scanModelFile(projectPath / entry.path, contents);

    // §6.4: match by key, then an unmatched old entry to an unmatched new one of the same
    // kind at the same index; what stays unmatched is dropped
    const std::vector<SubAsset> old = previous ? previous->subAssets : entry.subAssets;
    const std::vector<size_t> oldIndices = previous ? previous->subAssetIndices : entry.subAssetIndices;
    std::vector<bool> oldUsed(old.size(), false);
    std::vector<SubAsset> fresh;
    std::vector<size_t> freshIndices;
    std::vector<bool> matched(contents.subAssets.size(), false);
    fresh.resize(contents.subAssets.size());
    freshIndices.resize(contents.subAssets.size());

    for (size_t i = 0; i < contents.subAssets.size(); i++) {
        const ModelSubAsset& sub = contents.subAssets[i];
        fresh[i] = {sub.key, Guid(), sub.name, sub.kind};
        freshIndices[i] = sub.index;
        for (size_t o = 0; o < old.size(); o++) {
            if (!oldUsed[o] && old[o].key == sub.key) {
                fresh[i].guid = old[o].guid;
                oldUsed[o] = true;
                matched[i] = true;
                break;
            }
        }
    }
    auto oldIndex = [&](size_t o) -> size_t {
        SubAssetKind kind;
        std::string name;
        long index;
        if (parseSubAssetKey(old[o].key, kind, name, index) && index >= 0) {
            return static_cast<size_t>(index);
        }
        return o < oldIndices.size() ? oldIndices[o] : SIZE_MAX;
    };
    for (size_t i = 0; i < contents.subAssets.size(); i++) {
        if (matched[i]) continue;
        for (size_t o = 0; o < old.size(); o++) {
            if (!oldUsed[o] && old[o].kind == fresh[i].kind && oldIndex(o) == freshIndices[i]) {
                fresh[i].guid = old[o].guid;
                oldUsed[o] = true;
                matched[i] = true;
                break;
            }
        }
        if (!matched[i]) {
            fresh[i].guid = Guid::derive(entry.guid, fresh[i].key);
        }
    }

    // sorted by key, as the meta keeps them
    std::vector<size_t> order(fresh.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return fresh[a].key < fresh[b].key; });
    std::vector<SubAsset> sorted;
    std::vector<size_t> sortedIndices;
    for (size_t i : order) {
        sorted.push_back(fresh[i]);
        sortedIndices.push_back(freshIndices[i]);
    }
    auto same = [](const std::vector<SubAsset>& a, const std::vector<SubAsset>& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++) {
            if (a[i].key != b[i].key || a[i].guid != b[i].guid || a[i].name != b[i].name) return false;
        }
        return true;
    };
    const bool changed = !same(sorted, entry.subAssets);
    entry.subAssets = std::move(sorted);
    entry.subAssetIndices = std::move(sortedIndices);
    return changed;
}

editor::AssetDatabase::Entry editor::AssetDatabase::identify(const std::string& relative, AssetType type, const std::string& preferredGuid) {
    Entry entry;
    entry.path = relative;
    entry.type = type;
    statFile(relative, entry.writeTime, entry.size);
    // R3: the legacy GUID when nothing uses it, so two machines upgrading the same commit agree
    entry.guid = freshGuid(preferredGuid);
    refreshSubAssets(entry, nullptr);
    const bool written = writeIdentity(entry);
    if (!written) {
        // a clip that does not parse (a Unity YAML .anim, broken JSON) is reported by the clip
        // checks, not here
        std::string text;
        if (!(isClipFile(relative) && assetio::readFile(projectPath / relative, text) && !parsesAsJsonAsset(relative, text))) {
            Out::warning("The GUID of %s is kept in memory only", relative.c_str());
        }
    }
    stampAfterIdentityWrite(entry, written);
    if (AssetTypes::info(type).identity == AssetIdentity::Sidecar && !readOnly) {
        entry.hasMeta = true;
        statFile(AssetTypes::metaPathFor(relative).generic_string(), entry.metaWriteTime, entry.metaSize);
    }
    return entry;
}

// ==================================================================== scanning

void editor::AssetDatabase::collect(const fs::path& fullPath, std::vector<std::string>& assets, std::vector<std::string>& metas,
                                    std::vector<fs::path>* temporaries) const {
    std::error_code ec;
    // the walk's paths start with the project's: cut the prefix (toRelative canonicalizes, a
    // few syscalls per path, which a walk of tens of thousands of files cannot pay)
    const std::string prefix = projectPath.lexically_normal().generic_string();
    auto relativeOf = [&](const fs::path& path) -> fs::path {
        const std::string text = path.generic_string();
        if (!prefix.empty() && text.size() > prefix.size() + 1 && text.compare(0, prefix.size(), prefix) == 0 &&
            (prefix.back() == '/' || text[prefix.size()] == '/') && text.find("/./") == std::string::npos &&
            text.find("/../") == std::string::npos) {
            return fs::path(text.substr(prefix.size() + (prefix.back() == '/' ? 0 : 1)));
        }
        return toRelative(path);
    };
    auto consider = [&](const fs::path& path) {
        const fs::path relative = relativeOf(path);
        if (AssetTypes::isMetaFile(relative)) {
            if (relative.filename().string().rfind('.', 0) != 0) {
                metas.push_back(relative.generic_string());
            }
        } else if (AssetTypes::find(relative, assetsDir)) {
            assets.push_back(relative.generic_string());
        }
    };
    if (fs::is_regular_file(fullPath, ec)) {
        consider(fullPath);
        // a file's meta goes with it
        const fs::path meta = AssetTypes::metaPathFor(fullPath);
        if (fs::is_regular_file(meta, ec)) {
            consider(meta);
        }
        return;
    }
    if (!fs::is_directory(fullPath, ec)) {
        return;
    }
    for (fs::recursive_directory_iterator it(fullPath, fs::directory_options::skip_permission_denied, ec), end;
         it != end && !ec; it.increment(ec)) {
        const fs::directory_entry& entry = *it;
        std::error_code entryError;
        if (entry.is_directory(entryError)) {
            if (AssetTypes::isSkippedDirectory(entry.path().filename().string())) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (temporaries && io::isTemporarySiblingName(entry.path().filename().string())) {
            temporaries->push_back(entry.path());
            continue;
        }
        if (entry.is_regular_file(entryError)) {
            consider(entry.path());
        }
    }
    std::sort(assets.begin(), assets.end());
    std::sort(metas.begin(), metas.end());
}

void editor::AssetDatabase::collectOrphans(const std::vector<std::string>& metas, const std::map<std::string, Entry>& cached,
                                           const std::unordered_set<std::string>* presentKeys) {
    for (const std::string& metaRelative : metas) {
        const std::string assetRelative = AssetTypes::assetPathForMeta(metaRelative).generic_string();
        if ((presentKeys && presentKeys->count(relativeKeyOf(assetRelative))) || fileExists(assetRelative)) {
            continue; // its asset is here (or the meta belongs to a file that is not an asset)
        }
        const AssetTypeInfo* info = AssetTypes::find(assetRelative, assetsDir);
        if (info && info->identity == AssetIdentity::InFile) {
            continue;
        }
        std::string text;
        AssetMeta meta;
        if (!assetio::readFile(projectPath / metaRelative, text) || !AssetMeta::parse(text, meta)) {
            continue;
        }
        const std::string metaKey = relativeKeyOf(metaRelative);
        if (!orphans.count(metaKey)) {
            orphans[metaKey] = {metaRelative, assetRelative, meta.guid};
        }
        auto last = cached.find(relativeKeyOf(assetRelative));
        if (last != cached.end() && !orphanLast.count(relativeKeyOf(assetRelative))) {
            orphanLast[relativeKeyOf(assetRelative)] = last->second;
        }
    }
}

std::vector<editor::AssetDatabase::Change> editor::AssetDatabase::adoptOrphans(std::vector<std::string>& withoutIdentity) {
    std::vector<Change> changes;
    if (orphans.empty() || withoutIdentity.empty()) {
        return changes;
    }
    // R5: same extension and size, and the same content hash when the index has the orphan's;
    // without a hash, the same file name too. A match counts only when it is the only one on
    // both sides: one file for the orphan, one orphan for the file (two look-alike files leave
    // the orphan as it is)
    struct Candidates {
        std::vector<std::string> byHash, byName;
    };
    std::map<std::string, Candidates> ofFile;     // file -> orphan meta keys
    std::map<std::string, Candidates> ofOrphan;   // orphan meta key -> files
    for (const std::string& relative : withoutIdentity) {
        const AssetTypeInfo* info = AssetTypes::find(relative, assetsDir);
        if (!info || info->identity != AssetIdentity::Sidecar) {
            continue;
        }
        fs::file_time_type time;
        uint64_t size = 0;
        statFile(relative, time, size);
        const std::string extension = lowerDatabaseKey(fs::path(relative).extension().string());
        const std::string name = fs::path(relative).filename().string();
        bool fileHashed = false;
        uint64_t fileHash = 0;
        for (const auto& [metaKey, orphan] : orphans) {
            if (lowerDatabaseKey(fs::path(orphan.assetPath).extension().string()) != extension) {
                continue;
            }
            auto last = orphanLast.find(relativeKeyOf(orphan.assetPath));
            if (last == orphanLast.end() || last->second.size != size) {
                continue;
            }
            if (last->second.hasContentHash) {
                if (!fileHashed) {
                    fileHashed = ContentHash::ofFile(projectPath / relative, fileHash);
                }
                if (fileHashed && fileHash == last->second.contentHash) {
                    ofFile[relative].byHash.push_back(metaKey);
                    ofOrphan[metaKey].byHash.push_back(relative);
                }
            } else if (fs::path(orphan.assetPath).filename().string() == name) {
                ofFile[relative].byName.push_back(metaKey);
                ofOrphan[metaKey].byName.push_back(relative);
            }
        }
    }

    std::vector<std::string> remaining;
    for (const std::string& relative : withoutIdentity) {
        const AssetTypeInfo* info = AssetTypes::find(relative, assetsDir);
        auto found = ofFile.find(relative);
        if (!info || info->identity != AssetIdentity::Sidecar || found == ofFile.end()) {
            remaining.push_back(relative);
            continue;
        }
        std::string chosen;
        for (const std::string& metaKey : found->second.byHash) {
            // the same content: the first file (in path order) takes it
            if (orphans.find(metaKey) != orphans.end()) {
                chosen = metaKey;
                break;
            }
        }
        if (chosen.empty() && found->second.byHash.empty() && found->second.byName.size() == 1 &&
                ofOrphan[found->second.byName.front()].byName.size() == 1) {
            chosen = found->second.byName.front();
        }
        if (chosen.empty() || orphans.find(chosen) == orphans.end()) {
            remaining.push_back(relative);
            continue;
        }
        const OrphanMeta orphan = orphans[chosen];
        const fs::path newMeta = projectPath / AssetTypes::metaPathFor(relative);
        std::string error;
        if (!assetio::renameFile(projectPath / orphan.metaPath, newMeta, &error)) {
            Out::warning("Could not move %s to %s: %s", orphan.metaPath.c_str(), relative.c_str(), error.c_str());
            remaining.push_back(relative);
            continue;
        }
        orphans.erase(chosen);
        Entry entry;
        const Entry* previous = nullptr;
        auto last = orphanLast.find(relativeKeyOf(orphan.assetPath));
        if (last != orphanLast.end()) {
            previous = &last->second;
        }
        if (!readIdentity(relative, info->type, entry, nullptr)) {
            remaining.push_back(relative);
            continue;
        }
        if (previous) {
            entry.hasContentHash = previous->hasContentHash;
            entry.contentHash = previous->contentHash;
        }
        if (refreshSubAssets(entry, nullptr)) {
            writeIdentity(entry);
        }
        Out::info("%s moved outside the editor without its meta: it keeps its GUID (from %s)", relative.c_str(), orphan.metaPath.c_str());
        orphanLast.erase(relativeKeyOf(orphan.assetPath));
        changes.push_back({Change::Type::Moved, entry.guid.str(), relative, orphan.assetPath});
        insert(std::move(entry));
    }
    withoutIdentity = remaining;
    return changes;
}

// ==================================================================== cache

fs::path editor::AssetDatabase::cacheDir() const {
    return projectPath / ".jasperengine" / "assetdb";
}

fs::path editor::AssetDatabase::cacheFile() const {
    return cacheDir() / "index.json";
}

void editor::AssetDatabase::loadCache(std::map<std::string, Entry>& cached) const {
    std::string text;
    if (assetio::readFile(cacheFile(), text)) {
        Json root = Json::parse(text, nullptr, false);
        if (root.is_object() && root.value("version", 0) == databaseCacheVersion && root.contains("assets") && root["assets"].is_array()) {
            for (const Json& item : root["assets"]) {
                if (!item.is_object() || !item.contains("p") || !item.contains("g")) continue;
                Entry entry;
                entry.path = item.value("p", std::string());
                entry.type = assetTypeFromName(item.value("t", std::string()));
                entry.guid = Guid::parse(item.value("g", std::string()));
                if (item.contains("f") && item["f"].is_array()) {
                    for (const Json& former : item["f"]) {
                        if (former.is_string()) entry.formerGuids.push_back(Guid::parse(former.get<std::string>()));
                    }
                }
                if (item.contains("s") && item["s"].is_array()) {
                    for (const Json& sub : item["s"]) {
                        SubAsset subAsset;
                        subAsset.key = sub.value("k", std::string());
                        subAsset.guid = Guid::parse(sub.value("g", std::string()));
                        subAsset.name = sub.value("n", std::string());
                        std::string name;
                        long index;
                        if (!parseSubAssetKey(subAsset.key, subAsset.kind, name, index)) continue;
                        entry.subAssets.push_back(subAsset);
                        entry.subAssetIndices.push_back(sub.value("i", static_cast<size_t>(SIZE_MAX)));
                    }
                }
                entry.size = item.value("sz", static_cast<uint64_t>(0));
                entry.writeTime = timeFrom(item.value("mt", static_cast<int64_t>(0)));
                entry.hasMeta = item.contains("ms");
                entry.metaSize = item.value("ms", static_cast<uint64_t>(0));
                entry.metaWriteTime = timeFrom(item.value("mm", static_cast<int64_t>(0)));
                if (item.contains("h") && item["h"].is_string()) {
                    entry.hasContentHash = true;
                    entry.contentHash = std::stoull(item["h"].get<std::string>(), nullptr, 16);
                }
                if (!entry.path.empty() && !entry.guid.isNull()) {
                    cached[relativeKeyOf(entry.path)] = entry;
                }
            }
            return;
        }
    }
    // The cache before S04 only told who owned a duplicated prefab GUID
    std::error_code ec;
    const fs::path oldCache = projectPath / ".jasperengine" / "assetdb.yaml";
    if (fs::exists(oldCache, ec)) {
        try {
            YAML::Node node = YAML::LoadFile(oldCache.string());
            for (const YAML::Node& asset : node["assets"]) {
                if (asset["guid"] && asset["path"]) {
                    Entry entry;
                    entry.path = asset["path"].as<std::string>();
                    entry.guid = Guid::parse(asset["guid"].as<std::string>());
                    if (!entry.guid.isNull()) cached[relativeKeyOf(entry.path)] = entry;
                }
            }
        } catch (const std::exception& e) {
            Out::warning("Ignoring the asset database cache: %s", e.what());
        }
    }
}

void editor::AssetDatabase::saveCache() const {
    if (projectPath.empty() || readOnly) {
        return;
    }
    writeCache(snapshotCache());
}

editor::AssetDatabase::CacheSnapshot editor::AssetDatabase::snapshotCache() const {
    CacheSnapshot snapshot;
    snapshot.entries.reserve(byPath.size());
    for (const auto& [k, entry] : byPath) {
        snapshot.entries.push_back(entry);
    }
    // what is known of the assets whose metas are orphans, for R5 next session
    for (const auto& [k, entry] : orphanLast) {
        if (!byPath.count(k)) {
            snapshot.orphanEntries.push_back(entry);
        }
    }
    snapshot.caseInsensitive = caseInsensitive;
    snapshot.file = cacheFile();
    snapshot.legacyFile = projectPath / ".jasperengine" / "assetdb.yaml";
    return snapshot;
}

void editor::AssetDatabase::writeCache(const CacheSnapshot& snapshot) {
    if (snapshot.file.empty()) {
        return;
    }
    Json assets = Json::array();
    auto entryJson = [](const Entry& entry) {
        Json item;
        item["p"] = entry.path;
        item["t"] = assetTypeName(entry.type);
        item["g"] = entry.guid.str();
        if (!entry.formerGuids.empty()) {
            Json list = Json::array();
            for (const Guid& former : entry.formerGuids) list.push_back(former.str());
            item["f"] = list;
        }
        if (!entry.subAssets.empty()) {
            Json list = Json::array();
            for (size_t i = 0; i < entry.subAssets.size(); i++) {
                Json sub;
                sub["k"] = entry.subAssets[i].key;
                sub["g"] = entry.subAssets[i].guid.str();
                sub["n"] = entry.subAssets[i].name;
                if (i < entry.subAssetIndices.size() && entry.subAssetIndices[i] != SIZE_MAX) sub["i"] = entry.subAssetIndices[i];
                list.push_back(sub);
            }
            item["s"] = list;
        }
        item["sz"] = entry.size;
        item["mt"] = timeValue(entry.writeTime);
        if (entry.hasMeta) {
            item["ms"] = entry.metaSize;
            item["mm"] = timeValue(entry.metaWriteTime);
        }
        if (entry.hasContentHash) {
            char hex[17];
            snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(entry.contentHash));
            item["h"] = hex;
        }
        return item;
    };
    for (const Entry& entry : snapshot.entries) {
        assets.push_back(entryJson(entry));
    }
    for (const Entry& entry : snapshot.orphanEntries) {
        Json item = entryJson(entry);
        item["orphan"] = true;
        assets.push_back(item);
    }
    Json root;
    root["version"] = databaseCacheVersion;
    root["caseInsensitive"] = snapshot.caseInsensitive;
    root["assets"] = assets;
    std::string error;
    // the cache lives in the editor's internal folder, which an MCP call's deny-by-default guard
    // refuses otherwise (util/SafeWrite.h, EditorInternal; a token is per thread: named here)
    safewrite::EditorInternal ownCache(snapshot.file.parent_path());
    safewrite::EditorInternal ownLegacy(snapshot.legacyFile);
    if (!assetio::writeFile(snapshot.file, cacheText(root, "assets"), assetio::Durability::Cache, &error)) {
        Out::warning("Could not write the asset database cache: %s", error.c_str());
    }
    std::error_code ec;
    fs::remove(snapshot.legacyFile, ec);
}

std::vector<fs::path> editor::AssetDatabase::takeTemporaryFiles() {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return std::move(temporaryFiles);
}

// ==================================================================== open / close

editor::AssetDatabase::~AssetDatabase() {
    stopHashing();
}

void editor::AssetDatabase::open(const fs::path& path, const fs::path& assetsDirectory) {
    // the hashing thread is joined before the lock is taken: it takes the lock itself
    close();
    std::lock_guard<std::recursive_mutex> lock(mutex);
    projectPath = path;
    assetsDir = assetsDirectory;
    caseInsensitive = detectCaseInsensitive(projectPath);

    std::map<std::string, Entry> cached;
    loadCache(cached);

    std::vector<std::string> assets, metas;
    temporaryFiles.clear();
    collect(projectPath, assets, metas, &temporaryFiles);
    const std::unordered_set<std::string> metaSet(metas.begin(), metas.end());
    walkMetas = &metaSet;
    struct ResetWalkMetas {
        const std::unordered_set<std::string>*& pointer;
        ~ResetWalkMetas() { pointer = nullptr; }
    } resetWalkMetas{walkMetas};

    struct Found {
        Entry entry;
        bool identified = false;
        bool legacy = false;   // no identity at all (as opposed to an invalid one)
        bool needsWrite = false;
    };
    // read in parallel (S06): each file's identity is its own stat, meta and cache entry; the
    // helpers take no lock (the cache, the walk's metas and the entries are only read)
    std::vector<Found> found(assets.size());
    jobs::ParallelForOptions parallel;
    parallel.grain = 64;
    jobs::parallelFor(assets.size(), [&](size_t index) {
        const std::string& relative = assets[index];
        Found& file = found[index];
        const AssetType type = AssetTypes::find(relative, assetsDir)->type;
        auto cachedIt = cached.find(relativeKeyOf(relative));
        const Entry* cachedEntry = cachedIt != cached.end() && cachedIt->second.type == type ? &cachedIt->second : nullptr;
        file.identified = readIdentity(relative, type, file.entry, cachedEntry);
        if (!file.identified) {
            const bool inFile = AssetTypes::info(type).identity == AssetIdentity::InFile;
            const std::string written = inFile ? readGuid(projectPath / relative) : std::string();
            if (!written.empty()) {
                Out::warning("%s has an invalid GUID \"%s\", it gets a new one", relative.c_str(), written.c_str());
            } else {
                file.legacy = true;
            }
        } else if (AssetTypes::info(type).hasSubAssets) {
            const bool fileUnchanged = cachedEntry && cachedEntry->writeTime == file.entry.writeTime && cachedEntry->size == file.entry.size;
            if (!fileUnchanged || file.entry.subAssets.size() != file.entry.subAssetIndices.size()) {
                file.needsWrite = refreshSubAssets(file.entry, nullptr);
            }
        }
    }, parallel);

    // A GUID found twice stays with the file that had it last session, else the older one
    std::unordered_map<Guid, std::vector<size_t>> byFoundGuid;
    for (size_t i = 0; i < found.size(); i++) {
        if (found[i].identified) {
            byFoundGuid[found[i].entry.guid].push_back(i);
        }
    }
    std::vector<size_t> duplicates;
    for (auto& [guid, indices] : byFoundGuid) {
        if (indices.size() < 2) {
            continue;
        }
        size_t keeper = indices[0];
        bool ownerFound = false;
        for (size_t index : indices) {
            auto owner = cached.find(relativeKeyOf(found[index].entry.path));
            if (owner != cached.end() && owner->second.guid == guid) {
                keeper = index;
                ownerFound = true;
                break;
            }
        }
        if (!ownerFound) {
            for (size_t index : indices) {
                const Entry& candidate = found[index].entry;
                const Entry& best = found[keeper].entry;
                if (candidate.writeTime < best.writeTime || (candidate.writeTime == best.writeTime && candidate.path < best.path)) {
                    keeper = index;
                }
            }
        }
        for (size_t index : indices) {
            if (index != keeper) {
                Out::warning("%s has the GUID of %s (a copy?), it gets a new one",
                    found[index].entry.path.c_str(), found[keeper].entry.path.c_str());
                duplicates.push_back(index);
            }
        }
    }
    for (size_t index : duplicates) {
        found[index].identified = false;
        found[index].legacy = false;
    }

    for (Found& file : found) {
        if (file.identified) {
            if (file.needsWrite) {
                writeIdentity(file.entry);
                statFile(AssetTypes::metaPathFor(file.entry.path).generic_string(), file.entry.metaWriteTime, file.entry.metaSize);
            }
            insert(std::move(file.entry));
        }
    }
    reindexRedirects();

    // Files without identity: an orphan meta that matches one is its meta moved (R5), the
    // rest get a new identity (R3)
    std::unordered_set<std::string> presentKeys;
    presentKeys.reserve(assets.size());
    for (const std::string& relative : assets) presentKeys.insert(relativeKeyOf(relative));
    collectOrphans(metas, cached, &presentKeys);
    std::vector<std::string> withoutIdentity;
    std::set<std::string> legacyFiles;
    for (const Found& file : found) {
        if (!file.identified) {
            withoutIdentity.push_back(file.entry.path);
            if (file.legacy) legacyFiles.insert(file.entry.path);
        }
    }
    adoptOrphans(withoutIdentity);
    for (const std::string& relative : withoutIdentity) {
        const AssetType type = AssetTypes::find(relative, assetsDir)->type;
        const std::string preferred = legacyFiles.count(relative) ? legacyGuid(relative) : std::string();
        Entry entry = identify(relative, type, preferred);
        // the redirect maps change only with sub-assets (the next identity's collision check
        // reads them); rebuilding them per file made a first open quadratic
        const bool redirects = !entry.subAssets.empty() || !entry.formerGuids.empty();
        insert(std::move(entry));
        if (redirects) reindexRedirects();
    }
    reindexRedirects();

    // a warm open that found everything as the cache describes it writes nothing
    bool cacheCurrent = withoutIdentity.empty() && orphans.empty() && cached.size() == byPath.size();
    for (auto it = byPath.begin(); cacheCurrent && it != byPath.end(); ++it) {
        auto old = cached.find(it->first);
        const Entry& now = it->second;
        cacheCurrent = old != cached.end() && old->second.guid == now.guid && old->second.path == now.path &&
                       old->second.type == now.type && old->second.writeTime == now.writeTime && old->second.size == now.size &&
                       old->second.hasMeta == now.hasMeta && old->second.metaWriteTime == now.metaWriteTime &&
                       old->second.metaSize == now.metaSize && old->second.formerGuids == now.formerGuids &&
                       old->second.subAssets.size() == now.subAssets.size() && old->second.hasContentHash == now.hasContentHash &&
                       old->second.contentHash == now.contentHash;
    }
    if (!cacheCurrent) {
        saveCache();
    }
    for (const auto& [k, entry] : byPath) {
        if (!entry.hasContentHash) {
            queueHash(entry.path);
        }
    }
}

void editor::AssetDatabase::close() {
    stopHashing();
    std::lock_guard<std::recursive_mutex> lock(mutex);
    const bool dirty = cacheDirty.exchange(false);
    if (dirty && !projectPath.empty()) {
        saveCache();
    }
    projectPath.clear();
    assetsDir.clear();
    byPath.clear();
    byGuid.clear();
    liveEntries.clear();
    byFormer.clear();
    bySubAsset.clear();
    orphans.clear();
    orphanLast.clear();
    conflicts.clear();
    version++;
}

bool editor::AssetDatabase::isOpen() const {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return !projectPath.empty();
}

uint64_t editor::AssetDatabase::getVersion() const {
    return version.load();
}

// ==================================================================== refresh

editor::AssetDatabase::DiskScan editor::AssetDatabase::scanDisk() const {
    DiskScan scan;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        if (!isOpen()) {
            return scan;
        }
        scan.version = version.load();
    }
    // projectPath and assetsDir change only in open(), which the editor never runs during a scan
    std::vector<std::string> assets;
    collect(projectPath, assets, scan.metas);
    // the walk found the metas: no existence check per asset
    const std::unordered_set<std::string> metaSet(scan.metas.begin(), scan.metas.end());
    scan.assets.reserve(assets.size());
    for (const std::string& relative : assets) {
        DiskScan::File file;
        file.relative = relative;
        statFile(relative, file.time, file.size);
        const AssetTypeInfo* info = AssetTypes::find(relative, assetsDir);
        if (info && info->identity == AssetIdentity::Sidecar) {
            const std::string metaRelative = AssetTypes::metaPathFor(relative).generic_string();
            file.metaPresent = metaSet.count(metaRelative) > 0;
            if (file.metaPresent) statFile(metaRelative, file.metaTime, file.metaSize);
        }
        scan.assets.push_back(std::move(file));
    }
    scan.valid = true;
    return scan;
}

std::vector<editor::AssetDatabase::Change> editor::AssetDatabase::refresh() {
    return refresh(scanDisk());
}

std::vector<editor::AssetDatabase::Change> editor::AssetDatabase::refresh(const DiskScan& scan) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    std::vector<Change> changes;
    if (!isOpen() || !scan.valid || scan.version != version.load()) {
        return changes;
    }
    bool dirty = false;

    const std::vector<std::string>& metas = scan.metas;
    std::unordered_set<std::string> present;
    for (const DiskScan::File& file : scan.assets) {
        present.insert(relativeKeyOf(file.relative));
    }

    std::vector<std::string> fresh;
    for (const DiskScan::File& file : scan.assets) {
        const std::string& relative = file.relative;
        const std::string k = relativeKeyOf(relative);
        auto it = byPath.find(k);
        const AssetType type = AssetTypes::find(relative, assetsDir)->type;
        if (it == byPath.end() || it->second.type != type) {
            if (it != byPath.end()) {
                changes.push_back({Change::Type::Removed, it->second.guid.str(), it->second.path, {}});
                erase(relative);
            }
            fresh.push_back(relative);
            continue;
        }
        Entry& known = it->second;
        const fs::file_time_type time = file.time, metaTime = file.metaTime;
        const uint64_t size = file.size, metaSize = file.metaSize;
        const bool sidecar = AssetTypes::info(type).identity == AssetIdentity::Sidecar;
        const bool metaPresent = file.metaPresent;
        const bool fileSame = time == known.writeTime && size == known.size;
        const bool metaSame = !sidecar || (metaPresent == known.hasMeta && metaTime == known.metaWriteTime && metaSize == known.metaSize);
        if (fileSame && metaSame) {
            continue;
        }
        dirty = true;
        if (sidecar && metaSame && known.hasMeta) {
            // only the content changed: the identity is in the meta, which did not (R21's rule:
            // a meta is read when its size or time changed), so a checkout reads no metas
            Entry updated = known;
            updated.writeTime = time;
            updated.size = size;
            updated.hasContentHash = false;
            if (refreshSubAssets(updated, &known)) {
                writeIdentity(updated);
                statFile(AssetTypes::metaPathFor(relative).generic_string(), updated.metaWriteTime, updated.metaSize);
            }
            changes.push_back({Change::Type::Modified, updated.guid.str(), relative, {}});
            insert(std::move(updated));
            queueHash(relative);
            continue;
        }
        Entry updated;
        const bool identified = readIdentity(relative, type, updated, nullptr);
        // A clip rewritten with another existing clip's GUID (JSON copied from it by an agent):
        // still this asset, its references keep finding it (written again with its own)
        auto owner = identified ? byGuid.find(updated.guid) : byGuid.end();
        const bool copiedClipGuid = isClipFile(relative) && owner != byGuid.end() && owner->second != relativeKeyOf(relative) &&
                                    byPath.count(owner->second) && fileExists(byPath.at(owner->second).path);
        // (a walk older than the file's deletion: the next one finds it gone)
        if (!identified && !fileExists(relative)) {
            continue;
        }
        if (!identified || copiedClipGuid) {
            // Written again without its identity (another tool, or the meta deleted): it is
            // still the same asset
            updated = known;
            statFile(relative, updated.writeTime, updated.size);
            if (!fileSame) {
                updated.hasContentHash = false;
                refreshSubAssets(updated, &known);
            }
            stampAfterIdentityWrite(updated, writeIdentity(updated));
            if (sidecar) {
                updated.hasMeta = true;
                statFile(AssetTypes::metaPathFor(relative).generic_string(), updated.metaWriteTime, updated.metaSize);
            }
            changes.push_back({Change::Type::Modified, updated.guid.str(), relative, {}});
            insert(std::move(updated));
            if (!fileSame) queueHash(relative);
            continue;
        }
        if (updated.guid != known.guid) {
            // Another asset now (a checkout replaced it)
            changes.push_back({Change::Type::Removed, known.guid.str(), known.path, {}});
            erase(relative);
            fresh.push_back(relative);
            continue;
        }
        if (!fileSame) {
            updated.hasContentHash = false;
            if (refreshSubAssets(updated, &known)) {
                writeIdentity(updated);
                if (sidecar) statFile(AssetTypes::metaPathFor(relative).generic_string(), updated.metaWriteTime, updated.metaSize);
            }
        } else {
            updated.hasContentHash = known.hasContentHash;
            updated.contentHash = known.contentHash;
            if (updated.subAssets.size() != updated.subAssetIndices.size()) {
                updated.subAssetIndices = known.subAssetIndices;
            }
        }
        changes.push_back({Change::Type::Modified, updated.guid.str(), relative, {}});
        insert(std::move(updated));
        if (!fileSame) queueHash(relative);
    }

    std::map<Guid, Entry> gone;
    for (auto it = byPath.begin(); it != byPath.end();) {
        if (present.count(it->first)) {
            ++it;
            continue;
        }
        gone[it->second.guid] = it->second;
        orphanLast[it->first] = it->second;
        auto guidIt = byGuid.find(it->second.guid);
        if (guidIt != byGuid.end() && guidIt->second == it->first) {
            liveEntries.erase(guidIt->first);
            byGuid.erase(guidIt);
        }
        it = byPath.erase(it);
        version++;
        dirty = true;
    }
    if (!gone.empty()) {
        reindexRedirects();
    }

    std::vector<std::string> withoutIdentity;
    for (const std::string& relative : fresh) {
        // the walk is older than this file's deletion (a later step took it back, with the folder
        // made for it): no asset, and writing its identity would make the meta and the folder again
        if (!fileExists(relative)) {
            present.erase(relativeKeyOf(relative));
            continue;
        }
        dirty = true;
        const AssetType type = AssetTypes::find(relative, assetsDir)->type;
        Entry entry;
        if (!readIdentity(relative, type, entry, nullptr)) {
            withoutIdentity.push_back(relative);
            continue;
        }
        auto moved = gone.find(entry.guid);
        if (moved != gone.end()) {
            // moved with its identity (git mv, a file manager moving the meta too)
            entry.hasContentHash = moved->second.hasContentHash && moved->second.size == entry.size;
            entry.contentHash = moved->second.contentHash;
            if (refreshSubAssets(entry, &moved->second)) writeIdentity(entry);
            changes.push_back({Change::Type::Moved, entry.guid.str(), relative, moved->second.path});
            orphanLast.erase(relativeKeyOf(moved->second.path));
            gone.erase(moved);
            insert(std::move(entry));
            continue;
        }
        if (guidInUse(entry.guid)) {
            // A copy of a file the database has: the copy gets a new GUID, its sub-assets
            // derived from it (R6)
            entry.guid = freshGuid();
            entry.subAssets.clear();
            entry.subAssetIndices.clear();
            refreshSubAssets(entry, nullptr);
            writeIdentity(entry);
            statFile(relative, entry.writeTime, entry.size);
            if (entry.hasMeta) statFile(AssetTypes::metaPathFor(relative).generic_string(), entry.metaWriteTime, entry.metaSize);
            changes.push_back({Change::Type::Added, entry.guid.str(), relative, {}});
            insert(std::move(entry));
            indexRedirectsOf(relative);
            queueHash(relative);
            continue;
        }
        if (refreshSubAssets(entry, nullptr)) writeIdentity(entry);
        changes.push_back({Change::Type::Added, entry.guid.str(), relative, {}});
        insert(std::move(entry));
        indexRedirectsOf(relative);
        queueHash(relative);
    }

    // orphan metas: those of assets moved or deleted without them
    orphans.clear();
    collectOrphans(metas, std::map<std::string, Entry>(), &present);
    std::vector<Change> adopted = adoptOrphans(withoutIdentity);
    for (const Change& change : adopted) {
        gone.erase(Guid::parse(change.guid));
        queueHash(change.path.generic_string());
    }
    changes.insert(changes.end(), adopted.begin(), adopted.end());
    for (const std::string& relative : withoutIdentity) {
        const AssetType type = AssetTypes::find(relative, assetsDir)->type;
        Entry entry = identify(relative, type, legacyGuid(relative));
        changes.push_back({Change::Type::Added, entry.guid.str(), relative, {}});
        insert(std::move(entry));
        indexRedirectsOf(relative);
        queueHash(relative);
    }

    for (const auto& [guid, entry] : gone) {
        changes.push_back({Change::Type::Removed, guid.str(), entry.path, {}});
    }
    // what was known of an asset whose meta is no longer an orphan is not needed
    for (auto it = orphanLast.begin(); it != orphanLast.end();) {
        const std::string metaKey = relativeKeyOf(AssetTypes::metaPathFor(it->second.path).generic_string());
        it = orphans.count(metaKey) ? std::next(it) : orphanLast.erase(it);
    }

    if (dirty) {
        reindexRedirects();
        requestCacheSave();
    }
    return changes;
}

// ==================================================================== queries

bool editor::AssetDatabase::contains(const std::string& guid) const {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return byGuid.count(Guid::parse(guid)) != 0;
}

fs::path editor::AssetDatabase::getPath(const std::string& guid) const {
    return getPath(Guid::parse(guid));
}

fs::path editor::AssetDatabase::getPath(const Guid& guid) const {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (guid.isNull()) {
        return fs::path();
    }
    auto it = byGuid.find(guid);
    return it != byGuid.end() ? fs::path(byPath.at(it->second).path) : fs::path();
}

std::string editor::AssetDatabase::getGuid(const fs::path& path) const {
    return getAssetGuid(path).str();
}

Guid editor::AssetDatabase::getAssetGuid(const fs::path& path) const {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    auto it = byPath.find(key(path));
    return it != byPath.end() ? it->second.guid : Guid();
}

bool editor::AssetDatabase::getInfo(const fs::path& path, AssetInfo& info) const {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    auto it = byPath.find(key(path));
    if (it == byPath.end()) {
        return false;
    }
    const Entry& entry = it->second;
    info = {entry.guid, entry.path, entry.type, entry.formerGuids, entry.subAssets, entry.size, entry.hasContentHash, entry.contentHash};
    return true;
}

bool editor::AssetDatabase::getInfo(const Guid& guid, AssetInfo& info) const {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    auto it = byGuid.find(guid);
    return it != byGuid.end() && getInfo(fs::path(byPath.at(it->second).path), info);
}

void editor::AssetDatabase::forEachAsset(const std::function<void(const AssetInfo&)>& visitor) const {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    for (const auto& [k, entry] : byPath) {
        visitor({entry.guid, entry.path, entry.type, entry.formerGuids, entry.subAssets, entry.size, entry.hasContentHash, entry.contentHash});
    }
}

size_t editor::AssetDatabase::assetCount() const {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return byPath.size();
}

editor::AssetDatabase::Resolution editor::AssetDatabase::lookupPath(const std::string& projectRelativePath) const {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    Resolution result;
    if (projectRelativePath.empty()) {
        return result;
    }
    std::string container, subKey;
    if (splitSubAssetPath(projectRelativePath, container, subKey)) {
        auto it = byPath.find(key(container));
        if (it != byPath.end()) {
            for (const SubAsset& sub : it->second.subAssets) {
                if (sub.key == subKey) {
                    return {ResolvedBy::SubAsset, sub.guid, it->second.path + "#" + sub.key, AssetType::SubAsset};
                }
            }
        }
        return result;
    }
    auto it = byPath.find(key(projectRelativePath));
    if (it != byPath.end()) {
        return {ResolvedBy::Live, it->second.guid, it->second.path, it->second.type};
    }
    return result;
}

editor::AssetDatabase::Resolution editor::AssetDatabase::resolve(const Guid& guid, const std::string& hint, AssetType expected,
                                                                  bool allowPathFallback, bool preferHint) const {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    Resolution result;
    if (!guid.isNull()) {
        auto live = liveEntries.find(guid);
        if (live != liveEntries.end()) {
            const Entry& entry = *live->second;
            return {ResolvedBy::Live, entry.guid, entry.path, entry.type};
        }
        auto former = byFormer.find(guid);
        if (former != byFormer.end()) {
            const Entry& entry = byPath.at(former->second);
            return {ResolvedBy::Former, entry.guid, entry.path, entry.type};
        }
        auto sub = bySubAsset.find(guid);
        if (sub != bySubAsset.end()) {
            const Entry& entry = byPath.at(sub->second.first);
            return {ResolvedBy::SubAsset, guid, entry.path + "#" + sub->second.second, AssetType::SubAsset};
        }
        if (allowPathFallback && !hint.empty()) {
            Resolution atHint = lookupPath(hint);
            if (atHint.found() && (expected == AssetType::Unknown || atHint.type == expected || atHint.type == AssetType::SubAsset)) {
                atHint.by = ResolvedBy::PathFallback;
                return atHint;
            }
        }
        return result;
    }
    if (hint.empty()) {
        return result;
    }
    if (preferHint) {
        Resolution atHint = lookupPath(hint);
        if (atHint.found()) {
            atHint.by = ResolvedBy::LegacyPath;
            return atHint;
        }
    }
    // a reference written before GUIDs: the asset that had the path when the project was upgraded
    std::string container = hint, subKey;
    const bool isSub = splitSubAssetPath(hint, container, subKey);
    // (or the asset a redirect gave that GUID to: Retarget All of a reference by path)
    const Guid legacyId = Guid::parse(legacyGuid(container));
    const std::string* legacyKey = nullptr;
    if (auto live = byGuid.find(legacyId); live != byGuid.end()) {
        legacyKey = &live->second;
    } else if (auto former = byFormer.find(legacyId); former != byFormer.end()) {
        legacyKey = &former->second;
    }
    if (legacyKey) {
        const Entry& entry = byPath.at(*legacyKey);
        if (!isSub) {
            return {ResolvedBy::LegacyPath, entry.guid, entry.path, entry.type};
        }
        for (const SubAsset& sub : entry.subAssets) {
            if (sub.key == subKey) {
                return {ResolvedBy::LegacyPath, sub.guid, entry.path + "#" + sub.key, AssetType::SubAsset};
            }
        }
    }
    Resolution atHint = lookupPath(hint);
    if (atHint.found()) {
        atHint.by = ResolvedBy::LegacyPath;
    }
    return atHint;
}

editor::AssetDatabase::Resolution editor::AssetDatabase::resolveInAssetsDir(const std::string& hint) const {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return hint.empty() || assetsDir.empty() ? Resolution() : lookupPath((assetsDir / FileUtils::pathFromUtf8(hint)).generic_string());
}

std::vector<editor::AssetDatabase::OrphanMeta> editor::AssetDatabase::getOrphanMetas() const {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    std::vector<OrphanMeta> list;
    for (const auto& [k, orphan] : orphans) {
        list.push_back(orphan);
    }
    return list;
}

std::vector<editor::AssetDatabase::RedirectConflict> editor::AssetDatabase::getRedirectConflicts() const {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return conflicts;
}

// ==================================================================== edits

bool editor::AssetDatabase::addFormerGuid(const fs::path& path, const Guid& former, std::string* error) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    auto it = byPath.find(key(path));
    if (it == byPath.end()) {
        if (error) *error = "no asset at " + toRelative(path).generic_string();
        return false;
    }
    Entry& entry = it->second;
    if (former.isNull() || former == entry.guid) {
        if (error) *error = "the GUID is the asset's own";
        return false;
    }
    if (std::find(entry.formerGuids.begin(), entry.formerGuids.end(), former) != entry.formerGuids.end()) {
        return true;
    }
    entry.formerGuids.push_back(former);
    std::sort(entry.formerGuids.begin(), entry.formerGuids.end());
    if (!writeIdentity(entry)) {
        entry.formerGuids.erase(std::find(entry.formerGuids.begin(), entry.formerGuids.end(), former));
        if (error) *error = "could not write the identity of " + entry.path;
        return false;
    }
    statFile(entry.path, entry.writeTime, entry.size);
    if (entry.hasMeta) statFile(AssetTypes::metaPathFor(entry.path).generic_string(), entry.metaWriteTime, entry.metaSize);
    reindexRedirects();
    requestCacheSave();
    return true;
}

bool editor::AssetDatabase::removeFormerGuid(const fs::path& path, const Guid& former, std::string* error) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    auto it = byPath.find(key(path));
    if (it == byPath.end()) {
        if (error) *error = "no asset at " + toRelative(path).generic_string();
        return false;
    }
    Entry& entry = it->second;
    auto found = std::find(entry.formerGuids.begin(), entry.formerGuids.end(), former);
    if (found == entry.formerGuids.end()) {
        return true;
    }
    entry.formerGuids.erase(found);
    if (!writeIdentity(entry)) {
        entry.formerGuids.push_back(former);
        std::sort(entry.formerGuids.begin(), entry.formerGuids.end());
        if (error) *error = "could not write the identity of " + entry.path;
        return false;
    }
    statFile(entry.path, entry.writeTime, entry.size);
    if (entry.hasMeta) statFile(AssetTypes::metaPathFor(entry.path).generic_string(), entry.metaWriteTime, entry.metaSize);
    reindexRedirects();
    requestCacheSave();
    return true;
}

bool editor::AssetDatabase::removeOrphanMeta(const std::string& metaPath, const fs::path& trashDir, std::string* error) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    auto it = orphans.find(relativeKeyOf(metaPath));
    if (it == orphans.end()) {
        if (error) *error = metaPath + " is not an orphan meta";
        return false;
    }
    std::error_code ec;
    const fs::path full = projectPath / it->second.metaPath;
    if (!trashDir.empty()) {
        fs::path target = trashDir / fs::path(it->second.metaPath).filename();
        for (int n = 1; fs::exists(target, ec); n++) {
            target = trashDir / (fs::path(it->second.metaPath).stem().string() + " (" + std::to_string(n) + ").meta");
        }
        if (!assetio::renameFile(full, target, error)) {
            return false;
        }
    } else if (!fs::remove(full, ec)) {
        if (error) *error = ec.message();
        return false;
    }
    orphanLast.erase(relativeKeyOf(it->second.assetPath));
    orphans.erase(it);
    version++;
    requestCacheSave();
    return true;
}

std::vector<editor::AssetDatabase::Change> editor::AssetDatabase::moveAssets(const fs::path& oldPath, const fs::path& newPath) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    std::vector<Change> changes;
    if (!isOpen()) {
        return changes;
    }
    const std::string oldRelative = toRelative(oldPath).generic_string();
    const std::string newRelative = toRelative(newPath).generic_string();
    if (oldRelative.empty() || newRelative.empty()) {
        return changes;
    }
    const std::string oldKey = relativeKeyOf(oldRelative);

    std::vector<Entry> moved;
    for (const auto& [k, entry] : byPath) {
        if (k == oldKey || k.rfind(oldKey + "/", 0) == 0) {
            moved.push_back(entry);
        }
    }
    for (const Entry& entry : moved) {
        erase(entry.path);
    }
    for (Entry entry : moved) {
        const std::string oldEntryPath = entry.path;
        const std::string updated = newRelative + entry.path.substr(oldRelative.size());
        const AssetTypeInfo* info = AssetTypes::find(updated, assetsDir);
        if (info && info->type == entry.type && fileExists(updated)) {
            entry.path = updated;
            statFile(updated, entry.writeTime, entry.size);
            if (entry.hasMeta) statFile(AssetTypes::metaPathFor(updated).generic_string(), entry.metaWriteTime, entry.metaSize);
            changes.push_back({Change::Type::Moved, entry.guid.str(), updated, oldEntryPath});
            insert(std::move(entry));
        } else {
            changes.push_back({Change::Type::Removed, entry.guid.str(), oldEntryPath, {}});
        }
    }
    reindexRedirects();

    // What became a tracked file with the new name
    std::vector<Change> imported = importAssets(projectPath / newRelative);
    changes.insert(changes.end(), imported.begin(), imported.end());

    requestCacheSave();
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
    for (const auto& [k, entry] : byPath) {
        if (k == removedKey || k.rfind(removedKey + "/", 0) == 0) {
            removed.push_back(entry.path);
            changes.push_back({Change::Type::Removed, entry.guid.str(), entry.path, {}});
        }
    }
    for (const std::string& relative : removed) {
        erase(relative);
    }
    if (!removed.empty()) {
        reindexRedirects();
        requestCacheSave();
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
    std::vector<std::string> assets, metas;
    collect(path.is_absolute() ? path : projectPath / path, assets, metas);
    for (const std::string& relative : assets) {
        const AssetType type = AssetTypes::find(relative, assetsDir)->type;
        Entry entry;
        const bool identified = readIdentity(relative, type, entry, nullptr);

        auto existing = byPath.find(relativeKeyOf(relative));
        if (existing != byPath.end()) {
            if (identified && existing->second.guid == entry.guid && !(copy && copyGuids && copyGuids->count(relative))) {
                entry.hasContentHash = false;
                if (refreshSubAssets(entry, &existing->second)) writeIdentity(entry);
                insert(std::move(entry));
                dirty = true;
                continue;
            }
            // Overwritten by another file: the asset it was is gone
            if (!identified || existing->second.guid != entry.guid) {
                changes.push_back({Change::Type::Removed, existing->second.guid.str(), relative, {}});
            }
            erase(relative);
            reindexRedirects();
        }
        dirty = true;

        // A redo of a copy gives the copy the GUID it got the first time
        if (copy && copyGuids) {
            auto recorded = copyGuids->find(relative);
            const Guid wanted = recorded != copyGuids->end() ? Guid::parse(recorded->second) : Guid();
            if (!wanted.isNull() && !guidInUse(wanted)) {
                const bool sameIdentity = identified && entry.guid == wanted;
                entry.path = relative;
                entry.type = type;
                entry.guid = wanted;
                entry.subAssets.clear();
                entry.subAssetIndices.clear();
                refreshSubAssets(entry, nullptr);
                if (!sameIdentity || AssetTypes::info(type).hasSubAssets) writeIdentity(entry);
                statFile(relative, entry.writeTime, entry.size);
                if (AssetTypes::info(type).identity == AssetIdentity::Sidecar) {
                    entry.hasMeta = true;
                    statFile(AssetTypes::metaPathFor(relative).generic_string(), entry.metaWriteTime, entry.metaSize);
                }
                changes.push_back({Change::Type::Added, wanted.str(), relative, {}});
                insert(std::move(entry));
                reindexRedirects();
                queueHash(relative);
                continue;
            }
        }

        if (!identified) {
            entry = identify(relative, type, legacyGuid(relative));
        } else if (guidInUse(entry.guid)) {
            auto other = byGuid.find(entry.guid);
            if (other == byGuid.end() || fileExists(byPath.at(other->second).path)) {
                // a copy: a new GUID, the sub-assets derived from it
                entry.guid = freshGuid();
                entry.subAssets.clear();
                entry.subAssetIndices.clear();
                refreshSubAssets(entry, nullptr);
                stampAfterIdentityWrite(entry, writeIdentity(entry));
                if (entry.hasMeta) statFile(AssetTypes::metaPathFor(relative).generic_string(), entry.metaWriteTime, entry.metaSize);
            } else {
                // The file the database knew is gone: this is it, moved
                const std::string otherPath = byPath.at(other->second).path;
                erase(otherPath);
                if (refreshSubAssets(entry, nullptr)) writeIdentity(entry);
                changes.push_back({Change::Type::Moved, entry.guid.str(), relative, otherPath});
                if (copy && copyGuids) (*copyGuids)[relative] = entry.guid.str();
                insert(std::move(entry));
                reindexRedirects();
                queueHash(relative);
                continue;
            }
        } else if (refreshSubAssets(entry, nullptr)) {
            writeIdentity(entry);
        }
        const std::string guid = entry.guid.str();
        changes.push_back({Change::Type::Added, guid, relative, {}});
        if (copy && copyGuids) {
            (*copyGuids)[relative] = guid;
        }
        insert(std::move(entry));
        reindexRedirects();
        queueHash(relative);
    }
    if (dirty) {
        reindexRedirects();
        requestCacheSave();
    }
    return changes;
}

void editor::AssetDatabase::registerAsset(const fs::path& path, const std::string& guidText) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    const Guid guid = Guid::parse(guidText);
    if (!isOpen() || guid.isNull()) {
        return;
    }
    const std::string relative = toRelative(path).generic_string();
    const AssetTypeInfo* info = AssetTypes::find(relative, assetsDir);
    if (!info) {
        return;
    }
    auto other = byGuid.find(guid);
    const bool tookOver = other != byGuid.end() && other->second != relativeKeyOf(relative);
    if (tookOver) {
        erase(byPath.at(other->second).path);
    }
    Entry entry;
    auto existing = byPath.find(relativeKeyOf(relative));
    if (existing != byPath.end() && existing->second.guid == guid) {
        entry = existing->second;
    }
    entry.path = relative;
    entry.type = info->type;
    entry.guid = guid;
    if (info->identity == AssetIdentity::InFile) {
        Guid written;
        std::vector<Guid> formerGuids;
        if (readInFileIdentity(projectPath / relative, written, formerGuids) && written == guid) {
            entry.formerGuids = formerGuids;
        } else {
            writeIdentity(entry);
        }
    } else {
        refreshSubAssets(entry, existing != byPath.end() ? &existing->second : nullptr);
        writeIdentity(entry);
        entry.hasMeta = true;
        statFile(AssetTypes::metaPathFor(relative).generic_string(), entry.metaWriteTime, entry.metaSize);
    }
    statFile(relative, entry.writeTime, entry.size);
    entry.hasContentHash = false;
    // the redirect maps change only with an entry that has (or had) former GUIDs or sub-assets
    const bool redirects = !entry.formerGuids.empty() || !entry.subAssets.empty() ||
                           (existing != byPath.end() && (!existing->second.formerGuids.empty() || !existing->second.subAssets.empty())) ||
                           tookOver;
    insert(std::move(entry));
    if (redirects) reindexRedirects();
    requestCacheSave();
    queueHash(relative);
}

// ==================================================================== content hashes

void editor::AssetDatabase::setBackgroundHashing(bool enabled) {
    hashingEnabled = enabled;
}

jobs::SerialQueue& editor::AssetDatabase::strand() {
    std::lock_guard<std::mutex> lock(strandMutex);
    if (!hashStrand) {
        hashStrand = std::make_unique<jobs::SerialQueue>("asset-hash", jobs::Lane::Background);
    }
    return *hashStrand;
}

void editor::AssetDatabase::requestCacheSave() {
    if (!hashingEnabled) {
        saveCache();
        return;
    }
    cacheDirty = true;
    // one save pending at a time: it runs after the hashes queued before it
    if (!savePending.exchange(true)) {
        strand().push([this] { saveCacheInBackground(); }, "asset-cache-save");
    }
}

void editor::AssetDatabase::saveCacheInBackground() {
    savePending = false;
    if (hashStop || !cacheDirty.exchange(false)) {
        return;
    }
    // copied under the lock, serialized and written without it: the main thread is not kept
    // waiting for the file
    if (readOnly) {
        return;
    }
    CacheSnapshot snapshot;
    {
        std::lock_guard<std::recursive_mutex> dbLock(mutex);
        if (!projectPath.empty()) snapshot = snapshotCache();
    }
    writeCache(snapshot);
}

void editor::AssetDatabase::queueHash(const std::string& relative) {
    if (!hashingEnabled) {
        return;
    }
    strand().push([this, relative] { hashOne(relative); }, "asset-hash");
}

void editor::AssetDatabase::hashOne(const std::string& relative) {
    if (hashStop || jobs::cancelRequested()) {
        return;
    }
    fs::path full;
    fs::file_time_type time{};
    uint64_t size = 0;
    {
        std::lock_guard<std::recursive_mutex> dbLock(mutex);
        auto it = byPath.find(relativeKeyOf(relative));
        if (it == byPath.end() || it->second.hasContentHash) return;
        full = projectPath / it->second.path;
        time = it->second.writeTime;
        size = it->second.size;
    }
    uint64_t hash = 0;
    if (!ContentHash::ofFile(full, hash)) {
        return;
    }
    {
        std::lock_guard<std::recursive_mutex> dbLock(mutex);
        auto it = byPath.find(relativeKeyOf(relative));
        if (it == byPath.end() || it->second.writeTime != time || it->second.size != size) return;
        it->second.hasContentHash = true;
        it->second.contentHash = hash;
    }
    // the hashes reach the cache with one write after the batch
    requestCacheSave();
}

void editor::AssetDatabase::waitForBackgroundWork() {
    jobs::SerialQueue* queue = nullptr;
    {
        std::lock_guard<std::mutex> lock(strandMutex);
        queue = hashStrand.get();
    }
    if (queue) queue->waitIdle();
}

void editor::AssetDatabase::stopHashing() {
    hashStop = true;
    std::unique_ptr<jobs::SerialQueue> queue;
    {
        std::lock_guard<std::mutex> lock(strandMutex);
        queue = std::move(hashStrand);
    }
    if (queue) {
        // jobs not started are dropped, the running one sees hashStop; the destructor waits
        queue->cancelPending();
        queue->waitIdle();
        queue.reset();
    }
    savePending = false;
    hashStop = false;
}
