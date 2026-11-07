// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Git through its command-line tool, with Git LFS locking (docs/specs/S29-version-control.md R4,
// R9, R22). The command set follows OpenEngine's git package (Packages/git-vcs/Editor/
// GitIntegration.cpp, MIT, Copyright (c) 2026 Gamecrafters Guild Limited and contributors;
// NOTICE.md), with the fixes R9 lists: porcelain v2 with -z, `lfs locks --verify` on the slow
// cadence only, a NUL-separated log, messages through stdin and `--` before every pathspec.
//
// Every call passes -c core.fsmonitor=false -c core.pager=cat -c color.ui=false
// --no-optional-locks (diff and show also --no-ext-diff --no-textconv), so an untrusted
// repository's configuration runs nothing on a read; commits run the repository's hooks.

#include "vcs/IVcsProvider.h"
#include "vcs/ProcessExec.h"

#include <mutex>
#include <optional>

namespace jasperengine::editor::vcs {

    class GitProvider : public IVcsProvider {
    public:
        explicit GitProvider(ProviderConfig config);

        // `git --version` >= 2.30 (R3)
        static ToolVersion checkGit(const fs::path& git, const std::vector<std::pair<std::string, std::string>>& env);
        // `git lfs version` >= 3.0, through that git (it finds git-lfs on its PATH)
        static ToolVersion checkLfs(const fs::path& git, const std::vector<std::pair<std::string, std::string>>& env,
                                    const fs::path& projectDir);
        // `git rev-parse --show-toplevel` in dir: the working tree root git reports
        static VcsResult showToplevel(const fs::path& git, const fs::path& dir, const std::vector<std::pair<std::string, std::string>>& env,
                                      fs::path& out);

        VcsCaps capabilities() const override;
        const ProviderConfig& config() const override { return cfg; }

        VcsResult status(VcsSnapshot& out, uint32_t depth, const jobs::CancelToken* cancel = nullptr) override;
        VcsResult branchInfo(VcsBranchInfo& out) override;
        VcsResult baseContent(const RepoPath& path, std::string_view rev, std::string& out, size_t maxBytes = SIZE_MAX) override;
        VcsResult textDiff(const RepoPath& path, std::string_view rev, std::string& out) override;
        // `git describe --tags --exact-match HEAD`
        VcsResult headTag(std::string& out) override;

        VcsResult stage(std::span<const RepoPath> paths) override;
        VcsResult unstage(std::span<const RepoPath> paths) override;
        VcsResult commit(std::span<const RepoPath> paths, std::string_view message, std::string& revOut,
                         const jobs::CancelToken* cancel = nullptr) override;
        VcsResult revert(std::span<const RepoPath> paths) override;
        VcsResult log(const RepoPath* path, int limit, int skip, std::vector<VcsLogEntry>& out) override;

        VcsResult lock(std::span<const RepoPath> paths) override;
        VcsResult unlock(std::span<const RepoPath> paths, bool force) override;
        VcsResult lockable(std::span<const RepoPath> paths, std::vector<bool>& out) override;

        VcsResult fetch(const jobs::CancelToken* cancel = nullptr) override;
        VcsResult pull(const jobs::CancelToken* cancel = nullptr) override;
        VcsResult push(const jobs::CancelToken* cancel = nullptr) override;

        VcsResult conflictSides(const RepoPath& path, ConflictSides& out) override;
        VcsResult markResolved(std::span<const RepoPath> paths) override;
        VcsResult resolveWith(const RepoPath& path, bool ours) override;

        VcsResult setupMergeDriver(const fs::path& editorExecutable) override;
        bool mergeDriverConfigured() override;

        bool validRevision(std::string_view rev) const override;

        // `git status --porcelain=v2 -z --branch` output into a snapshot's entries and branch
        static void parsePorcelainV2(std::string_view output, VcsSnapshot& out);
        // The merge driver line R22 configures (the editor path single-quoted for git's shell)
        static std::string mergeDriverCommand(const fs::path& editorExecutable);

    private:
        // timeoutMs 0: the local timeout
        ProcessResult git(std::vector<std::string> args, int timeoutMs = 0, std::string stdinData = {},
                          const jobs::CancelToken* cancel = nullptr) const;
        // args + the paths: "--" and each path, or --pathspec-from-file over 64 of them (a temp
        // file in internalDir, removed after the call)
        ProcessResult gitWithPaths(std::vector<std::string> args, std::span<const RepoPath> paths, int timeoutMs = 0,
                                   std::string stdinData = {}, const jobs::CancelToken* cancel = nullptr) const;
        static VcsResult resultOf(const ProcessResult& process);
        // the project folder as a pathspec ("." at the root)
        std::string projectPathspec() const;
        VcsResult readLocks(VcsSnapshot& out);
        VcsResult readLockable(VcsSnapshot& out);
        VcsResult readRemote(VcsSnapshot& out);
        // the operation left unfinished (MERGE_HEAD, rebase-merge/, ...): files only, no process
        void readOperation(VcsBranchInfo& branch) const;
        // `git rev-parse --absolute-git-dir`, asked once (a linked worktree answers its own)
        fs::path gitDir() const;

        ProviderConfig cfg;
        RepoPath projectKey;
        std::vector<std::pair<std::string, std::string>> env;
        mutable std::mutex gitDirMutex;
        mutable std::optional<fs::path> gitDirPath;
    };

}
