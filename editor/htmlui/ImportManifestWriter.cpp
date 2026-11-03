// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ImportManifestWriter.h"
#include "io/AtomicFile.h"
#include "util/FileUtils.h"
#include "util/SafeWrite.h"

#include "json.hpp"
#include "util/FileUtils.h"

#include <atomic>
#include <chrono>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

using namespace jasperengine;
using namespace jasperengine::editor::htmlui;
using jasperengine::htmlui::compat::ImportEntry;
using jasperengine::htmlui::compat::ImportManifest;
namespace fs = std::filesystem;

namespace {

    std::mutex& manifestMutex(){
        static std::mutex mutex;
        return mutex;
    }

}

ImportManifestWriter::ImportManifestWriter(fs::path assetsDir): assetsDir(std::move(assetsDir)) {}

fs::path ImportManifestWriter::path() const{
    return assetsDir / editor::FileUtils::pathFromUtf8(ImportManifest::kPath);
}

ImportManifest ImportManifestWriter::read() const{
    ImportManifest manifest;
    std::ifstream file(path(), std::ios::binary);
    if (!file)
        return manifest;
    std::ostringstream stream;
    stream << file.rdbuf();
    ImportManifest::parse(stream.str(), manifest, nullptr);
    return manifest;
}

bool ImportManifestWriter::add(const std::vector<ImportEntry>& entries, std::string* error){
    std::lock_guard<std::mutex> lock(manifestMutex());
    ImportManifest manifest = read();
    for (ImportEntry entry : entries){
        entry.url = ImportManifest::normalizeUrl(entry.url);
        manifest.add(std::move(entry));
    }
    return write(manifest, error);
}

bool ImportManifestWriter::remove(const std::vector<std::string>& urls, std::string* error){
    std::lock_guard<std::mutex> lock(manifestMutex());
    const ImportManifest current = read();
    ImportManifest kept;
    for (const ImportEntry& entry : current.entries()){
        bool removed = false;
        for (const std::string& url : urls){
            if (ImportManifest::normalizeUrl(url) == entry.url)
                removed = true;
        }
        if (!removed)
            kept.add(entry);
    }
    return write(kept, error);
}

std::string ImportManifestWriter::toJson(const ImportManifest& manifest){
    nlohmann::ordered_json entries = nlohmann::ordered_json::array();
    for (const ImportEntry& entry : manifest.entries()){
        nlohmann::ordered_json item;
        item["url"] = entry.url;
        item["kind"] = entry.kind;
        item["path"] = entry.path;
        item["files"] = entry.files;
        item["license"] = entry.license;
        item["licenseName"] = entry.licenseName;
        item["source"] = entry.source;
        item["fetched"] = entry.fetched;
        item["sha256"] = entry.sha256;
        entries.push_back(std::move(item));
    }
    nlohmann::ordered_json root;
    root["version"] = ImportManifest::kVersion;
    root["entries"] = std::move(entries);
    return root.dump(2) + "\n";
}

bool ImportManifestWriter::write(const ImportManifest& manifest, std::string* error){
    return writeFileAtomic(path(), toJson(manifest), error);
}

bool ImportManifestWriter::writeFileAtomic(const fs::path& file, const std::string& content, std::string* error){
    // the engine's primitive (io/AtomicFile.h): fsync, retry, mode and read-only rules
    const io::WriteResult result = io::writeFileAtomic(file, content);
    if (!result.ok()){
        if (error)
            *error = "cannot write " + file.generic_string() + ": " + io::describeWriteError(result);
        return false;
    }
    return true;
}

std::string ImportManifestWriter::nowIso8601(){
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return text;
}
