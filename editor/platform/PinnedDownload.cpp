// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PinnedDownload.h"

#include "htmlui/HttpClient.h"
#include "io/AtomicFile.h"
#include "io/ScopedFileLock.h"
#include "util/Sha256.h"

#include <atomic>
#include <chrono>
#include <thread>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::platform;
namespace fs = std::filesystem;

namespace {

    uintmax_t sizeOf(const fs::path& path) {
        std::error_code ec;
        const uintmax_t size = fs::file_size(path, ec);
        return ec ? 0 : size;
    }

    // what a retry cannot change: a request the server refuses (408, 425 and 429 ask for a retry)
    bool permanent(const htmlui::HttpResponse& response) {
        return response.status >= 400 && response.status < 500 && response.status != 408 && response.status != 416 &&
               response.status != 425 && response.status != 429;
    }

    bool waitBeforeRetry(int milliseconds, const std::function<bool()>& cancelled) {
        for (int waited = 0; waited < milliseconds; waited += 50) {
            if (cancelled && cancelled()) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return true;
    }

}

bool platform::pinnedFileIsCached(const PinnedDownload& download, std::string& error) {
    std::error_code ec;
    if (!fs::is_regular_file(download.target, ec)) return false;
    std::string hex;
    if (sha256File(download.target, hex, &error) && hex == download.sha256) return true;
    fs::remove(download.target, ec);
    return false;
}

bool platform::fetchPinned(const PinnedDownload& download, const std::function<bool()>& cancelled, std::string& error) {
    std::error_code ec;
    fs::create_directories(download.target.parent_path(), ec);
    if (ec) {
        error = "cannot create " + download.target.parent_path().string() + " for " + download.what + ": " + ec.message();
        return false;
    }
    // one download at a time: a second editor waits, and finds the file cached
    io::ScopedFileLock lock(fs::path(download.target.string() + ".lock"));
    if (pinnedFileIsCached(download, error)) return true;

    const fs::path partial = download.target.string() + ".part";
    std::string lastError;
    int delay = download.retryDelayMs;
    for (int attempt = 1; attempt <= download.attempts; attempt++) {
        if (cancelled && cancelled()) {
            error = "cancelled";
            return false;
        }
        const bool resumed = sizeOf(partial) > 0;
        htmlui::HttpClient client;
        client.setTimeoutSeconds(download.attemptSeconds);
        client.setMaxFileSize(download.maxBytes);
        std::atomic<bool> done{false};
        std::thread watcher([&]() {
            while (!done.load()) {
                if (cancelled && cancelled()) {
                    client.cancel();
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
        const htmlui::HttpResponse response = client.getResuming(download.url, partial);
        done.store(true);
        watcher.join();
        if (cancelled && cancelled()) {
            error = "cancelled";
            return false;
        }

        if (response.ok()) {
            std::string hex;
            if (!sha256File(partial, hex, &error)) return false;
            if (hex == download.sha256) {
                io::AtomicWriteOptions cache;
                cache.durability = io::Durability::Cache;
                const io::WriteResult published = io::publishFile(partial, download.target, cache);
                if (!published.ok()) {
                    error = "cannot keep " + download.what + " in " + download.target.string() + ": " + published.message;
                    return false;
                }
                return true;
            }
            fs::remove(partial, ec);
            // bytes kept from an earlier run may be what differs: one more try from the start
            if (resumed) {
                lastError = "the resumed file did not match its pinned SHA-256";
                continue;
            }
            error = "the downloaded " + download.what + " does not match its pinned SHA-256 (expected " + download.sha256 + ", got " + hex +
                    "): it was not kept. Something between this computer and " + download.url +
                    " changed the file (a proxy or a captive portal?); try another network";
            return false;
        }

        lastError = response.error;
        if (response.status == 416 || response.error.find("byte ranges") != std::string::npos) {
            // the server cannot continue these bytes: start over at once
            fs::remove(partial, ec);
            continue;
        }
        if (permanent(response)) break;
        if (attempt < download.attempts) {
            if (!waitBeforeRetry(delay, cancelled)) {
                error = "cancelled";
                return false;
            }
            delay *= 2;
        }
    }
    error = "cannot download " + download.what + " from " + download.url + " (" + lastError + "). Check this computer's network, or download that file " +
            "yourself and put it at " + download.target.string() + " (its SHA-256 must be " + download.sha256 + ")";
    return false;
}
