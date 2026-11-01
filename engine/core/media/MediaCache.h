// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef MEDIACACHE_H
#define MEDIACACHE_H

#include "Export.h"
#include "GrowingFileReader.h"
#include "VideoTypes.h"

#include <cstdint>
#include <memory>
#include <string>

namespace jasperengine {

    class HttpRequestHandle;

    // One URL's file in the media cache (MediaCache::acquire): downloading, complete, or failed
    class JASPERENGINE_API MediaDownload {
    public:
        enum class Status { DOWNLOADING, COMPLETE, FAILED };

        const std::string& getUrl() const { return url; }
        Status getStatus() const { return status; }
        // the cache file (a system path); whole once COMPLETE
        const std::string& getFilePath() const { return filePath; }
        // the file while it grows (GrowingFileReader) and once complete
        std::shared_ptr<GrowingFileState> getFile() const { return file; }
        // 0..1 of the download (1 once complete), -1 while the size is unknown
        double getProgress() const;
        uint64_t getReceived() const { return received; }
        int64_t getTotal() const { return total; }
        const MediaError& getError() const { return error; }
        // played from an earlier download (no request was needed)
        bool isFromCache() const { return fromCache; }

        // MediaCache's (main thread): read through the accessors above
        std::string url;
        std::string filePath;
        Status status = Status::DOWNLOADING;
        std::shared_ptr<GrowingFileState> file;
        std::shared_ptr<HttpRequestHandle> request;
        std::shared_ptr<HttpRequestHandle> revalidation;
        uint64_t received = 0;
        int64_t total = -1;
        MediaError error;
        bool fromCache = false;
        bool resumed = false;
        bool replacePending = false;    // a newer copy waits in <file>.new until nobody plays it
        int users = 0;
    };

    // The cache of URL videos (docs/superpowers/specs/2026-10-31-video-mp4-design.md §5.6):
    // http(s) sources are downloaded by the engine's HTTP client into
    // <userdata>/.cache/media/<sha256(url)>.<ext> and played from the file, while it grows when
    // the backend can (progressive start). A cached copy plays at once and is revalidated in the
    // background (If-None-Match / If-Modified-Since: a 304 keeps it, a newer one replaces it
    // once nobody plays it; a failed check is not an error, so offline play works). A download
    // that fails keeps its .part and resumes with a Range request next time. Players share one
    // download per URL; when the last one lets go of an unfinished download it is cancelled (the
    // owner rule). The least recently used files go past the limit (512 MB by default).
    // Main thread only.
    class JASPERENGINE_API MediaCache {
    public:
        static constexpr uint64_t DEFAULT_LIMIT = 512ull << 20;

        // the folder (a system path) and the file a URL is kept in
        static std::string getDirectory();
        static std::string pathFor(const std::string& url);

        // the download of `url`, started or joined (FAILED at once without an HTTP client);
        // each acquire is matched by a release
        static std::shared_ptr<MediaDownload> acquire(const std::string& url);
        static void release(const std::shared_ptr<MediaDownload>& download);

        // deletes the cached files of `url` ("" every file) that nobody plays, partial ones too;
        // returns the files removed
        static int clear(const std::string& url = std::string());
        static uint64_t getLimit();
        static void setLimit(uint64_t bytes);
        // bytes of the cache folder
        static uint64_t getUsedBytes();
        // removes the least recently used files until the folder fits the limit
        static void evict();
        // Play stop / engine shutdown (after Http::shutdown dropped the callbacks): every
        // download is forgotten, partial files stay for a resume
        static void shutdown();
        // downloads running (tests, tooling)
        static int getActiveDownloads();
    };

}

#endif //MEDIACACHE_H
