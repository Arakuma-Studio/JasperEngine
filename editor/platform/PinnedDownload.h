// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// A tool the editor downloads once and keeps (bundletool): its version and SHA-256 pinned, cached
// in the user's cache folder, retried and resumed when the network fails, and never kept when the
// bytes are not the pinned ones.

#include <filesystem>
#include <functional>
#include <string>

namespace jasperengine::editor::platform {

    struct PinnedDownload {
        std::string what;                 // for messages: "bundletool 1.17.2"
        std::string url;
        std::string sha256;               // lowercase hex
        std::filesystem::path target;     // the cached file; <target>.part holds an unfinished download
        long long maxBytes = 256ll * 1024 * 1024;
        int attemptSeconds = 600;
        int attempts = 4;
        int retryDelayMs = 1000;          // before the second attempt, doubled for each later one
    };

    // The cached file when it has the pinned hash (one that does not is removed), else false
    bool pinnedFileIsCached(const PinnedDownload& download, std::string& error);

    // Makes `target` hold the pinned file, downloading it when the cache does not. false + a
    // message that says what to do: offline, an HTTP error (a 4xx is not retried), a hash that
    // differs, or "cancelled" when `cancelled` turned true.
    bool fetchPinned(const PinnedDownload& download, const std::function<bool()>& cancelled, std::string& error);

}
