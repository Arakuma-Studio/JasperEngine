// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "BuildReport.h"

#include "io/AtomicFile.h"
#include "texture/CookedTextures.h"
#include "util/FileUtils.h"

#include "miniz.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string_view>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    // a path's file name and its extension in lower case, as std::filesystem names them, without
    // building a path: the reader asks for 20,000 categories (R23)
    std::string_view fileNameOf(std::string_view path){
        const size_t slash = path.find_last_of('/');
        return slash == std::string_view::npos ? path : path.substr(slash + 1);
    }

    std::string lowerExtension(std::string_view path){
        const std::string_view name = fileNameOf(path);
        const size_t dot = name.rfind('.');
        if (dot == std::string_view::npos || dot == 0 || name == "..") return std::string();
        std::string extension(name.substr(dot));
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

    mz_bool buildReportCountBytes(const void*, int length, void* user){
        *static_cast<uint64_t*>(user) += (uint64_t)length;
        return MZ_TRUE;
    }

    // the file's size as `gzip -9` writes it: the deflate stream plus the 10-byte header and the
    // 8-byte trailer; streamed, so a large .wasm or content pack is never held whole
    uint64_t buildReportGzipBytes(const fs::path& file){
        std::ifstream in(file, std::ios::binary);
        if (!in)
            return 0;
        uint64_t deflated = 0;
        auto compressor = std::make_unique<tdefl_compressor>();
        const mz_uint flags = tdefl_create_comp_flags_from_zip_params(9, -MZ_DEFAULT_WINDOW_BITS, MZ_DEFAULT_STRATEGY);
        if (tdefl_init(compressor.get(), &buildReportCountBytes, &deflated, (int)flags) != TDEFL_STATUS_OKAY)
            return 0;
        std::vector<char> chunk(1 << 20);
        tdefl_status status = TDEFL_STATUS_OKAY;
        while (status == TDEFL_STATUS_OKAY){
            in.read(chunk.data(), (std::streamsize)chunk.size());
            const std::streamsize got = in.gcount();
            const bool last = !in;
            status = tdefl_compress_buffer(compressor.get(), chunk.data(), (size_t)got, last ? TDEFL_FINISH : TDEFL_NO_FLUSH);
            if (last)
                break;
        }
        return status == TDEFL_STATUS_DONE ? deflated + 18 : 0;
    }

}

const std::vector<std::string>& BuildReport::categoryNames(){
    static const std::vector<std::string> names = {"models", "textures", "audio", "scripts", "scenes", "engine", "other"};
    return names;
}

std::string BuildReport::categoryOf(const std::string& path){
    static const std::string cooked = std::string("assets/") + CookedTextures::kCookedDir + "/";
    static const std::string cookedModels = cooked + "models/";
    static const std::string nested = "/" + std::string(CookedTextures::kCookedDir) + "/";
    static const std::string nestedModels = nested + "models/";
    if (path.rfind(cookedModels, 0) == 0 || path.find(nestedModels) != std::string::npos)
        return "models";
    if (path.rfind(cooked, 0) == 0 || path.find(nested) != std::string::npos)
        return "textures";
    static const std::set<std::string, std::less<>> models = {".glb", ".gltf", ".fbx", ".obj", ".bin", ".vat"};
    static const std::set<std::string, std::less<>> textures = {".png", ".jpg", ".jpeg", ".tga", ".bmp", ".psd", ".psb", ".tif", ".tiff", ".gif", ".hdr", ".exr", ".ktx2", ".svg", ".webp", ".jlut"};
    static const std::set<std::string, std::less<>> audio = {".wav", ".ogg", ".mp3", ".flac", ".opus"};
    static const std::set<std::string, std::less<>> scripts = {".lua", ".luac"};
    static const std::set<std::string, std::less<>> scenes = {".scene", ".prefab", ".jscn", ".jpfb"};
    static const std::set<std::string, std::less<>> engine = {".exe", ".dll", ".so", ".dylib", ".wasm", ".js", ".a", ".apk", ".aab", ".pdb"};
    const std::string extension = lowerExtension(path);
    if (models.count(extension)) return "models";
    if (textures.count(extension)) return "textures";
    if (audio.count(extension)) return "audio";
    if (scripts.count(extension) || path.rfind("lua/", 0) == 0) return "scripts";
    if (scenes.count(extension) || path.rfind("scenes/", 0) == 0 || path.rfind("prefabs/", 0) == 0) return "scenes";
    const std::string_view name = fileNameOf(path);
    if (engine.count(extension) || name.rfind("jasperengine-player", 0) == 0 || name.find(".so.") != std::string_view::npos ||
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
        // '/' on every platform: categoryOf and the cooked sources match on it, and reports compare across platforms
        file.path = prefix + FileUtils::pathToGenericUtf8(it->path().lexically_relative(root));
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

    // What the scanner reads goes straight where it belongs: no document tree, each string moved.
    // A container the report does not use (or, for a comparison, does not need) is skipped unread.
    class ReportReader {
    public:
        ReportReader(BuildReport& report, bool comparisonOnly): report(report), comparisonOnly(comparisonOnly) {}

        bool valid() const { return version == 1 && sawTop; }

        // a value that is no unsigned integer or string (null, true, a float...)
        void other(){
            if (!frames.empty() && frames.back() == Frame::ReachItem) reachParts = 100;   // the entry is dropped
        }
        void string(std::string& value){
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
            } else if (top == Frame::StrippedItem){
                if (pending == "path") strippedItem.path = std::move(value);
                else if (pending == "reason") strippedItem.reason = std::move(value);
            }
        }
        // false: the scanner skips the object
        bool startObject(){
            if (frames.empty()){
                sawTop = true;
                frames.push_back(Frame::Top);
                return true;
            }
            const Frame top = frames.back();
            Frame next = Frame::Skip;
            if (top == Frame::Top && pending == "categories") next = Frame::Categories;
            else if (top == Frame::Top && pending == "reachability" && !comparisonOnly){ next = Frame::Reach; report.reachability.emplace(); }
            else if (top == Frame::Reach && pending == "assets") next = Frame::ReachAssets;
            else if (top == Frame::FileList){ next = Frame::FileItem; file = BuildReportFile(); fileHasCategory = fileHasAsset = false; }
            else if (top == Frame::LeftList){ next = Frame::LeftItem; left = BuildReportLeftOut(); }
            else if (top == Frame::BudgetList){ next = Frame::BudgetItem; budget = BuildReportBudget(); }
            else if (top == Frame::StrippedList){ next = Frame::StrippedItem; strippedItem = BuildReportStripped(); }
            if (next == Frame::Skip) return false;
            frames.push_back(next);
            return true;
        }
        void endObject(){
            const Frame top = frames.back();
            frames.pop_back();
            if (top == Frame::FileItem){
                if (!fileHasCategory) file.category = BuildReport::categoryOf(file.path);
                // a comparison reads no reachability: the asset a file ships is not asked
                if (!fileHasAsset && file.origin.empty() && !comparisonOnly) file.asset = impliedReportAsset(file);
                fileTarget->push_back(std::move(file));
            } else if (top == Frame::LeftItem){
                report.leftOut.push_back(std::move(left));
            } else if (top == Frame::BudgetItem){
                report.budgets.push_back(std::move(budget));
            } else if (top == Frame::StrippedItem){
                report.stripped.push_back(std::move(strippedItem));
            }
        }
        bool startArray(){
            const Frame top = frames.empty() ? Frame::Skip : frames.back();
            Frame next = Frame::Skip;
            if (top == Frame::Top && pending == "largest"){ next = Frame::FileList; fileTarget = &report.largest; }
            else if (top == Frame::Top && pending == "files"){ next = Frame::FileList; fileTarget = &report.files; report.hasFiles = true; }
            else if (top == Frame::Top && pending == "left_out" && !comparisonOnly) next = Frame::LeftList;
            else if (top == Frame::Top && pending == "budgets" && !comparisonOnly) next = Frame::BudgetList;
            else if (top == Frame::Top && pending == "stripped" && !comparisonOnly) next = Frame::StrippedList;
            else if (top == Frame::ReachAssets){ next = Frame::ReachItem; reachAsset = std::move(pending); reachParts = 0; }
            if (next == Frame::Skip) return false;
            frames.push_back(next);
            return true;
        }
        void endArray(){
            const Frame top = frames.back();
            frames.pop_back();
            // [via, field, kind] and the GameObject; the writer gives them in key order
            if (top == Frame::ReachItem && reachParts >= 3 && reachParts <= 4)
                report.reachability->emplace_hint(report.reachability->end(), std::move(reachAsset),
                                                  BuildReportReach{std::move(reachPart[0]), std::move(reachPart[1]), std::move(reachPart[2]),
                                                                   reachParts > 3 ? std::move(reachPart[3]) : std::string()});
        }
        void key(std::string& value){
            pending.swap(value);
        }
        void number(uint64_t value){
            const Frame top = frames.empty() ? Frame::Skip : frames.back();
            if (top == Frame::Top){
                if (pending == "version") version = (int64_t)value;
                else if (pending == "total_bytes") report.totalBytes = value;
                else if (pending == "download_bytes_gzip") report.downloadBytesGzip = value;
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
            } else if (top == Frame::StrippedItem){
                if (pending == "bytes") strippedItem.bytes = value;
            } else if (top == Frame::ReachItem){
                reachParts = 100;   // not a string: the entry is dropped
            }
        }

    private:
        enum class Frame { Top, Categories, FileList, FileItem, LeftList, LeftItem, Reach, ReachAssets, ReachItem, BudgetList, BudgetItem, StrippedList, StrippedItem, Skip };

        BuildReport& report;
        bool comparisonOnly = false;
        std::vector<Frame> frames;
        std::string pending;
        bool sawTop = false;
        int64_t version = 0;
        BuildReportFile file;
        bool fileHasCategory = false, fileHasAsset = false;
        std::vector<BuildReportFile>* fileTarget = nullptr;
        BuildReportLeftOut left;
        BuildReportBudget budget;
        BuildReportStripped strippedItem;
        std::string reachAsset;
        std::string reachPart[4];
        size_t reachParts = 0;
    };

    // A JSON scanner for build-report.json: strings are found with memchr and decoded into one
    // reused buffer, unsigned integers read in place, and a container the reader declines is
    // skipped by bracket counting. nlohmann's parser, even its SAX interface, read a 20,000-file
    // report at a third of the speed R23 needs.
    class ReportScanner {
    public:
        ReportScanner(const std::string& text, ReportReader& reader): p(text.data()), end(text.data() + text.size()), reader(reader) {}

        bool run(){
            space();
            if (p == end || *p != '{' || !value(0)) return false;
            space();
            return p == end;
        }

    private:
        const char* p;
        const char* end;
        ReportReader& reader;
        std::string buffer;
        static constexpr int kMaxDepth = 64;

        void space(){
            while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
        }
        bool literal(const char* word, size_t length){
            if ((size_t)(end - p) < length || std::memcmp(p, word, length) != 0) return false;
            p += length;
            return true;
        }
        static void utf8(std::string& out, uint32_t code){
            if (code < 0x80) out += (char)code;
            else if (code < 0x800){ out += (char)(0xC0 | (code >> 6)); out += (char)(0x80 | (code & 0x3F)); }
            else if (code < 0x10000){ out += (char)(0xE0 | (code >> 12)); out += (char)(0x80 | ((code >> 6) & 0x3F)); out += (char)(0x80 | (code & 0x3F)); }
            else { out += (char)(0xF0 | (code >> 18)); out += (char)(0x80 | ((code >> 12) & 0x3F)); out += (char)(0x80 | ((code >> 6) & 0x3F)); out += (char)(0x80 | (code & 0x3F)); }
        }
        bool hex4(uint32_t& out){
            if (end - p < 4) return false;
            out = 0;
            for (int i = 0; i < 4; i++){
                const char c = *p++;
                out <<= 4;
                if (c >= '0' && c <= '9') out |= (uint32_t)(c - '0');
                else if (c >= 'a' && c <= 'f') out |= (uint32_t)(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') out |= (uint32_t)(c - 'A' + 10);
                else return false;
            }
            return true;
        }
        // p at the opening quote; the decoded text in `buffer`
        bool string(){
            p++;
            buffer.clear();
            while (true){
                const char* quote = (const char*)std::memchr(p, '"', (size_t)(end - p));
                if (!quote) return false;
                const char* escape = (const char*)std::memchr(p, '\\', (size_t)(quote - p));
                if (!escape){
                    buffer.append(p, (size_t)(quote - p));
                    p = quote + 1;
                    return true;
                }
                buffer.append(p, (size_t)(escape - p));
                p = escape + 1;
                if (p == end) return false;
                const char c = *p++;
                switch (c){
                    case '"': buffer += '"'; break;
                    case '\\': buffer += '\\'; break;
                    case '/': buffer += '/'; break;
                    case 'b': buffer += '\b'; break;
                    case 'f': buffer += '\f'; break;
                    case 'n': buffer += '\n'; break;
                    case 'r': buffer += '\r'; break;
                    case 't': buffer += '\t'; break;
                    case 'u': {
                        uint32_t code = 0;
                        if (!hex4(code)) return false;
                        // a surrogate pair is one character
                        if (code >= 0xD800 && code < 0xDC00){
                            uint32_t low = 0;
                            if (!literal("\\u", 2) || !hex4(low) || low < 0xDC00 || low > 0xDFFF) return false;
                            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                        }
                        utf8(buffer, code);
                        break;
                    }
                    default: return false;
                }
            }
        }
        // a skipped string: only where it ends
        bool skipString(){
            p++;
            while (true){
                const char* quote = (const char*)std::memchr(p, '"', (size_t)(end - p));
                if (!quote) return false;
                // the quote is escaped when an odd number of backslashes precede it
                const char* back = quote;
                while (back > p && back[-1] == '\\') back--;
                p = quote + 1;
                if (((quote - back) & 1) == 0) return true;
            }
        }
        bool number(){
            const char* start = p;
            if (p < end && *p == '-') p++;
            uint64_t value = 0;
            bool digits = false, plain = *start != '-';
            while (p < end && *p >= '0' && *p <= '9'){
                value = value * 10 + (uint64_t)(*p - '0');
                p++;
                digits = true;
            }
            if (p < end && (*p == '.' || *p == 'e' || *p == 'E')){
                plain = false;
                while (p < end && (std::isdigit((unsigned char)*p) || *p == '.' || *p == 'e' || *p == 'E' || *p == '+' || *p == '-')) p++;
            }
            if (!digits) return false;
            if (plain) reader.number(value);
            else reader.other();
            return true;
        }
        // a container the reader declined: brackets counted, strings stepped over
        bool skipContainer(){
            int depth = 0;
            while (p < end){
                const char c = *p;
                if (c == '"'){
                    if (!skipString()) return false;
                    continue;
                }
                p++;
                if (c == '{' || c == '[') depth++;
                else if (c == '}' || c == ']'){
                    if (--depth == 0) return true;
                }
            }
            return false;
        }
        bool value(int depth){
            if (p == end || depth > kMaxDepth) return false;
            switch (*p){
                case '{': {
                    if (!reader.startObject()) return skipContainer();
                    p++;
                    space();
                    if (p < end && *p == '}'){ p++; reader.endObject(); return true; }
                    while (true){
                        space();
                        if (p == end || *p != '"' || !string()) return false;
                        reader.key(buffer);
                        space();
                        if (p == end || *p++ != ':') return false;
                        space();
                        if (!value(depth + 1)) return false;
                        space();
                        if (p == end) return false;
                        if (*p == ','){ p++; continue; }
                        if (*p++ != '}') return false;
                        reader.endObject();
                        return true;
                    }
                }
                case '[': {
                    if (!reader.startArray()) return skipContainer();
                    p++;
                    space();
                    if (p < end && *p == ']'){ p++; reader.endArray(); return true; }
                    while (true){
                        space();
                        if (!value(depth + 1)) return false;
                        space();
                        if (p == end) return false;
                        if (*p == ','){ p++; continue; }
                        if (*p++ != ']') return false;
                        reader.endArray();
                        return true;
                    }
                }
                case '"':
                    if (!string()) return false;
                    reader.string(buffer);
                    return true;
                case 't': reader.other(); return literal("true", 4);
                case 'f': reader.other(); return literal("false", 5);
                case 'n': reader.other(); return literal("null", 4);
                default: return number();
            }
        }
    };

}

Json BuildReport::categoriesJson(const std::map<std::string, uint64_t>& categories){
    Json out = Json::object();
    for (const std::string& name : categoryNames())
        out[name] = categories.count(name) ? categories.at(name) : 0;
    return out;
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
    if (downloadBytesGzip){ w.key("download_bytes_gzip", 1, first); w.number(downloadBytesGzip); }
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
    // S98 REQ-10: absent when nothing was stripped, so an old project's report is as it was
    if (!stripped.empty()){
        w.key("stripped", 1, first);
        w.lines(stripped, [&w](const BuildReportStripped& item){
            bool member = true;
            w.out += '{';
            w.member("bytes", member); w.number(item.bytes);
            w.member("path", member); w.string(item.path);
            w.member("reason", member); w.string(item.reason);
            w.out += '}';
        });
        w.key("stripped_bytes", 1, first); w.number(strippedBytes());
        w.key("stripped_count", 1, first); w.number(stripped.size());
    }
    w.key("total_bytes", 1, first); w.number(totalBytes);
    w.key("version", 1, first); w.number(1);
    w.close(0, '}');
    w.out += '\n';
    return std::move(w.out);
}

bool BuildReport::parse(const std::string& text, BuildReport& out, bool comparisonOnly){
    out = BuildReport();
    ReportReader reader(out, comparisonOnly);
    ReportScanner scanner(text, reader);
    if (!scanner.run() || !reader.valid()){
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

uint64_t BuildReport::strippedBytes() const{
    uint64_t bytes = 0;
    for (const BuildReportStripped& item : stripped) bytes += item.bytes;
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

namespace {

    // A report's files by key (the source asset, else the path), open-addressed in one array
    // sized up front, iterated in the list's order: an unordered_map's node per file was a third of
    // comparing two 20,000-file reports
    class ReportKeyedFiles {
    public:
        struct Side { const BuildReportFile* file = nullptr; uint64_t bytes = 0; };
        struct Entry { std::string_view key; Side side; };

        explicit ReportKeyedFiles(const std::vector<BuildReportFile>& list){
            size_t capacity = 16;
            while (capacity < list.size() * 2) capacity <<= 1;
            slots.assign(capacity, kEmpty);
            mask = capacity - 1;
            entries.reserve(list.size());
            for (const BuildReportFile& file : list){
                const std::string_view key = file.source.empty() ? std::string_view(file.path) : std::string_view(file.source);
                size_t at = std::hash<std::string_view>()(key) & mask;
                while (slots[at] != kEmpty && entries[slots[at]].key != key) at = (at + 1) & mask;
                if (slots[at] == kEmpty){
                    slots[at] = static_cast<uint32_t>(entries.size());
                    entries.push_back({key, {}});
                }
                Side& side = entries[slots[at]].side;
                if (!side.file) side.file = &file;
                side.bytes += file.bytes;
            }
        }

        const Side* find(std::string_view key) const{
            for (size_t at = std::hash<std::string_view>()(key) & mask; slots[at] != kEmpty; at = (at + 1) & mask){
                if (entries[slots[at]].key == key) return &entries[slots[at]].side;
            }
            return nullptr;
        }

        const std::vector<Entry>& all() const{ return entries; }

    private:
        static constexpr uint32_t kEmpty = 0xFFFFFFFFu;
        std::vector<uint32_t> slots;
        std::vector<Entry> entries;
        size_t mask = 0;
    };

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
    using Side = ReportKeyedFiles::Side;
    auto keyed = [](const std::vector<BuildReportFile>& list){ return ReportKeyedFiles(list); };
    const ReportKeyedFiles before = keyed(base.hasFiles ? base.files : base.largest);
    const ReportKeyedFiles after = keyed(head.hasFiles ? head.files : head.largest);
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
        for (const ReportKeyedFiles::Entry& entry : before.all())
            row(entry.key, &entry.side, after.find(entry.key));
        for (const ReportKeyedFiles::Entry& entry : after.all())
            if (!before.find(entry.key)) row(entry.key, nullptr, &entry.side);
    } else {
        // a report without the list names its largest only: a file missing from a 25-row list may
        // still have shipped, smaller, so only the base's largest are compared
        const ReportKeyedFiles largest = keyed(base.largest);
        for (const ReportKeyedFiles::Entry& entry : largest.all()){
            const Side* from = before.find(entry.key);
            const Side* to = after.find(entry.key);
            if (!to && !head.hasFiles) continue;
            row(entry.key, from, to);
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

bool BuildReport::load(const fs::path& file, BuildReport& out, std::string& error, bool comparisonOnly){
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in){
        error = "no " + std::string(kFileName) + " at " + FileUtils::pathToUtf8(file.parent_path());
        return false;
    }
    // read whole, once: a stringstream would copy a large report twice
    std::string text((size_t)std::max<std::streamoff>(0, in.tellg()), '\0');
    in.seekg(0);
    in.read(text.data(), (std::streamsize)text.size());
    if (!parse(text, out, comparisonOnly)){
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
    if (downloadBytesGzip)
        out += " (download " + formatBytes(downloadBytesGzip) + ", gzip estimate)";
    std::string parts;
    for (const std::string& name : categoryNames()){
        auto it = categories.find(name);
        if (it == categories.end() || it->second == 0)
            continue;
        parts += (parts.empty() ? "" : ", ") + name + " " + formatBytes(it->second);
    }
    if (!parts.empty()) out += "; " + parts;
    if (!stripped.empty()) out += "; " + std::to_string(stripped.size()) + " asset(s) left out (" + formatBytes(strippedBytes()) + ")";
    return out;
}

bool BuildReport::isWebDownload(const std::string& path){
    const std::string name = path.substr(path.rfind('/') + 1);
    const size_t slash = path.find('/');
    const std::string first = slash == std::string::npos ? std::string() : path.substr(0, slash);
    if (first == "licenses" || first == "threads" || first == "symbols")
        return false;
    if (name == kFileName || name == "content-manifest.json" || name == "_headers" || name == "coi-serviceworker.js")
        return false;
    if (name.rfind("jasperengine-player-mt", 0) == 0)
        return false;
    std::string upper = name;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c){ return (char)std::toupper(c); });
    auto endsWith = [&upper](const std::string& tail){ return upper.size() >= tail.size() && upper.compare(upper.size() - tail.size(), tail.size(), tail) == 0; };
    return !endsWith("LICENSE") && !endsWith("LICENSE.TXT") && !endsWith("LICENSE.MD") && upper.find("NOTICES") == std::string::npos;
}

uint64_t BuildReport::webDownloadGzipBytes(const fs::path& root){
    uint64_t total = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)){
        if (!it->is_regular_file(ec))
            continue;
        if (isWebDownload(FileUtils::pathToGenericUtf8(it->path().lexically_relative(root))))
            total += buildReportGzipBytes(it->path());
    }
    return total;
}

bool BuildReport::budgetFamilyOf(const std::string& platform, ModelFamily& family){
    if (platform == "web") family = ModelFamily::Web;
    else if (platform == "android" || platform == "ios") family = ModelFamily::Mobile;
    else if (platform == "windows" || platform == "linux" || platform == "macos" || platform == "desktop") family = ModelFamily::Desktop;
    else return false;
    return true;
}

uint64_t BuildReport::budgetedBytes() const{
    return platform == "web" ? downloadBytesGzip : totalBytes;
}

uint64_t BuildReport::budgetBytesOf(double megabytes){
    return std::isfinite(megabytes) && megabytes > 0.0 ? (uint64_t)std::llround(megabytes * 1048576.0) : 0;
}

std::vector<std::pair<std::string, uint64_t>> BuildReport::largestCategories(size_t count) const{
    std::vector<std::pair<std::string, uint64_t>> out;
    for (const std::string& name : categoryNames()){
        auto it = categories.find(name);
        if (it != categories.end() && it->second > 0)
            out.push_back(*it);
    }
    // ties keep the display order
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b){ return a.second > b.second; });
    if (out.size() > count)
        out.resize(count);
    return out;
}

std::string BuildReport::overBudgetMessage(uint64_t budgetBytes) const{
    ModelFamily family;
    const uint64_t actual = budgetedBytes();
    if (budgetBytes == 0 || actual <= budgetBytes || !budgetFamilyOf(platform, family))
        return std::string();
    std::string message = "the " + platform + " build " + (platform == "web" ? "downloads " + formatBytes(actual) + " (gzip estimate)"
                                                                            : "weighs " + formatBytes(actual)) +
                          ", over its Download Budget of " + formatBytes(budgetBytes) + " (player.download_budget_mb." + modelFamilyName(family) + ")";
    std::string parts;
    for (const auto& [name, bytes] : largestCategories(3))
        parts += (parts.empty() ? "" : ", ") + name + " " + formatBytes(bytes);
    return parts.empty() ? message : message + "; largest categories: " + parts;
}

void BuildReport::setLast(const fs::path& file){
    std::lock_guard<std::mutex> lock(lastMutex());
    lastReport() = file;
}

fs::path BuildReport::last(){
    std::lock_guard<std::mutex> lock(lastMutex());
    return lastReport();
}
