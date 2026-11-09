// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TextureCookCache.h"

#include "TextureEncoders.h"

#include "htmlui/compat/Hash.h"
#include "io/AtomicFile.h"

#include <algorithm>
#include <chrono>
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

    // SHA-256 of a file, read in one go (sources are at most a few hundred MB)
    bool fileSha256(const fs::path& path, std::string& out){
        std::vector<uint8_t> bytes;
        if (!readAll(path, bytes))
            return false;
        out = htmlui::compat::sha256Hex(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        return true;
    }

    void touch(const fs::path& path){
        std::error_code ec;
        fs::last_write_time(path, fs::file_time_type::clock::now(), ec);
    }

}

bool TextureSourceAnalysis::hasAlpha(TextureAlphaSource source) const{
    switch (source){
        case TextureAlphaSource::None: return false;
        case TextureAlphaSource::FromGray: return grayAlpha;
        case TextureAlphaSource::Input: default: return inputAlpha;
    }
}

namespace {
void addStepFields(const TextureSourceAnalysis& analysis, Json& json){
    if (!analysis.stepState.empty())
        json["stepState"] = analysis.stepState;
    if (!analysis.stepGuard.empty())
        json["stepGuard"] = analysis.stepGuard;
}
}

Json TextureSourceAnalysis::toJson() const{
    Json json = {{"width", width}, {"height", height}, {"faces", faces}, {"inputAlpha", inputAlpha}, {"grayAlpha", grayAlpha},
                 {"float", floatSource}, {"sixteenBit", sixteenBit}, {"singleChannel", singleChannel}, {"bytes", bytes}};
    addStepFields(*this, json);
    return json;
}

bool TextureSourceAnalysis::fromJson(const Json& json, TextureSourceAnalysis& out){
    if (!json.is_object())
        return false;
    out.width = json.value("width", 0);
    out.height = json.value("height", 0);
    out.faces = json.value("faces", 1);
    out.inputAlpha = json.value("inputAlpha", false);
    out.grayAlpha = json.value("grayAlpha", false);
    out.floatSource = json.value("float", false);
    out.sixteenBit = json.value("sixteenBit", false);
    out.singleChannel = json.value("singleChannel", false);
    out.stepState = json.value("stepState", std::string());
    out.stepGuard = json.value("stepGuard", std::string());
    out.bytes = json.value("bytes", (uint64_t)0);
    return out.width > 0 && out.height > 0;
}

Json TextureCookRecord::toJson() const{
    return {{"version", kTextureCookVersion}, {"key", key}, {"family", family}, {"format", format}, {"label", label},
            {"width", width}, {"height", height}, {"levels", levels}, {"faces", faces},
            {"logicalWidth", logicalWidth}, {"logicalHeight", logicalHeight},
            {"gpuBytes", gpuBytes}, {"diskBytes", diskBytes}, {"sourceBytes", sourceBytes}, {"tailBytes", tailBytes},
            {"cpuBytes", cpuBytes}, {"psnr", psnr}, {"psnrAlpha", psnrAlpha}, {"normalMeanError", normalMeanError},
            {"normalP99Error", normalP99Error}, {"hdrRelativeRmse", hdrRelativeRmse}, {"cookMs", cookMs}, {"flags", flags},
            {"warnings", warnings}, {"encoders", encoders}};
}

bool TextureCookRecord::fromJson(const Json& json, TextureCookRecord& out){
    if (!json.is_object() || json.value("version", 0) != kTextureCookVersion)
        return false;
    out.key = json.value("key", std::string());
    out.family = json.value("family", std::string());
    out.format = json.value("format", std::string());
    out.label = json.value("label", std::string());
    out.width = json.value("width", 0);
    out.height = json.value("height", 0);
    out.levels = json.value("levels", 0);
    out.faces = json.value("faces", 1);
    out.logicalWidth = json.value("logicalWidth", 0);
    out.logicalHeight = json.value("logicalHeight", 0);
    out.gpuBytes = json.value("gpuBytes", (uint64_t)0);
    out.diskBytes = json.value("diskBytes", (uint64_t)0);
    out.sourceBytes = json.value("sourceBytes", (uint64_t)0);
    out.tailBytes = json.value("tailBytes", (uint64_t)0);
    out.cpuBytes = json.value("cpuBytes", (uint64_t)0);
    out.psnr = json.value("psnr", 0.0);
    out.psnrAlpha = json.value("psnrAlpha", 0.0);
    out.normalMeanError = json.value("normalMeanError", 0.0);
    out.normalP99Error = json.value("normalP99Error", 0.0);
    out.hdrRelativeRmse = json.value("hdrRelativeRmse", 0.0);
    out.cookMs = json.value("cookMs", 0.0);
    out.flags = json.value("flags", 0u);
    if (json.contains("warnings") && json["warnings"].is_array())
        out.warnings = json["warnings"].get<std::vector<std::string>>();
    if (json.contains("encoders") && json["encoders"].is_array())
        out.encoders = json["encoders"].get<std::vector<std::string>>();
    return !out.key.empty();
}

void TextureCookCache::open(const fs::path& projectCache, const std::string& globalDir, uint64_t cap){
    std::lock_guard<std::mutex> lock(mutex);
    dir = globalDir.empty() ? projectCache : fs::path(globalDir) / "textures" / "v1";
    capBytes = cap;
    sources.clear();
    sourcesDirty = false;
    opened = true;
    std::error_code ec;
    fs::create_directories(dir, ec);
    // the sources table is per project (machine paths): next to the project's cache
    std::vector<uint8_t> bytes;
    if (readAll(projectCache / "sources.json", bytes)){
        const Json table = Json::parse(bytes.begin(), bytes.end(), nullptr, false);
        if (table.is_object() && table.value("version", 0) == kTextureCookVersion && table.contains("sources") && table["sources"].is_object()){
            for (const auto& [path, value] : table["sources"].items()){
                SourceEntry entry;
                entry.size = value.value("size", (uint64_t)0);
                entry.mtime = value.value("mtime", (int64_t)0);
                entry.sha256 = value.value("sha256", std::string());
                if (entry.sha256.size() == 64 && value.contains("analysis") && TextureSourceAnalysis::fromJson(value["analysis"], entry.analysis))
                    sources[path] = entry;
            }
        }
    }
    sourcesFile = projectCache / "sources.json";
}

void TextureCookCache::close(){
    saveSources();
    std::lock_guard<std::mutex> lock(mutex);
    opened = false;
    sources.clear();
}

bool TextureCookCache::isOpen() const{
    std::lock_guard<std::mutex> lock(mutex);
    return opened;
}

bool TextureCookCache::sourceHash(const fs::path& absolute, std::string& sha256, TextureSourceAnalysis& analysis,
                                  const std::function<bool(TextureSourceAnalysis&)>& analyze, const std::string& variant){
    std::error_code ec;
    const uint64_t size = (uint64_t)fs::file_size(absolute, ec);
    if (ec)
        return false;
    const int64_t mtime = fileTime(absolute);
    std::string key = absolute.lexically_normal().generic_string();
    if (!variant.empty())
        key += "#" + variant;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = sources.find(key);
        if (it != sources.end() && it->second.size == size && it->second.mtime == mtime){
            sha256 = it->second.sha256;
            analysis = it->second.analysis;
            return true;
        }
    }
    if (!fileSha256(absolute, sha256))
        return false;
    TextureSourceAnalysis fresh;
    if (!analyze(fresh))
        return false;
    fresh.bytes = size;
    analysis = fresh;
    std::lock_guard<std::mutex> lock(mutex);
    sources[key] = SourceEntry{size, mtime, sha256, fresh};
    sourcesDirty = true;
    return true;
}

void TextureCookCache::saveSources(){
    std::lock_guard<std::mutex> lock(mutex);
    if (!sourcesDirty || sourcesFile.empty())
        return;
    Json table = {{"version", kTextureCookVersion}, {"sources", Json::object()}};
    // sorted: the file only changes with its content
    std::vector<std::string> keys;
    for (const auto& [path, entry] : sources)
        keys.push_back(path);
    std::sort(keys.begin(), keys.end());
    for (const std::string& path : keys){
        const SourceEntry& entry = sources[path];
        table["sources"][path] = {{"size", entry.size}, {"mtime", entry.mtime}, {"sha256", entry.sha256}, {"analysis", entry.analysis.toJson()}};
    }
    io::AtomicWriteOptions options;
    options.durability = io::Durability::Cache;
    if (io::writeFileAtomic(sourcesFile, table.dump(1), options).ok())
        sourcesDirty = false;
}

std::string TextureCookCache::key(const std::string& sourceSha256, const ResolvedTextureSettings& settings, TextureFamily family,
                                  const TextureFormatChoice& choice, const std::string& stepKey){
    const ITextureEncoder& encoder = textureEncoderFor(choice);
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << "jasperengine-texture-cook\n"
         << "version " << kTextureCookVersion << "\n"
         << "source " << sourceSha256 << "\n"
         << "settings " << settings.canonical() << "\n"
         << "family " << textureFamilyName(family) << "\n"
         << "codec " << textureCodecName(choice.codec) << " format " << gpuFormatName(choice.format)
         << " rdo " << textureCanonicalNumber(choice.rdoLambda) << " etc1s " << choice.etc1sQuality << " zstd " << choice.zstdLevel
         << " opaque " << choice.opaqueBc7 << " multiple " << choice.sizeMultiple << " uncompressed " << choice.uncompressed << "\n"
         << "encoder " << encoder.name() << " " << encoder.version() << "\n";
    if (!encoder.isaInvariant())
        text << "isa " << textureEncoderIsa() << "\n";
    if (!stepKey.empty())
        text << "step " << stepKey << "\n";
    return htmlui::compat::sha256Hex(text.str());
}

fs::path TextureCookCache::artifactPath(const std::string& key, const char* suffix) const{
    return dir / key.substr(0, 2) / (key + suffix);
}

bool TextureCookCache::has(const std::string& key) const{
    std::error_code ec;
    return fs::is_regular_file(artifactPath(key, ".ktx2"), ec) && fs::is_regular_file(artifactPath(key, ".json"), ec);
}

bool TextureCookCache::load(const std::string& key, TextureCookRecord& record){
    std::vector<uint8_t> bytes;
    if (!readAll(artifactPath(key, ".json"), bytes))
        return false;
    const Json json = Json::parse(bytes.begin(), bytes.end(), nullptr, false);
    if (!TextureCookRecord::fromJson(json, record) || record.key != key || !has(key))
        return false;
    touch(artifactPath(key, ".ktx2"));
    touch(artifactPath(key, ".json"));
    if (record.cpuBytes > 0)
        touch(artifactPath(key, ".cpu.ktx2"));
    return true;
}

bool TextureCookCache::readArtifact(const std::string& key, std::vector<uint8_t>& bytes, const char* suffix) const{
    return readAll(artifactPath(key, suffix), bytes);
}

bool TextureCookCache::store(const std::string& key, const std::vector<uint8_t>& ktx2, const std::vector<uint8_t>* cpuCopy,
                             const TextureCookRecord& record, std::string& error){
    io::AtomicWriteOptions options;
    options.durability = io::Durability::Cache;
    // the file first, the record last: a record means a complete artifact
    io::WriteResult result = io::writeFileAtomic(artifactPath(key, ".ktx2"),
                                                 std::string_view(reinterpret_cast<const char*>(ktx2.data()), ktx2.size()), options);
    if (result.ok() && cpuCopy)
        result = io::writeFileAtomic(artifactPath(key, ".cpu.ktx2"),
                                     std::string_view(reinterpret_cast<const char*>(cpuCopy->data()), cpuCopy->size()), options);
    if (result.ok())
        result = io::writeFileAtomic(artifactPath(key, ".json"), record.toJson().dump(1), options);
    if (!result.ok()){
        error = result.message;
        return false;
    }
    bool sweepNow = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        sweepNow = ++writesSinceSweep >= 32;
        if (sweepNow)
            writesSinceSweep = 0;
    }
    if (sweepNow)
        sweep();
    return true;
}

uint64_t TextureCookCache::totalBytes(){
    uint64_t total = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)){
        if (it->is_regular_file(ec) && it->path().filename() != "sources.json")
            total += (uint64_t)it->file_size(ec);
    }
    return total;
}

uint64_t TextureCookCache::sweep(){
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

void TextureCookCache::clear(){
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)){
        if (it->is_directory(ec) && it->path().filename().string().size() == 2)
            fs::remove_all(it->path(), ec);
    }
}
