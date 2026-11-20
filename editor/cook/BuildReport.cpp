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
#include <string_view>
#include <unordered_map>

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

    // the asset a file ships when its entry names none: a cooked file's source, assets/<rel>'s <rel>
    std::string impliedReportAsset(const BuildReportFile& file){
        if (!file.source.empty()) return file.source;
        return file.path.rfind("assets/", 0) == 0 ? file.path.substr(7) : std::string();
    }

    // build-report.json is written straight into one string: a document tree of 20,000 files and
    // their reachability costs ten times the writing (R22). The layout is the reader's: keys in
    // order, a container of scalars on one line, others one child per line.
    class ReportWriter {
    public:
        std::string out;

        void string(const std::string& text){
            out += '"';
            for (unsigned char c : text){
                switch (c){
                    case '"': out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\b': out += "\\b"; break;
                    case '\f': out += "\\f"; break;
                    case '\n': out += "\\n"; break;
                    case '\r': out += "\\r"; break;
                    case '\t': out += "\\t"; break;
                    default:
                        if (c < 0x20){
                            char escaped[8];
                            std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
                            out += escaped;
                        } else {
                            out += (char)c;
                        }
                }
            }
            out += '"';
        }
        void number(uint64_t value){
            char text[24];
            const int length = std::snprintf(text, sizeof(text), "%llu", (unsigned long long)value);
            out.append(text, (size_t)length);
        }
        // "key": at `depth`, after the previous member's ",\n" (first: none)
        void key(const char* name, int depth, bool& first){
            out += first ? "\n" : ",\n";
            first = false;
            out.append((size_t)depth * 2, ' ');
            out += '"';
            out += name;
            out += "\": ";
        }
        void member(const char* name, bool& first){
            out += first ? "" : ",";
            first = false;
            out += '"';
            out += name;
            out += "\":";
        }
        void close(int depth, char bracket){
            out += '\n';
            out.append((size_t)depth * 2, ' ');
            out += bracket;
        }
        void file(const BuildReportFile& entry, bool fileList){
            bool first = true;
            out += '{';
            if (fileList && entry.origin.empty() && entry.asset != impliedReportAsset(entry)){ member("asset", first); string(entry.asset); }
            member("bytes", first); number(entry.bytes);
            // the file list leaves out what its path says (largest keeps every key, for older readers)
            if (!fileList || entry.category != BuildReport::categoryOf(entry.path)){ member("category", first); string(entry.category); }
            if (fileList && !entry.origin.empty()){ member("origin", first); string(entry.origin); }
            member("path", first); string(entry.path);
            if (!entry.source.empty()){
                member("source", first); string(entry.source);
                member("source_bytes", first); number(entry.sourceBytes);
            }
            out += '}';
        }
        // an array of one-line items at depth 1, one per line
        template <typename List, typename Item>
        void lines(const List& list, Item item){
            if (list.empty()){
                out += "[]";
                return;
            }
            out += '[';
            bool first = true;
            for (const auto& entry : list){
                out += first ? "\n    " : ",\n    ";
                first = false;
                item(entry);
            }
            close(1, ']');
        }
    };

    // build-report.json read as it streams (SAX): no document tree, each value moved where it goes
    class ReportReader : public Json::json_sax_t {
    public:
        explicit ReportReader(BuildReport& report): report(report) {}

        bool valid() const { return version == 1 && sawTop; }

        bool null() override { return true; }
        bool boolean(bool) override { return true; }
        bool number_integer(Json::number_integer_t value) override { return value < 0 ? true : number((uint64_t)value); }
        bool number_unsigned(Json::number_unsigned_t value) override { return number(value); }
        bool number_float(Json::number_float_t, const Json::string_t&) override { return true; }
        bool binary(Json::binary_t&) override { return true; }
        bool string(Json::string_t& value) override {
            const Frame top = frames.empty() ? Frame::Skip : frames.back();
            if (top == Frame::Top){
                if (pending == "platform") report.platform = std::move(value);
                else if (pending == "method") report.method = std::move(value);
                else if (pending == "buildProfile") report.buildProfile = std::move(value);
                else if (pending == "id") report.id = std::move(value);
                else if (pending == "created") report.created = std::move(value);
                else if (pending == "editor_version") report.editorVersion = std::move(value);
            } else if (top == Frame::FileItem){
                if (pending == "path") file.path = std::move(value);
                else if (pending == "category"){ file.category = std::move(value); fileHasCategory = true; }
                else if (pending == "source") file.source = std::move(value);
                else if (pending == "origin") file.origin = std::move(value);
                else if (pending == "asset"){ file.asset = std::move(value); fileHasAsset = true; }
            } else if (top == Frame::LeftItem){
                if (pending == "source") left.source = std::move(value);
                else if (pending == "reason") left.reason = std::move(value);
            } else if (top == Frame::ReachItem){
                if (reachParts < 4) reachPart[reachParts] = std::move(value);
                reachParts++;
            } else if (top == Frame::BudgetItem){
                if (pending == "platform") budget.platform = std::move(value);
                else if (pending == "category") budget.category = std::move(value);
            }
            return true;
        }
        bool start_object(std::size_t) override {
            if (frames.empty()){
                sawTop = true;
                frames.push_back(Frame::Top);
                return true;
            }
            const Frame top = frames.back();
            Frame next = Frame::Skip;
            if (top == Frame::Top && pending == "categories") next = Frame::Categories;
            else if (top == Frame::Top && pending == "reachability"){ next = Frame::Reach; report.reachability.emplace(); }
            else if (top == Frame::Reach && pending == "assets") next = Frame::ReachAssets;
            else if (top == Frame::FileList){ next = Frame::FileItem; file = BuildReportFile(); fileHasCategory = fileHasAsset = false; }
            else if (top == Frame::LeftList){ next = Frame::LeftItem; left = BuildReportLeftOut(); }
            else if (top == Frame::BudgetList){ next = Frame::BudgetItem; budget = BuildReportBudget(); }
            frames.push_back(next);
            return true;
        }
        bool end_object() override {
            const Frame top = frames.back();
            frames.pop_back();
            if (top == Frame::FileItem){
                if (!fileHasCategory) file.category = BuildReport::categoryOf(file.path);
                if (!fileHasAsset && file.origin.empty()) file.asset = impliedReportAsset(file);
                fileTarget->push_back(std::move(file));
            } else if (top == Frame::LeftItem){
                report.leftOut.push_back(std::move(left));
            } else if (top == Frame::BudgetItem){
                report.budgets.push_back(std::move(budget));
            }
            return true;
        }
        bool start_array(std::size_t) override {
            const Frame top = frames.empty() ? Frame::Skip : frames.back();
            Frame next = Frame::Skip;
            if (top == Frame::Top && pending == "largest"){ next = Frame::FileList; fileTarget = &report.largest; }
            else if (top == Frame::Top && pending == "files"){ next = Frame::FileList; fileTarget = &report.files; report.hasFiles = true; }
            else if (top == Frame::Top && pending == "left_out") next = Frame::LeftList;
            else if (top == Frame::Top && pending == "budgets") next = Frame::BudgetList;
            else if (top == Frame::ReachAssets){ next = Frame::ReachItem; reachAsset = std::move(pending); reachParts = 0; }
            frames.push_back(next);
            return true;
        }
        bool end_array() override {
            const Frame top = frames.back();
            frames.pop_back();
            // [via, field, kind] and the GameObject; the writer gives them in key order
            if (top == Frame::ReachItem && reachParts >= 3 && reachParts <= 4)
                report.reachability->emplace_hint(report.reachability->end(), std::move(reachAsset),
                                                  BuildReportReach{std::move(reachPart[0]), std::move(reachPart[1]), std::move(reachPart[2]),
                                                                   reachParts > 3 ? std::move(reachPart[3]) : std::string()});
            return true;
        }
        bool key(Json::string_t& value) override {
            pending = std::move(value);
            return true;
        }
        bool parse_error(std::size_t, const std::string&, const Json::exception&) override { return false; }

    private:
        enum class Frame { Top, Categories, FileList, FileItem, LeftList, LeftItem, Reach, ReachAssets, ReachItem, BudgetList, BudgetItem, Skip };

        BuildReport& report;
        std::vector<Frame> frames;
        std::string pending;
        bool sawTop = false;
        int64_t version = 0;
        BuildReportFile file;
        bool fileHasCategory = false, fileHasAsset = false;
        std::vector<BuildReportFile>* fileTarget = nullptr;
        BuildReportLeftOut left;
        BuildReportBudget budget;
        std::string reachAsset;
        std::string reachPart[4];
        size_t reachParts = 0;

        bool number(uint64_t value){
            const Frame top = frames.empty() ? Frame::Skip : frames.back();
            if (top == Frame::Top){
                if (pending == "version") version = (int64_t)value;
                else if (pending == "total_bytes") report.totalBytes = value;
            } else if (top == Frame::Categories){
                report.categories[pending] = value;
            } else if (top == Frame::FileItem){
                if (pending == "bytes") file.bytes = value;
                else if (pending == "source_bytes") file.sourceBytes = value;
            } else if (top == Frame::LeftItem){
                if (pending == "bytes") left.bytes = value;
            } else if (top == Frame::BudgetItem){
                if (pending == "limit_bytes") budget.limitBytes = value;
                else if (pending == "bytes") budget.bytes = value;
            } else if (top == Frame::ReachItem){
                reachParts = 100;   // not a string: the entry is dropped
            }
            return true;
        }
    };

}

std::string BuildReport::toText() const{
    ReportWriter w;
    w.out.reserve(256 + (files.size() + largest.size()) * 120 + (reachability ? reachability->size() * 110 : 0) + leftOut.size() * 80);
    bool first = true;
    w.out += '{';
    if (hasFiles){
        w.key("budgets", 1, first);
        w.lines(budgets, [&w](const BuildReportBudget& budget){
            bool item = true;
            w.out += '{';
            w.member("bytes", item); w.number(budget.bytes);
            w.member("category", item); w.string(budget.category);
            w.member("limit_bytes", item); w.number(budget.limitBytes);
            w.member("over_bytes", item); w.number(budget.overBytes());
            w.member("platform", item); w.string(budget.platform);
            w.out += '}';
        });
    }
    if (!buildProfile.empty()){ w.key("buildProfile", 1, first); w.string(buildProfile); }
    // the categories in key order, every one of them (0 when empty)
    std::vector<std::string> names = categoryNames();
    std::sort(names.begin(), names.end());
    w.key("categories", 1, first);
    bool item = true;
    w.out += '{';
    for (const std::string& name : names){
        w.member(name.c_str(), item);
        const auto it = categories.find(name);
        w.number(it == categories.end() ? 0 : it->second);
    }
    w.out += '}';
    if (!created.empty()){ w.key("created", 1, first); w.string(created); }
    if (!editorVersion.empty()){ w.key("editor_version", 1, first); w.string(editorVersion); }
    if (hasFiles){
        std::vector<const BuildReportFile*> sorted;
        sorted.reserve(files.size());
        for (const BuildReportFile& file : files) sorted.push_back(&file);
        std::sort(sorted.begin(), sorted.end(), [](const BuildReportFile* a, const BuildReportFile* b){ return a->path < b->path; });
        w.key("files", 1, first);
        w.lines(sorted, [&w](const BuildReportFile* file){ w.file(*file, true); });
    }
    if (!id.empty()){ w.key("id", 1, first); w.string(id); }
    w.key("largest", 1, first);
    w.lines(largest, [&w](const BuildReportFile& file){ w.file(file, false); });
    if (hasFiles){
        w.key("left_out", 1, first);
        w.lines(leftOut, [&w](const BuildReportLeftOut& left){
            bool member = true;
            w.out += '{';
            w.member("bytes", member); w.number(left.bytes);
            w.member("reason", member); w.string(left.reason);
            w.member("source", member); w.string(left.source);
            w.out += '}';
        });
        w.key("left_out_bytes", 1, first);
        w.number(leftOutBytes());
    }
    w.key("method", 1, first); w.string(method);
    w.key("platform", 1, first); w.string(platform);
    if (hasFiles){
        w.key("reachability", 1, first);
        if (!reachability){
            w.out += "null";
        } else {
            // [via, field, kind] and the GameObject when there is one: a key per entry would cost
            // a fifth of a large report
            w.out += '{';
            bool inner = true;
            w.key("assets", 2, inner);
            if (reachability->empty()){
                w.out += "{}";
            } else {
                w.out += '{';
                bool entry = true;
                for (const auto& [asset, reach] : *reachability){
                    w.out += entry ? "\n      " : ",\n      ";
                    entry = false;
                    w.string(asset);
                    w.out += ": [";
                    w.string(reach.via); w.out += ',';
                    w.string(reach.field); w.out += ',';
                    w.string(reach.kind);
                    if (!reach.object.empty()){ w.out += ','; w.string(reach.object); }
                    w.out += ']';
                }
                w.close(2, '}');
            }
            w.close(1, '}');
        }
    }
    w.key("total_bytes", 1, first); w.number(totalBytes);
    w.key("version", 1, first); w.number(1);
    w.close(0, '}');
    w.out += '\n';
    return std::move(w.out);
}

bool BuildReport::parse(const std::string& text, BuildReport& out){
    out = BuildReport();
    ReportReader reader(out);
    if (!Json::sax_parse(text, &reader) || !reader.valid()){
        out = BuildReport();
        return false;
    }
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
    // a cooked file's name changes with its content: its source asset is what stays the same (D2).
    // Keyed by views into the reports' own strings: nothing is copied but the changed rows
    struct Side { const BuildReportFile* file = nullptr; uint64_t bytes = 0; };
    using Keyed = std::unordered_map<std::string_view, Side>;
    auto keyed = [](const std::vector<BuildReportFile>& list){
        Keyed out;
        out.reserve(list.size());
        for (const BuildReportFile& file : list){
            Side& side = out[file.source.empty() ? std::string_view(file.path) : std::string_view(file.source)];
            if (!side.file) side.file = &file;
            side.bytes += file.bytes;
        }
        return out;
    };
    const Keyed before = keyed(base.hasFiles ? base.files : base.largest);
    const Keyed after = keyed(head.hasFiles ? head.files : head.largest);
    auto row = [&](std::string_view key, const Side* from, const Side* to){
        const uint64_t a = from ? from->bytes : 0, b = to ? to->bytes : 0;
        if (from && to && a == b){ diff.unchanged++; return; }
        BuildReportChange change;
        change.key = std::string(key);
        change.path = to ? to->file->path : from->file->path;
        change.category = to ? to->file->category : from->file->category;
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
            const std::string_view key = entry.first;
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
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in){
        error = "no " + std::string(kFileName) + " at " + FileUtils::pathToUtf8(file.parent_path());
        return false;
    }
    // read whole, once: a stringstream would copy a large report twice
    std::string text((size_t)std::max<std::streamoff>(0, in.tellg()), '\0');
    in.seekg(0);
    in.read(text.data(), (std::streamsize)text.size());
    if (!parse(text, out)){
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
