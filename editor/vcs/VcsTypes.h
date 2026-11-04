// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The provider-neutral version control types (docs/specs/S29-version-control.md R1, R6, D4).
// A file's state is a set of flags (index, working tree, conflict, lock, remote), never one
// collapsed value; the overlays decide what to draw from them (VcsOverlay.h).

#include "vcs/VcsPathKey.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::editor::vcs {

    enum class FileState : uint8_t { Unmodified, Added, Modified, Deleted, Renamed, Untracked, Ignored };
    enum class LockState : uint8_t { Unsupported, Unlocked, LockedByMe, LockedByOther };

    const char* fileStateName(FileState state);     // "unmodified", "added", ...
    const char* lockStateName(LockState state);     // "unsupported", "unlocked", "locked_by_me", "locked_by_other"

    struct VcsChange {
        RepoPath path;
        RepoPath oldPath;                   // Renamed: the path it was renamed from
        FileState index = FileState::Unmodified;      // staged against HEAD (git); svn: Unmodified
        FileState worktree = FileState::Unmodified;   // the working copy against the index / BASE
        bool conflict = false;
        LockState lock = LockState::Unsupported;
        std::string lockOwner;
        std::string lockId;                 // the LFS lock id (git), the lock token (svn)
        bool remoteChanged = false;         // the server has a newer revision of it
        // git: the blob of HEAD the status reported; svn: the BASE revision; empty: none known
        std::string baseOid;

        // A change to commit or revert: staged, modified, untracked or conflicted
        bool changed() const;
        // Nothing to show: unchanged, no lock of anyone, nothing newer on the server
        bool clean() const;
        // Untracked, or added and not in HEAD: nothing to revert to (R11)
        bool newFile() const;
        bool deleted() const;
    };

    // What every provider call returns (R1): never an exception
    struct VcsResult {
        bool ok = true;
        int exitCode = 0;
        std::string message;   // what failed (the tool's stderr), or a summary of what was done

        static VcsResult success(std::string message = {}) { return {true, 0, std::move(message)}; }
        static VcsResult failure(std::string message, int exitCode = -1) { return {false, exitCode, std::move(message)}; }
        static VcsResult unsupported() { return {false, -1, "not supported by this version control system"}; }
    };

    // What a provider can do (R1): the optional operations and the lock support
    struct VcsCaps {
        std::string provider;       // "git", "svn"
        bool staging = false;       // an index (git)
        bool locking = false;       // git-lfs 3.0+, svn
        bool fetch = false;
        bool pull = false;          // git: fast-forward only; svn: update
        bool push = false;
        bool mergeDriver = false;   // R22 (git)
    };

    // How much a status poll reads (bit flags, D6): the local state always; the locks and the
    // server's changes (network) and the ignored set only on their own cadences (R5)
    enum StatusDepth : uint32_t {
        Fast = 0,
        WithLocksAndRemote = 1u << 0,
        WithIgnored = 1u << 1
    };

    // A git operation left unfinished in the working tree (merge, rebase, ...): git status does
    // not say, the files in the git directory do
    enum class VcsOperation : uint8_t { None, Merge, Rebase, CherryPick, Revert, Bisect };
    const char* vcsOperationName(VcsOperation operation);   // "", "merge", "rebase", "cherry_pick", "revert", "bisect"

    struct VcsBranchInfo {
        std::string name;           // "main"; svn: the URL below the repository root ("^/trunk")
        std::string headOid;        // git: the HEAD commit; svn: the working copy's revision
        std::string upstream;       // "origin/main"; empty: none
        int ahead = 0;
        int behind = 0;
        bool detached = false;
        std::string tag;            // git: the tag a detached HEAD is at; svn: a ^/tags/<tag> working copy
        VcsOperation operation = VcsOperation::None;
        int operationStep = 0;      // a rebase's progress (0: unknown)
        int operationTotal = 0;
        std::string operationBranch;   // the branch a rebase works on
    };

    // The status bar's name of the HEAD, in git's prompt words: "main", "tag v1.2", "a1b2c3d",
    // "feature | REBASE 2/5", "main | MERGING"
    std::string headLabel(const VcsBranchInfo& branch);

    struct VcsLogEntry {
        std::string revision;
        std::string author;
        std::string date;           // ISO 8601
        std::string message;        // the subject (git), the whole message (svn)
    };

    struct VcsLock {
        RepoPath path;
        std::string owner;
        std::string id;
        bool mine = false;
    };

    // A directory's aggregated state (R6): the worst of its entries
    enum class DirState : uint8_t { Clean, Untracked, Changed, LockedByOther, Conflict };
    const char* dirStateName(DirState state);

    struct VcsDirEntry {
        RepoPath path;
        DirState state = DirState::Clean;
        uint32_t changed = 0;       // changed files below it
    };

    // One status poll's result, published as shared_ptr<const VcsSnapshot> (R6): a sorted flat
    // vector keyed by repository path. The provider fills the raw lists; finalize() merges them
    // into `entries` and aggregates the directories.
    struct VcsSnapshot {
        // ---- what the provider fills
        std::vector<VcsChange> entries;   // the changed paths (any order before finalize)
        VcsBranchInfo branch;
        bool hasLocks = false;            // locks were read this poll
        std::vector<VcsLock> locks;
        bool hasLockable = false;         // lockable (and mergeDriverMissing) were read this poll
        std::vector<RepoPath> lockable;   // every lockable file of the project
        bool hasRemote = false;           // remoteChanged was read this poll
        std::vector<RepoPath> remoteChanged;
        bool hasIgnored = false;          // ignored was read this poll
        std::vector<RepoPath> ignored;    // ignored files, and ignored directories with a trailing '/'
        bool mergeDriverMissing = false;  // R22: the attribute is set, the driver is not
        // the last lock refresh's failure (network), empty when fine; a poll that did not read the
        // locks keeps the previous one
        std::string lockError;

        // ---- after finalize
        uint64_t version = 0;
        std::vector<VcsDirEntry> dirs;    // sorted

        // Takes the lists this poll did not read from `previous`, merges locks, lockable files
        // and server changes into entries (clean lockable files get an entry too), sorts and
        // aggregates the directories. Linear after the sort.
        void finalize(const VcsSnapshot* previous);

        // Binary searches (R25: <= 2 us)
        const VcsChange* find(std::string_view path) const;
        const VcsDirEntry* findDir(std::string_view path) const;
        bool isIgnored(std::string_view path) const;
        bool isLockable(std::string_view path) const;
        // The entries at or below a directory ("" for all), in order
        std::pair<const VcsChange*, const VcsChange*> range(std::string_view dir) const;
        size_t changedCount() const;
    };

    using SnapshotPtr = std::shared_ptr<const VcsSnapshot>;

    // project.yaml versionControl.provider, MCP editor.vcs.provider (R7)
    enum class ProviderChoice : uint8_t { Auto, Git, Svn, None };
    const char* providerChoiceName(ProviderChoice choice);     // "auto", "git", "svn", "none"
    bool parseProviderChoice(std::string_view text, ProviderChoice& out);

    // R8: "x.png.meta" pairs with "x.png"; the asset of a meta path (empty when it is not one)
    std::string_view metaAssetOf(std::string_view path);
    inline std::string metaOf(std::string_view path) { return std::string(path) + ".meta"; }

}
