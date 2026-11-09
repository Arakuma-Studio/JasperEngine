// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The sprite tools' session (docs/specs/S47-agent-sprite-drawing.md R2, R34): each file decoded
// once and kept, keyed by project-relative path, followed on disk through S05's watcher events and
// a stat on every call. Up to 16 documents or 256 MB, least recently used first out. Main thread.

#pragma once

#include "SpriteDoc.h"
#include "SpriteEdit.h"
#include "SpritePatch.h"

#include "io/watch/FileWatchService.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::pixelart::draw {

    struct LastEdit {
        std::string by;                // "Claude Code"
        std::string revision;
        std::string summary;           // "line, fill x2"
        std::vector<DirtyRect> rects;
        int64_t timeMs = 0;            // since the epoch: the Inspector's "2 min ago"
    };

    struct SpriteSession {
        std::string path;              // project-relative
        std::string assetPath;         // relative to the assets directory
        std::filesystem::path absolute;
        SpriteDoc sprite;
        std::shared_ptr<const std::vector<uint8_t>> bytes;
        std::string revision;          // the first 12 hex digits of the file's SHA-256
        uint64_t size = 0;
        int64_t mtimeNs = 0;
        std::map<std::string, std::string> lastSeen;   // per MCP session
        std::optional<LastEdit> lastEdit;
        // the last state an outside save replaced, for changed_rects (R34)
        std::shared_ptr<const SpriteDoc> replaced;
        std::string replacedRevision;
        bool changedExternally = false;                // the newest change came from outside the editor
        std::vector<std::weak_ptr<const SpritePatchSet>> patches;
        uint64_t lastUse = 0;
        size_t memory = 0;
    };

    class SpriteDocCache {
    public:
        static constexpr size_t kMaxDocuments = 16;
        static constexpr size_t kMaxBytes = 256ull << 20;

        static SpriteDocCache& get();

        // `given`: a project-relative path or an S04 GUID. Stats the file, reloads it when its
        // content changed. Null with `problem` (sprite_missing, not_a_sprite, outside_assets, ...).
        std::shared_ptr<SpriteSession> open(Project* project, const std::string& given, PixelProblem& problem);
        // the same for an absolute path (SpriteEditCmd; a file no longer readable gives null)
        std::shared_ptr<SpriteSession> openAbsolute(Project* project, const std::filesystem::path& absolute, PixelProblem& problem);
        // the session as it is, no disk access: null when none (the editor's own write must not
        // look like an outside change)
        std::shared_ptr<SpriteSession> find(const std::filesystem::path& absolute) const;
        // the editor wrote the file: the session is `sprite` at those bytes, and the watcher's
        // view of them is settled
        void adoptWrite(SpriteSession& session, SpriteDoc sprite, std::shared_ptr<const std::vector<uint8_t>> bytes, const std::string& revision);
        void addPatch(SpriteSession& session, const std::shared_ptr<const SpritePatchSet>& patch);
        void forget(const std::filesystem::path& absolute);
        // what the session remembers of a file, no disk access (the Inspector's line)
        std::optional<LastEdit> lastEditOf(const std::filesystem::path& absolute) const;

        // the state at a revision, rebuilt from the session and the live patches (R29)
        std::optional<SpriteDoc> rebuild(const SpriteSession& session, const std::string& revision) const;
        std::vector<std::string> knownRevisions(const SpriteSession& session) const;
        // the newest live patch ending at the current revision, null when none
        std::shared_ptr<const SpritePatchSet> newestPatch(const SpriteSession& session) const;

        static std::string revisionOf(std::span<const uint8_t> bytes);
        // drops every session (a project switch)
        void clear();

    private:
        SpriteDocCache() = default;

        Project* project = nullptr;
        std::map<std::string, std::shared_ptr<SpriteSession>> sessions;   // by absolute generic path
        FileWatchSubscription watcher;
        uint64_t clock = 0;

        void attach(Project* project);
        bool sync(SpriteSession& session, PixelProblem& problem);
        void overwrittenBy(SpriteSession& session, const SpriteDoc& fresh);
        void evict(const SpriteSession* keep);
        std::shared_ptr<SpriteSession> openResolved(const std::filesystem::path& absolute, const std::string& path, const std::string& assetPath, PixelProblem& problem);
    };

}
