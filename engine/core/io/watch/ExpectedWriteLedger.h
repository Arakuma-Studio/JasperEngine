// Copyright (c) 2026 Gamecrafters Guild Limited and contributors (OpenEngine, MIT)
// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// From OpenEngine's Engine/Include/Assets/ExpectedWriteLedger.h (NOTICE.md). Added here:
// removals (a deleted or renamed-away path is an echo while it stays gone) and counters.

#ifndef JASPERENGINE_EXPECTEDWRITELEDGER_H
#define JASPERENGINE_EXPECTEDWRITELEDGER_H

#include "Export.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace jasperengine {

    // The writes this process announced, so the file watcher's report of one is recognisable
    // as that write arriving rather than as a change nobody made.
    //
    // A writer announces a path before it touches the file, publishes the bytes, then reports
    // the write. With a file watcher the same write is delivered a second time (the watcher
    // sees it land) and everything the change drives would run twice for one save. A change
    // that matches nothing here is somebody else's edit and is always delivered.
    //
    // Announce-then-write is the order that makes this work: the watcher's report of a new
    // file can reach the ledger before the writer's own, so the announcement has to be in
    // place before the file exists.
    //
    // Matching is by path, qualified by content: a reported write remembers the size and
    // write time it left on disk, and a change to that path only counts as its echo while the
    // file still has them. An edit that moved the file on is a change of its own and is
    // delivered even inside the echo window. A reported removal is an echo while the path
    // stays absent.
    //
    // The content check is only as fine as the filesystem's timestamp: a rewrite that keeps
    // the size and lands inside that resolution, or a tool that restores the original write
    // time, is indistinguishable from the echo inside the window and is treated as one
    // (accepted, as in OpenEngine; the window is short).
    //
    // An edit that lands between the report and the echo delivers twice rather than once:
    // never a lost change, which is the direction that matters.
    //
    // Threading: any thread may announce, report and query. The lock covers the two vectors
    // only; the stat behind the content check is taken outside it.
    class JASPERENGINE_API ExpectedWriteLedger {
    public:
        using Registration = uint64_t;
        static constexpr Registration noRegistration = 0;

        // echoWindow: how long after a report a matching change is still that write arriving
        explicit ExpectedWriteLedger(std::chrono::milliseconds echoWindow = std::chrono::milliseconds(2000));

        void setEchoWindow(std::chrono::milliseconds echoWindow);
        std::chrono::milliseconds getEchoWindow() const;

        // Announces a write about to happen to `path` (with `subtree`, to everything under it).
        // Supersedes what was recorded for it. noRegistration for an empty path.
        Registration expect(const std::filesystem::path& path, bool subtree);
        // `written` is on disk: its echo is recognisable by content from now on (a folder: by
        // being there)
        void reported(const std::filesystem::path& written);
        // `removed` is gone (deleted, or renamed away) because of this process
        void reportedRemoved(const std::filesystem::path& removed);
        // Stops expecting the announced write (paths reported keep their echo windows)
        void retire(Registration id);

        // Whether this change repeats a write this process has already driven. False for
        // anything unannounced, and for a path whose bytes moved on since it was reported.
        bool isEchoOfOurWrite(const std::filesystem::path& path);

        // Recorded entries (tests, editor_status)
        size_t expectedCount() const;
        size_t reportedCount() const;
        void clear();

        // Absolute, '/' separators, case-folded where the filesystem is case-insensitive
        static std::string makeKey(const std::filesystem::path& path);

    private:
        struct Expectation {
            Registration id = noRegistration;
            std::string key;
            bool subtree = false;
        };
        struct ReportedWrite {
            std::string key;
            bool removed = false;
            bool directory = false;   // a folder written (created, renamed here): present is enough
            std::uintmax_t size = 0;
            std::filesystem::file_time_type modified{};
            std::chrono::steady_clock::time_point deadline{};
        };

        void pruneExpiredLocked(std::chrono::steady_clock::time_point now);
        void recordLocked(ReportedWrite write, std::chrono::steady_clock::time_point now);

        std::chrono::milliseconds echoWindow;
        mutable std::mutex mutex;
        Registration nextRegistration = noRegistration + 1;
        std::vector<Expectation> expected;
        std::vector<ReportedWrite> reportedWrites;
    };

}

#endif // JASPERENGINE_EXPECTEDWRITELEDGER_H
