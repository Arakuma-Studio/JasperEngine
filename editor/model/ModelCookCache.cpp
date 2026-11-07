// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ModelCookCache.h"

#include "htmlui/compat/Hash.h"
#include "io/AtomicFile.h"
#include "pool/MeshLodPool.h"

#include "meshoptimizer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <locale>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    int64_t fileTime(const fs::path& path){
        std::error_code ec;
        const auto time = fs::last_write_time(path, ec);
        return ec ? 0 : (int64_t)time.time_since_epoch().count();
    }

    bool readAll(const fs::path& path, std::vector<uint8_t>& out){
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return false;
        file.seekg(0, std::ios::end);
        const std::streamoff size = file.tellg();
        if (size < 0)
            return false;
        file.seekg(0);
        out.resize((size_t)size);
        file.read(reinterpret_cast<char*>(out.data()), size);
        return (bool)file || out.empty();
    }

    void touch(const fs::path& path){
        std::error_code ec;
        fs::last_write_time(path, fs::file_time_type::clock::now(), ec);
    }

    std::vector<std::string> stringsOf(const Json& json, const char* key){
        std::vector<std::string> out;
        if (json.contains(key) && json[key].is_array())
            for (const Json& item : json[key])
                if (item.is_string()) out.push_back(item.get<std::string>());
        return out;
    }

}

Json ModelGeometryStats::toJson() const{
    return {{"triangles", triangles}, {"vertices", vertices}, {"vertexBytes", vertexBytes}, {"indexBytes", indexBytes},
            {"acmr", acmr}, {"overdraw", overdraw}};
}

ModelGeometryStats ModelGeometryStats::fromJson(const Json& json){
    ModelGeometryStats out;
    if (!json.is_object())
        return out;
    out.triangles = json.value("triangles", 0LL);
    out.vertices = json.value("vertices", 0LL);
    out.vertexBytes = json.value("vertexBytes", (uint64_t)0);
    out.indexBytes = json.value("indexBytes", (uint64_t)0);
    out.acmr = json.value("acmr", 0.0);
    out.overdraw = json.value("overdraw", 0.0);
    return out;
}

Json ModelLodLevelStats::toJson() const{
    return {{"level", level}, {"triangles", triangles}, {"share", share}, {"error", error}, {"deviation", deviation}};
}

ModelLodLevelStats ModelLodLevelStats::fromJson(const Json& json){
    ModelLodLevelStats out;
    if (!json.is_object())
        return out;
    out.level = json.value("level", 1);
    out.triangles = json.value("triangles", 0LL);
    out.share = json.value("share", 0.0);
    out.error = json.value("error", 0.0);
    out.deviation = json.value("deviation", 0.0);
    return out;
}

std::string ModelCookRecord::visualChange() const{
    if (positionError <= 0.0 || modelSize <= 0.0)
        return "None";
    char text[96];
    std::snprintf(text, sizeof(text), "Up to %.3g%% of the model's size", positionError / modelSize * 100.0);
    return text;
}

namespace {

    const char* severityName(GltfDiagnosticSeverity severity){
        switch (severity){
            case GltfDiagnosticSeverity::Info: return "info";
            case GltfDiagnosticSeverity::Error: return "error";
            case GltfDiagnosticSeverity::Warning: default: return "warning";
        }
    }

}

Json editor::modelDiagnosticJson(const GltfDiagnostic& diagnostic){
    return {{"severity", severityName(diagnostic.severity)}, {"extension", diagnostic.extension}, {"part", diagnostic.part},
            {"message", diagnostic.message}, {"fix", diagnostic.fix}, {"cook_fixes", diagnostic.cookFixes}};
}

bool editor::modelDiagnosticFromJson(const Json& json, GltfDiagnostic& out){
    if (!json.is_object())
        return false;
    const std::string severity = json.value("severity", std::string("warning"));
    out.severity = severity == "error" ? GltfDiagnosticSeverity::Error : severity == "info" ? GltfDiagnosticSeverity::Info
                                                                                              : GltfDiagnosticSeverity::Warning;
    out.extension = json.value("extension", std::string());
    out.part = json.value("part", std::string());
    out.message = json.value("message", std::string());
    out.fix = json.value("fix", std::string());
    out.cookFixes = json.value("cook_fixes", false);
    return !out.message.empty();
}

Json ModelCookRecord::toJson() const{
    Json lodsJson = Json::array();
    for (const ModelLodLevelStats& level : lods)
        lodsJson.push_back(level.toJson());
    Json diagnosticsJson = Json::array();
    for (const GltfDiagnostic& diagnostic : diagnostics)
        diagnosticsJson.push_back(modelDiagnosticJson(diagnostic));
    return {{"version", kModelCookVersion}, {"key", key}, {"family", family}, {"compression", compression},
            {"sourceBytes", sourceBytes}, {"diskBytes", diskBytes}, {"before", before.toJson()}, {"after", after.toJson()},
            {"duplicatePercent", duplicatePercent}, {"meshes", meshes}, {"primitives", primitives},
            {"lodPrimitives", lodPrimitives}, {"lods", lodsJson}, {"modelSize", modelSize}, {"positionError", positionError},
            {"passThrough", passThrough}, {"requiredExtensions", requiredExtensions}, {"sourceExtensions", sourceExtensions},
            {"keptOrder", keptOrder}, {"warnings", warnings}, {"diagnostics", diagnosticsJson}, {"transcoded", transcoded},
            {"authoredLodPrimitives", authoredLodPrimitives}, {"cookMs", cookMs}};
}

bool ModelCookRecord::fromJson(const Json& json, ModelCookRecord& out){
    if (!json.is_object() || json.value("version", 0) != kModelCookVersion)
        return false;
    out = ModelCookRecord();
    out.key = json.value("key", std::string());
    out.family = json.value("family", std::string());
    out.compression = json.value("compression", std::string());
    out.sourceBytes = json.value("sourceBytes", (uint64_t)0);
    out.diskBytes = json.value("diskBytes", (uint64_t)0);
    out.before = ModelGeometryStats::fromJson(json.value("before", Json::object()));
    out.after = ModelGeometryStats::fromJson(json.value("after", Json::object()));
    out.duplicatePercent = json.value("duplicatePercent", 0.0);
    out.meshes = json.value("meshes", 0);
    out.primitives = json.value("primitives", 0);
    out.lodPrimitives = json.value("lodPrimitives", 0);
    if (json.contains("lods") && json["lods"].is_array())
        for (const Json& level : json["lods"])
            out.lods.push_back(ModelLodLevelStats::fromJson(level));
    out.modelSize = json.value("modelSize", 0.0);
    out.positionError = json.value("positionError", 0.0);
    out.passThrough = json.value("passThrough", false);
    out.requiredExtensions = stringsOf(json, "requiredExtensions");
    out.sourceExtensions = stringsOf(json, "sourceExtensions");
    out.keptOrder = stringsOf(json, "keptOrder");
    out.warnings = stringsOf(json, "warnings");
    if (json.contains("diagnostics") && json["diagnostics"].is_array()){
        for (const Json& item : json["diagnostics"]){
            GltfDiagnostic diagnostic;
            if (modelDiagnosticFromJson(item, diagnostic))
                out.diagnostics.push_back(diagnostic);
        }
    }
    out.transcoded = stringsOf(json, "transcoded");
    out.authoredLodPrimitives = json.value("authoredLodPrimitives", 0);
    out.cookMs = json.value("cookMs", 0.0);
    return !out.key.empty();
}

void ModelCookCache::open(const fs::path& directory, uint64_t cap){
    std::lock_guard<std::mutex> lock(mutex);
    dir = directory;
    capBytes = cap;
    sources.clear();
    sourcesDirty = false;
    opened = true;
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::vector<uint8_t> bytes;
    if (!readAll(dir / "sources.json", bytes))
        return;
    const Json table = Json::parse(bytes.begin(), bytes.end(), nullptr, false);
    if (!table.is_object() || table.value("version", 0) != kModelCookVersion || !table.contains("sources") || !table["sources"].is_object())
        return;
    for (const auto& [path, value] : table["sources"].items()){
        SourceEntry entry;
        entry.sha256 = value.value("sha256", std::string());
        if (value.contains("files") && value["files"].is_array())
            for (const Json& file : value["files"])
                if (file.is_array() && file.size() == 2)
                    entry.files.push_back({file[0].get<uint64_t>(), file[1].get<int64_t>()});
        if (entry.sha256.size() == 64 && !entry.files.empty())
            sources[path] = entry;
    }
}

void ModelCookCache::close(){
    saveSources();
    std::lock_guard<std::mutex> lock(mutex);
    opened = false;
    sources.clear();
}

bool ModelCookCache::isOpen() const{
    std::lock_guard<std::mutex> lock(mutex);
    return opened;
}

void ModelCookCache::setCap(uint64_t bytes){
    std::lock_guard<std::mutex> lock(mutex);
    capBytes = bytes;
}

bool ModelCookCache::sourceHash(const fs::path& absolute, const std::vector<fs::path>& extra, std::string& sha256){
    std::vector<fs::path> files = {absolute};
    files.insert(files.end(), extra.begin(), extra.end());
    std::vector<std::pair<uint64_t, int64_t>> stamps;
    for (const fs::path& file : files){
        std::error_code ec;
        const uint64_t size = (uint64_t)fs::file_size(file, ec);
        if (ec)
            return false;
        stamps.push_back({size, fileTime(file)});
    }
    const std::string key = absolute.lexically_normal().generic_string();
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = sources.find(key);
        if (it != sources.end() && it->second.files == stamps){
            sha256 = it->second.sha256;
            return true;
        }
    }
    std::string bytes;
    for (const fs::path& file : files){
        std::vector<uint8_t> data;
        if (!readAll(file, data))
            return false;
        bytes.append(reinterpret_cast<const char*>(data.data()), data.size());
    }
    sha256 = htmlui::compat::sha256Hex(bytes);
    std::lock_guard<std::mutex> lock(mutex);
    sources[key] = SourceEntry{stamps, sha256};
    sourcesDirty = true;
    return true;
}

void ModelCookCache::saveSources(){
    std::lock_guard<std::mutex> lock(mutex);
    if (!sourcesDirty || dir.empty())
        return;
    Json table = {{"version", kModelCookVersion}, {"sources", Json::object()}};
    std::vector<std::string> keys;
    for (const auto& [path, entry] : sources)
        keys.push_back(path);
    std::sort(keys.begin(), keys.end());
    for (const std::string& path : keys){
        const SourceEntry& entry = sources[path];
        Json files = Json::array();
        for (const auto& [size, mtime] : entry.files)
            files.push_back(Json::array({size, mtime}));
        table["sources"][path] = {{"files", files}, {"sha256", entry.sha256}};
    }
    io::AtomicWriteOptions options;
    options.durability = io::Durability::Cache;
    if (io::writeFileAtomic(dir / "sources.json", table.dump(1), options).ok())
        sourcesDirty = false;
}

std::string ModelCookCache::key(const std::string& sourceSha256, const ResolvedModelSettings& settings, ModelFamily family,
                                const std::set<std::pair<int, int>>& blended){
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << "jasperengine-model-cook\n"
         << "version " << kModelCookVersion << "\n"
         << "source " << sourceSha256 << "\n"
         << "settings " << settings.canonical() << "\n"
         << "family " << modelFamilyName(family) << "\n"
         << "meshoptimizer " << MESHOPTIMIZER_VERSION << "\n"
         << "lod-recipe " << kMeshLodRecipe << "\n"
         << "blended";
    for (const auto& [mesh, primitive] : blended)
        text << " " << mesh << "/" << primitive;
    text << "\n";
    return htmlui::compat::sha256Hex(text.str());
}

fs::path ModelCookCache::artifactPath(const std::string& key, const char* suffix) const{
    return dir / key.substr(0, 2) / (key + suffix);
}

bool ModelCookCache::has(const std::string& key) const{
    std::error_code ec;
    return fs::is_regular_file(artifactPath(key, ".glb"), ec) && fs::is_regular_file(artifactPath(key, ".json"), ec);
}

bool ModelCookCache::load(const std::string& key, ModelCookRecord& record){
    std::vector<uint8_t> bytes;
    if (!readAll(artifactPath(key, ".json"), bytes))
        return false;
    const Json json = Json::parse(bytes.begin(), bytes.end(), nullptr, false);
    if (!ModelCookRecord::fromJson(json, record) || record.key != key || !has(key))
        return false;
    touch(artifactPath(key, ".glb"));
    touch(artifactPath(key, ".json"));
    return true;
}

bool ModelCookCache::readArtifact(const std::string& key, std::vector<uint8_t>& bytes) const{
    return readAll(artifactPath(key, ".glb"), bytes);
}

bool ModelCookCache::store(const std::string& key, const std::vector<uint8_t>& glb, const ModelCookRecord& record, std::string& error){
    io::AtomicWriteOptions options;
    options.durability = io::Durability::Cache;
    // the file first, the record last: a record means a complete artifact
    io::WriteResult result = io::writeFileAtomic(artifactPath(key, ".glb"),
                                                 std::string_view(reinterpret_cast<const char*>(glb.data()), glb.size()), options);
    if (result.ok())
        result = io::writeFileAtomic(artifactPath(key, ".json"), record.toJson().dump(1), options);
    if (!result.ok()){
        error = result.message;
        return false;
    }
    bool sweepNow = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        sweepNow = ++writesSinceSweep >= 16;
        if (sweepNow)
            writesSinceSweep = 0;
    }
    if (sweepNow)
        sweep();
    return true;
}

uint64_t ModelCookCache::totalBytes() const{
    uint64_t total = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)){
        if (it->is_regular_file(ec) && it->path().filename() != "sources.json")
            total += (uint64_t)it->file_size(ec);
    }
    return total;
}

uint64_t ModelCookCache::sweep(){
    struct Artifact {
        std::string key;
        int64_t used = 0;
        uint64_t bytes = 0;
        std::vector<fs::path> files;
    };
    std::unordered_map<std::string, Artifact> artifacts;
    uint64_t total = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)){
        if (!it->is_regular_file(ec))
            continue;
        const std::string name = it->path().filename().string();
        if (name == "sources.json" || name.size() < 64)
            continue;
        Artifact& artifact = artifacts[name.substr(0, 64)];
        artifact.key = name.substr(0, 64);
        const uint64_t size = (uint64_t)it->file_size(ec);
        artifact.bytes += size;
        artifact.used = std::max(artifact.used, fileTime(it->path()));
        artifact.files.push_back(it->path());
        total += size;
    }
    uint64_t cap;
    {
        std::lock_guard<std::mutex> lock(mutex);
        cap = capBytes;
    }
    if (cap == 0 || total <= cap)
        return 0;
    std::vector<Artifact*> order;
    for (auto& [key, artifact] : artifacts)
        order.push_back(&artifact);
    std::sort(order.begin(), order.end(), [](const Artifact* a, const Artifact* b){
        return a->used != b->used ? a->used < b->used : a->key < b->key;
    });
    uint64_t freed = 0;
    for (const Artifact* artifact : order){
        if (total - freed <= cap)
            break;
        // the record goes first: a half-deleted artifact is never a hit
        std::vector<fs::path> files = artifact->files;
        std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b){
            return (a.extension() == ".json") > (b.extension() == ".json");
        });
        for (const fs::path& file : files)
            fs::remove(file, ec);
        freed += artifact->bytes;
    }
    return freed;
}

void ModelCookCache::clear(){
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)){
        if (it->is_directory(ec) && it->path().filename().string().size() == 2)
            fs::remove_all(it->path(), ec);
    }
}
