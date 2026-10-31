// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "MediaCache.h"

#include "Log.h"
#include "System.h"
#include "htmlui/compat/Hash.h"
#include "net/Http.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <system_error>
#include <vector>

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#endif

using namespace jasperengine;
namespace fs = std::filesystem;

namespace jasperengine {

    struct MediaCacheState {
        std::map<std::string, std::shared_ptr<MediaDownload>> downloads;
        uint64_t limit = MediaCache::DEFAULT_LIMIT;
        uint64_t generation = 0;      // shutdown(): callbacks of an earlier generation do nothing
    };

}

namespace {

    MediaCacheState& state(){
        static MediaCacheState value;
        return value;
    }

    fs::path pathOf(const std::string& utf8){
#if defined(_WIN32)
        if (utf8.empty()) return fs::path();
        const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
        if (size <= 0) return fs::path(utf8);
        std::wstring wide((size_t)size, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), &wide[0], size);
        return fs::path(wide);
#else
        return fs::path(utf8);
#endif
    }

    std::string utf8Of(const fs::path& path){
#if defined(_WIN32)
        const std::wstring& wide = path.native();
        if (wide.empty()) return std::string();
        const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
        if (size <= 0) return std::string();
        std::string text((size_t)size, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), &text[0], size, nullptr, nullptr);
        return text;
#else
        return path.string();
#endif
    }

    bool exists(const std::string& path){
        std::error_code ec;
        return fs::is_regular_file(pathOf(path), ec);
    }

    int64_t sizeOf(const std::string& path){
        std::error_code ec;
        const auto size = fs::file_size(pathOf(path), ec);
        return ec ? -1 : (int64_t)size;
    }

    void removeFile(const std::string& path){
        std::error_code ec;
        fs::remove(pathOf(path), ec);
    }

    bool renameFile(const std::string& from, const std::string& to){
        std::error_code ec;
        fs::rename(pathOf(from), pathOf(to), ec);
#if defined(_WIN32)
        if (ec && MoveFileExW(pathOf(from).c_str(), pathOf(to).c_str(), MOVEFILE_REPLACE_EXISTING))
            return true;
#endif
        return !ec;
    }

    void touch(const std::string& path){
        std::error_code ec;
        fs::last_write_time(pathOf(path), fs::file_time_type::clock::now(), ec);
    }

    // the extension of a URL's path (lower case, at most 5 characters), "bin" when it has none
    std::string extensionOf(const std::string& url){
        std::string path = url;
        const size_t scheme = path.find("://");
        if (scheme != std::string::npos){
            const size_t slash = path.find('/', scheme + 3);
            path = slash == std::string::npos ? std::string() : path.substr(slash);
        }
        const size_t cut = path.find_first_of("?#");
        if (cut != std::string::npos)
            path = path.substr(0, cut);
        const size_t dot = path.find_last_of('.');
        const size_t slash = path.find_last_of('/');
        if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
            return "bin";
        std::string extension = path.substr(dot + 1);
        if (extension.empty() || extension.size() > 5)
            return "bin";
        for (char& c : extension){
            if (!std::isalnum((unsigned char)c))
                return "bin";
            c = (char)std::tolower((unsigned char)c);
        }
        return extension;
    }

    // <file>.meta: "key: value" lines (url, etag, last-modified, size)
    std::map<std::string, std::string> readMeta(const std::string& file){
        std::map<std::string, std::string> values;
        FILE* handle = nullptr;
#if defined(_WIN32)
        handle = _wfopen(pathOf(file + ".meta").c_str(), L"rb");
#else
        handle = std::fopen((file + ".meta").c_str(), "rb");
#endif
        if (!handle)
            return values;
        std::string text;
        char buffer[1024];
        size_t got;
        while ((got = std::fread(buffer, 1, sizeof(buffer), handle)) > 0 && text.size() < 65536)
            text.append(buffer, got);
        std::fclose(handle);
        size_t start = 0;
        while (start < text.size()){
            size_t end = text.find('\n', start);
            if (end == std::string::npos)
                end = text.size();
            const std::string line = text.substr(start, end - start);
            const size_t colon = line.find(": ");
            if (colon != std::string::npos)
                values[line.substr(0, colon)] = line.substr(colon + 2);
            start = end + 1;
        }
        return values;
    }

    void writeMeta(const std::string& file, const std::map<std::string, std::string>& values){
        std::string text;
        for (const auto& [key, value] : values){
            // one line per value: no line breaks in it
            std::string clean = value;
            clean.erase(std::remove_if(clean.begin(), clean.end(), [](char c){ return c == '\n' || c == '\r'; }), clean.end());
            text += key + ": " + clean + "\n";
        }
        const std::string temp = file + ".meta.tmp";
        FILE* handle = nullptr;
#if defined(_WIN32)
        handle = _wfopen(pathOf(temp).c_str(), L"wb");
#else
        handle = std::fopen(temp.c_str(), "wb");
#endif
        if (!handle)
            return;
        const bool ok = std::fwrite(text.data(), 1, text.size(), handle) == text.size();
        std::fclose(handle);
        if (ok)
            renameFile(temp, file + ".meta");
        else
            removeFile(temp);
    }

    // a complete earlier download: its meta says so and the size matches
    bool isCachedComplete(const std::string& file, std::map<std::string, std::string>& meta){
        if (!exists(file))
            return false;
        meta = readMeta(file);
        auto size = meta.find("size");
        return size != meta.end() && std::to_string(sizeOf(file)) == size->second;
    }

    bool inUse(const std::string& file){
        for (const auto& [url, download] : state().downloads){
            if (download->getFilePath() == file && (download->users > 0 || download->getStatus() == MediaDownload::Status::DOWNLOADING))
                return true;
        }
        return false;
    }

    void forget(const std::shared_ptr<MediaDownload>& download){
        auto it = state().downloads.find(download->getUrl());
        if (it != state().downloads.end() && it->second == download)
            state().downloads.erase(it);
    }

    void startDownload(const std::shared_ptr<MediaDownload>& download, bool resume);

    void finish(const std::shared_ptr<MediaDownload>& download, const HttpResponse& response, uint64_t generation, bool resumed){
        if (generation != state().generation || download->getStatus() != MediaDownload::Status::DOWNLOADING)
            return;
        MediaDownload& d = *download;
        if (response.ok){
            const int64_t size = sizeOf(d.filePath);
            std::map<std::string, std::string> meta;
            meta["url"] = d.url;
            meta["size"] = std::to_string(size);
            if (const std::string* etag = response.header("etag"))
                meta["etag"] = *etag;
            if (const std::string* modified = response.header("last-modified"))
                meta["last-modified"] = *modified;
            writeMeta(d.filePath, meta);
            d.received = (uint64_t)std::max<int64_t>(size, 0);
            d.total = size;
            d.status = MediaDownload::Status::COMPLETE;
            d.file->complete(size);
            d.request.reset();
            MediaCache::evict();
            return;
        }
        // a .part that no longer fits what the server has (416): once more from the start
        if (resumed && response.status == 416){
            startDownload(download, false);
            return;
        }
        if (response.errorCode == "cancelled")
            return;
        d.status = MediaDownload::Status::FAILED;
        d.error.code = MediaErrorCode::NETWORK;
        d.error.message = response.error.empty() ? "the download failed: " + d.url : response.error;
        d.file->fail(d.error.message);
        d.request.reset();
        // a later acquire tries again (resuming the kept .part)
        forget(download);
    }

    void startDownload(const std::shared_ptr<MediaDownload>& download, bool resume){
        MediaDownload& d = *download;
        HttpRequestDesc desc;
        desc.url = d.url;
        desc.responseType = HttpResponseType::File;
        desc.filePath = d.filePath;
        desc.reportProgress = true;
        desc.resume = resume;
        desc.keepPartial = true;
        // a video can be long: no total limit, the connection one stays
        desc.timeout = 0.0;
        d.resumed = resume;
        const uint64_t generation = state().generation;
        std::weak_ptr<MediaDownload> weak = download;
        d.request = Http::send(desc,
            [weak, generation, resume](const HttpResponse& response){
                if (std::shared_ptr<MediaDownload> live = weak.lock())
                    finish(live, response, generation, resume);
            },
            [weak, generation](uint64_t received, int64_t total){
                std::shared_ptr<MediaDownload> live = weak.lock();
                if (!live || generation != state().generation || live->getStatus() != MediaDownload::Status::DOWNLOADING)
                    return;
                live->received = received;
                live->total = total;
                live->file->update((int64_t)received, total);
            });
    }

    void revalidate(const std::shared_ptr<MediaDownload>& download, const std::map<std::string, std::string>& meta){
        MediaDownload& d = *download;
        HttpRequestDesc desc;
        desc.url = d.url;
        desc.responseType = HttpResponseType::File;
        desc.filePath = d.filePath + ".new";
        auto etag = meta.find("etag");
        if (etag != meta.end())
            desc.headers.push_back({"If-None-Match", etag->second});
        auto modified = meta.find("last-modified");
        if (modified != meta.end())
            desc.headers.push_back({"If-Modified-Since", modified->second});
        if (desc.headers.empty())
            return;     // nothing to compare with: the copy is kept as it is
        desc.timeout = 0.0;
        const uint64_t generation = state().generation;
        std::weak_ptr<MediaDownload> weak = download;
        d.revalidation = Http::send(desc, [weak, generation](const HttpResponse& response){
            std::shared_ptr<MediaDownload> live = weak.lock();
            if (!live || generation != state().generation)
                return;
            live->revalidation.reset();
            // 304 (or offline, or anything else): the cached copy stays; it is no error
            if (!response.ok || response.status != 200)
                return;
            std::map<std::string, std::string> fresh;
            fresh["url"] = live->url;
            fresh["size"] = std::to_string(sizeOf(live->filePath + ".new"));
            if (const std::string* tag = response.header("etag"))
                fresh["etag"] = *tag;
            if (const std::string* date = response.header("last-modified"))
                fresh["last-modified"] = *date;
            writeMeta(live->filePath + ".new", fresh);
            if (live->users > 0){
                live->replacePending = true;
                return;
            }
            if (renameFile(live->filePath + ".new", live->filePath))
                renameFile(live->filePath + ".new.meta", live->filePath + ".meta");
        });
    }

}

double MediaDownload::getProgress() const{
    if (status == Status::COMPLETE)
        return 1.0;
    if (total <= 0)
        return -1.0;
    return std::clamp((double)received / (double)total, 0.0, 1.0);
}

std::string MediaCache::getDirectory(){
    return System::instance().getUserDataPath() + "/.cache/media";
}

std::string MediaCache::pathFor(const std::string& url){
    return getDirectory() + "/" + htmlui::compat::sha256Hex(url) + "." + extensionOf(url);
}

std::shared_ptr<MediaDownload> MediaCache::acquire(const std::string& url){
    MediaCacheState& s = state();
    auto it = s.downloads.find(url);
    if (it != s.downloads.end() && it->second->getStatus() != MediaDownload::Status::FAILED){
        it->second->users++;
        return it->second;
    }
    std::shared_ptr<MediaDownload> download = std::make_shared<MediaDownload>();
    download->url = url;
    download->filePath = pathFor(url);
    download->users = 1;
    download->file = std::make_shared<GrowingFileState>(download->filePath + ".part", download->filePath);

    std::map<std::string, std::string> meta;
    // a newer copy a revalidation fetched while the old one played: it takes over now
    if (isCachedComplete(download->filePath + ".new", meta) && renameFile(download->filePath + ".new", download->filePath))
        renameFile(download->filePath + ".new.meta", download->filePath + ".meta");
    meta.clear();
    if (isCachedComplete(download->filePath, meta)){
        const int64_t size = sizeOf(download->filePath);
        download->status = MediaDownload::Status::COMPLETE;
        download->fromCache = true;
        download->received = (uint64_t)size;
        download->total = size;
        download->file->complete(size);
        touch(download->filePath + ".meta");
        s.downloads[url] = download;
        if (Http::isAvailable())
            revalidate(download, meta);
        return download;
    }
    if (!Http::isAvailable()){
        download->status = MediaDownload::Status::FAILED;
        download->error.code = MediaErrorCode::NETWORK;
        download->error.message = "URL video needs the HTTP client (Http.isAvailable() is false): " + Http::unavailableReason();
        download->file->fail(download->error.message);
        return download;
    }
    std::error_code ec;
    fs::create_directories(pathOf(getDirectory()), ec);
    // a stale finished file without its meta (an older engine, a crash between the two): again
    if (exists(download->filePath))
        removeFile(download->filePath);
    s.downloads[url] = download;
    startDownload(download, exists(download->filePath + ".part"));
    return download;
}

void MediaCache::release(const std::shared_ptr<MediaDownload>& download){
    if (!download)
        return;
    download->users = std::max(download->users - 1, 0);
    if (download->users > 0)
        return;
    if (download->status == MediaDownload::Status::DOWNLOADING){
        // nobody wants it any more: cancelled with its last owner (the .part stays for a resume)
        download->status = MediaDownload::Status::FAILED;
        download->error.code = MediaErrorCode::ABORTED;
        download->error.message = "cancelled";
        download->file->fail("cancelled");
        if (download->request)
            download->request->cancel();
        download->request.reset();
    }
    if (download->replacePending){
        download->replacePending = false;
        if (renameFile(download->filePath + ".new", download->filePath))
            renameFile(download->filePath + ".new.meta", download->filePath + ".meta");
    }
    forget(download);
}

int MediaCache::clear(const std::string& url){
    const std::string directory = getDirectory();
    std::error_code ec;
    if (!fs::is_directory(pathOf(directory), ec))
        return 0;
    int removed = 0;
    const std::string only = url.empty() ? std::string() : pathFor(url);
    std::vector<fs::path> files;
    for (const fs::directory_entry& entry : fs::directory_iterator(pathOf(directory), ec))
        files.push_back(entry.path());
    for (const fs::path& file : files){
        const std::string path = utf8Of(file);
        // the base file of every companion (.meta, .part, .new, .tmp)
        std::string base = path;
        for (const char* suffix : {".meta.tmp", ".meta", ".part", ".new"}){
            const std::string s = suffix;
            if (base.size() > s.size() && base.compare(base.size() - s.size(), s.size(), s) == 0){
                base = base.substr(0, base.size() - s.size());
                if (s == ".meta" && base.size() > 4 && base.compare(base.size() - 4, 4, ".new") == 0)
                    base = base.substr(0, base.size() - 4);
                break;
            }
        }
        if (!only.empty() && base != only)
            continue;
        if (inUse(base))
            continue;
        removeFile(path);
        removed++;
    }
    return removed;
}

uint64_t MediaCache::getLimit(){
    return state().limit;
}

void MediaCache::setLimit(uint64_t bytes){
    state().limit = bytes;
    evict();
}

uint64_t MediaCache::getUsedBytes(){
    std::error_code ec;
    uint64_t used = 0;
    for (const fs::directory_entry& entry : fs::directory_iterator(pathOf(getDirectory()), ec)){
        std::error_code sizeError;
        const auto size = entry.file_size(sizeError);
        if (!sizeError)
            used += size;
    }
    return used;
}

void MediaCache::evict(){
    const std::string directory = getDirectory();
    std::error_code ec;
    if (!fs::is_directory(pathOf(directory), ec))
        return;
    struct Item {
        std::string base;
        uint64_t bytes = 0;
        fs::file_time_type used;
    };
    std::map<std::string, Item> items;
    uint64_t total = 0;
    for (const fs::directory_entry& entry : fs::directory_iterator(pathOf(directory), ec)){
        std::error_code sizeError;
        const uint64_t size = entry.file_size(sizeError);
        if (sizeError)
            continue;
        std::string path = utf8Of(entry.path());
        std::string base = path;
        for (const char* suffix : {".new.meta", ".meta.tmp", ".meta", ".part", ".new"}){
            const std::string s = suffix;
            if (base.size() > s.size() && base.compare(base.size() - s.size(), s.size(), s) == 0){
                base = base.substr(0, base.size() - s.size());
                break;
            }
        }
        Item& item = items[base];
        item.base = base;
        item.bytes += size;
        std::error_code timeError;
        const fs::file_time_type time = entry.last_write_time(timeError);
        if (!timeError && (item.used == fs::file_time_type() || time > item.used))
            item.used = time;
        total += size;
    }
    if (total <= state().limit)
        return;
    std::vector<Item> order;
    for (const auto& [base, item] : items)
        order.push_back(item);
    std::sort(order.begin(), order.end(), [](const Item& a, const Item& b){ return a.used < b.used; });
    for (const Item& item : order){
        if (total <= state().limit)
            break;
        if (inUse(item.base))
            continue;
        for (const char* suffix : {"", ".meta", ".part", ".new", ".new.meta", ".meta.tmp"})
            removeFile(item.base + suffix);
        total -= std::min(total, item.bytes);
    }
}

void MediaCache::shutdown(){
    MediaCacheState& s = state();
    s.generation++;
    for (auto& [url, download] : s.downloads){
        if (download->status == MediaDownload::Status::DOWNLOADING){
            download->status = MediaDownload::Status::FAILED;
            download->error.code = MediaErrorCode::ABORTED;
            download->error.message = "stopped";
            download->file->fail("stopped");
        }
        download->request.reset();
        download->revalidation.reset();
    }
    s.downloads.clear();
}

int MediaCache::getActiveDownloads(){
    int count = 0;
    for (const auto& [url, download] : state().downloads){
        if (download->getStatus() == MediaDownload::Status::DOWNLOADING)
            count++;
    }
    return count;
}
