// Copyright (c) 2026 Gamecrafters Guild Limited and contributors (OpenEngine, MIT)
// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// From OpenEngine's Engine/Source/Assets/ExpectedWriteLedger.cpp (NOTICE.md).

#include "ExpectedWriteLedger.h"

#include "FileWatcher.h"

#include <algorithm>
#include <cctype>
#include <system_error>

using namespace jasperengine;

namespace fs = std::filesystem;

namespace {

    bool keyIsWithin(const std::string& key, const std::string& root) {
        if (key.size() < root.size() || key.compare(0, root.size(), root) != 0) return false;
        return key.size() == root.size() || key[root.size()] == '/';
    }

}

ExpectedWriteLedger::ExpectedWriteLedger(std::chrono::milliseconds echoWindow)
    : echoWindow(echoWindow) {
}

void ExpectedWriteLedger::setEchoWindow(std::chrono::milliseconds window) {
    std::lock_guard<std::mutex> lock(mutex);
    echoWindow = window;
}

std::chrono::milliseconds ExpectedWriteLedger::getEchoWindow() const {
    std::lock_guard<std::mutex> lock(mutex);
    return echoWindow;
}

std::string ExpectedWriteLedger::makeKey(const fs::path& path) {
    if (path.empty()) return {};
    // Watcher events and writers name the same file from different starting points
    std::error_code ec;
    const fs::path absolute = fs::absolute(path, ec);
    std::string key = watch::pathToUtf8((ec ? path : absolute).lexically_normal());
#if defined(_WIN32) || defined(__APPLE__)
    // NTFS and APFS are case-insensitive by default: the writer's spelling and the watcher's
    // on-disk casing must give one key
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    // a trailing separator would make the subtree prefix test miss its own root
    while (key.size() > 1 && key.back() == '/') key.pop_back();
    return key;
}

ExpectedWriteLedger::Registration ExpectedWriteLedger::expect(const fs::path& path, bool subtree) {
    std::string key = makeKey(path);
    if (key.empty()) return noRegistration;

    std::lock_guard<std::mutex> lock(mutex);
    // Announcing a write makes whatever was recorded for that path obsolete: those bytes are
    // about to be replaced, so they cannot decide whether this write's own echo is an echo.
    reportedWrites.erase(std::remove_if(reportedWrites.begin(), reportedWrites.end(),
                                        [&key, subtree](const ReportedWrite& r) { return subtree ? keyIsWithin(r.key, key) : r.key == key; }),
                         reportedWrites.end());

    const Registration id = nextRegistration++;
    expected.push_back(Expectation{id, std::move(key), subtree});
    return id;
}

void ExpectedWriteLedger::recordLocked(ReportedWrite write, std::chrono::steady_clock::time_point now) {
    pruneExpiredLocked(now);
    write.deadline = now + echoWindow;
    auto existing = std::find_if(reportedWrites.begin(), reportedWrites.end(), [&write](const ReportedWrite& r) { return r.key == write.key; });
    if (existing != reportedWrites.end()) {
        *existing = std::move(write);
        return;
    }
    reportedWrites.push_back(std::move(write));
}

void ExpectedWriteLedger::reported(const fs::path& written) {
    std::string key = makeKey(written);
    if (key.empty()) return;

    ReportedWrite write;
    std::error_code dirEc;
    if (fs::is_directory(written, dirEc)) {
        write.directory = true;
    } else {
        std::error_code sizeEc, timeEc;
        write.size = fs::file_size(written, sizeEc);
        write.modified = fs::last_write_time(written, timeEc);
        // A file that cannot be stat'ed has no identity to match a later change against, and
        // recording a blank one would swallow the next real change to the path.
        if (sizeEc || timeEc) return;
    }
    write.key = std::move(key);
    std::lock_guard<std::mutex> lock(mutex);
    recordLocked(std::move(write), std::chrono::steady_clock::now());
}

void ExpectedWriteLedger::reportedRemoved(const fs::path& removed) {
    std::string key = makeKey(removed);
    if (key.empty()) return;
    ReportedWrite write;
    write.key = std::move(key);
    write.removed = true;
    std::lock_guard<std::mutex> lock(mutex);
    recordLocked(std::move(write), std::chrono::steady_clock::now());
}

void ExpectedWriteLedger::retire(Registration id) {
    if (id == noRegistration) return;
    std::lock_guard<std::mutex> lock(mutex);
    auto it = std::find_if(expected.begin(), expected.end(), [id](const Expectation& e) { return e.id == id; });
    if (it != expected.end()) expected.erase(it);
}

bool ExpectedWriteLedger::isEchoOfOurWrite(const fs::path& path) {
    // Nothing is being written: the state on every change the watcher reports for an edit
    // made outside this process. Checked before the key is built.
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (expected.empty() && reportedWrites.empty()) return false;
    }

    const std::string key = makeKey(path);
    if (key.empty()) return false;

    const auto now = std::chrono::steady_clock::now();
    auto coveredByExpectation = [this, &key]() {
        return std::any_of(expected.begin(), expected.end(),
                           [&key](const Expectation& e) { return e.subtree ? keyIsWithin(key, e.key) : e.key == key; });
    };
    auto findReported = [this, &key]() {
        return std::find_if(reportedWrites.begin(), reportedWrites.end(), [&key](const ReportedWrite& r) { return r.key == key; });
    };

    ReportedWrite expectedState;
    {
        std::lock_guard<std::mutex> lock(mutex);
        pruneExpiredLocked(now);
        auto reported = findReported();
        // Nothing reported for this path: either the write is still in flight and its own
        // report will drive it, or this change is somebody else's
        if (reported == reportedWrites.end()) return coveredByExpectation();
        expectedState = *reported;
    }

    // Stat outside the lock: it can block on a slow or remote volume
    std::error_code existsEc, sizeEc, timeEc;
    const bool exists = fs::exists(path, existsEc);
    bool sameBytes = false;
    if (expectedState.removed) {
        sameBytes = !exists;
    } else if (expectedState.directory) {
        std::error_code dirEc;
        sameBytes = exists && fs::is_directory(path, dirEc);
    } else if (exists) {
        const std::uintmax_t size = fs::file_size(path, sizeEc);
        const fs::file_time_type modified = fs::last_write_time(path, timeEc);
        sameBytes = !sizeEc && !timeEc && size == expectedState.size && modified == expectedState.modified;
    }

    std::lock_guard<std::mutex> lock(mutex);
    auto reported = findReported();
    if (reported == reportedWrites.end()) return coveredByExpectation();
    if (sameBytes) return true;
    // The file moved on since this process wrote it: the change is an edit of its own, and
    // the record it outlived goes with it
    reportedWrites.erase(reported);
    return false;
}

size_t ExpectedWriteLedger::expectedCount() const {
    std::lock_guard<std::mutex> lock(mutex);
    return expected.size();
}

size_t ExpectedWriteLedger::reportedCount() const {
    std::lock_guard<std::mutex> lock(mutex);
    return reportedWrites.size();
}

void ExpectedWriteLedger::clear() {
    std::lock_guard<std::mutex> lock(mutex);
    expected.clear();
    reportedWrites.clear();
}

void ExpectedWriteLedger::pruneExpiredLocked(std::chrono::steady_clock::time_point now) {
    reportedWrites.erase(std::remove_if(reportedWrites.begin(), reportedWrites.end(), [now](const ReportedWrite& r) { return now > r.deadline; }),
                         reportedWrites.end());
}
