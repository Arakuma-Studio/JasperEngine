// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JASPERENGINE_PROJECTFILEINDEX_H
#define JASPERENGINE_PROJECTFILEINDEX_H

#include "FileWatcher.h"
#include "LinkedFolders.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine {

    // What is under a watched root: relative path -> {size, mtime, identity, directory}, for
    // the files and folders the watch exclusions keep (S05 R5). Safe-save temporaries are not
    // indexed. The file watch service keeps one (as last delivered) and the polling backend
    // its own (as last seen). In memory only (R35).
    //
    // Not thread-safe: the owner serializes access.
    class JASPERENGINE_API ProjectFileIndex {
    public:
        using Entry = watch::PathStat;

        // Empties the index and sets its root (absolute)
        void reset(const std::filesystem::path& root);
        // Walks go on through folders linked in from out of the root (LinkFollower): what they hold is
        // indexed under the link's name. The file watch service's index does; a polling backend's
        // does not, as a linked folder gets a backend of its own.
        void setFollowLinks(bool follow) { followLinks = follow; }
        const std::filesystem::path& root() const { return rootPath; }

        // Walks the whole root (`cancel` stops it half-way)
        void build(const std::atomic<bool>* cancel = nullptr);

        bool find(const std::filesystem::path& path, Entry& out) const;
        bool contains(const std::filesystem::path& path) const;
        void set(const std::filesystem::path& path, const Entry& entry);
        // `path` and, for a folder, everything under it
        void erase(const std::filesystem::path& path);
        // `from` (and what is under it) is now `to`
        void move(const std::filesystem::path& from, const std::filesystem::path& to);
        // The entries strictly under `dir`, as absolute paths, parents before children
        std::vector<std::pair<std::filesystem::path, Entry>> under(const std::filesystem::path& dir) const;

        // Stat-diff of `dir` (and what is under it) against the index: Added, Modified,
        // Removed, and a Removed + Added of one identity as one Moved (a folder's Moved comes
        // with one Moved per entry under it). The index takes what is on disk.
        std::vector<FileChange> diff(const std::filesystem::path& dir, const std::atomic<bool>* cancel = nullptr);

        // Whether the index holds exactly what is on disk now (tests)
        bool matchesDisk() const;

        // The links walks went through: key of the link -> real path of its folder. The version
        // changes with the map.
        const std::map<std::string, std::filesystem::path>& linkedFolders() const { return linkedRoots; }
        uint64_t linkedFoldersVersion() const { return linkedVersion; }

        size_t size() const { return entries.size(); }
        size_t directoryCount() const;

        // Whether the index keeps this name (exclusions and temporaries)
        static bool indexes(const std::string& name);
        // The absolute path of an index key, and the key of an absolute path (false: not under the root)
        std::filesystem::path absolute(const std::string& key) const;
        bool keyOf(const std::filesystem::path& path, std::string& key) const;

        // Raw access for the polling backend's tiers
        const std::map<std::string, Entry>& all() const { return entries; }

    private:
        std::filesystem::path rootPath;
        std::map<std::string, Entry> entries;   // generic UTF-8 relative path -> entry ("" = never)
        bool followLinks = false;
        std::map<std::string, std::filesystem::path> linkedRoots;
        uint64_t linkedVersion = 0;

        // What one walk finds of the links (followLinks): each folder is entered once per walk
        struct LinkWalk {
            explicit LinkWalk(const std::filesystem::path& root) : follower(root) {}
            watch::LinkFollower follower;
            std::map<std::string, std::filesystem::path> found;
        };

        // Every indexable entry under `key` (a folder, "" = the root) on disk now
        void walk(const std::string& key, std::map<std::string, Entry>& out, const std::atomic<bool>* cancel, int depth, LinkWalk* links) const;
        // `key` and what is under it no longer are links the index went through
        void dropLinkedFolders(const std::string& key);
    };

}

#endif // JASPERENGINE_PROJECTFILEINDEX_H
