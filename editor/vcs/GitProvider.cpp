// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "vcs/GitProvider.h"

#include "io/AtomicFile.h"
#include "mcp/McpJson.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace jasperengine::editor::vcs {

namespace {

    using Json = mcp::Json;

    // R4: more paths than this go through a pathspec file, not the command line
    constexpr size_t kMaxArgPaths = 64;

    const char* const kReadOverrides[] = {
        "-c", "core.fsmonitor=false",
        "-c", "core.pager=cat",
        "-c", "color.ui=false",
        "-c", "core.quotepath=false",
        "-c", "log.showSignature=false",
        "--no-optional-locks",
    };

    std::vector<std::string_view> splitNul(std::string_view text) {
        std::vector<std::string_view> parts;
        size_t start = 0;
        while (start < text.size()) {
            const size_t end = text.find('\0', start);
            if (end == std::string_view::npos) {
                parts.push_back(text.substr(start));
                break;
            }
            parts.push_back(text.substr(start, end - start));
            start = end + 1;
        }
        return parts;
    }

    std::string trimmed(std::string_view text) {
        size_t begin = 0;
        size_t end = text.size();
        while (begin < end && static_cast<unsigned char>(text[begin]) <= ' ') begin++;
        while (end > begin && static_cast<unsigned char>(text[end - 1]) <= ' ') end--;
        return std::string(text.substr(begin, end - begin));
    }

    FileState stateOf(char code) {
        switch (code) {
            case 'M': case 'T': return FileState::Modified;
            case 'A': case 'C': return FileState::Added;
            case 'D': return FileState::Deleted;
            case 'R': return FileState::Renamed;
            default: return FileState::Unmodified;
        }
    }

    // "a b c rest with spaces": the first n fields, then the rest
    bool splitFields(std::string_view record, size_t n, std::vector<std::string_view>& fields, std::string_view& rest) {
        fields.clear();
        size_t pos = 0;
        for (size_t i = 0; i < n; i++) {
            const size_t space = record.find(' ', pos);
            if (space == std::string_view::npos) return false;
            fields.push_back(record.substr(pos, space - pos));
            pos = space + 1;
        }
        rest = record.substr(pos);
        return true;
    }

    std::vector<std::pair<std::string, std::string>> mergedEnv(std::vector<std::pair<std::string, std::string>> base,
                                                               const std::vector<std::pair<std::string, std::string>>& extra) {
        for (const auto& [key, value] : extra) {
            auto it = std::find_if(base.begin(), base.end(), [&key](const auto& entry) { return entry.first == key; });
            if (it != base.end()) it->second = value;
            else base.emplace_back(key, value);
        }
        return base;
    }

    std::vector<std::pair<std::string, std::string>> gitEnvironment(const fs::path& projectDir,
                                                                    const std::vector<std::pair<std::string, std::string>>& extra) {
        const char* path = std::getenv("PATH");
        std::vector<std::pair<std::string, std::string>> env = {
            {"GIT_TERMINAL_PROMPT", "0"},
            {"GCM_INTERACTIVE", "never"},
            {"LC_ALL", "C"},
            {"PATH", sanitizedPath(path ? path : "", projectDir)},
        };
        return mergedEnv(std::move(env), extra);
    }

    const std::vector<std::string>& unsetGitEnvironment() {
        // a hook's environment (an editor started from one) must not redirect these calls
        static const std::vector<std::string> names = {"GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_OBJECT_DIRECTORY",
                                                       "GIT_COMMON_DIR", "GIT_PREFIX", "GIT_LITERAL_PATHSPECS", "GIT_GLOB_PATHSPECS",
                                                       "GIT_NOGLOB_PATHSPECS", "GIT_ICASE_PATHSPECS"};
        return names;
    }

    ProcessResult runGit(const fs::path& git, const fs::path& cwd, const std::vector<std::pair<std::string, std::string>>& env,
                         std::vector<std::string> args, int timeoutMs, std::string stdinData = {},
                         const jobs::CancelToken* cancel = nullptr, bool literal = true, size_t maxOutputBytes = 0) {
        ProcessSpec spec;
        spec.exe = git;
        spec.cwd = cwd;
        spec.env = env;
        spec.unsetEnv = unsetGitEnvironment();
        spec.args.assign(std::begin(kReadOverrides), std::end(kReadOverrides));
        // pathspecs are file names, never globs or magic (a file called "*.png" is one file);
        // the calls that need a magic say :(literal) themselves
        if (literal) spec.args.push_back("--literal-pathspecs");
        for (std::string& arg : args) spec.args.push_back(std::move(arg));
        spec.stdinData = std::move(stdinData);
        spec.timeoutMs = timeoutMs;
        spec.cancel = cancel;
        if (maxOutputBytes > 0) spec.maxOutputBytes = maxOutputBytes;
        return runProcess(spec);
    }

    std::atomic<uint64_t> tempCounter{0};

}

GitProvider::GitProvider(ProviderConfig config): cfg(std::move(config)) {
    repoRelative(cfg.projectDir, cfg.repoRoot, projectKey);
    env = gitEnvironment(cfg.projectDir, cfg.extraEnv);
}

ToolVersion GitProvider::checkGit(const fs::path& git, const std::vector<std::pair<std::string, std::string>>& extraEnv) {
    ToolVersion version;
    const ProcessResult result = runGit(git, {}, gitEnvironment({}, extraEnv), {"--version"}, 10000);
    if (!result.ok()) {
        version.message = "git --version failed: " + result.message();
        return version;
    }
    if (!versionAtLeast(result.out, "2.30", &version.version)) {
        version.message = "Git " + (version.version.empty() ? trimmed(result.out) : version.version) + " is too old: 2.30 or newer is needed";
        return version;
    }
    version.ok = true;
    return version;
}

ToolVersion GitProvider::checkLfs(const fs::path& git, const std::vector<std::pair<std::string, std::string>>& extraEnv,
                                  const fs::path& projectDir) {
    ToolVersion version;
    const ProcessResult result = runGit(git, {}, gitEnvironment(projectDir, extraEnv), {"lfs", "version"}, 10000);
    if (!result.ok()) {
        version.message = "Git LFS is not installed: file locking is off";
        return version;
    }
    if (!versionAtLeast(result.out, "3.0", &version.version)) {
        version.message = "Git LFS " + version.version + " is too old for locking: 3.0 or newer is needed";
        return version;
    }
    version.ok = true;
    return version;
}

VcsResult GitProvider::showToplevel(const fs::path& git, const fs::path& dir, const std::vector<std::pair<std::string, std::string>>& extraEnv,
                                    fs::path& out) {
    const ProcessResult result = runGit(git, dir, gitEnvironment(dir, extraEnv), {"rev-parse", "--show-toplevel"}, 10000);
    if (!result.ok()) return resultOf(result);
    out = pathFromUtf8(trimmed(result.out));
    return VcsResult::success();
}

VcsCaps GitProvider::capabilities() const {
    VcsCaps caps;
    caps.provider = "git";
    caps.staging = true;
    caps.locking = cfg.lfs;
    caps.fetch = caps.pull = caps.push = true;
    caps.mergeDriver = true;
    return caps;
}

ProcessResult GitProvider::git(std::vector<std::string> args, int timeoutMs, std::string stdinData, const jobs::CancelToken* cancel) const {
    return runGit(cfg.executable, cfg.repoRoot, env, std::move(args), timeoutMs > 0 ? timeoutMs : cfg.localTimeoutMs,
                  std::move(stdinData), cancel);
}

ProcessResult GitProvider::gitWithPaths(std::vector<std::string> args, std::span<const RepoPath> paths, int timeoutMs,
                                        std::string stdinData, const jobs::CancelToken* cancel) const {
    if (paths.size() <= kMaxArgPaths) {
        args.push_back("--");
        for (const RepoPath& path : paths) args.push_back(path);
        return git(std::move(args), timeoutMs, std::move(stdinData), cancel);
    }
    // only these take a pathspec file (git 2.26+); the readers (ls-files, diff) run per chunk,
    // their outputs one after the other
    static const char* const kPathspecFileCommands[] = {"add", "checkout", "commit", "reset", "restore", "rm", "stash"};
    const bool pathspecFile = !args.empty() && std::any_of(std::begin(kPathspecFileCommands), std::end(kPathspecFileCommands),
                                                           [&](const char* command) { return args[0] == command; });
    if (!pathspecFile) {
        ProcessResult combined;
        // (256 paths: well under Windows' 32,767-character command line)
        constexpr size_t kChunkPaths = 256;
        for (size_t start = 0; start < paths.size(); start += kChunkPaths) {
            const size_t end = std::min(paths.size(), start + kChunkPaths);
            std::vector<std::string> chunk = args;
            chunk.push_back("--");
            for (size_t i = start; i < end; i++) chunk.push_back(paths[i]);
            ProcessResult part = git(std::move(chunk), timeoutMs, stdinData, cancel);
            if (!part.ok()) return part;
            combined.exitCode = part.exitCode;
            combined.out += part.out;
            combined.err += part.err;
        }
        return combined;
    }
    std::error_code ec;
    fs::create_directories(cfg.internalDir, ec);
    const fs::path file = cfg.internalDir / ("pathspec-" + std::to_string(io::currentProcessId()) + "-" + std::to_string(tempCounter++));
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        for (const RepoPath& path : paths) {
            out.write(path.data(), static_cast<std::streamsize>(path.size()));
            out.put('\0');
        }
        if (!out) {
            ProcessResult failed;
            failed.spawnFailed = true;
            failed.spawnError = "could not write " + file.string();
            return failed;
        }
    }
    args.push_back("--pathspec-from-file=" + genericUtf8(file));
    args.push_back("--pathspec-file-nul");
    ProcessResult result = git(std::move(args), timeoutMs, std::move(stdinData), cancel);
    fs::remove(file, ec);
    return result;
}

VcsResult GitProvider::resultOf(const ProcessResult& process) {
    if (process.ok()) return VcsResult::success(trimmed(process.out));
    return VcsResult::failure(process.message(), process.exitCode);
}

std::string GitProvider::projectPathspec() const {
    return projectKey.empty() ? std::string(".") : projectKey;
}

void GitProvider::parsePorcelainV2(std::string_view output, VcsSnapshot& out) {
    const std::vector<std::string_view> records = splitNul(output);
    std::vector<std::string_view> fields;
    std::string_view rest;
    for (size_t i = 0; i < records.size(); i++) {
        const std::string_view record = records[i];
        if (record.empty()) continue;
        if (record[0] == '#') {
            auto value = [&](std::string_view key) -> std::string_view {
                return record.substr(key.size());
            };
            if (record.rfind("# branch.oid ", 0) == 0) {
                const std::string_view oid = value("# branch.oid ");
                out.branch.headOid = oid == "(initial)" ? std::string() : std::string(oid);
            } else if (record.rfind("# branch.head ", 0) == 0) {
                const std::string_view head = value("# branch.head ");
                out.branch.detached = head == "(detached)";
                out.branch.name = out.branch.detached ? std::string() : std::string(head);
            } else if (record.rfind("# branch.upstream ", 0) == 0) {
                out.branch.upstream = std::string(value("# branch.upstream "));
            } else if (record.rfind("# branch.ab ", 0) == 0) {
                const std::string ab(value("# branch.ab "));
                int ahead = 0, behind = 0;
                if (std::sscanf(ab.c_str(), "+%d -%d", &ahead, &behind) == 2) {
                    out.branch.ahead = ahead;
                    out.branch.behind = behind;
                }
            }
            continue;
        }
        VcsChange change;
        switch (record[0]) {
            case '1':
                if (!splitFields(record, 8, fields, rest)) continue;
                change.path = std::string(rest);
                change.index = stateOf(fields[1][0]);
                change.worktree = stateOf(fields[1].size() > 1 ? fields[1][1] : '.');
                change.baseOid = std::string(fields[6]);
                break;
            case '2':
                if (!splitFields(record, 9, fields, rest)) continue;
                change.path = std::string(rest);
                change.index = stateOf(fields[1][0]);
                change.worktree = stateOf(fields[1].size() > 1 ? fields[1][1] : '.');
                change.baseOid = std::string(fields[6]);
                // the path it came from is the next record
                if (i + 1 < records.size()) change.oldPath = std::string(records[++i]);
                break;
            case 'u':
                if (!splitFields(record, 10, fields, rest)) continue;
                change.path = std::string(rest);
                change.conflict = true;
                change.index = FileState::Modified;
                change.worktree = FileState::Modified;
                change.baseOid = std::string(fields[7]);
                break;
            case '?':
                change.path = std::string(record.substr(2));
                change.index = FileState::Untracked;
                change.worktree = FileState::Untracked;
                break;
            case '!':
                out.ignored.emplace_back(record.substr(2));
                continue;
            default:
                continue;
        }
        if (change.baseOid.find_first_not_of('0') == std::string::npos) change.baseOid.clear();
        out.entries.push_back(std::move(change));
    }
}

fs::path GitProvider::gitDir() const {
    std::lock_guard<std::mutex> lock(gitDirMutex);
    if (!gitDirPath) {
        const ProcessResult result = git({"rev-parse", "--absolute-git-dir"});
        // a failure (a timeout, a repository not there yet) is asked again next time
        if (!result.ok()) return fs::path();
        gitDirPath = pathFromUtf8(trimmed(result.out));
    }
    return *gitDirPath;
}

void GitProvider::readOperation(VcsBranchInfo& branch) const {
    const fs::path dir = gitDir();
    if (dir.empty()) return;
    std::error_code ec;
    auto exists = [&](const char* name) { return fs::exists(dir / name, ec); };
    auto readNumber = [&](const fs::path& file) {
        std::ifstream in(file);
        int value = 0;
        return (in >> value) ? value : 0;
    };
    auto readBranch = [&](const fs::path& file) {
        std::ifstream in(file);
        std::string ref;
        std::getline(in, ref);
        ref = trimmed(ref);
        constexpr std::string_view heads = "refs/heads/";
        return ref.rfind(heads, 0) == 0 ? ref.substr(heads.size()) : ref;
    };
    if (exists("rebase-merge")) {
        branch.operation = VcsOperation::Rebase;
        branch.operationStep = readNumber(dir / "rebase-merge" / "msgnum");
        branch.operationTotal = readNumber(dir / "rebase-merge" / "end");
        branch.operationBranch = readBranch(dir / "rebase-merge" / "head-name");
    } else if (exists("rebase-apply")) {
        branch.operation = VcsOperation::Rebase;
        branch.operationStep = readNumber(dir / "rebase-apply" / "next");
        branch.operationTotal = readNumber(dir / "rebase-apply" / "last");
        branch.operationBranch = readBranch(dir / "rebase-apply" / "head-name");
    } else if (exists("MERGE_HEAD")) {
        branch.operation = VcsOperation::Merge;
    } else if (exists("CHERRY_PICK_HEAD")) {
        branch.operation = VcsOperation::CherryPick;
    } else if (exists("REVERT_HEAD")) {
        branch.operation = VcsOperation::Revert;
    } else if (exists("BISECT_LOG")) {
        branch.operation = VcsOperation::Bisect;
    }
}

VcsResult GitProvider::headTag(std::string& out) {
    const ProcessResult result = git({"describe", "--tags", "--exact-match", "HEAD"});
    if (!result.ok()) return resultOf(result);
    out = trimmed(result.out);
    return out.empty() ? VcsResult::failure("no tag") : VcsResult::success();
}

VcsResult GitProvider::status(VcsSnapshot& out, uint32_t depth, const jobs::CancelToken* cancel) {
    std::vector<std::string> args = {"status", "--porcelain=v2", "-z", "--branch", "--untracked-files=all"};
    const bool ignored = (depth & WithIgnored) != 0;
    if (ignored) args.push_back("--ignored=matching");
    args.push_back("--");
    args.push_back(projectPathspec());
    const ProcessResult result = git(std::move(args), 0, {}, cancel);
    if (!result.ok()) return resultOf(result);
    parsePorcelainV2(result.out, out);
    readOperation(out.branch);
    out.hasIgnored = ignored;
    // the lockable attribute: with the ignored set (a .gitattributes change) and the locks
    if (depth & (WithLocksAndRemote | WithIgnored)) readLockable(out);
    if (depth & WithLocksAndRemote) {
        // a failed lock read (the server is down) keeps the last locks and says why
        if (cfg.lfs) {
            const VcsResult locks = readLocks(out);
            if (!locks.ok) out.lockError = locks.message;
        }
        readRemote(out);
    }
    return VcsResult::success();
}

VcsResult GitProvider::readLocks(VcsSnapshot& out) {
    const ProcessResult result = git({"lfs", "locks", "--verify", "--json"}, cfg.networkTimeoutMs);
    if (!result.ok()) return resultOf(result);
    Json json = Json::parse(result.out, nullptr, false);
    if (json.is_discarded() || !json.is_object()) return VcsResult::failure("git lfs locks: unreadable output");
    for (const char* side : {"ours", "theirs"}) {
        const Json& list = json.contains(side) ? json[side] : Json();
        if (!list.is_array()) continue;
        for (const Json& item : list) {
            if (!item.is_object() || !item.contains("path") || !item["path"].is_string()) continue;
            VcsLock lock;
            lock.path = item["path"].get<std::string>();
            if (item.contains("id") && item["id"].is_string()) lock.id = item["id"].get<std::string>();
            if (item.contains("owner") && item["owner"].is_object() && item["owner"].contains("name") && item["owner"]["name"].is_string()) {
                lock.owner = item["owner"]["name"].get<std::string>();
            }
            lock.mine = std::string_view(side) == "ours";
            // only the project's (the server lists the repository's)
            if (pathKeyIsWithin(lock.path, projectKey)) out.locks.push_back(std::move(lock));
        }
    }
    out.hasLocks = true;
    return VcsResult::success();
}

VcsResult GitProvider::readLockable(VcsSnapshot& out) {
    const ProcessResult files = git({"ls-files", "-z", "--", projectPathspec()});
    if (!files.ok()) return resultOf(files);
    std::vector<RepoPath> paths;
    for (std::string_view path : splitNul(files.out)) {
        if (!path.empty()) paths.emplace_back(path);
    }
    for (const VcsChange& change : out.entries) {
        if (change.index == FileState::Untracked) paths.push_back(change.path);
    }
    std::string input;
    for (const RepoPath& path : paths) {
        input += path;
        input.push_back('\0');
    }
    if (input.empty()) {
        out.hasLockable = true;
        return VcsResult::success();
    }
    const ProcessResult attrs = git({"check-attr", "-z", "--stdin", "lockable", "merge"}, 0, std::move(input));
    if (!attrs.ok()) return resultOf(attrs);
    const std::vector<std::string_view> parts = splitNul(attrs.out);
    bool sceneMerge = false;
    for (size_t i = 0; i + 2 < parts.size(); i += 3) {
        const std::string_view path = parts[i];
        const std::string_view attribute = parts[i + 1];
        const std::string_view value = parts[i + 2];
        if (attribute == "lockable") {
            if (cfg.lfs && value != "unspecified" && value != "unset") out.lockable.emplace_back(path);
        } else if (attribute == "merge" && value == "jasperengine-scene") {
            sceneMerge = true;
        }
    }
    out.mergeDriverMissing = sceneMerge && !mergeDriverConfigured();
    out.hasLockable = true;
    return VcsResult::success();
}

VcsResult GitProvider::readRemote(VcsSnapshot& out) {
    out.hasRemote = true;
    if (out.branch.upstream.empty() || out.branch.behind <= 0) return VcsResult::success();
    const ProcessResult result = git({"diff", "--no-ext-diff", "--no-textconv", "--name-only", "-z", "HEAD...@{upstream}", "--", projectPathspec()});
    if (!result.ok()) {
        out.hasRemote = false;
        return resultOf(result);
    }
    for (std::string_view path : splitNul(result.out)) {
        if (!path.empty()) out.remoteChanged.emplace_back(path);
    }
    return VcsResult::success();
}

VcsResult GitProvider::branchInfo(VcsBranchInfo& out) {
    VcsSnapshot snapshot;
    const ProcessResult result = git({"status", "--porcelain=v2", "-z", "--branch", "--untracked-files=no", "--", projectPathspec()});
    if (!result.ok()) return resultOf(result);
    parsePorcelainV2(result.out, snapshot);
    readOperation(snapshot.branch);
    out = snapshot.branch;
    return VcsResult::success();
}

bool GitProvider::validRevision(std::string_view rev) const {
    auto hex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); };
    if (rev.size() >= 7 && rev.size() <= 64 && std::all_of(rev.begin(), rev.end(), hex)) return true;
    if (rev.rfind("HEAD", 0) != 0) return false;
    if (rev.size() == 4) return true;
    if (rev[4] != '~') return false;
    const std::string_view digits = rev.substr(5);
    return !digits.empty() && digits.size() <= 4 && std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; });
}

VcsResult GitProvider::baseContent(const RepoPath& path, std::string_view rev, std::string& out, size_t maxBytes) {
    if (!validRevision(rev)) return VcsResult::failure("not a revision: " + std::string(rev));
    ProcessResult result = runGit(cfg.executable, cfg.repoRoot, env, {"cat-file", "blob", std::string(rev) + ":" + path},
                                  cfg.localTimeoutMs, {}, nullptr, true, keptOutputBytes(maxBytes));
    if (!result.ok()) return resultOf(result);
    out = std::move(result.out);
    if (maxBytes != SIZE_MAX && out.size() > maxBytes) out.resize(maxBytes + 1);
    return VcsResult::success();
}

VcsResult GitProvider::textDiff(const RepoPath& path, std::string_view rev, std::string& out) {
    if (!validRevision(rev)) return VcsResult::failure("not a revision: " + std::string(rev));
    const RepoPath single[] = {path};
    const ProcessResult result = gitWithPaths({"diff", "--no-ext-diff", "--no-textconv", "--no-color", std::string(rev)}, single);
    if (!result.ok()) return resultOf(result);
    out = result.out;
    if (!out.empty()) return VcsResult::success();
    // an untracked file: git diffs nothing, so it reads as all added
    const ProcessResult tracked = gitWithPaths({"ls-files", "-z"}, single);
    if (!tracked.ok() || !tracked.out.empty()) return VcsResult::success();
    std::ifstream in(absoluteOf(cfg.repoRoot, path), std::ios::binary);
    if (!in) return VcsResult::success();
    std::ostringstream content;
    content << in.rdbuf();
    const std::string text = content.str();
    if (text.find('\0') != std::string::npos) {
        out = "Binary file " + path + " added\n";
        return VcsResult::success();
    }
    std::vector<std::string_view> lines;
    size_t start = 0;
    while (start < text.size()) {
        const size_t end = text.find('\n', start);
        lines.push_back(std::string_view(text).substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    out = "--- /dev/null\n+++ b/" + path + "\n@@ -0,0 +1," + std::to_string(lines.size()) + " @@\n";
    for (std::string_view line : lines) {
        out += "+";
        out += line;
        out += "\n";
    }
    return VcsResult::success();
}

VcsResult GitProvider::stage(std::span<const RepoPath> paths) {
    if (paths.empty()) return VcsResult::success();
    return resultOf(gitWithPaths({"add", "-A"}, paths));
}

VcsResult GitProvider::unstage(std::span<const RepoPath> paths) {
    if (paths.empty()) return VcsResult::success();
    const ProcessResult restored = gitWithPaths({"restore", "--staged"}, paths);
    if (restored.ok()) return VcsResult::success();
    // before the first commit there is no HEAD to restore from
    VcsBranchInfo branch;
    if (branchInfo(branch).ok && branch.headOid.empty()) return resultOf(gitWithPaths({"rm", "--cached", "-r", "-q"}, paths));
    return resultOf(restored);
}

VcsResult GitProvider::commit(std::span<const RepoPath> paths, std::string_view message, std::string& revOut, const jobs::CancelToken* cancel) {
    if (paths.empty()) return VcsResult::failure("nothing to commit: no paths");
    if (trimmed(message).empty()) return VcsResult::failure("the commit message is empty");
    // untracked selections are added first: --only commits paths git knows
    const ProcessResult untracked = gitWithPaths({"ls-files", "-z", "--others", "--exclude-standard"}, paths);
    if (!untracked.ok()) return resultOf(untracked);
    std::vector<RepoPath> toAdd;
    for (std::string_view path : splitNul(untracked.out)) {
        if (!path.empty()) toAdd.emplace_back(path);
    }
    if (!toAdd.empty()) {
        const VcsResult added = stage(toAdd);
        if (!added.ok) return added;
    }
    // other files the user staged stay staged; the hooks run (never --no-verify)
    std::string text(message);
    if (text.empty() || text.back() != '\n') text.push_back('\n');
    const ProcessResult committed = gitWithPaths({"commit", "--only", "--cleanup=whitespace", "-F", "-"}, paths, cfg.commitTimeoutMs,
                                                 std::move(text), cancel);
    if (!committed.ok()) return resultOf(committed);
    const ProcessResult head = git({"rev-parse", "HEAD"});
    if (!head.ok()) return resultOf(head);
    revOut = trimmed(head.out);
    return VcsResult::success(trimmed(committed.out));
}

VcsResult GitProvider::revert(std::span<const RepoPath> paths) {
    if (paths.empty()) return VcsResult::success();
    return resultOf(gitWithPaths({"restore", "--source=HEAD", "--staged", "--worktree"}, paths));
}

VcsResult GitProvider::log(const RepoPath* path, int limit, int skip, std::vector<VcsLogEntry>& out) {
    std::vector<std::string> args = {"log", "-z", "--format=%H%x1f%an%x1f%aI%x1f%s", "-n", std::to_string(std::max(1, limit))};
    if (skip > 0) args.push_back("--skip=" + std::to_string(skip));
    args.push_back("--");
    const std::string pathspec = path ? *path : projectPathspec();
    // case-insensitive file systems: the history of the file whatever case a commit gave it
    if constexpr (kCaseInsensitivePaths) args.push_back(":(literal,icase)" + pathspec);
    else args.push_back(pathspec);
    ProcessResult result = runGit(cfg.executable, cfg.repoRoot, env, std::move(args), cfg.localTimeoutMs, {}, nullptr,
                                  !kCaseInsensitivePaths);
    // a repository without commits has no log
    if (!result.ok() && result.err.find("does not have any commits") != std::string::npos) return VcsResult::success();
    if (!result.ok()) return resultOf(result);
    for (std::string_view record : splitNul(result.out)) {
        if (record.empty()) continue;
        if (record.front() == '\n') record.remove_prefix(1);
        VcsLogEntry entry;
        size_t field = 0;
        size_t start = 0;
        for (size_t i = 0; i <= record.size(); i++) {
            if (i < record.size() && record[i] != '\x1f') continue;
            const std::string value(record.substr(start, i - start));
            switch (field++) {
                case 0: entry.revision = value; break;
                case 1: entry.author = value; break;
                case 2: entry.date = value; break;
                default: entry.message = entry.message.empty() ? value : entry.message + "\x1f" + value; break;
            }
            start = i + 1;
        }
        if (!entry.revision.empty()) out.push_back(std::move(entry));
    }
    return VcsResult::success();
}

VcsResult GitProvider::lock(std::span<const RepoPath> paths) {
    if (!cfg.lfs) return VcsResult::unsupported();
    std::string failures;
    for (const RepoPath& path : paths) {
        const ProcessResult result = git({"lfs", "lock", "--json", "--", path}, cfg.networkTimeoutMs);
        if (!result.ok()) failures += (failures.empty() ? "" : "\n") + path + ": " + result.message();
    }
    if (!failures.empty()) return VcsResult::failure(failures);
    return VcsResult::success("Locked " + std::to_string(paths.size()) + (paths.size() == 1 ? " file" : " files"));
}

VcsResult GitProvider::unlock(std::span<const RepoPath> paths, bool force) {
    if (!cfg.lfs) return VcsResult::unsupported();
    std::string failures;
    for (const RepoPath& path : paths) {
        std::vector<std::string> args = {"lfs", "unlock", "--json"};
        if (force) args.push_back("--force");
        args.push_back("--");
        args.push_back(path);
        const ProcessResult result = git(std::move(args), cfg.networkTimeoutMs);
        if (!result.ok()) failures += (failures.empty() ? "" : "\n") + path + ": " + result.message();
    }
    if (!failures.empty()) return VcsResult::failure(failures);
    return VcsResult::success("Unlocked " + std::to_string(paths.size()) + (paths.size() == 1 ? " file" : " files"));
}

VcsResult GitProvider::lockable(std::span<const RepoPath> paths, std::vector<bool>& out) {
    out.assign(paths.size(), false);
    if (!cfg.lfs || paths.empty()) return VcsResult::success();
    std::string input;
    for (const RepoPath& path : paths) {
        input += path;
        input.push_back('\0');
    }
    const ProcessResult attrs = git({"check-attr", "-z", "--stdin", "lockable"}, 0, std::move(input));
    if (!attrs.ok()) return resultOf(attrs);
    const std::vector<std::string_view> parts = splitNul(attrs.out);
    for (size_t i = 0; i + 2 < parts.size(); i += 3) {
        if (parts[i + 2] == "unspecified" || parts[i + 2] == "unset") continue;
        for (size_t p = 0; p < paths.size(); p++) {
            if (pathKeysEqual(paths[p], parts[i])) out[p] = true;
        }
    }
    return VcsResult::success();
}

VcsResult GitProvider::fetch(const jobs::CancelToken* cancel) {
    return resultOf(git({"fetch"}, cfg.networkTimeoutMs, {}, cancel));
}

VcsResult GitProvider::pull(const jobs::CancelToken* cancel) {
    return resultOf(git({"pull", "--ff-only"}, cfg.networkTimeoutMs, {}, cancel));
}

VcsResult GitProvider::push(const jobs::CancelToken* cancel) {
    return resultOf(git({"push"}, cfg.networkTimeoutMs, {}, cancel));
}

VcsResult GitProvider::conflictSides(const RepoPath& path, ConflictSides& out) {
    auto stage = [this, &path](int number, std::string& into) {
        const ProcessResult result = git({"cat-file", "blob", ":" + std::to_string(number) + ":" + path});
        if (!result.ok()) return false;
        into = result.out;
        return true;
    };
    out.hasBase = stage(1, out.base);
    if (!stage(2, out.ours)) return VcsResult::failure(path + " has no \"ours\" side (stage 2): it is not in conflict");
    if (!stage(3, out.theirs)) return VcsResult::failure(path + " has no \"theirs\" side (stage 3): it is not in conflict");
    return VcsResult::success();
}

VcsResult GitProvider::markResolved(std::span<const RepoPath> paths) {
    if (paths.empty()) return VcsResult::success();
    return resultOf(gitWithPaths({"add"}, paths));
}

VcsResult GitProvider::resolveWith(const RepoPath& path, bool ours) {
    const RepoPath single[] = {path};
    const ProcessResult checkout = gitWithPaths({"checkout", ours ? "--ours" : "--theirs"}, single);
    if (!checkout.ok()) return resultOf(checkout);
    return markResolved(single);
}

std::string GitProvider::mergeDriverCommand(const fs::path& editorExecutable) {
    // git runs the driver through its shell: the path single-quoted (a quote as '\'')
    std::string quoted = "'";
    for (char c : genericUtf8(editorExecutable)) {
        if (c == '\'') quoted += "'\\''";
        else quoted += c;
    }
    quoted += "'";
    return quoted + " merge-scene %O %A %B %P";
}

VcsResult GitProvider::setupMergeDriver(const fs::path& editorExecutable) {
    if (!editorExecutable.is_absolute()) return VcsResult::failure("the editor's path is not absolute");
    const ProcessResult driver = git({"config", "--local", "merge.jasperengine-scene.driver", mergeDriverCommand(editorExecutable)});
    if (!driver.ok()) return resultOf(driver);
    return resultOf(git({"config", "--local", "merge.jasperengine-scene.name", "JasperEngine scene and prefab merge"}));
}

bool GitProvider::mergeDriverConfigured() {
    return git({"config", "--local", "--get", "merge.jasperengine-scene.driver"}).ok();
}

}
