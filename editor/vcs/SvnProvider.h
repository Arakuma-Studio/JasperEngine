// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Subversion through its command-line tool (docs/specs/S29-version-control.md R4, R10). The command
// set follows OpenEngine's svn package (Packages/svn-vcs/Editor/SVNIntegration.cpp, MIT, Copyright
// (c) 2026 Gamecrafters Guild Limited and contributors; NOTICE.md: lock/unlock, cat -r BASE,
// log --xml), reading every answer as XML: every call passes --non-interactive, --xml where the
// subcommand has it, --encoding UTF-8 with a message; messages and path lists go through temp
// files (-F, --targets). svn has no index: stage adds unversioned files, the baseline is BASE (D5).

#include "vcs/IVcsProvider.h"
#include "vcs/ProcessExec.h"

namespace jasperengine::editor::vcs {

    struct XmlElement;

    class SvnProvider : public IVcsProvider {
    public:
        explicit SvnProvider(ProviderConfig config);

        // `svn --version --quiet` >= 1.10 (R3)
        static ToolVersion checkSvn(const fs::path& svn, const std::vector<std::pair<std::string, std::string>>& env);
        // `svn info --xml` in dir: the working copy root
        static VcsResult workingCopyRoot(const fs::path& svn, const fs::path& dir, const std::vector<std::pair<std::string, std::string>>& env,
                                         fs::path& out);

        VcsCaps capabilities() const override;
        const ProviderConfig& config() const override { return cfg; }

        VcsResult status(VcsSnapshot& out, uint32_t depth, const jobs::CancelToken* cancel = nullptr) override;
        VcsResult branchInfo(VcsBranchInfo& out) override;
        VcsResult baseContent(const RepoPath& path, std::string_view rev, std::string& out, size_t maxBytes = SIZE_MAX) override;
        VcsResult textDiff(const RepoPath& path, std::string_view rev, std::string& out) override;

        VcsResult stage(std::span<const RepoPath> paths) override;
        VcsResult unstage(std::span<const RepoPath> paths) override;
        VcsResult commit(std::span<const RepoPath> paths, std::string_view message, std::string& revOut,
                         const jobs::CancelToken* cancel = nullptr) override;
        VcsResult revert(std::span<const RepoPath> paths) override;
        VcsResult log(const RepoPath* path, int limit, int skip, std::vector<VcsLogEntry>& out) override;

        VcsResult lock(std::span<const RepoPath> paths) override;
        VcsResult unlock(std::span<const RepoPath> paths, bool force) override;
        VcsResult lockable(std::span<const RepoPath> paths, std::vector<bool>& out) override;

        // svn update (R10: pull maps to update; fetch and push are not svn's)
        VcsResult pull(const jobs::CancelToken* cancel = nullptr) override;

        VcsResult conflictSides(const RepoPath& path, ConflictSides& out) override;
        VcsResult markResolved(std::span<const RepoPath> paths) override;
        VcsResult resolveWith(const RepoPath& path, bool ours) override;

        bool validRevision(std::string_view rev) const override;

        // `svn status --xml` into a snapshot's entries and locks (remote: the -u columns)
        static void parseStatusXml(const XmlElement& root, bool remote, VcsSnapshot& out, int& againstRevision);

    private:
        ProcessResult svn(std::vector<std::string> args, int timeoutMs = 0, const jobs::CancelToken* cancel = nullptr) const;
        // args + --targets <temp file> listing the paths (removed after the call)
        ProcessResult svnWithTargets(std::vector<std::string> args, std::span<const RepoPath> paths, int timeoutMs = 0,
                                     const jobs::CancelToken* cancel = nullptr) const;
        fs::path tempFile(const char* prefix) const;
        static VcsResult resultOf(const ProcessResult& process);
        std::string projectTarget() const;
        VcsResult readLockable(VcsSnapshot& out);

        ProviderConfig cfg;
        RepoPath projectKey;
        std::vector<std::pair<std::string, std::string>> env;
    };

}
