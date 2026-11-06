// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The rename pairing, the delete hold and the FSEvents/ReadDirectoryChangesW loops of the
// backends follow OpenEngine's Engine/Modules/FileWatcher (MIT, NOTICE.md).

#ifndef JASPERENGINE_FILEWATCHER_H
#define JASPERENGINE_FILEWATCHER_H

#include "Export.h"
#include "FileIdentity.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine {

    // What a delivered change says happened to `path` (S05 R1)
    enum class FileChangeKind : uint8_t { Added, Modified, Removed, Moved, Rescan };
    // Who made it: another program, or this editor (ScopedExpectedWrite::reported)
    enum class ChangeOrigin : uint8_t { External, Editor };

    struct JASPERENGINE_API FileChange {
        FileChangeKind kind = FileChangeKind::Modified;
        std::filesystem::path path;      // absolute, lexically normal
        std::filesystem::path oldPath;   // Moved only
        bool directory = false;
        uint64_t size = 0;               // what the file had when the change settled
        int64_t mtimeNs = 0;             // ns since the Unix epoch
        FileIdentity identity;
        ChangeOrigin origin = ChangeOrigin::External;
        uint64_t sequence = 0;           // journal order (FileWatchService)
    };

    JASPERENGINE_API const char* fileChangeKindName(FileChangeKind kind);

    // Auto = Native, else Polling (S05 R1)
    enum class WatchBackendKind : uint8_t { Auto, Native, Polling, Off };

    JASPERENGINE_API const char* watchBackendKindName(WatchBackendKind kind);
    // "auto", "native", "polling", "off" (case-insensitive); false for anything else
    JASPERENGINE_API bool parseWatchBackendKind(const std::string& text, WatchBackendKind& out);

    namespace watch {

        // What one stat of a path says
        struct JASPERENGINE_API PathStat {
            uint64_t size = 0;
            int64_t mtimeNs = 0;
            bool directory = false;
            FileIdentity identity;   // filled when asked for (free on POSIX)

            bool sameContent(const PathStat& other) const {
                return size == other.size && mtimeNs == other.mtimeNs && directory == other.directory;
            }
        };

        // False when nothing is at `path`. `identity`: also read the file identity (a handle
        // open on Windows, part of the stat on POSIX).
        JASPERENGINE_API bool statPath(const std::filesystem::path& path, PathStat& out, bool identity = true);

        // A stamp taken this close to the file's mtime cannot tell the write it saw from a later
        // one in the same timestamp tick (ext4's coarse clock ticks every 1-10 ms, NTFS updates
        // the time every ~16 ms): two saves 1 ms apart can have the same mtime and size. Such a
        // stamp is "racy" (as git calls its index entries) and is not trusted to say a file is
        // unchanged. Shorter than the coalescer's quiet window, so the watcher's own stamps are
        // not racy.
        constexpr std::chrono::milliseconds RACY_STAMP_WINDOW{100};
        // `mtimeNs` (ns since the Unix epoch) is within RACY_STAMP_WINDOW of now (or ahead of it)
        JASPERENGINE_API bool isRacyStamp(int64_t mtimeNs);

        // The watch exclusions (S05 R2, the rule McpAutoSync used): a name starting with '.',
        // and build, node_modules and trash in any case. Applied to every component.
        JASPERENGINE_API bool isExcludedName(const std::string& name);
        // Whether `path` (under `root`) has an excluded component below the root. Lexical, no
        // filesystem call (no fs::relative: a path outside the root is excluded).
        JASPERENGINE_API bool isExcludedPath(const std::filesystem::path& root, const std::filesystem::path& path);
        // Safe-save temporaries and backups (S05 R3): *.tmp, *~, .#*, *.swp, *.swx, 4913,
        // *.crdownload, *___jb_tmp___, *___jb_old___, *.kra~ and the atomic writer's *.je-tmp.
        // They take part in rename pairing but never reach a subscriber.
        JASPERENGINE_API bool isTemporaryName(const std::string& filename);
        // UTF-8 text of a path with '/' separators, and back (C++17 and C++20 alike)
        JASPERENGINE_API std::string pathToUtf8(const std::filesystem::path& path);
        JASPERENGINE_API std::filesystem::path pathFromUtf8(const std::string& text);
        // Lower-case extension with its dot ("" when none)
        JASPERENGINE_API std::string lowerExtension(const std::filesystem::path& path);
        // '*' (any run without '/'), '**' (any run), '?' (one character); case-insensitive
        JASPERENGINE_API bool globMatch(const std::string& pattern, const std::string& text);
        // Generic path of `path` relative to `root` ("" for the root itself), lexically; false
        // when `path` is not under `root`
        JASPERENGINE_API bool relativeTo(const std::filesystem::path& root, const std::filesystem::path& path, std::string& out);

        // What a backend saw, before coalescing. Backends drop excluded paths first.
        struct JASPERENGINE_API RawEvent {
            enum class Type : uint8_t {
                Created,    // appeared (also the new half of an unpaired rename)
                Modified,   // content or attributes
                Removed,    // gone (also the old half of an unpaired rename)
                Moved,      // oldPath -> path, paired by the backend
                Changed,    // something happened at path: the stat decides (FSEvents)
                Rescan      // events were lost under path (overflow): diff it against the index
            };
            Type type = Type::Modified;
            std::filesystem::path path;
            std::filesystem::path oldPath;
            bool directory = false;   // known to be a directory (a backend may not know)
        };

        // The service side of a backend
        class JASPERENGINE_API WatchSink {
        public:
            virtual ~WatchSink() = default;
            // From the backend's thread, or from flush() on the caller's
            virtual void onRawEvents(std::vector<RawEvent>&& events) = 0;
            // The watch cannot go on (inotify ran out of watches, the root vanished...): the
            // service falls back to polling. `reason` is shown once in the Console.
            virtual void onBackendFailed(const std::string& reason) = 0;
            // The probe file of the self-test was seen (FileWatchService, Auto)
            virtual void onProbe() {}
        };

    }

    // One watched root, recursively, on one thread (S05 D2). Implementations live in
    // backends/: inotify, ReadDirectoryChangesW, FSEvents, polling and null.
    class JASPERENGINE_API FileWatcher {
    public:
        virtual ~FileWatcher() = default;

        // Starts watching `root` (absolute) and reporting to `sink`. False when it cannot
        // (`error` says why); then nothing runs.
        virtual bool start(const std::filesystem::path& root, watch::WatchSink& sink, std::string& error) = 0;
        // Joins the thread; no sink call after it returns. Never from a sink callback.
        virtual void stop() = 0;
        // R7: when it returns true, every change that completed on disk before the call has
        // been handed to the sink. False when the timeout passed first.
        virtual bool flush(std::chrono::milliseconds timeout) = 0;
        // flush only drains what the system already holds (not a round trip, not a walk of the tree)
        virtual bool flushIsDrain() const { return false; }

        // "inotify", "ReadDirectoryChangesW", "FSEvents", "polling", "null"
        virtual const char* name() const = 0;
        virtual bool isNative() const = 0;
        // Kernel watches held (inotify: one per directory; others: 1 while watching)
        virtual size_t watchCount() const = 0;

        // The self-test probe: a path under an excluded folder whose events still reach
        // WatchSink::onProbe (and nothing else). Set before start.
        void setProbePath(const std::filesystem::path& path) { probePath = path.lexically_normal(); }

        // Test hooks (AC5): while suppressed, the backend drops what it reads (the changes
        // are lost, as in an overflow); injectOverflow then makes it handle its native
        // overflow signal (IN_Q_OVERFLOW, a zero-byte completion, MustScanSubDirs).
        void setSuppressDeliveryForTesting(bool suppress) { suppressForTesting = suppress; }
        virtual void injectOverflowForTesting() = 0;

        static std::unique_ptr<FileWatcher> createNative();          // nullptr where there is none
        static std::unique_ptr<FileWatcher> createPolling(std::chrono::milliseconds interval);
        static std::unique_ptr<FileWatcher> createNull();
        // Whether this platform has a native backend at all
        static bool hasNative();

    protected:
        std::filesystem::path probePath;
        std::atomic<bool> suppressForTesting{false};
    };

}

#endif // JASPERENGINE_FILEWATCHER_H
