// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ProjectFileIndex.h"

#include <algorithm>
#include <optional>
#include <unordered_map>

using namespace jasperengine;

namespace fs = std::filesystem;

namespace {

    constexpr int MAX_DEPTH = 64;

    std::string childKey(const std::string& dir, const std::string& name) {
        return dir.empty() ? name : dir + "/" + name;
    }

    // [first, last) of the keys strictly under `dir` in an ordered map
    template <typename Map>
    std::pair<typename Map::const_iterator, typename Map::const_iterator> subtree(const Map& map, const std::string& dir) {
        if (dir.empty()) {
            return {map.begin(), map.end()};
        }
        // '/' + 1 == '0': every "dir/..." key sorts in ["dir/", "dir0")
        return {map.lower_bound(dir + "/"), map.lower_bound(dir + "0")};
    }

}

bool ProjectFileIndex::indexes(const std::string& name) {
    return !watch::isExcludedName(name) && !watch::isTemporaryName(name);
}

void ProjectFileIndex::reset(const fs::path& root) {
    rootPath = root.lexically_normal();
    entries.clear();
    if (!linkedRoots.empty()) linkedVersion++;
    linkedRoots.clear();
}

void ProjectFileIndex::dropLinkedFolders(const std::string& key) {
    if (linkedRoots.empty()) return;
    const size_t before = linkedRoots.size();
    if (key.empty()) {
        linkedRoots.clear();
    } else {
        linkedRoots.erase(key);
        auto range = subtree(linkedRoots, key);
        linkedRoots.erase(range.first, range.second);
    }
    if (linkedRoots.size() != before) linkedVersion++;
}

fs::path ProjectFileIndex::absolute(const std::string& key) const {
    return key.empty() ? rootPath : (rootPath / watch::pathFromUtf8(key)).lexically_normal();
}

bool ProjectFileIndex::keyOf(const fs::path& path, std::string& key) const {
    return watch::relativeTo(rootPath, path, key);
}

void ProjectFileIndex::walk(const std::string& key, std::map<std::string, Entry>& out, const std::atomic<bool>* cancel, int depth, LinkWalk* links) const {
    if (depth > MAX_DEPTH) return;
    std::error_code ec;
    fs::directory_iterator it(absolute(key), fs::directory_options::skip_permission_denied, ec);
    for (fs::directory_iterator end; !ec && it != end; it.increment(ec)) {
        if (cancel && cancel->load(std::memory_order_relaxed)) return;
        const std::string name = watch::pathToUtf8(it->path().filename());
        if (!indexes(name)) continue;
        // a linked folder could loop: its link is listed, and followed only when the link follower
        // says (a folder out of the root, once)
        const bool link = watch::isLink(*it);
        Entry entry;
        if (!watch::statPath(it->path(), entry)) continue;
        const std::string child = childKey(key, name);
        out[child] = entry;
        if (!entry.directory) continue;
        if (!link) {
            walk(child, out, cancel, depth + 1, links);
        } else if (links) {
            const fs::path real = links->follower.enter(it->path());
            if (real.empty()) continue;
            links->found[child] = real;
            walk(child, out, cancel, depth + 1, links);
        }
    }
}

void ProjectFileIndex::build(const std::atomic<bool>* cancel) {
    entries.clear();
    if (rootPath.empty()) return;
    if (!followLinks) {
        walk("", entries, cancel, 0, nullptr);
        return;
    }
    LinkWalk links(rootPath);
    walk("", entries, cancel, 0, &links);
    if (links.found != linkedRoots) linkedVersion++;
    linkedRoots = std::move(links.found);
}

bool ProjectFileIndex::find(const fs::path& path, Entry& out) const {
    std::string key;
    if (!keyOf(path, key) || key.empty()) return false;
    auto it = entries.find(key);
    if (it == entries.end()) return false;
    out = it->second;
    return true;
}

bool ProjectFileIndex::contains(const fs::path& path) const {
    Entry entry;
    return find(path, entry);
}

void ProjectFileIndex::set(const fs::path& path, const Entry& entry) {
    std::string key;
    if (!keyOf(path, key) || key.empty()) return;
    auto it = entries.find(key);
    if (it != entries.end() && it->second.directory && !entry.directory) {
        // a folder replaced by a file: what it held is gone
        auto range = subtree(entries, key);
        entries.erase(range.first, range.second);
        dropLinkedFolders(key);
    }
    entries[key] = entry;
}

void ProjectFileIndex::erase(const fs::path& path) {
    std::string key;
    if (!keyOf(path, key)) return;
    dropLinkedFolders(key);
    if (key.empty()) {
        entries.clear();
        return;
    }
    entries.erase(key);
    auto range = subtree(entries, key);
    entries.erase(range.first, range.second);
}

void ProjectFileIndex::move(const fs::path& from, const fs::path& to) {
    std::string fromKey, toKey;
    if (!keyOf(from, fromKey) || fromKey.empty()) return;
    if (!keyOf(to, toKey) || toKey.empty()) {
        erase(from);
        return;
    }
    if (fromKey == toKey) return;
    std::vector<std::pair<std::string, Entry>> moved;
    auto self = entries.find(fromKey);
    if (self != entries.end()) {
        moved.emplace_back(toKey, self->second);
        entries.erase(self);
    }
    auto range = subtree(entries, fromKey);
    for (auto it = range.first; it != range.second; ++it) {
        moved.emplace_back(toKey + it->first.substr(fromKey.size()), it->second);
    }
    entries.erase(range.first, range.second);
    // what was at the destination is replaced
    entries.erase(toKey);
    auto old = subtree(entries, toKey);
    entries.erase(old.first, old.second);
    for (auto& [key, entry] : moved) entries[key] = entry;
    // a link moved with its folder leads where it did
    if (!linkedRoots.empty()) {
        std::map<std::string, fs::path> remapped;
        for (const auto& [link, real] : linkedRoots) {
            const bool inside = link == fromKey || link.compare(0, fromKey.size() + 1, fromKey + "/") == 0;
            remapped[inside ? toKey + link.substr(fromKey.size()) : link] = real;
        }
        if (remapped != linkedRoots) linkedVersion++;
        linkedRoots = std::move(remapped);
    }
}

std::vector<std::pair<fs::path, ProjectFileIndex::Entry>> ProjectFileIndex::under(const fs::path& dir) const {
    std::vector<std::pair<fs::path, Entry>> out;
    std::string key;
    if (!keyOf(dir, key)) return out;
    auto range = subtree(entries, key);
    for (auto it = range.first; it != range.second; ++it) out.emplace_back(absolute(it->first), it->second);
    return out;
}

size_t ProjectFileIndex::directoryCount() const {
    return static_cast<size_t>(std::count_if(entries.begin(), entries.end(), [](const auto& item) { return item.second.directory; }));
}

std::vector<FileChange> ProjectFileIndex::diff(const fs::path& dir, const std::atomic<bool>* cancel) {
    std::vector<FileChange> out;
    std::string key;
    if (!keyOf(dir, key)) return out;

    // What is on disk now under `key` (and `key` itself). A partial walk knows the folders the
    // rest of the index went through: they are entered there, not again here.
    std::optional<LinkWalk> links;
    if (followLinks) {
        links.emplace(rootPath);
        for (const auto& [link, real] : linkedRoots) {
            const bool below = key.empty() || link == key || link.compare(0, key.size() + 1, key + "/") == 0;
            if (!below) links->follower.alreadyEntered(real);
        }
    }
    std::map<std::string, Entry> disk;
    if (!key.empty()) {
        Entry self;
        const std::string name = watch::pathToUtf8(dir.filename());
        if (indexes(name) && watch::statPath(dir, self)) {
            disk[key] = self;
            if (self.directory) {
                if (!links || !watch::isLink(dir)) {
                    walk(key, disk, cancel, 0, links ? &*links : nullptr);
                } else if (const fs::path real = links->follower.enter(dir); !real.empty()) {
                    links->found[key] = real;
                    walk(key, disk, cancel, 0, &*links);
                }
            }
        }
    } else {
        walk("", disk, cancel, 0, links ? &*links : nullptr);
    }
    if (cancel && cancel->load(std::memory_order_relaxed)) return out;

    // What the index has there
    std::map<std::string, Entry> known;
    if (!key.empty()) {
        auto self = entries.find(key);
        if (self != entries.end()) known[key] = self->second;
    }
    auto range = subtree(entries, key);
    known.insert(range.first, range.second);

    auto change = [this](FileChangeKind kind, const std::string& path, const Entry& entry) {
        FileChange c;
        c.kind = kind;
        c.path = absolute(path);
        c.directory = entry.directory;
        c.size = entry.size;
        c.mtimeNs = entry.mtimeNs;
        c.identity = entry.identity;
        return c;
    };

    std::vector<std::string> added, removed;
    std::vector<FileChange> modified;
    for (const auto& [path, entry] : disk) {
        auto it = known.find(path);
        if (it == known.end()) {
            added.push_back(path);
        } else if (it->second.directory != entry.directory) {
            removed.push_back(path);
            added.push_back(path);
        } else if (!entry.directory && (!it->second.sameContent(entry) || it->second.identity != entry.identity)) {
            modified.push_back(change(FileChangeKind::Modified, path, entry));
        }
    }
    for (const auto& [path, entry] : known) {
        if (!disk.count(path)) removed.push_back(path);
    }

    // A removed and an added entry with one identity are one entry renamed
    std::unordered_map<FileIdentity, std::string> removedByIdentity;
    for (const std::string& path : removed) {
        const Entry& entry = known[path];
        if (entry.identity.valid && !(disk.count(path) && known[path].directory != disk[path].directory)) {
            removedByIdentity[entry.identity] = path;
        }
    }
    std::vector<std::string> plainAdded;
    std::map<std::string, std::string> movedTo;   // old -> new
    for (const std::string& path : added) {
        const Entry& entry = disk[path];
        auto it = entry.identity.valid ? removedByIdentity.find(entry.identity) : removedByIdentity.end();
        if (it != removedByIdentity.end() && known[it->second].directory == entry.directory) {
            movedTo[it->second] = path;
            removedByIdentity.erase(it);
        } else {
            plainAdded.push_back(path);
        }
    }

    // Moves first (folders before what they held), then additions, changes and removals
    std::vector<std::pair<std::string, std::string>> moves(movedTo.begin(), movedTo.end());
    std::sort(moves.begin(), moves.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    for (const auto& [from, to] : moves) {
        FileChange c = change(FileChangeKind::Moved, to, disk[to]);
        c.oldPath = absolute(from);
        out.push_back(c);
        // moved with different bytes (a folder rename does not change its files)
        const Entry& before = known[from];
        if (!disk[to].directory && !before.sameContent(disk[to])) out.push_back(change(FileChangeKind::Modified, to, disk[to]));
    }
    for (const std::string& path : plainAdded) out.push_back(change(FileChangeKind::Added, path, disk[path]));
    for (FileChange& c : modified) out.push_back(std::move(c));
    std::vector<std::string> gone;
    for (const std::string& path : removed) {
        if (movedTo.count(path)) continue;
        gone.push_back(path);
    }
    // children before their folder
    std::sort(gone.begin(), gone.end(), [](const std::string& a, const std::string& b) { return a > b; });
    for (const std::string& path : gone) out.push_back(change(FileChangeKind::Removed, path, known[path]));

    // The index takes the disk's state
    if (!key.empty()) {
        entries.erase(key);
        auto old = subtree(entries, key);
        entries.erase(old.first, old.second);
    } else {
        entries.clear();
    }
    for (auto& [path, entry] : disk) entries[path] = entry;
    if (links) {
        // the links below `key` are what this walk found
        std::map<std::string, fs::path> linked = linkedRoots;
        if (key.empty()) {
            linked.clear();
        } else {
            linked.erase(key);
            auto below = subtree(linked, key);
            linked.erase(below.first, below.second);
        }
        linked.insert(links->found.begin(), links->found.end());
        if (linked != linkedRoots) linkedVersion++;
        linkedRoots = std::move(linked);
    }
    return out;
}

bool ProjectFileIndex::matchesDisk() const {
    std::map<std::string, Entry> disk;
    std::optional<LinkWalk> links;
    if (followLinks) links.emplace(rootPath);
    walk("", disk, nullptr, 0, links ? &*links : nullptr);
    if (disk.size() != entries.size()) return false;
    for (const auto& [path, entry] : disk) {
        auto it = entries.find(path);
        if (it == entries.end() || it->second.directory != entry.directory) return false;
        if (!entry.directory && (!it->second.sameContent(entry) || it->second.identity != entry.identity)) return false;
    }
    return true;
}
