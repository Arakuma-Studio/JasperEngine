// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "hub/StagingFolders.h"

#include "Out.h"
#include "hub/RecentProjects.h"
#include "io/AtomicFile.h"
#include "util/FileUtils.h"

#include "json.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace jasperengine::editor::hub::staging {

namespace {

    constexpr const char* kFolderSuffix = ".importing";
    constexpr const char* kMarkerSuffix = ".lock";
    constexpr const char* kOwnerKind = "jasperengine-staging";
    constexpr int kMaxNumber = 1000;
    constexpr const char* kNoMarker = "It has no owner marker: an older editor left it, or it is your own folder.";

    std::mutex stateMutex;
    fs::path journalDirectory;
    // this process's folders on a volume without locks: they have no marker to tell a sweep
    std::set<fs::path> unmarkedClaims;
    RemovalFaultInjector removalFault;

    fs::path markerOf(const fs::path& folder) {
        fs::path marker = folder;
        marker += kMarkerSuffix;
        return marker;
    }

    std::string text(const fs::path& path) {
        return FileUtils::pathToUtf8(path);
    }

    // The text that marks a file as ours. The claim number tells two claims of the same name apart:
    // Claim reads it back to see that the file it locked is still the one at that path
    std::string ownerText() {
        static std::atomic<uint64_t> claims{0};
        nlohmann::ordered_json owner;
        owner["kind"] = kOwnerKind;
        owner["pid"] = io::currentProcessId();
        owner["claim"] = ++claims;
        owner["started"] = recent::iso(std::chrono::system_clock::now());
        return owner.dump(2) + "\n";
    }

    bool isOwnerText(const std::string& owner) {
        const nlohmann::json parsed = nlohmann::json::parse(owner, nullptr, false);
        return parsed.is_object() && parsed.contains("kind") && parsed["kind"] == kOwnerKind;
    }

    // FNV-1a: a file name per folder, the same in every build
    std::string journalName(const fs::path& folder) {
        uint64_t hash = 14695981039346656037ull;
        for (const char c : FileUtils::pathToGenericUtf8(folder)) {
            hash ^= (unsigned char)c;
            hash *= 1099511628211ull;
        }
        char name[32];
        std::snprintf(name, sizeof(name), "%016llx.txt", (unsigned long long)hash);
        return name;
    }

    fs::path journal() {
        std::lock_guard<std::mutex> guard(stateMutex);
        return journalDirectory;
    }

    void journalAdd(const fs::path& folder) {
        const fs::path directory = journal();
        if (directory.empty()) return;
        io::AtomicWriteOptions options;
        options.durability = io::Durability::Cache;
        const io::WriteResult written = io::writeFileAtomic(directory / journalName(folder), text(folder) + "\n", options);
        // the parents the Hub knows are still looked at
        if (!written.ok()) Out::warning("Could not note a staging folder: %s", written.message.c_str());
    }

    void journalRemoveFile(const fs::path& file) {
        std::error_code ec;
        fs::remove(file, ec);
        fs::remove(file.parent_path(), ec);   // when nothing else is in it: no empty folder in the settings folder
    }

    void journalRemove(const fs::path& folder) {
        const fs::path directory = journal();
        if (!directory.empty()) journalRemoveFile(directory / journalName(folder));
    }

    // The journal's folders; the entries without a folder or a marker there are removed
    std::vector<fs::path> journalFolders(bool change) {
        std::vector<fs::path> folders;
        const fs::path directory = journal();
        std::error_code ec;
        if (directory.empty() || !fs::is_directory(directory, ec)) return folders;
        std::vector<fs::path> files;
        for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) files.push_back(it->path());
        for (const fs::path& file : files) {
            const std::string fileName = FileUtils::pathToUtf8(file.filename());
            // a note this crash cut short: it has no folder yet; another editor's stays until it ended
            if (io::isTemporarySiblingName(fileName)) {
                if (change && !io::isProcessRunning(io::temporarySiblingWriter(fileName))) journalRemoveFile(file);
                continue;
            }
            std::ifstream in(file, std::ios::binary);
            std::string line;
            std::getline(in, line);
            const fs::path folder = FileUtils::pathFromUtf8(line);
            const bool usable = folder.is_absolute() && isStagingName(FileUtils::pathToUtf8(folder.filename()));
            if (usable && (fs::exists(fs::symlink_status(folder, ec)) || fs::exists(fs::symlink_status(markerOf(folder), ec)))) {
                folders.push_back(folder);
            } else if (change) {
                journalRemoveFile(file);
            }
        }
        return folders;
    }

    void removeFolder(const fs::path& folder, std::error_code& ec) {
        ec.clear();
        {
            std::lock_guard<std::mutex> guard(stateMutex);
            if (removalFault) ec = removalFault(folder);
        }
        if (!ec) fs::remove_all(folder, ec);
    }

    void removeMarker(const fs::path& marker) {
        std::error_code ec;
        fs::remove(marker, ec);
    }

    // One staging folder of `scope` (or just its marker). `change` false only looks.
    void visit(const fs::path& folder, bool change, const std::set<fs::path>& projects, Sweep& out) {
        std::error_code ec;
        const fs::file_status status = fs::symlink_status(folder, ec);
        const bool there = fs::is_directory(status);
        // a file or a link with such a name: nothing of ours
        if (!there && fs::exists(status)) return;
        const fs::path marker = markerOf(folder);
        auto leftover = [&](const char* reason) {
            if (there && !projects.count(folder)) out.leftovers.push_back({folder, reason});
        };
        auto gone = [&]() {
            if (!change || there) return;
            journalRemove(folder);
        };
        if (!fs::exists(marker, ec)) {
            {
                std::lock_guard<std::mutex> guard(stateMutex);
                if (unmarkedClaims.count(folder)) {
                    out.live.push_back(folder);
                    return;
                }
            }
            leftover(kNoMarker);
            gone();
            return;
        }
        // whoever holds the marker's lock owns the folder; the system frees it when a process dies
        io::ScopedFileLock lock(marker, io::ScopedFileLock::Mode::Exclusive, false);
        if (lock.getStatus() == io::ScopedFileLock::Status::Busy) {
            out.live.push_back(folder);
            return;
        }
        if (!lock.isLocked()) {
            leftover("Its owner cannot be told: this volume has no file locks.");
            return;
        }
        const std::string owner = io::ScopedFileLock::readOwnerInfo(marker);
        if (!isOwnerText(owner)) {
            // Empty: a claim died before it wrote it (it had made no folder then), or this look made
            // the file as another sweep removed it. Any other text is not our marker
            if (owner.empty() && change) removeMarker(marker);
            leftover(kNoMarker);
            gone();
            return;
        }
        if (!change) return;
        if (there) {
            removeFolder(folder, ec);
            if (ec) {
                Out::warning("Could not remove the staging folder %s (retried next time): %s", text(folder).c_str(), ec.message().c_str());
                out.failed.push_back(folder);
                return;
            }
            Out::info("Removed %s, left by a stopped import or template copy", text(folder).c_str());
            out.removed.push_back(folder);
        }
        removeMarker(marker);
        journalRemove(folder);
        // the user templates' staging parent is ours (TemplateCatalog) and goes when it is empty
        if (folder.parent_path().filename() == ".staging") fs::remove(folder.parent_path(), ec);
    }

    Sweep look(const Scope& scope, bool change) {
        // by folder: a folder in a parent and in the journal is visited once; a marker without its
        // folder counts as well (a crash after the folder moved to its place)
        std::set<fs::path> folders;
        for (const fs::path& parent : scope.parents) {
            std::error_code ec;
            for (fs::directory_iterator it(parent, ec), end; !ec && it != end; it.increment(ec)) {
                std::string name = FileUtils::pathToUtf8(it->path().filename());
                const size_t suffix = std::char_traits<char>::length(kMarkerSuffix);
                if (name.size() > suffix && name.compare(name.size() - suffix, suffix, kMarkerSuffix) == 0) name.resize(name.size() - suffix);
                if (isStagingName(name)) folders.insert((parent / FileUtils::pathFromUtf8(name)).lexically_normal());
            }
        }
        for (const fs::path& folder : journalFolders(change)) folders.insert(folder.lexically_normal());
        std::set<fs::path> projects;
        for (const fs::path& project : scope.projects) projects.insert(project.lexically_normal());
        Sweep out;
        for (const fs::path& folder : folders) visit(folder, change, projects, out);
        return out;
    }

}

bool isStagingName(const std::string& folderName) {
    const size_t at = folderName.rfind(kFolderSuffix);
    if (at == std::string::npos || at == 0) return false;
    const size_t digits = at + std::char_traits<char>::length(kFolderSuffix);
    return std::all_of(folderName.begin() + (std::ptrdiff_t)digits, folderName.end(), [](char c) { return c >= '0' && c <= '9'; });
}

void setJournalDirectory(const fs::path& directory) {
    std::lock_guard<std::mutex> guard(stateMutex);
    journalDirectory = directory;
}

Claim::Claim(const fs::path& parent, const std::string& name) {
    std::error_code ec;
    fs::create_directories(parent, ec);
    for (int number = 1; number <= kMaxNumber; number++) {
        const fs::path folder = (parent / (name + kFolderSuffix + (number > 1 ? std::to_string(number) : std::string()))).lexically_normal();
        const fs::path marker = markerOf(folder);
        std::string noLocks;
        if (fs::exists(fs::symlink_status(folder, ec)) || fs::exists(fs::symlink_status(marker, ec))) continue;
        auto taken = std::make_unique<io::ScopedFileLock>(marker, io::ScopedFileLock::Mode::Exclusive, false);
        if (taken->getStatus() == io::ScopedFileLock::Status::Busy) continue;
        if (taken->isLocked()) {
            // a sweep may have removed this file between our open and our lock (it removes the file
            // under its own lock): then the lock is on a file nobody else can see
            const std::string owner = ownerText();
            if (!taken->writeOwnerInfo(owner)) {
                removeMarker(marker);
                failure = "cannot write " + text(marker);
                return;
            }
            if (io::ScopedFileLock::readOwnerInfo(marker) != owner) continue;
        } else {
            noLocks = taken->failureReason();
            taken.reset();
        }
        const bool made = fs::create_directory(folder, ec);
        if (!made) {
            if (taken) removeMarker(marker);
            if (ec) {
                failure = "cannot create " + text(folder) + ": " + ec.message();
                return;
            }
            continue;   // another editor's, an instant ago
        }
        if (!taken) {
            // a share without locks: a folder no sweep will remove, and the copy goes on (S03 R10)
            Out::warning("%s has no file locks: if the editor stops during this copy, its staging folder stays to be removed by hand (%s)",
                         text(parent).c_str(), noLocks.c_str());
            std::lock_guard<std::mutex> guard(stateMutex);
            unmarkedClaims.insert(folder);
        }
        journalAdd(folder);
        folderPath = folder;
        lock = std::move(taken);
        return;
    }
    failure = "no free staging folder for " + name + " in " + text(parent);
}

Claim::~Claim() {
    release();
}

void Claim::release() {
    if (folderPath.empty()) return;
    std::error_code ec;
    const bool stays = fs::exists(fs::symlink_status(folderPath, ec));
    if (!stays) {
        if (lock) removeMarker(markerOf(folderPath));
        journalRemove(folderPath);
    }
    {
        std::lock_guard<std::mutex> guard(stateMutex);
        unmarkedClaims.erase(folderPath);
    }
    lock.reset();
    folderPath.clear();
}

Sweep sweep(const Scope& scope) {
    return look(scope, true);
}

Sweep inspect(const Scope& scope) {
    return look(scope, false);
}

bool removeLeftover(const fs::path& given, std::string& error) {
    const fs::path folder = given.lexically_normal();
    if (!isStagingName(FileUtils::pathToUtf8(folder.filename()))) {
        error = text(folder) + " is not a staging folder";
        return false;
    }
    std::error_code ec;
    if (!fs::is_directory(fs::symlink_status(folder, ec))) {
        error = text(folder) + " is not there any more";
        return false;
    }
    {
        std::lock_guard<std::mutex> guard(stateMutex);
        if (unmarkedClaims.count(folder)) {
            error = text(folder) + " is in use by this editor";
            return false;
        }
    }
    const fs::path marker = markerOf(folder);
    std::unique_ptr<io::ScopedFileLock> lock;
    if (fs::exists(marker, ec)) {
        lock = std::make_unique<io::ScopedFileLock>(marker, io::ScopedFileLock::Mode::Exclusive, false);
        if (lock->getStatus() == io::ScopedFileLock::Status::Busy) {
            error = text(folder) + " is in use by another editor";
            return false;
        }
    }
    removeFolder(folder, ec);
    if (ec) {
        error = "cannot remove " + text(folder) + ": " + ec.message();
        Out::warning("%s", error.c_str());
        return false;
    }
    if (lock && lock->isLocked()) {
        const std::string owner = io::ScopedFileLock::readOwnerInfo(marker);
        if (owner.empty() || isOwnerText(owner)) removeMarker(marker);
    }
    journalRemove(folder);
    return true;
}

void setRemovalFaultInjector(RemovalFaultInjector injector) {
    std::lock_guard<std::mutex> guard(stateMutex);
    removalFault = std::move(injector);
}

void holdForTest() {
    const char* hold = std::getenv("JASPER_TEST_STAGING_HOLD");
    if (!hold || !*hold) return;
    const fs::path path(hold);
    std::error_code ec;
    if (!fs::exists(path, ec)) return;
    io::writeFileAtomic(fs::path(path.string() + ".held"), "held\n");
    while (fs::exists(path, ec)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

}
