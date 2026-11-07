// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// One version control system behind the editor (docs/specs/S29-version-control.md R1, D6): Git
// (with Git LFS locks) and SVN implement it through their command-line tools (D1). Every
// operation takes path lists and returns a VcsResult; none throws. Calls block on a child
// process, so they run on VcsService's poller and worker threads, never on the main thread
// (R24). A provider is safe to call from two threads at once (the poll and a user operation):
// it holds its configuration and nothing else that changes.

#include "vcs/VcsTypes.h"

#include "jobs/Primitives.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jasperengine::editor::vcs {

    namespace fs = std::filesystem;

    // What a provider is started with
    struct ProviderConfig {
        fs::path repoRoot;               // the working tree root, in the editor's form of the path
        fs::path projectDir;             // the project folder (status is limited to it)
        fs::path internalDir;            // <project>/.jasperengine/vcs: temp files (pathspecs, messages)
        fs::path executable;             // git / svn, absolute
        bool lfs = false;                // git: `git lfs` 3.0+ answers (locking)
        std::string userName;            // the editor user (svn lock comments)
        // set for every child (tests point HOME and the global config at temp files)
        std::vector<std::pair<std::string, std::string>> extraEnv;
        // svn: global options before every subcommand (--username: two users of one file://
        // repository in the tests; an account the user's auth cache does not hold)
        std::vector<std::string> svnGlobalArgs;
        // timeouts (R4): local operations, commit (hooks run), network
        int localTimeoutMs = 30000;
        int commitTimeoutMs = 120000;
        int networkTimeoutMs = 300000;
    };

    // The three sides of a conflicted file (R19): git index stages :1:/:2:/:3:, or svn's
    // .rOLD/.mine/.rNEW files
    struct ConflictSides {
        std::string base;
        std::string ours;
        std::string theirs;
        bool hasBase = false;
    };

    class IVcsProvider {
    public:
        virtual ~IVcsProvider() = default;

        virtual VcsCaps capabilities() const = 0;
        virtual const ProviderConfig& config() const = 0;

        // The project's state; depth says what else to read (StatusDepth flags)
        virtual VcsResult status(VcsSnapshot& out, uint32_t depth, const jobs::CancelToken* cancel = nullptr) = 0;
        virtual VcsResult branchInfo(VcsBranchInfo& out) = 0;
        // The file as `rev` has it (git: HEAD, an oid, HEAD~n; svn: BASE, HEAD, a number). One over
        // maxBytes is not kept: `out` holds its first maxBytes + 1 bytes, so a size check sees it.
        virtual VcsResult baseContent(const RepoPath& path, std::string_view rev, std::string& out,
                                      size_t maxBytes = SIZE_MAX) = 0;
        // A unified diff of the working file against `rev`
        virtual VcsResult textDiff(const RepoPath& path, std::string_view rev, std::string& out) = 0;
        // The tag the (detached) HEAD is at; a failure when it is at none
        virtual VcsResult headTag(std::string&) { return VcsResult::unsupported(); }

        virtual VcsResult stage(std::span<const RepoPath> paths) = 0;
        virtual VcsResult unstage(std::span<const RepoPath> paths) = 0;
        // Commits exactly `paths` (untracked ones are added first); revOut: the new revision
        virtual VcsResult commit(std::span<const RepoPath> paths, std::string_view message, std::string& revOut,
                                 const jobs::CancelToken* cancel = nullptr) = 0;
        // Puts the files back as the baseline has them (D5: HEAD / BASE)
        virtual VcsResult revert(std::span<const RepoPath> paths) = 0;
        // Newest first; path null: the project's history. skip: entries passed over (Load more)
        virtual VcsResult log(const RepoPath* path, int limit, int skip, std::vector<VcsLogEntry>& out) = 0;

        virtual VcsResult lock(std::span<const RepoPath> paths) = 0;
        virtual VcsResult unlock(std::span<const RepoPath> paths, bool force) = 0;
        // Which of `paths` may be locked (git: the lockable attribute; svn: svn:needs-lock)
        virtual VcsResult lockable(std::span<const RepoPath> paths, std::vector<bool>& out) = 0;

        virtual VcsResult fetch(const jobs::CancelToken* = nullptr) { return VcsResult::unsupported(); }
        virtual VcsResult pull(const jobs::CancelToken* = nullptr) { return VcsResult::unsupported(); }
        virtual VcsResult push(const jobs::CancelToken* = nullptr) { return VcsResult::unsupported(); }

        // ---- conflicts (R19)
        virtual VcsResult conflictSides(const RepoPath& path, ConflictSides& out) = 0;
        // The working file is the resolution: git add / svn resolve --accept working
        virtual VcsResult markResolved(std::span<const RepoPath> paths) = 0;
        // Use Ours / Use Theirs for a whole file
        virtual VcsResult resolveWith(const RepoPath& path, bool ours) = 0;

        // ---- the scene merge driver (R22, git)
        virtual VcsResult setupMergeDriver(const fs::path&) { return VcsResult::unsupported(); }
        virtual bool mergeDriverConfigured() { return false; }

        // R33: a revision this provider accepts as one argument (never a flag)
        virtual bool validRevision(std::string_view rev) const = 0;
    };

    // The first line of a version text ("git version 2.34.1"), and whether it is new enough
    struct ToolVersion {
        bool ok = false;
        std::string version;    // "2.34.1"
        std::string message;    // why the tool is not usable
    };

}
