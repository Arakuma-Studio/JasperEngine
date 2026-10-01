// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Unity's AssetDatabase, for the asset types referenced by GUID: a 128-bit id written in
// the asset file itself (a top-level `guid:` key), mapped to the file's project-relative
// path. References store the GUID and the path as a hint, so a file renamed or moved,
// inside the editor or outside it, is still found. Only .prefab files are tracked now;
// isTrackedFile is the one place another type (scenes, materials) would be added.
// The contract is in editor/project/PREFABS.md.

#pragma once

#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace jasperengine::editor {

    class AssetDatabase {
    public:
        struct Change {
            // Modified: the file's content changed outside the editor, same GUID
            enum class Type { Added, Removed, Moved, Modified };
            Type type = Type::Added;
            std::string guid;
            std::filesystem::path path;     // project-relative; for Removed the last one it had
            std::filesystem::path oldPath;  // Moved only
        };

        // GUID-tracked asset types
        static bool isTrackedFile(const std::filesystem::path& path);

        // 32 lowercase hex digits, random
        static std::string generateGuid();
        // The GUID a file without one gets on its upgrade: derived from its path, so every
        // machine upgrading the same project agrees, and a reference by path alone (written
        // before GUIDs) still finds the file after it moves
        static std::string legacyGuid(const std::filesystem::path& relativePath);
        static bool isGuid(const std::string& text);

        // The file's top-level `guid:` value, empty without one
        static std::string readGuid(const std::filesystem::path& fullPath);
        // Writes `guid` as the file's top-level `guid:` value, the rest of the file unchanged
        // (a line replaced or inserted at the top)
        static bool writeGuid(const std::filesystem::path& fullPath, const std::string& guid);

        // Scans the project: every tracked file gets a GUID (legacy files are upgraded on
        // disk), a GUID found twice stays with the file that had it before (the cache from the
        // last session, else the older file) and the other file gets a new one
        void open(const std::filesystem::path& projectPath);
        void close();
        bool isOpen() const;

        // What changed on disk since the last look (files moved, copied, deleted or edited
        // outside the editor), applied to the database and returned
        std::vector<Change> refresh();

        // Empty when unknown
        std::filesystem::path getPath(const std::string& guid) const;
        std::string getGuid(const std::filesystem::path& path) const;
        bool contains(const std::string& guid) const;

        // Editor file operations, each on a file or a directory, after they happened on disk.
        // moveAssets: the files keep their GUIDs (a file whose new name is no longer a tracked
        // type is removed, one that became one is imported).
        std::vector<Change> moveAssets(const std::filesystem::path& oldPath, const std::filesystem::path& newPath);
        std::vector<Change> removeAssets(const std::filesystem::path& path);
        // A file or directory that appeared: deleted ones put back keep their GUIDs; with
        // `copy`, a file whose GUID another file has gets a new one, and copyGuids (by
        // relative path) records what each copy got, so doing the copy again (a redo) gives
        // the same GUIDs
        std::vector<Change> importAssets(const std::filesystem::path& path, bool copy = false,
                                         std::map<std::string, std::string>* copyGuids = nullptr);
        // A file the editor wrote with this GUID
        void registerAsset(const std::filesystem::path& path, const std::string& guid);

        std::filesystem::path toRelative(const std::filesystem::path& path) const;

    private:
        struct Entry {
            std::string guid;
            std::filesystem::file_time_type writeTime{};
            std::uintmax_t size = 0;
        };

        // The editor saves scenes (and their prefabs) on a background thread too
        mutable std::recursive_mutex mutex;
        std::filesystem::path projectPath;
        std::map<std::string, Entry> byPath;               // generic relative path -> entry
        std::unordered_map<std::string, std::string> byGuid; // guid -> generic relative path

        std::filesystem::path cacheFile() const;
        void loadCache(std::unordered_map<std::string, std::string>& owners) const;
        void saveCache() const;

        std::string key(const std::filesystem::path& path) const;
        bool fileExists(const std::string& relative) const;
        void stat(const std::string& relative, Entry& entry) const;
        void insert(const std::string& relative, const std::string& guid);
        void erase(const std::string& relative);
        // A GUID no other registered file has, written into the file
        std::string assignNewGuid(const std::string& relative, const std::string& preferred = std::string());
        // Tracked files below a path (the path itself when it is a file), generic relative
        std::vector<std::string> collectFiles(const std::filesystem::path& fullPath) const;
    };

}
