// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "vcs/SvnProvider.h"

#include "vcs/SvnXml.h"

#include "io/AtomicFile.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

namespace jasperengine::editor::vcs {

namespace {

    std::atomic<uint64_t> tempCounter{0};

    std::string trimmed(std::string_view text) {
        size_t begin = 0;
        size_t end = text.size();
        while (begin < end && static_cast<unsigned char>(text[begin]) <= ' ') begin++;
        while (end > begin && static_cast<unsigned char>(text[end - 1]) <= ' ') end--;
        return std::string(text.substr(begin, end - begin));
    }

    // svn reads "name@rev" as a peg revision: a trailing '@' makes every path literal
    // ("icon@2x.png@")
    std::string pegged(std::string_view path) {
        return std::string(path) + "@";
    }

    // a path svn printed: relative to the working copy root (status, as it was given) or
    // absolute (propget prints them so)
    RepoPath keyOf(std::string path, const fs::path& root = {}) {
        if (!root.empty()) {
            const fs::path printed = pathFromUtf8(path);
            RepoPath key;
            if (printed.is_absolute() && repoRelative(printed, root, key)) return key;
        }
        std::replace(path.begin(), path.end(), '\\', '/');
        while (path.rfind("./", 0) == 0) path.erase(0, 2);
        if (path == ".") path.clear();
        while (!path.empty() && path.back() == '/') path.pop_back();
        return path;
    }

    std::vector<std::pair<std::string, std::string>> svnEnvironment(const fs::path& projectDir,
                                                                    const std::vector<std::pair<std::string, std::string>>& extra) {
        const char* path = std::getenv("PATH");
        std::vector<std::pair<std::string, std::string>> env = {
            // English messages; paths stay UTF-8 (LC_ALL=C would make svn refuse non-ASCII names)
            {"LC_MESSAGES", "C"},
#if defined(__APPLE__)
            {"LC_CTYPE", "UTF-8"},
#elif !defined(_WIN32)
            {"LC_CTYPE", "C.UTF-8"},
#endif
            {"PATH", sanitizedPath(path ? path : "", projectDir)},
        };
        for (const auto& [key, value] : extra) {
            auto it = std::find_if(env.begin(), env.end(), [&key](const auto& entry) { return entry.first == key; });
            if (it != env.end()) it->second = value;
            else env.emplace_back(key, value);
        }
        return env;
    }

    ProcessResult runSvn(const fs::path& svn, const fs::path& cwd, const std::vector<std::pair<std::string, std::string>>& env,
                         std::vector<std::string> args, int timeoutMs, const jobs::CancelToken* cancel = nullptr,
                         const std::vector<std::string>& globalArgs = {}, size_t maxOutputBytes = 0) {
        ProcessSpec spec;
        spec.exe = svn;
        spec.cwd = cwd;
        spec.env = env;
        spec.unsetEnv = {"LC_ALL"};
        // before the subcommand: after a "--" they would be paths
        spec.args.push_back("--non-interactive");
        for (const std::string& arg : globalArgs) spec.args.push_back(arg);
        for (std::string& arg : args) spec.args.push_back(std::move(arg));
        spec.timeoutMs = timeoutMs;
        spec.cancel = cancel;
        if (maxOutputBytes > 0) spec.maxOutputBytes = maxOutputBytes;
        return runProcess(spec);
    }

    FileState stateOfItem(const std::string& item) {
        if (item == "modified" || item == "replaced" || item == "merged" || item == "obstructed" || item == "incomplete") return FileState::Modified;
        if (item == "added") return FileState::Added;
        if (item == "deleted" || item == "missing") return FileState::Deleted;
        if (item == "unversioned") return FileState::Untracked;
        return FileState::Unmodified;
    }

    // every element of that name below root, in document order
    void collect(const XmlElement& root, std::string_view name, std::vector<const XmlElement*>& out) {
        for (const XmlElement& child : root.children) {
            if (child.name == name) out.push_back(&child);
            collect(child, name, out);
        }
    }

    const XmlElement* findDeep(const XmlElement& root, std::string_view name) {
        for (const XmlElement& child : root.children) {
            if (child.name == name) return &child;
            if (const XmlElement* found = findDeep(child, name)) return found;
        }
        return nullptr;
    }

    bool readFile(const fs::path& path, std::string& out) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        std::ostringstream content;
        content << in.rdbuf();
        out = content.str();
        return true;
    }

}

SvnProvider::SvnProvider(ProviderConfig config): cfg(std::move(config)) {
    repoRelative(cfg.projectDir, cfg.repoRoot, projectKey);
    env = svnEnvironment(cfg.projectDir, cfg.extraEnv);
}

ToolVersion SvnProvider::checkSvn(const fs::path& svn, const std::vector<std::pair<std::string, std::string>>& extraEnv) {
    ToolVersion version;
    const ProcessResult result = runSvn(svn, {}, svnEnvironment({}, extraEnv), {"--version", "--quiet"}, 10000);
    if (!result.ok()) {
        version.message = "svn --version failed: " + result.message();
        return version;
    }
    if (!versionAtLeast(result.out, "1.10", &version.version)) {
        version.message = "Subversion " + (version.version.empty() ? trimmed(result.out) : version.version) + " is too old: 1.10 or newer is needed";
        return version;
    }
    version.ok = true;
    return version;
}

VcsResult SvnProvider::workingCopyRoot(const fs::path& svn, const fs::path& dir, const std::vector<std::pair<std::string, std::string>>& extraEnv,
                                       fs::path& out) {
    const ProcessResult result = runSvn(svn, dir, svnEnvironment(dir, extraEnv), {"info", "--xml", "--", "."}, 10000);
    if (!result.ok()) return resultOf(result);
    XmlElement root;
    std::string error;
    if (!parseXml(result.out, root, &error)) return VcsResult::failure("svn info: " + error);
    const XmlElement* wcRoot = findDeep(root, "wcroot-abspath");
    if (!wcRoot) return VcsResult::failure("svn info: no working copy root");
    out = pathFromUtf8(trimmed(wcRoot->text));
    return VcsResult::success();
}

VcsCaps SvnProvider::capabilities() const {
    VcsCaps caps;
    caps.provider = "svn";
    caps.locking = true;
    caps.pull = true;
    return caps;
}

ProcessResult SvnProvider::svn(std::vector<std::string> args, int timeoutMs, const jobs::CancelToken* cancel) const {
    return runSvn(cfg.executable, cfg.repoRoot, env, std::move(args), timeoutMs > 0 ? timeoutMs : cfg.localTimeoutMs, cancel,
                  cfg.svnGlobalArgs);
}

fs::path SvnProvider::tempFile(const char* prefix) const {
    std::error_code ec;
    fs::create_directories(cfg.internalDir, ec);
    return cfg.internalDir / (std::string(prefix) + std::to_string(io::currentProcessId()) + "-" + std::to_string(tempCounter++));
}

namespace {

    // `part`'s top-level children appended inside `into`'s root element (the documents of one
    // command run over chunks of paths); text without a root element is appended as it is
    void appendXmlDocument(std::string& into, const std::string& part) {
        auto rootOf = [](const std::string& text, size_t& open, size_t& close) {
            size_t at = 0;
            while ((at = text.find('<', at)) != std::string::npos && (text.compare(at, 2, "<?") == 0 || text.compare(at, 2, "<!") == 0)) at++;
            if (at == std::string::npos) return false;
            const size_t nameEnd = text.find_first_of(" \t\r\n/>", at + 1);
            if (nameEnd == std::string::npos) return false;
            const std::string name = text.substr(at + 1, nameEnd - at - 1);
            const size_t openEnd = text.find('>', nameEnd);
            close = text.rfind("</" + name + ">");
            if (openEnd == std::string::npos || close == std::string::npos || close < openEnd) return false;
            open = openEnd + 1;
            return true;
        };
        size_t intoOpen = 0, intoClose = 0, partOpen = 0, partClose = 0;
        if (!rootOf(into, intoOpen, intoClose) || !rootOf(part, partOpen, partClose)) {
            into += part;
            return;
        }
        into.insert(intoClose, part.substr(partOpen, partClose - partOpen));
    }

}

ProcessResult SvnProvider::svnWithTargets(std::vector<std::string> args, std::span<const RepoPath> paths, int timeoutMs,
                                          const jobs::CancelToken* cancel) const {
    // status and propget take no --targets: their paths go on the command line, in chunks (after
    // --, each with its peg escape), the outputs one XML document again
    static const char* const kTargetsCommands[] = {"add", "commit", "delete", "info", "lock", "unlock", "revert", "resolve", "update"};
    const bool targetsFile = !args.empty() && std::any_of(std::begin(kTargetsCommands), std::end(kTargetsCommands),
                                                          [&](const char* command) { return args[0] == command; });
    if (!targetsFile) {
        constexpr size_t kChunkPaths = 256;
        ProcessResult combined;
        for (size_t start = 0; start < paths.size(); start += kChunkPaths) {
            std::vector<std::string> chunk = args;
            chunk.push_back("--");
            for (size_t i = start; i < std::min(paths.size(), start + kChunkPaths); i++) chunk.push_back(pegged(paths[i].empty() ? "." : paths[i]));
            ProcessResult part = svn(std::move(chunk), timeoutMs, cancel);
            if (!part.ok()) return part;
            combined.exitCode = part.exitCode;
            combined.err += part.err;
            if (combined.out.empty()) combined.out = std::move(part.out);
            else appendXmlDocument(combined.out, part.out);
        }
        return combined;
    }
    const fs::path file = tempFile("targets-");
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        for (const RepoPath& path : paths) out << pegged(path.empty() ? "." : path) << "\n";
        if (!out) {
            ProcessResult failed;
            failed.spawnFailed = true;
            failed.spawnError = "could not write " + file.string();
            return failed;
        }
    }
    args.push_back("--targets");
    args.push_back(file.string());
    ProcessResult result = svn(std::move(args), timeoutMs, cancel);
    std::error_code ec;
    fs::remove(file, ec);
    return result;
}

VcsResult SvnProvider::resultOf(const ProcessResult& process) {
    if (process.ok()) return VcsResult::success(trimmed(process.out));
    return VcsResult::failure(process.message(), process.exitCode);
}

std::string SvnProvider::projectTarget() const {
    return projectKey.empty() ? std::string(".") : projectKey;
}

bool SvnProvider::validRevision(std::string_view rev) const {
    if (rev == "BASE" || rev == "HEAD") return true;
    return !rev.empty() && rev.size() <= 12 && std::all_of(rev.begin(), rev.end(), [](char c) { return c >= '0' && c <= '9'; });
}

void SvnProvider::parseStatusXml(const XmlElement& root, bool remote, VcsSnapshot& out, int& againstRevision) {
    std::vector<const XmlElement*> entries;
    collect(root, "entry", entries);
    for (const XmlElement* entry : entries) {
        const XmlElement* wc = entry->child("wc-status");
        if (!wc) continue;
        const RepoPath path = keyOf(entry->attribute("path"));
        const std::string& item = wc->attribute("item");
        if (item == "ignored") {
            out.ignored.push_back(path);
            continue;
        }
        VcsChange change;
        change.path = path;
        change.worktree = stateOfItem(item);
        if (change.worktree == FileState::Untracked) change.index = FileState::Untracked;
        if (change.worktree == FileState::Unmodified && wc->attribute("props") == "modified") change.worktree = FileState::Modified;
        change.conflict = item == "conflicted" || wc->attribute("tree-conflicted") == "true";
        if (change.conflict && change.worktree == FileState::Unmodified) change.worktree = FileState::Modified;
        change.baseOid = wc->attribute("revision");
        // a move's new path: its BASE is the source's (a copy without a move counts as new, as in git)
        if (const std::string& from = wc->attribute("moved-from"); change.worktree == FileState::Added && !from.empty()) change.oldPath = keyOf(from);
        const XmlElement* wcLock = wc->child("lock");
        const XmlElement* repos = entry->child("repos-status");
        if (remote && repos) {
            const std::string& reposItem = repos->attribute("item");
            if (reposItem != "none" && !reposItem.empty()) change.remoteChanged = true;
            const XmlElement* reposLock = repos->child("lock");
            if (reposLock) {
                VcsLock lock;
                lock.path = path;
                lock.owner = reposLock->childText("owner");
                lock.id = reposLock->childText("token");
                // this working copy holds the server's lock token
                lock.mine = wcLock && wcLock->childText("token") == lock.id;
                out.locks.push_back(std::move(lock));
            }
        }
        // a fast poll reads no locks (the slow poll's stay); a token the server no longer has
        // (broken or stolen) is no lock
        if (change.changed() || change.remoteChanged) out.entries.push_back(std::move(change));
    }
    if (const XmlElement* against = findDeep(root, "against")) {
        againstRevision = std::atoi(against->attribute("revision").c_str());
    }
}

VcsResult SvnProvider::status(VcsSnapshot& out, uint32_t depth, const jobs::CancelToken* cancel) {
    const bool remote = (depth & WithLocksAndRemote) != 0;
    std::vector<std::string> args = {"status", "--xml"};
    if (remote) args.push_back("-u");
    if (depth & WithIgnored) args.push_back("--no-ignore");
    args.push_back("--");
    args.push_back(pegged(projectTarget()));
    const ProcessResult result = svn(std::move(args), remote ? cfg.networkTimeoutMs : 0, cancel);
    if (!result.ok()) {
        if (!remote) return resultOf(result);
        // the server is unreachable: the local state, and why the locks are stale
        const VcsResult local = status(out, depth & ~uint32_t(WithLocksAndRemote), cancel);
        if (local.ok) out.lockError = result.message();
        return local;
    }
    XmlElement root;
    std::string error;
    if (!parseXml(result.out, root, &error)) return VcsResult::failure("svn status: " + error);
    int against = 0;
    parseStatusXml(root, remote, out, against);
    // the editor's own folder (.jasperengine, which svn does not ignore unless told) is never a change
    RepoPath internalKey;
    if (repoRelative(cfg.projectDir / ".jasperengine", cfg.repoRoot, internalKey) && !internalKey.empty()) {
        std::erase_if(out.entries, [&internalKey](const VcsChange& change) { return pathKeyIsWithin(change.path, internalKey); });
    }
    out.hasIgnored = (depth & WithIgnored) != 0;
    VcsBranchInfo branch;
    if (branchInfo(branch).ok) out.branch = branch;
    if (remote) {
        out.hasLocks = true;
        out.hasRemote = true;
        const int working = std::atoi(out.branch.headOid.c_str());
        out.branch.behind = against > working ? against - working : 0;
    }
    if (depth & (WithLocksAndRemote | WithIgnored)) readLockable(out);
    return VcsResult::success();
}

VcsResult SvnProvider::readLockable(VcsSnapshot& out) {
    const ProcessResult result = svn({"propget", "svn:needs-lock", "-R", "--xml", "--", pegged(projectTarget())});
    if (!result.ok()) return resultOf(result);
    XmlElement root;
    if (!parseXml(result.out, root)) return VcsResult::failure("svn propget: unreadable output");
    for (const XmlElement& target : root.children) {
        if (target.name == "target") out.lockable.push_back(keyOf(target.attribute("path"), cfg.repoRoot));
    }
    out.hasLockable = true;
    return VcsResult::success();
}

VcsResult SvnProvider::branchInfo(VcsBranchInfo& out) {
    const ProcessResult result = svn({"info", "--xml", "--", pegged(projectTarget())});
    if (!result.ok()) return resultOf(result);
    XmlElement root;
    if (!parseXml(result.out, root)) return VcsResult::failure("svn info: unreadable output");
    const XmlElement* entry = findDeep(root, "entry");
    if (!entry) return VcsResult::failure("svn info: no entry");
    out.headOid = entry->attribute("revision");
    out.name = entry->childText("relative-url");
    if (out.name.empty()) out.name = entry->childText("url");
    // a working copy of ^/tags/<tag>/...: named by its tag, as a detached git HEAD is
    constexpr std::string_view tags = "^/tags/";
    if (out.name.rfind(tags, 0) == 0) {
        const std::string rest = out.name.substr(tags.size());
        out.tag = rest.substr(0, rest.find('/'));
    }
    return VcsResult::success();
}

VcsResult SvnProvider::baseContent(const RepoPath& path, std::string_view rev, std::string& out, size_t maxBytes) {
    if (!validRevision(rev)) return VcsResult::failure("not a revision: " + std::string(rev));
    ProcessResult result = runSvn(cfg.executable, cfg.repoRoot, env, {"cat", "-r", std::string(rev), "--", pegged(path)},
                                  cfg.localTimeoutMs, nullptr, cfg.svnGlobalArgs, keptOutputBytes(maxBytes));
    if (!result.ok()) return resultOf(result);
    out = std::move(result.out);
    if (maxBytes != SIZE_MAX && out.size() > maxBytes) out.resize(maxBytes + 1);
    return VcsResult::success();
}

VcsResult SvnProvider::textDiff(const RepoPath& path, std::string_view rev, std::string& out) {
    if (!validRevision(rev)) return VcsResult::failure("not a revision: " + std::string(rev));
    std::vector<std::string> args = {"diff", "--internal-diff"};
    if (rev != "BASE") {
        args.push_back("-r");
        args.push_back(std::string(rev));
    }
    args.push_back("--");
    // svn 1.14 diff reads a peg revision only with -r: against BASE "a.lua@" names no file,
    // against a revision "icon@2x.lua" is a peg revision "2x.lua"
    args.push_back(rev != "BASE" ? pegged(path) : std::string(path));
    const ProcessResult result = svn(std::move(args));
    // a versioned file: its diff, empty when it has no changes
    if (result.ok()) {
        out = result.out;
        return VcsResult::success();
    }
    // an unversioned file (svn refuses to diff it): all of it is new
    std::string text;
    if (!readFile(absoluteOf(cfg.repoRoot, path), text)) return resultOf(result);
    if (text.find('\0') != std::string::npos) {
        out = "Binary file " + path + " added\n";
        return VcsResult::success();
    }
    size_t lines = 0;
    std::string body;
    size_t start = 0;
    while (start < text.size()) {
        const size_t end = text.find('\n', start);
        body += "+" + text.substr(start, end == std::string::npos ? std::string::npos : end - start) + "\n";
        lines++;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    out = "--- /dev/null\n+++ " + path + "\n@@ -0,0 +1," + std::to_string(lines) + " @@\n" + body;
    return VcsResult::success();
}

VcsResult SvnProvider::stage(std::span<const RepoPath> paths) {
    if (paths.empty()) return VcsResult::success();
    // svn has no index: staging puts unversioned files (and their folders) under version control
    return resultOf(svnWithTargets({"add", "--force", "--parents", "--depth", "empty"}, paths));
}

VcsResult SvnProvider::unstage(std::span<const RepoPath> paths) {
    if (paths.empty()) return VcsResult::success();
    // only what stage added goes back to unversioned (its file stays)
    const ProcessResult status = svnWithTargets({"status", "--xml", "--depth", "empty"}, paths);
    if (!status.ok()) return resultOf(status);
    XmlElement root;
    if (!parseXml(status.out, root)) return VcsResult::failure("svn status: unreadable output");
    std::vector<RepoPath> added;
    std::vector<const XmlElement*> entries;
    collect(root, "entry", entries);
    for (const XmlElement* entry : entries) {
        const XmlElement* wc = entry->child("wc-status");
        if (wc && wc->attribute("item") == "added") added.push_back(keyOf(entry->attribute("path")));
    }
    if (added.empty()) return VcsResult::success();
    return resultOf(svnWithTargets({"revert"}, added));
}

VcsResult SvnProvider::commit(std::span<const RepoPath> paths, std::string_view message, std::string& revOut, const jobs::CancelToken* cancel) {
    if (paths.empty()) return VcsResult::failure("nothing to commit: no paths");
    if (trimmed(message).empty()) return VcsResult::failure("the commit message is empty");
    // unversioned selections are added and missing ones deleted, as the commit list shows them
    const ProcessResult status = svnWithTargets({"status", "--xml", "--depth", "empty"}, paths);
    if (!status.ok()) return resultOf(status);
    XmlElement root;
    if (!parseXml(status.out, root)) return VcsResult::failure("svn status: unreadable output");
    std::vector<RepoPath> toAdd, toDelete;
    std::vector<const XmlElement*> entries;
    collect(root, "entry", entries);
    std::set<RepoPath, PathKeyLess> reported;
    for (const XmlElement* entry : entries) {
        const XmlElement* wc = entry->child("wc-status");
        if (!wc) continue;
        reported.insert(keyOf(entry->attribute("path")));
        if (wc->attribute("item") == "unversioned") toAdd.push_back(keyOf(entry->attribute("path")));
        else if (wc->attribute("item") == "missing") toDelete.push_back(keyOf(entry->attribute("path")));
    }
    // a file in an unversioned folder: status says nothing of it ("not found"); it is added
    // with its folders (an add of a versioned file changes nothing)
    for (const RepoPath& path : paths) {
        std::error_code ec;
        if (!reported.count(path) && fs::exists(absoluteOf(cfg.repoRoot, path), ec)) toAdd.push_back(path);
    }
    if (!toAdd.empty()) {
        const VcsResult added = stage(toAdd);
        if (!added.ok) return added;
    }
    if (!toDelete.empty()) {
        const ProcessResult deleted = svnWithTargets({"delete"}, toDelete);
        if (!deleted.ok()) return resultOf(deleted);
    }
    // a file in a folder that is new itself: the folder's add is part of the commit
    std::vector<RepoPath> targets(paths.begin(), paths.end());
    std::set<RepoPath, PathKeyLess> parents;
    for (const RepoPath& path : paths) {
        for (std::string_view dir = parentKey(path); !dir.empty(); dir = parentKey(dir)) parents.insert(std::string(dir));
    }
    if (!parents.empty()) {
        const std::vector<RepoPath> dirs(parents.begin(), parents.end());
        const ProcessResult dirStatus = svnWithTargets({"status", "--xml", "--depth", "empty"}, dirs);
        XmlElement dirRoot;
        if (dirStatus.ok() && parseXml(dirStatus.out, dirRoot)) {
            std::vector<const XmlElement*> dirEntries;
            collect(dirRoot, "entry", dirEntries);
            for (const XmlElement* entry : dirEntries) {
                const XmlElement* wc = entry->child("wc-status");
                if (wc && wc->attribute("item") == "added") targets.push_back(keyOf(entry->attribute("path")));
            }
        }
    }
    const fs::path messageFile = tempFile("message-");
    {
        std::ofstream out(messageFile, std::ios::binary | std::ios::trunc);
        out.write(message.data(), static_cast<std::streamsize>(message.size()));
        if (!out) return VcsResult::failure("could not write " + messageFile.string());
    }
    // exactly the listed paths (their folders' adds), never what else is modified below them
    const ProcessResult committed = svnWithTargets({"commit", "-F", messageFile.string(), "--encoding", "UTF-8", "--depth", "empty"},
                                                   targets, cfg.commitTimeoutMs, cancel);
    std::error_code ec;
    fs::remove(messageFile, ec);
    if (!committed.ok()) return resultOf(committed);
    const size_t at = committed.out.rfind("Committed revision ");
    if (at != std::string::npos) {
        size_t end = at + 19;
        while (end < committed.out.size() && committed.out[end] >= '0' && committed.out[end] <= '9') end++;
        revOut = committed.out.substr(at + 19, end - at - 19);
    }
    return VcsResult::success(trimmed(committed.out));
}

VcsResult SvnProvider::revert(std::span<const RepoPath> paths) {
    if (paths.empty()) return VcsResult::success();
    return resultOf(svnWithTargets({"revert"}, paths));
}

VcsResult SvnProvider::log(const RepoPath* path, int limit, int skip, std::vector<VcsLogEntry>& out) {
    // svn has no skip: the first `skip` of a longer log are passed over
    const int count = std::max(1, limit) + std::max(0, skip);
    // @HEAD: the history as the repository has it (a working copy folder's own revision lags
    // behind the commits of its files)
    const ProcessResult result = svn({"log", "--xml", "-l", std::to_string(count), "--", std::string(path ? *path : projectTarget()) + "@HEAD"});
    if (!result.ok()) return resultOf(result);
    XmlElement root;
    if (!parseXml(result.out, root)) return VcsResult::failure("svn log: unreadable output");
    int index = 0;
    for (const XmlElement& entry : root.children) {
        if (entry.name != "logentry") continue;
        if (index++ < skip) continue;
        VcsLogEntry log;
        log.revision = entry.attribute("revision");
        log.author = entry.childText("author");
        log.date = entry.childText("date");
        log.message = trimmed(entry.childText("msg"));
        out.push_back(std::move(log));
    }
    return VcsResult::success();
}

VcsResult SvnProvider::lock(std::span<const RepoPath> paths) {
    if (paths.empty()) return VcsResult::success();
    const std::string comment = cfg.userName.empty() ? std::string("JasperEngine Editor") : cfg.userName;
    const ProcessResult result = svnWithTargets({"lock", "-m", comment, "--encoding", "UTF-8"}, paths, cfg.networkTimeoutMs);
    // svn warns per path it could not lock and exits 0 or 1: either way stderr says which
    if (!result.ok() || result.err.find("warning") != std::string::npos) return VcsResult::failure(result.message(), result.exitCode);
    return VcsResult::success("Locked " + std::to_string(paths.size()) + (paths.size() == 1 ? " file" : " files"));
}

VcsResult SvnProvider::unlock(std::span<const RepoPath> paths, bool force) {
    if (paths.empty()) return VcsResult::success();
    std::vector<std::string> args = {"unlock"};
    if (force) args.push_back("--force");
    const ProcessResult result = svnWithTargets(std::move(args), paths, cfg.networkTimeoutMs);
    if (!result.ok() || result.err.find("warning") != std::string::npos) return VcsResult::failure(result.message(), result.exitCode);
    return VcsResult::success("Unlocked " + std::to_string(paths.size()) + (paths.size() == 1 ? " file" : " files"));
}

VcsResult SvnProvider::lockable(std::span<const RepoPath> paths, std::vector<bool>& out) {
    out.assign(paths.size(), false);
    if (paths.empty()) return VcsResult::success();
    // one recursive query of the project: every file that has the property (naming a file
    // without it is an error to svn)
    const ProcessResult result = svn({"propget", "svn:needs-lock", "--xml", "--depth", "infinity", "--", pegged(projectTarget())});
    if (!result.ok()) return resultOf(result);
    XmlElement root;
    if (!parseXml(result.out, root)) return VcsResult::failure("svn propget: unreadable output");
    for (const XmlElement& target : root.children) {
        if (target.name != "target") continue;
        const RepoPath key = keyOf(target.attribute("path"), cfg.repoRoot);
        for (size_t i = 0; i < paths.size(); i++) {
            if (pathKeysEqual(paths[i], key)) out[i] = true;
        }
    }
    return VcsResult::success();
}

VcsResult SvnProvider::pull(const jobs::CancelToken* cancel) {
    return resultOf(svn({"update", "--accept", "postpone", "--", pegged(projectTarget())}, cfg.networkTimeoutMs, cancel));
}

VcsResult SvnProvider::conflictSides(const RepoPath& path, ConflictSides& out) {
    const ProcessResult result = svn({"info", "--xml", "--", pegged(path)});
    if (!result.ok()) return resultOf(result);
    XmlElement root;
    if (!parseXml(result.out, root)) return VcsResult::failure("svn info: unreadable output");
    // .rOLD (the base), .mine (ours), .rNEW (theirs)
    auto sideFile = [this, &root, &path](std::string_view element) -> fs::path {
        const XmlElement* found = findDeep(root, element);
        if (!found || trimmed(found->text).empty()) return {};
        const fs::path file = pathFromUtf8(trimmed(found->text));
        if (file.is_absolute()) return file;
        // relative to the conflicted file's folder (older clients) or to the working copy root
        const fs::path beside = absoluteOf(cfg.repoRoot, path).parent_path() / file;
        std::error_code ec;
        return fs::exists(beside, ec) ? beside : cfg.repoRoot / file;
    };
    const fs::path base = sideFile("prev-base-file");
    const fs::path mine = sideFile("prev-wc-file");
    const fs::path theirs = sideFile("cur-base-file");
    if (mine.empty() || theirs.empty()) return VcsResult::failure(path + " has no text conflict files (.mine/.rNEW)");
    out.hasBase = !base.empty() && readFile(base, out.base);
    if (!readFile(mine, out.ours)) return VcsResult::failure("could not read " + mine.string());
    if (!readFile(theirs, out.theirs)) return VcsResult::failure("could not read " + theirs.string());
    return VcsResult::success();
}

VcsResult SvnProvider::markResolved(std::span<const RepoPath> paths) {
    if (paths.empty()) return VcsResult::success();
    return resultOf(svnWithTargets({"resolve", "--accept", "working"}, paths));
}

VcsResult SvnProvider::resolveWith(const RepoPath& path, bool ours) {
    const RepoPath single[] = {path};
    return resultOf(svnWithTargets({"resolve", "--accept", ours ? "mine-full" : "theirs-full"}, single));
}

}
