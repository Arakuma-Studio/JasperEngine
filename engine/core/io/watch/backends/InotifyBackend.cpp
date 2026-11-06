// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Linux: one inotify watch per folder, renames paired by cookie (S05 R16).

#include "Backends.h"

#if defined(__linux__) && !defined(__ANDROID__)

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>

using namespace jasperengine;

namespace fs = std::filesystem;

namespace jasperengine::watch {

    class InotifyBackend : public FileWatcher {
    public:
        ~InotifyBackend() override { stop(); }

        bool start(const fs::path& rootIn, WatchSink& sinkIn, std::string& error) override;
        void stop() override;
        bool flush(std::chrono::milliseconds timeout) override;
        bool flushIsDrain() const override { return true; }
        const char* name() const override { return "inotify"; }
        bool isNative() const override { return true; }
        size_t watchCount() const override { return watches.load(); }
        void injectOverflowForTesting() override;

    private:
        static constexpr uint32_t MASK = IN_CLOSE_WRITE | IN_MODIFY | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
                                         IN_DELETE_SELF | IN_MOVE_SELF | IN_ATTRIB;
        // An IN_MOVED_FROM waits this long for its IN_MOVED_TO (then it was a move out)
        static constexpr std::chrono::milliseconds MOVE_HOLD{200};

        struct PendingMove {
            uint32_t cookie = 0;
            fs::path path;
            bool directory = false;
            std::chrono::steady_clock::time_point at{};
        };

        fs::path root;
        WatchSink* sink = nullptr;
        int fd = -1;
        int wakeFd = -1;
        int probeWd = -1;
        std::string probeName;
        std::thread thread;
        std::atomic<bool> running{false};
        std::atomic<bool> failed{false};
        std::atomic<size_t> watches{0};

        // the fd reads, the folder maps and the pending moves (the thread and flush())
        std::mutex readMutex;
        std::unordered_map<int, fs::path> folders;   // wd -> folder
        std::unordered_map<std::string, int> wdOf;    // folder (generic) -> wd
        std::vector<PendingMove> pendingMoves;

        void run();
        void drainLocked();
        void resolveMovesLocked(std::chrono::steady_clock::time_point now, bool all, std::vector<RawEvent>& out);
        // Watches `folder` and the folders under it; `found` gets what is in them
        bool addTree(const fs::path& folder, std::vector<RawEvent>* found);
        void forgetTree(const fs::path& folder);
        void renameTree(const fs::path& from, const fs::path& to);
        void deliver(std::vector<RawEvent>&& events);
    };

}

using namespace jasperengine::watch;

bool InotifyBackend::start(const fs::path& rootIn, WatchSink& sinkIn, std::string& error) {
    stop();
    root = rootIn.lexically_normal();
    sink = &sinkIn;
    failed = false;
    fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) {
        error = std::string("inotify_init1 failed: ") + std::strerror(errno);
        return false;
    }
    wakeFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeFd < 0) {
        error = std::string("eventfd failed: ") + std::strerror(errno);
        close(fd);
        fd = -1;
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(readMutex);
        if (!addTree(root, nullptr)) {
            error = failed ? "the inotify watch limit is reached (raise fs.inotify.max_user_watches)"
                           : "the project folder cannot be watched";
            for (const auto& [wd, folder] : folders) inotify_rm_watch(fd, wd);
            folders.clear();
            wdOf.clear();
            watches = 0;
            close(fd);
            close(wakeFd);
            fd = wakeFd = -1;
            return false;
        }
        // the self-test probe lives in an excluded folder: watched for its own name only
        if (!probePath.empty()) {
            probeName = probePath.filename().string();
            const int wd = inotify_add_watch(fd, probePath.parent_path().c_str(), IN_CLOSE_WRITE | IN_CREATE | IN_MOVED_TO | IN_ONLYDIR);
            probeWd = wd >= 0 ? wd : -1;
        }
    }
    running = true;
    thread = std::thread([this]() { run(); });
    return true;
}

void InotifyBackend::stop() {
    if (thread.joinable()) {
        running = false;
        const uint64_t one = 1;
        if (write(wakeFd, &one, sizeof(one)) < 0) {
            // the loop also wakes on its next event; join below waits either way
        }
        thread.join();
    }
    running = false;
    std::lock_guard<std::mutex> lock(readMutex);
    if (fd >= 0) close(fd);
    if (wakeFd >= 0) close(wakeFd);
    fd = wakeFd = -1;
    probeWd = -1;
    folders.clear();
    wdOf.clear();
    pendingMoves.clear();
    watches = 0;
}

bool InotifyBackend::addTree(const fs::path& folder, std::vector<RawEvent>* found) {
    const int wd = inotify_add_watch(fd, folder.c_str(), MASK | IN_ONLYDIR | (folder == root ? 0 : IN_DONT_FOLLOW));
    if (wd < 0) {
        if (errno == ENOSPC) {
            failed = true;
            return false;
        }
        // gone already, or not readable: nothing to watch there
        return folder != root;
    }
    auto known = folders.find(wd);
    if (known != folders.end()) wdOf.erase(known->second.generic_string());
    folders[wd] = folder;
    wdOf[folder.generic_string()] = wd;
    watches = folders.size();

    std::error_code ec;
    fs::directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec);
    for (fs::directory_iterator end; !ec && it != end; it.increment(ec)) {
        const std::string entryName = it->path().filename().string();
        if (isExcludedName(entryName)) continue;
        std::error_code typeEc;
        const bool link = it->is_symlink(typeEc);
        const bool directory = it->is_directory(typeEc);
        // files written before the watch existed still count (S05 R5)
        if (found) found->push_back(RawEvent{RawEvent::Type::Created, it->path(), {}, directory});
        if (directory && !link && !addTree(it->path(), found)) return false;
    }
    return true;
}

void InotifyBackend::forgetTree(const fs::path& folder) {
    const std::string prefix = folder.generic_string() + "/";
    for (auto it = folders.begin(); it != folders.end();) {
        const std::string path = it->second.generic_string();
        if (path == folder.generic_string() || path.compare(0, prefix.size(), prefix) == 0) {
            inotify_rm_watch(fd, it->first);
            wdOf.erase(path);
            it = folders.erase(it);
        } else {
            ++it;
        }
    }
    watches = folders.size();
}

void InotifyBackend::renameTree(const fs::path& from, const fs::path& to) {
    const std::string fromText = from.generic_string();
    const std::string prefix = fromText + "/";
    for (auto& [wd, folder] : folders) {
        const std::string path = folder.generic_string();
        if (path == fromText) {
            folder = to;
        } else if (path.compare(0, prefix.size(), prefix) == 0) {
            folder = to / fs::path(path.substr(prefix.size()));
        } else {
            continue;
        }
        wdOf.erase(path);
        wdOf[folder.generic_string()] = wd;
    }
}

void InotifyBackend::deliver(std::vector<RawEvent>&& events) {
    if (events.empty() || suppressForTesting) return;
    sink->onRawEvents(std::move(events));
}

void InotifyBackend::resolveMovesLocked(std::chrono::steady_clock::time_point now, bool all, std::vector<RawEvent>& out) {
    for (auto it = pendingMoves.begin(); it != pendingMoves.end();) {
        if (!all && now - it->at < MOVE_HOLD) {
            ++it;
            continue;
        }
        // no IN_MOVED_TO: it left the watched tree (or went to an excluded name)
        out.push_back(RawEvent{RawEvent::Type::Removed, it->path, {}, it->directory});
        if (it->directory) forgetTree(it->path);
        it = pendingMoves.erase(it);
    }
}

void InotifyBackend::drainLocked() {
    alignas(inotify_event) char buffer[64 * 1024];
    std::vector<RawEvent> events;
    bool rootGone = false;
    while (true) {
        const ssize_t length = read(fd, buffer, sizeof(buffer));
        if (length <= 0) break;   // EAGAIN: drained
        for (char* p = buffer; p < buffer + length;) {
            const inotify_event* ev = reinterpret_cast<const inotify_event*>(p);
            p += sizeof(inotify_event) + ev->len;

            if (ev->mask & IN_Q_OVERFLOW) {
                // the kernel queue overflowed: what was lost is found by a rescan (S05 R6)
                events.push_back(RawEvent{RawEvent::Type::Rescan, root, {}, true});
                continue;
            }
            if (ev->wd == probeWd && probeWd >= 0) {
                if (ev->len > 0 && probeName == ev->name) sink->onProbe();
                continue;
            }
            auto folderIt = folders.find(ev->wd);
            if (folderIt == folders.end()) continue;
            const fs::path folder = folderIt->second;

            if (ev->mask & IN_IGNORED) {
                wdOf.erase(folder.generic_string());
                folders.erase(folderIt);
                watches = folders.size();
                continue;
            }
            if (ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT)) {
                // a subfolder's own removal or rename comes from its parent's events
                if (folder == root) rootGone = true;
                continue;
            }
            if (ev->len == 0) continue;
            const std::string entryName = ev->name;
            if (isExcludedName(entryName)) continue;
            const fs::path path = folder / entryName;
            const bool directory = (ev->mask & IN_ISDIR) != 0;

            if (ev->mask & IN_CREATE) {
                events.push_back(RawEvent{RawEvent::Type::Created, path, {}, directory});
                if (directory && !addTree(path, &events)) {
                    sink->onBackendFailed("the inotify watch limit is reached (raise fs.inotify.max_user_watches)");
                    failed = true;
                }
            } else if (ev->mask & IN_DELETE) {
                events.push_back(RawEvent{RawEvent::Type::Removed, path, {}, directory});
            } else if (ev->mask & IN_MOVED_FROM) {
                pendingMoves.push_back(PendingMove{ev->cookie, path, directory, std::chrono::steady_clock::now()});
            } else if (ev->mask & IN_MOVED_TO) {
                auto from = std::find_if(pendingMoves.begin(), pendingMoves.end(), [ev](const PendingMove& m) { return m.cookie == ev->cookie; });
                if (from != pendingMoves.end()) {
                    events.push_back(RawEvent{RawEvent::Type::Moved, path, from->path, directory});
                    if (directory) renameTree(from->path, path);
                    pendingMoves.erase(from);
                } else {
                    // moved in from outside the tree
                    events.push_back(RawEvent{RawEvent::Type::Created, path, {}, directory});
                    if (directory && !addTree(path, &events)) {
                        sink->onBackendFailed("the inotify watch limit is reached (raise fs.inotify.max_user_watches)");
                        failed = true;
                    }
                }
            } else if (!directory && (ev->mask & (IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB))) {
                events.push_back(RawEvent{RawEvent::Type::Modified, path, {}, false});
            }
        }
    }
    resolveMovesLocked(std::chrono::steady_clock::now(), false, events);
    deliver(std::move(events));
    if (rootGone) sink->onBackendFailed("the project folder was moved or deleted");
}

void InotifyBackend::run() {
    while (running) {
        int timeout = -1;   // idle: blocked in the kernel, no wakeup (S05 R21)
        {
            std::lock_guard<std::mutex> lock(readMutex);
            if (!pendingMoves.empty()) {
                const auto now = std::chrono::steady_clock::now();
                auto earliest = pendingMoves.front().at;
                for (const PendingMove& move : pendingMoves) earliest = std::min(earliest, move.at);
                const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(earliest + MOVE_HOLD - now).count();
                timeout = static_cast<int>(std::max<long long>(0, wait) + 1);
            }
        }
        pollfd fds[2] = {{fd, POLLIN, 0}, {wakeFd, POLLIN, 0}};
        const int ready = poll(fds, 2, timeout);
        if (!running) break;
        if (ready < 0 && errno != EINTR) break;
        if (fds[1].revents & POLLIN) {
            uint64_t value = 0;
            if (read(wakeFd, &value, sizeof(value)) < 0) {
                // a spurious wake: nothing to consume
            }
        }
        std::lock_guard<std::mutex> lock(readMutex);
        drainLocked();
    }
}

bool InotifyBackend::flush(std::chrono::milliseconds timeout) {
    if (!lockWithin(readMutex, timeout)) return false;
    std::unique_lock<std::mutex> lock(readMutex, std::adopt_lock);
    if (fd < 0) return false;
    drainLocked();
    // every IN_MOVED_TO that belongs to a completed rename is read by now
    std::vector<RawEvent> events;
    resolveMovesLocked(std::chrono::steady_clock::now(), true, events);
    deliver(std::move(events));
    return true;
}

void InotifyBackend::injectOverflowForTesting() {
    std::lock_guard<std::mutex> lock(readMutex);
    // what drainLocked does with IN_Q_OVERFLOW
    std::vector<RawEvent> events{RawEvent{RawEvent::Type::Rescan, root, {}, true}};
    sink->onRawEvents(std::move(events));
}

std::unique_ptr<FileWatcher> jasperengine::watch::createInotifyBackend() {
    return std::make_unique<InotifyBackend>();
}

#endif
