// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "vcs/VcsTypes.h"

#include <algorithm>

namespace jasperengine::editor::vcs {

const char* fileStateName(FileState state) {
    switch (state) {
        case FileState::Unmodified: return "unmodified";
        case FileState::Added: return "added";
        case FileState::Modified: return "modified";
        case FileState::Deleted: return "deleted";
        case FileState::Renamed: return "renamed";
        case FileState::Untracked: return "untracked";
        case FileState::Ignored: return "ignored";
    }
    return "unmodified";
}

const char* lockStateName(LockState state) {
    switch (state) {
        case LockState::Unsupported: return "unsupported";
        case LockState::Unlocked: return "unlocked";
        case LockState::LockedByMe: return "locked_by_me";
        case LockState::LockedByOther: return "locked_by_other";
    }
    return "unsupported";
}

const char* vcsOperationName(VcsOperation operation) {
    switch (operation) {
        case VcsOperation::None: return "";
        case VcsOperation::Merge: return "merge";
        case VcsOperation::Rebase: return "rebase";
        case VcsOperation::CherryPick: return "cherry_pick";
        case VcsOperation::Revert: return "revert";
        case VcsOperation::Bisect: return "bisect";
    }
    return "";
}

std::string headLabel(const VcsBranchInfo& branch) {
    std::string label;
    if (branch.operation == VcsOperation::Rebase && !branch.operationBranch.empty()) {
        // a rebase detaches HEAD: the branch it works on is the one to name
        label = branch.operationBranch;
    } else if (!branch.tag.empty()) {
        label = "tag " + branch.tag;
    } else if (branch.detached) {
        label = branch.headOid.empty() ? std::string("(no commit)") : branch.headOid.substr(0, 7);
    } else {
        label = branch.name;
    }
    switch (branch.operation) {
        case VcsOperation::None: break;
        case VcsOperation::Merge: label += " | MERGING"; break;
        case VcsOperation::Rebase:
            label += " | REBASE";
            if (branch.operationStep > 0 && branch.operationTotal > 0) {
                label += " " + std::to_string(branch.operationStep) + "/" + std::to_string(branch.operationTotal);
            }
            break;
        case VcsOperation::CherryPick: label += " | CHERRY-PICKING"; break;
        case VcsOperation::Revert: label += " | REVERTING"; break;
        case VcsOperation::Bisect: label += " | BISECTING"; break;
    }
    return label;
}

const char* dirStateName(DirState state) {
    switch (state) {
        case DirState::Clean: return "clean";
        case DirState::Untracked: return "untracked";
        case DirState::Changed: return "changed";
        case DirState::LockedByOther: return "locked_by_other";
        case DirState::Conflict: return "conflict";
    }
    return "clean";
}

namespace {

    bool isChange(FileState state) {
        return state != FileState::Unmodified && state != FileState::Ignored;
    }

    // R6: Conflict > LockedByOther > Modified/Added/Deleted > Untracked > Clean
    DirState rankOf(const VcsChange& change) {
        if (change.conflict) return DirState::Conflict;
        if (change.lock == LockState::LockedByOther) return DirState::LockedByOther;
        if (change.index == FileState::Untracked || change.worktree == FileState::Untracked) return DirState::Untracked;
        if (isChange(change.index) || isChange(change.worktree)) return DirState::Changed;
        return DirState::Clean;
    }

    bool keyLess(const VcsChange& a, const VcsChange& b) {
        return comparePathKeys(a.path, b.path) < 0;
    }

    void sortKeys(std::vector<RepoPath>& keys) {
        if (!std::is_sorted(keys.begin(), keys.end(), PathKeyLess())) std::sort(keys.begin(), keys.end(), PathKeyLess());
        keys.erase(std::unique(keys.begin(), keys.end(), [](const RepoPath& a, const RepoPath& b) { return pathKeysEqual(a, b); }), keys.end());
    }

    // `into` takes what `from` (the same path) knows that it does not
    void merge(VcsChange& into, const VcsChange& from) {
        if (into.index == FileState::Unmodified) into.index = from.index;
        if (into.worktree == FileState::Unmodified) into.worktree = from.worktree;
        if (into.oldPath.empty()) into.oldPath = from.oldPath;
        into.conflict = into.conflict || from.conflict;
        if (into.lock == LockState::Unsupported || (into.lock == LockState::Unlocked && from.lock > LockState::Unlocked)) {
            into.lock = from.lock;
            into.lockOwner = from.lockOwner;
            into.lockId = from.lockId;
        }
        into.remoteChanged = into.remoteChanged || from.remoteChanged;
        if (into.baseOid.empty()) into.baseOid = from.baseOid;
    }

}

bool VcsChange::changed() const {
    return conflict || isChange(index) || isChange(worktree);
}

bool VcsChange::clean() const {
    return !changed() && lock != LockState::LockedByMe && lock != LockState::LockedByOther && !remoteChanged;
}

bool VcsChange::newFile() const {
    // git stages an add (index), svn schedules one (worktree); a move has its source's history
    return index == FileState::Untracked || worktree == FileState::Untracked ||
           ((index == FileState::Added || worktree == FileState::Added) && worktree != FileState::Deleted && oldPath.empty());
}

bool VcsChange::deleted() const {
    return index == FileState::Deleted || worktree == FileState::Deleted;
}

void VcsSnapshot::finalize(const VcsSnapshot* previous) {
    if (previous) {
        if (!hasLocks) {
            locks = previous->locks;
            if (lockError.empty()) lockError = previous->lockError;
        }
        if (!hasLockable) {
            lockable = previous->lockable;
            mergeDriverMissing = previous->mergeDriverMissing;
        }
        if (!hasRemote) remoteChanged = previous->remoteChanged;
        if (!hasIgnored) ignored = previous->ignored;
    }
    sortKeys(lockable);
    sortKeys(remoteChanged);
    sortKeys(ignored);
    std::sort(locks.begin(), locks.end(), [](const VcsLock& a, const VcsLock& b) { return comparePathKeys(a.path, b.path) < 0; });

    // the lock state of every lockable file, and the locks and server changes of files that
    // are otherwise clean, become entries of their own
    entries.reserve(entries.size() + lockable.size() + locks.size() + remoteChanged.size());
    for (const RepoPath& path : lockable) {
        VcsChange change;
        change.path = path;
        change.lock = LockState::Unlocked;
        entries.push_back(std::move(change));
    }
    for (const VcsLock& lock : locks) {
        VcsChange change;
        change.path = lock.path;
        change.lock = lock.mine ? LockState::LockedByMe : LockState::LockedByOther;
        change.lockOwner = lock.owner;
        change.lockId = lock.id;
        entries.push_back(std::move(change));
    }
    for (const RepoPath& path : remoteChanged) {
        VcsChange change;
        change.path = path;
        change.remoteChanged = true;
        entries.push_back(std::move(change));
    }
    if (!std::is_sorted(entries.begin(), entries.end(), keyLess)) std::stable_sort(entries.begin(), entries.end(), keyLess);
    // coalesce one path's records: the provider's first, then lockable, locks, remote
    size_t out = 0;
    for (size_t i = 0; i < entries.size(); i++) {
        if (out > 0 && pathKeysEqual(entries[out - 1].path, entries[i].path)) {
            merge(entries[out - 1], entries[i]);
            continue;
        }
        if (out != i) entries[out] = std::move(entries[i]);
        out++;
    }
    entries.resize(out);

    // Directories: one sweep over the sorted entries with a stack of the open ancestors; a
    // directory closes when an entry outside it comes, handing its state to its parent
    struct Open {
        size_t length;          // of the directory's key in the current entry's path
        DirState state;
        uint32_t changed;
        std::string path;
    };
    dirs.clear();
    std::vector<Open> stack;
    stack.push_back({0, DirState::Clean, 0, std::string()});
    auto close = [this, &stack]() {
        Open top = std::move(stack.back());
        stack.pop_back();
        Open& parent = stack.back();
        parent.state = std::max(parent.state, top.state);
        parent.changed += top.changed;
        if (top.state != DirState::Clean || top.changed > 0) dirs.push_back({std::move(top.path), top.state, top.changed});
    };
    for (const VcsChange& change : entries) {
        const std::string_view path = change.path;
        // close the directories this path is not below
        while (stack.size() > 1 && !pathKeyIsWithin(path, std::string_view(stack.back().path))) close();
        // open its directories below the deepest open one
        size_t from = stack.back().length == 0 ? 0 : stack.back().length + 1;
        for (size_t slash = path.find('/', from); slash != std::string_view::npos; slash = path.find('/', slash + 1)) {
            stack.push_back({slash, DirState::Clean, 0, std::string(path.substr(0, slash))});
        }
        Open& top = stack.back();
        top.state = std::max(top.state, rankOf(change));
        if (change.changed()) top.changed++;
    }
    while (stack.size() > 1) close();
    // the repository root
    if (stack.front().state != DirState::Clean || stack.front().changed > 0) dirs.push_back({std::string(), stack.front().state, stack.front().changed});
    std::sort(dirs.begin(), dirs.end(), [](const VcsDirEntry& a, const VcsDirEntry& b) { return comparePathKeys(a.path, b.path) < 0; });
}

const VcsChange* VcsSnapshot::find(std::string_view path) const {
    auto it = std::lower_bound(entries.begin(), entries.end(), path,
                               [](const VcsChange& change, std::string_view key) { return comparePathKeys(change.path, key) < 0; });
    return it != entries.end() && pathKeysEqual(it->path, path) ? &*it : nullptr;
}

const VcsDirEntry* VcsSnapshot::findDir(std::string_view path) const {
    auto it = std::lower_bound(dirs.begin(), dirs.end(), path,
                               [](const VcsDirEntry& dir, std::string_view key) { return comparePathKeys(dir.path, key) < 0; });
    return it != dirs.end() && pathKeysEqual(it->path, path) ? &*it : nullptr;
}

bool VcsSnapshot::isIgnored(std::string_view path) const {
    auto has = [this](std::string_view key) { return std::binary_search(ignored.begin(), ignored.end(), key, PathKeyLess()); };
    if (has(path)) return true;
    // an ignored directory ("build/") covers what is below it
    std::string prefix;
    for (size_t slash = path.find('/'); slash != std::string_view::npos; slash = path.find('/', slash + 1)) {
        prefix.assign(path.substr(0, slash + 1));
        if (has(prefix)) return true;
    }
    prefix.assign(path);
    prefix.push_back('/');
    return has(prefix);
}

bool VcsSnapshot::isLockable(std::string_view path) const {
    return std::binary_search(lockable.begin(), lockable.end(), path, PathKeyLess());
}

std::pair<const VcsChange*, const VcsChange*> VcsSnapshot::range(std::string_view dir) const {
    const VcsChange* begin = entries.data();
    const VcsChange* end = entries.data() + entries.size();
    if (dir.empty()) return {begin, end};
    const std::string low = std::string(dir) + "/";
    std::string high = std::string(dir);
    high.push_back(static_cast<char>('/' + 1));
    auto less = [](const VcsChange& change, std::string_view key) { return comparePathKeys(change.path, key) < 0; };
    const VcsChange* first = std::lower_bound(begin, end, std::string_view(low), less);
    const VcsChange* last = std::lower_bound(first, end, std::string_view(high), less);
    return {first, last};
}

size_t VcsSnapshot::changedCount() const {
    return static_cast<size_t>(std::count_if(entries.begin(), entries.end(), [](const VcsChange& change) { return change.changed(); }));
}

std::string_view metaAssetOf(std::string_view path) {
    constexpr std::string_view suffix = ".meta";
    if (path.size() <= suffix.size()) return {};
    if (path.substr(path.size() - suffix.size()) != suffix) return {};
    const std::string_view asset = path.substr(0, path.size() - suffix.size());
    // ".meta" alone, or "dir/.meta", is no sidecar
    if (asset.empty() || asset.back() == '/') return {};
    return asset;
}

}
