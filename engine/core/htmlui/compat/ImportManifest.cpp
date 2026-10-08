// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ImportManifest.h"

#include "VirtualFiles.h"

#include "json.hpp"

#include <algorithm>
#include <mutex>

using namespace jasperengine::htmlui::compat;

namespace {

    struct Cache {
        std::mutex mutex;
        std::shared_ptr<const ImportManifest> manifest;
    };

    Cache& cache(){
        static Cache instance;
        return instance;
    }

    std::string lower(std::string_view text){
        std::string result(text);
        for (char& c : result){
            if (c >= 'A' && c <= 'Z')
                c = (char)(c - 'A' + 'a');
        }
        return result;
    }

    std::string trim(std::string_view text){
        size_t begin = 0, end = text.size();
        while (begin < end && (unsigned char)text[begin] <= ' ')
            begin++;
        while (end > begin && (unsigned char)text[end - 1] <= ' ')
            end--;
        return std::string(text.substr(begin, end - begin));
    }

    std::string stringField(const nlohmann::json& object, const char* key){
        const auto it = object.find(key);
        if (it != object.end() && it->is_string())
            return it->get<std::string>();
        return std::string();
    }

}

std::string ImportManifest::normalizeUrl(std::string_view input){
    std::string url = trim(input);

    // the fragment never reaches a server
    const size_t hash = url.find('#');
    if (hash != std::string::npos)
        url.erase(hash);

    std::string scheme;
    std::string rest;
    if (url.compare(0, 2, "//") == 0){
        scheme = "https";
        rest = url.substr(2);
    }else{
        const size_t colon = url.find("://");
        if (colon == std::string::npos)
            return url;
        scheme = lower(url.substr(0, colon));
        rest = url.substr(colon + 3);
        if (scheme == "http")
            scheme = "https";
    }

    // host[:port] up to the first '/', '?'
    size_t hostEnd = rest.find_first_of("/?");
    if (hostEnd == std::string::npos)
        hostEnd = rest.size();
    std::string host = lower(rest.substr(0, hostEnd));
    // user info is dropped with the host's case
    if (host.size() > 4 && host.compare(host.size() - 4, 4, ":443") == 0 && scheme == "https")
        host.erase(host.size() - 4);
    if (host.size() > 3 && host.compare(host.size() - 3, 3, ":80") == 0)
        host.erase(host.size() - 3);

    std::string path = rest.substr(hostEnd);
    if (path.empty() || path[0] == '?')
        path.insert(0, "/");
    return scheme + "://" + host + path;
}

const ImportManifest& ImportManifest::get(){
    Cache& c = cache();
    std::lock_guard<std::mutex> lock(c.mutex);
    if (!c.manifest)
        c.manifest = std::make_shared<const ImportManifest>(load(FileAccess()));
    return *c.manifest;
}

std::shared_ptr<const ImportManifest> ImportManifest::current(){
    get();
    Cache& c = cache();
    std::lock_guard<std::mutex> lock(c.mutex);
    return c.manifest;
}

void ImportManifest::reload(){
    Cache& c = cache();
    std::lock_guard<std::mutex> lock(c.mutex);
    // the previous object stays alive for holders of current(); get()'s reference too
    // until its next call, since the cache keeps one more generation
    static std::shared_ptr<const ImportManifest> previous;
    previous = c.manifest;
    c.manifest.reset();
}

void ImportManifest::notifyFileChanged(std::string_view assetPath){
    std::string path(assetPath);
    while (!path.empty() && path[0] == '/')
        path.erase(0, 1);
    if (path.compare(0, 8, "asset://") == 0)
        path.erase(0, 8);
    if (path == kPath)
        reload();
}

ImportManifest ImportManifest::load(const FileAccess& files){
    ImportManifest manifest;
    std::string text;
    if (!readAssetFile(files, kPath, text))
        return manifest;
    std::string error;
    if (!parse(text, manifest, &error))
        return ImportManifest();
    return manifest;
}

bool ImportManifest::parse(std::string_view text, ImportManifest& manifest, std::string* error){
    manifest = ImportManifest();
    nlohmann::json json = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
    if (json.is_discarded() || !json.is_object()){
        if (error)
            *error = "not a JSON object";
        return false;
    }
    const auto version = json.find("version");
    if (version == json.end() || !version->is_number_integer() || version->get<int>() != kVersion){
        if (error)
            *error = "unsupported manifest version";
        return false;
    }
    const auto entries = json.find("entries");
    if (entries != json.end() && entries->is_array()){
        for (const nlohmann::json& item : *entries){
            if (!item.is_object())
                continue;
            ImportEntry entry;
            entry.url = normalizeUrl(stringField(item, "url"));
            entry.kind = stringField(item, "kind");
            entry.path = stringField(item, "path");
            entry.license = stringField(item, "license");
            entry.licenseName = stringField(item, "licenseName");
            entry.source = stringField(item, "source");
            entry.fetched = stringField(item, "fetched");
            entry.sha256 = stringField(item, "sha256");
            const auto filesIt = item.find("files");
            if (filesIt != item.end() && filesIt->is_array()){
                for (const nlohmann::json& file : *filesIt){
                    if (file.is_string())
                        entry.files.push_back(file.get<std::string>());
                }
            }
            if (entry.url.empty() || entry.path.empty())
                continue;
            manifest.add(std::move(entry));
        }
    }
    manifest.fileFound = true;
    return true;
}

void ImportManifest::add(ImportEntry entry){
    const auto it = std::lower_bound(list.begin(), list.end(), entry.url,
                                     [](const ImportEntry& e, const std::string& url){ return e.url < url; });
    if (it != list.end() && it->url == entry.url)
        *it = std::move(entry);
    else
        list.insert(it, std::move(entry));
}

const ImportEntry* ImportManifest::find(std::string_view url) const{
    const std::string normalized = normalizeUrl(url);
    const auto it = std::lower_bound(list.begin(), list.end(), normalized,
                                     [](const ImportEntry& e, const std::string& key){ return e.url < key; });
    if (it != list.end() && it->url == normalized)
        return &*it;
    return nullptr;
}
