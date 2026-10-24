// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JASPERENGINE_FILECHANGECOALESCER_H
#define JASPERENGINE_FILECHANGECOALESCER_H

#include "FileWatcher.h"

#include <chrono>
#include <map>
#include <string>
#include <vector>

namespace jasperengine {

    // Turns a backend's raw events into settled changes, one per path (S05 R3, R4). Pure:
    // the clock is a parameter and the index and the disk come through an Environment, so
    // the rules are tested with a fake clock and no I/O.
    //
    // Per path, inside the quiet window: Added+Modified* = Added, Modified+ = Modified,
    // Added..Removed = nothing, Removed..Added = Modified, Moved(A->B)+Modified(B) = Moved +
    // Modified(B), Moved(A->B)+Moved(B->C) = Moved(A->C). A removal is held for the delete
    // hold (a safe-save replaces the file right after), and a removal plus an addition of one
    // file identity are one Moved. A rename from a path that appeared inside the window (the
    // temp file of a safe-save) is a change of the target, and the temporaries never come out.
    //
    // A file is settled once it had no event for the quiet window and two stats `settleGap`
    // apart agree on size and mtime; one still growing comes out anyway after `growCap`.
    // Folders settle on the quiet window alone.
    //
    // Not thread-safe: the owner serializes access.
    class JASPERENGINE_API FileChangeCoalescer {
    public:
        using Clock = std::chrono::steady_clock;

        class Environment {
        public:
            virtual ~Environment() = default;
            // The index: the path's state as last delivered
            virtual bool known(const std::filesystem::path& path, watch::PathStat& out) = 0;
            // The disk now
            virtual bool stat(const std::filesystem::path& path, watch::PathStat& out) = 0;
        };

        struct Settings {
            std::chrono::milliseconds quiet{150};       // R4 default, 50..2000
            std::chrono::milliseconds deleteHold{200};  // a removal waits at least this long
            std::chrono::milliseconds settleGap{50};    // between the two settle stats
            std::chrono::milliseconds growCap{5000};    // a growing file comes out after this
        };

        explicit FileChangeCoalescer(Environment& environment);
        FileChangeCoalescer(Environment& environment, const Settings& settings);

        void add(const watch::RawEvent& event, Clock::time_point now);
        // The changes settled by `now`, in the order their paths first changed. `force`
        // (flush): everything pending, with one stat and no windows.
        std::vector<FileChange> take(Clock::time_point now, bool force = false);
        // When take() may return something next; time_point::max() when nothing is pending
        Clock::time_point nextDeadline() const;

        size_t pendingCount() const;
        // Raw events folded into an earlier one of the same path
        uint64_t coalescedCount() const { return coalesced; }
        void clear();

        void setQuietWindow(std::chrono::milliseconds quiet);
        // At least 200 ms. A polling watcher holds a removal for one poll more: the save that
        // deletes and writes the file again is seen by two scans, an interval apart
        void setDeleteHold(std::chrono::milliseconds hold);
        const Settings& getSettings() const { return settings; }

    private:
        enum class State : uint8_t {
            Added,      // new inside the window
            Modified,   // known, changed
            Removed,    // known, gone (held)
            MovedFrom,  // known, renamed to `link`; comes out with its partner, never alone
            Moved       // renamed from `link`
        };
        struct Entry {
            State state = State::Modified;
            std::string link;               // Moved: the old path; MovedFrom: the new one
            bool directory = false;
            bool existedBefore = false;     // the index knew it when it first changed
            bool modifiedAfterMove = false; // Moved with other bytes than its source had
            watch::PathStat before;         // the index entry (identity for pairing)
            Clock::time_point last{};
            uint64_t order = 0;
            bool sampled = false;
            watch::PathStat sample;
            Clock::time_point sampledAt{};
            Clock::time_point first{};
        };

        Environment& env;
        Settings settings;
        std::map<std::string, Entry> entries;   // generic UTF-8 path -> state
        std::vector<std::string> rescans;
        uint64_t nextOrder = 1;
        uint64_t coalesced = 0;

        Entry& fresh(const std::string& key, State state, Clock::time_point now);
        void touch(Entry& entry, Clock::time_point now);
        void created(const std::string& key, bool directory, Clock::time_point now);
        void modified(const std::string& key, bool directory, Clock::time_point now);
        void removed(const std::string& key, bool directory, Clock::time_point now);
        void moved(const std::string& from, const std::string& to, bool directory, Clock::time_point now);
        // A Moved entry lost its file: its source is a plain removal
        void dropMoved(const std::string& key);
        // Pairs held removals with additions of one identity (both pending)
        void pairByIdentity(Clock::time_point now);
        void rekeyUnder(const std::string& from, const std::string& to);
        Clock::time_point readyAt(const Entry& entry) const;
        FileChange makeChange(FileChangeKind kind, const std::string& key, const watch::PathStat& stat, bool directory) const;
        static std::filesystem::path pathOf(const std::string& key);
        static bool isTemporary(const std::string& key);
    };

}

#endif // JASPERENGINE_FILECHANGECOALESCER_H
