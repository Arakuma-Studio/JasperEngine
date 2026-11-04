// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Every platform: a stat scanner on its own thread (S05 R19). The tiers come from
// McpAutoSync's scan: folders, recently changed files and code files are checked whole on
// every pass (code files up to a cap, then rotating), assets rotate within a time budget.
// A removal and an addition of one file identity in a pass are one rename (OpenEngine's
// polling diff).

#include "Backends.h"
#include "jobs/detail/TimedWait.h"
#include "LinkedFolders.h"
#include "ProjectFileIndex.h"

#include <algorithm>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

using namespace jasperengine;
using namespace jasperengine::watch;

namespace fs = std::filesystem;

namespace {

    constexpr size_t HOT_MAX = 128;
    constexpr size_t CODE_WHOLE_MAX = 2000;
    // the asset tier's share of a pass, and of a flush on a big project
    constexpr std::chrono::microseconds ASSET_BUDGET{8000};
    // a flush checks every file up to this many
    constexpr size_t FLUSH_WHOLE_MAX = 20000;

    bool isCodeTier(const std::string& key) {
        static const std::set<std::string> code = {
            ".lua", ".cpp", ".cc", ".cxx", ".c", ".h", ".hh", ".hpp", ".hxx", ".inl", ".glsl", ".vert", ".frag", ".hlsl",
            ".material", ".scene", ".prefab", ".yaml", ".yml", ".html", ".htm", ".css", ".json", ".txt", ".md", ".xml"
        };
        return code.count(lowerExtension(pathFromUtf8(key))) > 0;
    }

    std::string parentKey(const std::string& key) {
        const size_t slash = key.find_last_of('/');
        return slash == std::string::npos ? std::string() : key.substr(0, slash);
    }

    bool keyWithin(const std::string& key, const std::string& dir) {
        return key.size() > dir.size() && key.compare(0, dir.size(), dir) == 0 && key[dir.size()] == '/';
    }

}

namespace jasperengine::watch {

    class PollingBackend : public FileWatcher {
    public:
        explicit PollingBackend(std::chrono::milliseconds interval)
            : interval(std::max(interval, std::chrono::milliseconds(250))) {
        }
        ~PollingBackend() override { stop(); }

        bool start(const fs::path& rootIn, WatchSink& sinkIn, std::string& error) override;
        void stop() override;
        bool flush(std::chrono::milliseconds timeout) override;
        const char* name() const override { return "polling"; }
        bool isNative() const override { return false; }
        size_t watchCount() const override { return running ? 1 : 0; }
        void injectOverflowForTesting() override;

    private:
        std::chrono::milliseconds interval;
        fs::path root;
        WatchSink* sink = nullptr;
        std::thread thread;
        std::atomic<bool> running{false};

        std::mutex wakeMutex;
        std::condition_variable wakeCv;
        bool stopRequested = false;

        // the scan state (the thread and flush())
        std::mutex scanMutex;
        bool built = false;
        ProjectFileIndex index;
        int64_t rootMtime = 0;
        // Folders whose mtime was stamped within the timestamp granularity of a change (racy:
        // another entry added in the same tick keeps it): listed again on the next scan
        bool rootRacy = false;
        std::set<std::string> racyFolders;
        void noteFolderStamp(const std::string& key, int64_t mtimeNs);
        std::vector<std::string> hot;
        std::string codeCursor, assetCursor;

        void run();
        void scanLocked(bool flushing);
        void markHot(const std::string& key);
        // folders whose entries changed: additions, removals and renames
        void scanFolders(std::vector<RawEvent>& events);
        void checkFile(const std::string& key, std::vector<RawEvent>& events);
    };

}

bool PollingBackend::start(const fs::path& rootIn, WatchSink& sinkIn, std::string& error) {
    stop();
    root = rootIn.lexically_normal();
    sink = &sinkIn;
    PathStat rootStat;
    if (!statPath(root, rootStat, false) || !rootStat.directory) {
        error = "the project folder does not exist";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(wakeMutex);
        stopRequested = false;
    }
    built = false;
    running = true;
    thread = std::thread([this]() { run(); });
    return true;
}

void PollingBackend::stop() {
    if (thread.joinable()) {
        {
            std::lock_guard<std::mutex> lock(wakeMutex);
            stopRequested = true;
        }
        wakeCv.notify_all();
        thread.join();
    }
    running = false;
    std::lock_guard<std::mutex> lock(scanMutex);
    index.reset({});
    hot.clear();
    racyFolders.clear();
    rootRacy = false;
    built = false;
}

void PollingBackend::run() {
    {
        // the first look at the tree is the baseline, on this thread (not the caller's)
        std::lock_guard<std::mutex> lock(scanMutex);
        index.reset(root);
        index.build();
        PathStat rootStat;
        rootMtime = statPath(root, rootStat, false) ? rootStat.mtimeNs : 0;
        rootRacy = isRacyStamp(rootMtime);
        racyFolders.clear();
        for (const auto& [key, entry] : index.all()) {
            if (entry.directory) noteFolderStamp(key, entry.mtimeNs);
        }
        built = true;
    }
    while (true) {
        {
            std::unique_lock<std::mutex> lock(wakeMutex);
            // (TimedWait: GCC 11's TSan does not see libstdc++'s steady-clock wait)
            jobs::detail::timedWait(wakeCv, lock, interval, [this]() { return stopRequested; });
            if (stopRequested) break;
        }
        std::lock_guard<std::mutex> lock(scanMutex);
        scanLocked(false);
    }
}

void PollingBackend::markHot(const std::string& key) {
    auto it = std::find(hot.begin(), hot.end(), key);
    if (it != hot.end()) hot.erase(it);
    hot.push_back(key);
    if (hot.size() > HOT_MAX) hot.erase(hot.begin());
}

void PollingBackend::noteFolderStamp(const std::string& key, int64_t mtimeNs) {
    if (isRacyStamp(mtimeNs)) racyFolders.insert(key);
    else racyFolders.erase(key);
}

void PollingBackend::scanFolders(std::vector<RawEvent>& events) {
    // folders to list again: their mtime moved (an entry was added, removed or renamed), or
    // it was racy when it was stamped
    std::vector<std::string> changed;
    PathStat rootStat;
    if (statPath(root, rootStat, false) && (rootStat.mtimeNs != rootMtime || rootRacy)) {
        rootMtime = rootStat.mtimeNs;
        rootRacy = isRacyStamp(rootMtime);
        changed.push_back("");
    }
    std::vector<std::string> folders;
    for (const auto& [key, entry] : index.all()) {
        if (entry.directory) folders.push_back(key);
    }
    std::set<std::string> gone;
    for (const std::string& key : folders) {
        PathStat now;
        if (!statPath(index.absolute(key), now, false) || !now.directory) {
            gone.insert(key);   // its parent's listing says how it went
            const std::string parent = parentKey(key);
            if (std::find(changed.begin(), changed.end(), parent) == changed.end()) changed.push_back(parent);
            continue;
        }
        ProjectFileIndex::Entry known;
        index.find(index.absolute(key), known);
        if (now.mtimeNs != known.mtimeNs || racyFolders.count(key)) {
            // a link to a folder is an entry here, not a folder of this tree: listing it would take
            // its target's entries for new ones (and a link to itself, level after level). The
            // service gives a linked folder a backend of its own.
            if (isLink(index.absolute(key))) {
                racyFolders.erase(key);
                continue;
            }
            changed.push_back(key);
        }
    }
    for (const std::string& key : gone) racyFolders.erase(key);
    if (changed.empty()) return;

    // What each changed folder lists now against what the index has in it
    std::map<std::string, PathStat> added;     // new entries (with what is under new folders)
    std::set<std::string> removed;             // index entries no longer there
    std::function<void(const std::string&)> walkNew = [&](const std::string& dir) {
        std::error_code ec;
        fs::directory_iterator it(index.absolute(dir), fs::directory_options::skip_permission_denied, ec);
        for (fs::directory_iterator end; !ec && it != end; it.increment(ec)) {
            const std::string entryName = pathToUtf8(it->path().filename());
            if (!ProjectFileIndex::indexes(entryName)) continue;
            PathStat stat;
            if (!statPath(it->path(), stat)) continue;
            const std::string child = dir.empty() ? entryName : dir + "/" + entryName;
            added[child] = stat;
            std::error_code typeEc;
            if (stat.directory && !it->is_symlink(typeEc)) walkNew(child);
        }
    };
    // what the index holds directly in each changed folder
    std::map<std::string, std::vector<std::string>> children;
    {
        const std::set<std::string> wanted(changed.begin(), changed.end());
        for (const auto& [key, entry] : index.all()) {
            const std::string parent = parentKey(key);
            if (wanted.count(parent)) children[parent].push_back(key);
        }
    }
    for (const std::string& dir : changed) {
        if (gone.count(dir)) continue;
        std::set<std::string> present;
        std::error_code ec;
        fs::directory_iterator it(index.absolute(dir), fs::directory_options::skip_permission_denied, ec);
        for (fs::directory_iterator end; !ec && it != end; it.increment(ec)) {
            const std::string entryName = pathToUtf8(it->path().filename());
            if (!ProjectFileIndex::indexes(entryName)) continue;
            const std::string child = dir.empty() ? entryName : dir + "/" + entryName;
            present.insert(child);
            ProjectFileIndex::Entry known;
            const bool isKnown = index.find(it->path(), known);
            PathStat stat;
            if (!statPath(it->path(), stat)) continue;
            if (isKnown && known.directory == stat.directory) continue;   // modifications: checkFile
            if (isKnown) removed.insert(child);                           // a file replaced by a folder (or back)
            added[child] = stat;
            std::error_code typeEc;
            if (stat.directory && !it->is_symlink(typeEc)) walkNew(child);
        }
        if (ec) continue;   // a listing that failed half-way removes nothing
        for (const std::string& key : children[dir]) {
            if (!present.count(key)) removed.insert(key);
        }
        if (!dir.empty()) {
            PathStat now;
            if (statPath(index.absolute(dir), now)) {
                ProjectFileIndex::Entry known;
                if (index.find(index.absolute(dir), known)) {
                    known.mtimeNs = now.mtimeNs;
                    index.set(index.absolute(dir), known);
                    noteFolderStamp(dir, now.mtimeNs);
                }
            }
        }
    }

    // A removal and an addition of one identity are a rename (parents first, so a folder's
    // rename covers what it holds)
    std::unordered_map<FileIdentity, std::string> removedByIdentity;
    for (const std::string& key : removed) {
        ProjectFileIndex::Entry known;
        if (index.find(index.absolute(key), known) && known.identity.valid) removedByIdentity[known.identity] = key;
    }
    std::vector<std::pair<std::string, std::string>> moves;   // from, to
    std::set<std::string> movedTo, movedFrom;
    for (const auto& [key, stat] : added) {
        bool covered = false;
        for (const std::string& to : movedTo) {
            if (keyWithin(key, to)) covered = true;
        }
        if (covered) continue;
        auto it = stat.identity.valid ? removedByIdentity.find(stat.identity) : removedByIdentity.end();
        if (it == removedByIdentity.end()) continue;
        ProjectFileIndex::Entry known;
        index.find(index.absolute(it->second), known);
        if (known.directory != stat.directory) continue;
        moves.emplace_back(it->second, key);
        movedTo.insert(key);
        movedFrom.insert(it->second);
        removedByIdentity.erase(it);
    }

    auto underAny = [](const std::string& key, const std::set<std::string>& roots) {
        for (const std::string& rootKey : roots) {
            if (key == rootKey || keyWithin(key, rootKey)) return true;
        }
        return false;
    };

    for (const auto& [from, to] : moves) {
        ProjectFileIndex::Entry known;
        index.find(index.absolute(from), known);
        const PathStat now = added[to];
        index.move(index.absolute(from), index.absolute(to));
        index.set(index.absolute(to), now);
        if (now.directory) {
            racyFolders.erase(from);
            noteFolderStamp(to, now.mtimeNs);
        }
        events.push_back(RawEvent{RawEvent::Type::Moved, index.absolute(to), index.absolute(from), now.directory});
        if (!now.directory && !known.sameContent(now)) events.push_back(RawEvent{RawEvent::Type::Modified, index.absolute(to), {}, false});
        if (!now.directory) markHot(to);
    }
    // removals: the top-most entry only (the service expands a folder from its own index)
    for (const std::string& key : removed) {
        if (underAny(key, movedFrom)) continue;
        bool parentRemoved = false;
        for (const std::string& other : removed) {
            if (keyWithin(key, other) && !underAny(other, movedFrom)) parentRemoved = true;
        }
        ProjectFileIndex::Entry known;
        const bool isKnown = index.find(index.absolute(key), known);
        if (!parentRemoved) events.push_back(RawEvent{RawEvent::Type::Removed, index.absolute(key), {}, isKnown && known.directory});
        index.erase(index.absolute(key));
    }
    for (const auto& [key, stat] : added) {
        if (stat.directory) noteFolderStamp(key, stat.mtimeNs);
        if (underAny(key, movedTo)) continue;
        index.set(index.absolute(key), stat);
        events.push_back(RawEvent{RawEvent::Type::Created, index.absolute(key), {}, stat.directory});
        if (!stat.directory) markHot(key);
    }
}

void PollingBackend::checkFile(const std::string& key, std::vector<RawEvent>& events) {
    ProjectFileIndex::Entry known;
    const fs::path path = index.absolute(key);
    if (!index.find(path, known) || known.directory) return;
    PathStat now;
    // the identity only when the stamp moved: it costs a handle open on Windows
    if (!statPath(path, now, false)) {
        // its folder's listing reports it (with a rename it may be part of)
        return;
    }
    if (now.sameContent(known)) return;
    PathStat withIdentity;
    if (!statPath(path, withIdentity)) return;
    index.set(path, withIdentity);
    events.push_back(RawEvent{RawEvent::Type::Modified, path, {}, false});
    markHot(key);
}

void PollingBackend::scanLocked(bool flushing) {
    if (!built) return;
    std::vector<RawEvent> events;
    scanFolders(events);

    const auto start = std::chrono::steady_clock::now();
    const bool whole = flushing && index.size() <= FLUSH_WHOLE_MAX;

    for (const std::string& key : std::vector<std::string>(hot)) checkFile(key, events);

    // code files: whole up to a cap, then rotating
    std::vector<std::string> code, assets;
    for (const auto& [key, entry] : index.all()) {
        if (entry.directory) continue;
        (isCodeTier(key) ? code : assets).push_back(key);
    }
    auto rotate = [&](std::vector<std::string>& keys, std::string& cursor, size_t maxCount, std::chrono::microseconds budget) {
        if (keys.empty()) return;
        auto first = std::lower_bound(keys.begin(), keys.end(), cursor);
        size_t index0 = static_cast<size_t>(first - keys.begin()) % keys.size();
        for (size_t done = 0; done < keys.size(); done++) {
            if (done >= maxCount || (budget.count() > 0 && done > 0 && std::chrono::steady_clock::now() - start >= budget)) {
                cursor = keys[(index0 + done) % keys.size()];
                return;
            }
            checkFile(keys[(index0 + done) % keys.size()], events);
        }
        cursor.clear();
    };
    rotate(code, codeCursor, whole ? code.size() : CODE_WHOLE_MAX, std::chrono::microseconds(0));
    rotate(assets, assetCursor, assets.size(), whole ? std::chrono::microseconds(0) : ASSET_BUDGET);

    if (events.empty() || suppressForTesting) return;
    sink->onRawEvents(std::move(events));
}

bool PollingBackend::flush(std::chrono::milliseconds timeout) {
    if (!lockWithin(scanMutex, timeout)) return false;
    std::unique_lock<std::mutex> lock(scanMutex, std::adopt_lock);
    if (!built) return false;
    scanLocked(true);
    return true;
}

void PollingBackend::injectOverflowForTesting() {
    std::lock_guard<std::mutex> lock(scanMutex);
    // the scanner has no overflow of its own: the same answer, a rescan of everything
    if (built) index.build();
    std::vector<RawEvent> events{RawEvent{RawEvent::Type::Rescan, root, {}, true}};
    sink->onRawEvents(std::move(events));
}

std::unique_ptr<FileWatcher> jasperengine::watch::createPollingBackend(std::chrono::milliseconds interval) {
    return std::make_unique<PollingBackend>(interval);
}
