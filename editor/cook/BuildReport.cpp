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
    files.push_back(file);
    hasFiles = true;
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

namespace {

    Json reportFileJson(const BuildReportFile& file, bool withAsset){
        Json entry = {{"path", file.path}, {"category", file.category}, {"bytes", file.bytes}};
        if (!file.source.empty()){
            entry["source"] = file.source;
            entry["source_bytes"] = file.sourceBytes;
        }
        if (withAsset){
            // only what the path does not already say: assets/<rel> ships <rel>, a cooked file its source
            const std::string implied = !file.source.empty() ? file.source : file.path.rfind("assets/", 0) == 0 ? file.path.substr(7) : std::string();
            if (!file.origin.empty()) entry["origin"] = file.origin;
            else if (file.asset != implied) entry["asset"] = file.asset;
        }
        return entry;
    }

    BuildReportFile reportFileFromJson(const Json& entry){
        BuildReportFile file;
        file.path = entry.value("path", std::string());
        file.category = entry.value("category", std::string());
        file.bytes = entry.value("bytes", (uint64_t)0);
        file.source = entry.value("source", std::string());
        file.sourceBytes = entry.value("source_bytes", (uint64_t)0);
        file.origin = entry.value("origin", std::string());
        if (entry.contains("asset") && entry["asset"].is_string()) file.asset = entry["asset"].get<std::string>();
        else if (file.origin.empty()) file.asset = !file.source.empty() ? file.source : file.path.rfind("assets/", 0) == 0 ? file.path.substr(7) : std::string();
        return file;
    }

    // a container whose children are all scalars goes on one line; others open one line per child
    void appendReportLines(std::string& out, const Json& value, int depth){
        bool nested = false;
        if (value.is_structured())
            for (const Json& child : value) nested = nested || child.is_structured();
        if (!nested || value.empty()){
            out += value.dump();
            return;
        }
        const std::string indent((size_t)(depth + 1) * 2, ' ');
        out += value.is_object() ? "{\n" : "[\n";
        bool first = true;
        if (value.is_object()){
            for (const auto& [key, child] : value.items()){
                out += (first ? "" : ",\n") + indent + Json(key).dump() + ": ";
                appendReportLines(out, child, depth + 1);
                first = false;
            }
        } else {
            for (const Json& child : value){
                out += (first ? "" : ",\n") + indent;
                appendReportLines(out, child, depth + 1);
                first = false;
            }
        }
        out += "\n" + std::string((size_t)depth * 2, ' ') + (value.is_object() ? "}" : "]");
    }

}

Json BuildReport::toJson() const{
    Json categoryJson = Json::object();
    for (const std::string& name : categoryNames())
        categoryJson[name] = categories.count(name) ? categories.at(name) : 0;
    Json largestJson = Json::array();
    for (const BuildReportFile& file : largest)
        largestJson.push_back(reportFileJson(file, false));
    Json out = {{"version", 1}, {"platform", platform}, {"method", method}, {"total_bytes", totalBytes}, {"categories", categoryJson}, {"largest", largestJson}};
    if (!buildProfile.empty()) out["buildProfile"] = buildProfile;
    if (!id.empty()) out["id"] = id;
    if (!created.empty()) out["created"] = created;
    if (!editorVersion.empty()) out["editor_version"] = editorVersion;
    if (hasFiles){
        std::vector<const BuildReportFile*> sorted;
        sorted.reserve(files.size());
        for (const BuildReportFile& file : files) sorted.push_back(&file);
        std::sort(sorted.begin(), sorted.end(), [](const BuildReportFile* a, const BuildReportFile* b){ return a->path < b->path; });
        Json list = Json::array();
        for (const BuildReportFile* file : sorted) list.push_back(reportFileJson(*file, true));
        out["files"] = std::move(list);
        Json left = Json::array();
        for (const BuildReportLeftOut& item : leftOut)
            left.push_back({{"source", item.source}, {"bytes", item.bytes}, {"reason", item.reason}});
        out["left_out"] = std::move(left);
        out["left_out_bytes"] = leftOutBytes();
        if (reachability){
            Json assets = Json::object();
            for (const auto& [asset, reach] : *reachability){
                Json entry = {{"kind", reach.kind}};
                if (!reach.via.empty()) entry["via"] = reach.via;
                if (!reach.field.empty()) entry["field"] = reach.field;
                if (!reach.object.empty()) entry["object"] = reach.object;
                assets[asset] = std::move(entry);
            }
            out["reachability"] = {{"assets", std::move(assets)}};
        } else {
            out["reachability"] = nullptr;
        }
        Json budgetJson = Json::array();
        for (const BuildReportBudget& budget : budgets)
            budgetJson.push_back({{"platform", budget.platform}, {"category", budget.category}, {"limit_bytes", budget.limitBytes},
                                  {"bytes", budget.bytes}, {"over_bytes", budget.overBytes()}});
        out["budgets"] = std::move(budgetJson);
    }
    return out;
}

std::string BuildReport::toText() const{
    std::string out;
    appendReportLines(out, toJson(), 0);
    return out + "\n";
}

bool BuildReport::fromJson(const Json& json, BuildReport& out){
    if (!json.is_object() || json.value("version", 0) != 1)
        return false;
    out = BuildReport();
    out.platform = json.value("platform", std::string());
    out.method = json.value("method", std::string());
    out.buildProfile = json.value("buildProfile", std::string());
    out.totalBytes = json.value("total_bytes", (uint64_t)0);
    out.id = json.value("id", std::string());
    out.created = json.value("created", std::string());
    out.editorVersion = json.value("editor_version", std::string());
    if (json.contains("categories") && json["categories"].is_object())
        for (const auto& [name, bytes] : json["categories"].items())
            if (bytes.is_number_unsigned()) out.categories[name] = bytes.get<uint64_t>();
    if (json.contains("largest") && json["largest"].is_array())
        for (const Json& entry : json["largest"])
            if (entry.is_object()) out.largest.push_back(reportFileFromJson(entry));
    if (json.contains("files") && json["files"].is_array()){
        out.hasFiles = true;
        out.files.reserve(json["files"].size());
        for (const Json& entry : json["files"])
            if (entry.is_object()) out.files.push_back(reportFileFromJson(entry));
    }
    if (json.contains("left_out") && json["left_out"].is_array())
        for (const Json& entry : json["left_out"])
            if (entry.is_object())
                out.leftOut.push_back({entry.value("source", std::string()), entry.value("bytes", (uint64_t)0), entry.value("reason", std::string())});
    if (json.contains("reachability") && json["reachability"].is_object() && json["reachability"].contains("assets") &&
        json["reachability"]["assets"].is_object()){
        std::map<std::string, BuildReportReach> assets;
        for (const auto& [asset, entry] : json["reachability"]["assets"].items())
            if (entry.is_object())
                assets[asset] = {entry.value("via", std::string()), entry.value("field", std::string()), entry.value("kind", std::string()),
                                 entry.value("object", std::string())};
        out.reachability = std::move(assets);
    }
    if (json.contains("budgets") && json["budgets"].is_array())
        for (const Json& entry : json["budgets"])
            if (entry.is_object())
                out.budgets.push_back({entry.value("platform", std::string()), entry.value("category", std::string()),
                                       entry.value("limit_bytes", (uint64_t)0), entry.value("bytes", (uint64_t)0)});
    return true;
}

uint64_t BuildReport::leftOutBytes() const{
    uint64_t bytes = 0;
    for (const BuildReportLeftOut& item : leftOut) bytes += item.bytes;
    return bytes;
}

std::string BuildReport::leftOutReasonText(const std::string& reason){
    static const std::map<std::string, std::string> texts = {
        {"cooked_texture", "the texture cook ships it as je-cooked/"},
        {"cooked_model", "the model cook ships it as je-cooked/models/"},
        {"atlas_only", "only sprites read it: it ships in a Sprite Atlas's pages"},
        {"font_only", "a bitmap font reads it: it ships inside the .jfont"},
        {"aseprite_import", "ships as its import (je-imported/)"},
        {"title_art_unused", "no Realtime or Glyph Atlas title reads it (its baked PNGs ship)"},
        {"language_dropped", "a language the build leaves out"},
        {"procedural_material_baked", "ships as what it baked (PNGs and a .material)"},
        {"profile_excluded", "the Build Profile excludes it"},
        {"string_table_cooked", "ships cooked in assets/l10n"},
    };
    const auto it = texts.find(reason);
    return it == texts.end() ? reason : it->second;
}

const char* BuildReport::unreferencedNote(){
    return "no scene, prefab, material, project setting or script reaches it in the index (a script may still load it by a path "
           "string: the index sees `require` only)";
}

std::vector<BuildReportStep> BuildReport::chainOf(const std::string& asset) const{
    std::vector<BuildReportStep> chain;
    if (!reachability) return chain;
    std::set<std::string> seen;
    std::string at = asset;
    while (chain.size() < 32 && seen.insert(at).second){
        const auto it = reachability->find(at);
        if (it == reachability->end()) break;
        chain.push_back({at, it->second.field, it->second.kind, it->second.object});
        if (it->second.via.empty()) break;
        at = it->second.via;
    }
    return chain;
}

BuildReportReferrers BuildReport::referencedBy(const std::string& fileOrAsset) const{
    BuildReportReferrers out;
    const BuildReportFile* file = nullptr;
    for (const BuildReportFile& entry : files)
        if (entry.path == fileOrAsset){ file = &entry; break; }
    if (!file)
        for (const BuildReportFile& entry : files)
            if (!entry.asset.empty() && entry.asset == fileOrAsset){ file = &entry; break; }
    if (file){
        out.file = file->path;
        out.asset = file->asset;
        if (file->asset.empty()){
            out.reason = file->origin.empty() ? "generated" : file->origin;
            return out;
        }
    } else {
        out.asset = fileOrAsset;
        out.reason = "not_in_build";
        out.chain = chainOf(fileOrAsset);
        return out;
    }
    out.chain = chainOf(out.asset);
    out.reason = out.chain.empty() ? "unreferenced" : "reached";
    return out;
}

std::vector<const BuildReportFile*> BuildReport::unreferenced() const{
    std::vector<const BuildReportFile*> out;
    if (!reachability) return out;
    for (const BuildReportFile& file : files)
        if (!file.asset.empty() && !reachability->count(file.asset)) out.push_back(&file);
    std::sort(out.begin(), out.end(), [](const BuildReportFile* a, const BuildReportFile* b){
        return a->bytes != b->bytes ? a->bytes > b->bytes : a->path < b->path;
    });
    return out;
}

BuildReportDiff jasperengine::editor::compareBuildReports(const BuildReport& base, const BuildReport& head){
    BuildReportDiff diff;
    diff.platformDiffers = base.platform != head.platform;
    diff.partial = !base.hasFiles || !head.hasFiles;
    diff.totalBefore = base.totalBytes;
    diff.totalAfter = head.totalBytes;
    for (const std::string& name : BuildReport::categoryNames()){
        const auto before = base.categories.find(name);
        const auto after = head.categories.find(name);
        diff.categories[name] = {before == base.categories.end() ? 0 : before->second, after == head.categories.end() ? 0 : after->second};
    }
    // a cooked file's name changes with its content: its source asset is what stays the same (D2)
    struct Side { std::string path, category; uint64_t bytes = 0; };
    auto keyed = [](const std::vector<BuildReportFile>& list){
        std::map<std::string, Side> out;
        for (const BuildReportFile& file : list){
            Side& side = out[file.source.empty() ? file.path : file.source];
            if (side.path.empty()){ side.path = file.path; side.category = file.category; }
            side.bytes += file.bytes;
        }
        return out;
    };
    const std::map<std::string, Side> before = keyed(base.hasFiles ? base.files : base.largest);
    const std::map<std::string, Side> after = keyed(head.hasFiles ? head.files : head.largest);
    auto row = [&](const std::string& key, const Side* from, const Side* to){
        const uint64_t a = from ? from->bytes : 0, b = to ? to->bytes : 0;
        if (from && to && a == b){ diff.unchanged++; return; }
        BuildReportChange change;
        change.key = key;
        change.path = to ? to->path : from->path;
        change.category = to ? to->category : from->category;
        change.before = a;
        change.after = b;
        change.delta = (int64_t)b - (int64_t)a;
        change.change = !from ? "added" : !to ? "removed" : b > a ? "grown" : "shrunk";
        (change.change == "added" ? diff.added : change.change == "removed" ? diff.removed : change.change == "grown" ? diff.grown : diff.shrunk)++;
        diff.changes.push_back(std::move(change));
    };
    if (!diff.partial){
        for (const auto& [key, side] : before){
            const auto it = after.find(key);
            row(key, &side, it == after.end() ? nullptr : &it->second);
        }
        for (const auto& [key, side] : after)
            if (!before.count(key)) row(key, nullptr, &side);
    } else {
        // a report without the list names its largest only: a file missing from a 25-row list may
        // still have shipped, smaller, so only the base's largest are compared
        for (const auto& entry : keyed(base.largest)){
            const std::string& key = entry.first;
            const auto from = before.find(key);
            const auto to = after.find(key);
            if (to == after.end() && !head.hasFiles) continue;
            row(key, &from->second, to == after.end() ? nullptr : &to->second);
        }
    }
    std::sort(diff.changes.begin(), diff.changes.end(), [](const BuildReportChange& a, const BuildReportChange& b){
        const uint64_t da = (uint64_t)(a.delta < 0 ? -a.delta : a.delta), db = (uint64_t)(b.delta < 0 ? -b.delta : b.delta);
        return da != db ? da > db : a.key < b.key;
    });
    return diff;
}

bool BuildReport::write(const fs::path& file, std::string& error) const{
    io::AtomicWriteOptions options;
    options.durability = io::Durability::Cache;
    const io::WriteResult result = io::writeFileAtomic(file, toText(), options);
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
