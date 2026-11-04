// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Unity's AssetDatabase: every asset of the project (editor/asset/AssetTypes.cpp decides
// which files are assets) has a 128-bit GUID that survives renames and moves, inside the
// editor or outside it. Engine YAML assets (.prefab, .scene, .material) keep it in the file
// as a top-level `guid:` key, .anim clips and .vat Vertex Animations as the top-level "guid"
// member of their JSON (animation clips spec 9.R8, vertex animation spec 2.1); every other
// asset in a `<file>.meta` sidecar (AssetMeta).
// Model files list their sub-assets (meshes, materials, ...) with derived GUIDs. An asset
// may list former GUIDs (redirects): references to them resolve to it.
//
// The database is the index of those GUIDs, rebuilt by a scan when the project opens and
// cached in .jasperengine/assetdb/index.json (a deletable cache: deleting it changes no
// result). The contracts are in docs/specs/S04-asset-identity.md and
// editor/project/PREFABS.md §3.
//
// Links (S68). One file is one asset, whatever the paths it is reached by. A link (symbolic link,
// junction, mount point) leading into the project is another name of a file or folder the scan
// reaches anyway: nothing is tracked under it, and a lookup by it answers with the real path. A link
// leading out of the project is a folder of the project: what it holds is tracked under the link's
// name, its metas beside the files. The scan learns the links (LinkTable); a lookup translates a
// path by that table's text, so it costs no file system call and a project without links pays
// nothing. A hard link of a tracked file is compared by file identity where copies are decided,
// and is an alias of the asset tracked first, never an entry.

#pragma once

#include "asset/AssetType.h"
#include "asset/Guid.h"
#include "asset/SubAssetKeys.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <unordered_set>
#include <mutex>
#include <string>
#include <thread>
#include <memory>
#include "jobs/SerialQueue.h"
#include "jobs/JobSystem.h"
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

        struct SubAsset {
            std::string key;
            Guid guid;
            std::string name;
            SubAssetKind kind = SubAssetKind::Mesh;
        };

        // What the database knows of one asset
        struct AssetInfo {
            Guid guid;
            std::string path;               // project-relative, generic
            AssetType type = AssetType::Unknown;
            std::vector<Guid> formerGuids;
            std::vector<SubAsset> subAssets;
            uint64_t size = 0;
            bool hasContentHash = false;
            uint64_t contentHash = 0;
        };

        // How a reference resolved (S04 R10)
        enum class ResolvedBy { Live, Former, SubAsset, LegacyPath, PathFallback, Missing };
        struct Resolution {
            ResolvedBy by = ResolvedBy::Missing;
            Guid guid;                      // the live GUID (of the sub-asset for a sub-asset)
            std::string path;               // project-relative; "<container>#<key>" for a sub-asset
            AssetType type = AssetType::Unknown;
            bool found() const { return by != ResolvedBy::Missing; }
        };

        // A meta whose asset is gone (the file moved without it, or was deleted outside the editor)
        struct OrphanMeta {
            std::string metaPath;           // project-relative
            std::string assetPath;          // the asset it belonged to
            Guid guid;
        };

        // A GUID that is live on one asset and a former GUID of another (R8): the live one wins
        struct RedirectConflict {
            Guid guid;
            std::string livePath;
            std::string formerOwnerPath;
        };

        // ---- identity of files

        // The asset type of a project-relative path, Unknown when it is not a tracked asset
        AssetType typeOf(const std::filesystem::path& path) const;
        // GUID-tracked: an asset type of the table (AssetTypes)
        bool isTrackedFile(const std::filesystem::path& path) const;

        // 32 lowercase hex digits, random
        static std::string generateGuid();
        // The GUID a file without one gets on its upgrade: derived from its path, so every
        // machine upgrading the same project agrees, and a reference by path alone (written
        // before GUIDs) still finds the file after it moves. Frozen: prefabs depend on it.
        static std::string legacyGuid(const std::filesystem::path& relativePath);
        static bool isGuid(const std::string& text);

        // The file's top-level `guid:` value, empty without one (block style: the first such
        // line; a flow style document is parsed; an .anim: its "guid" member)
        static std::string readGuid(const std::filesystem::path& fullPath);
        // Writes `guid` as the file's top-level `guid:` value, the rest of the file unchanged
        // (a line replaced or inserted at the top); a flow style document is written again
        // in block style, the GUID first. An .anim: the "guid" member replaced or inserted
        // after "version"; false for a file that is not a JasperEngine clip
        static bool writeGuid(const std::filesystem::path& fullPath, const std::string& guid);
        // In-file identity with the former GUIDs (`formerGuids:` right after `guid:`); a flow
        // style document is parsed when read and written again in block style, identity first
        static bool readInFileIdentity(const std::filesystem::path& fullPath, Guid& guid, std::vector<Guid>& formerGuids);
        static bool writeInFileIdentity(const std::filesystem::path& fullPath, const Guid& guid, const std::vector<Guid>& formerGuids);

        // ---- lifecycle

        // Scans the project: every tracked file gets a GUID (legacy files are upgraded on
        // disk, metas written), a GUID found twice stays with the file that had it before
        // (the cache from the last session, else the older file) and the other file gets a
        // new one. assetsDir is relative to the project (data files are assets only there).
        void open(const std::filesystem::path& projectPath, const std::filesystem::path& assetsDir = std::filesystem::path());
        // A session that only reads the project (the CLI's check, a scene dump that saves
        // nothing; no project lock, maybe next to an open editor): the identities open() and the
        // edits would write (metas, in-file GUIDs) and the cache stay in memory. Before open().
        void setReadOnly(bool enabled) { readOnly = enabled; }
        bool isReadOnly() const { return readOnly; }
        void close();
        // The temporary siblings of atomic writes the last open() walked past (the project's
        // orphan sweep uses this walk instead of one of its own)
        std::vector<std::filesystem::path> takeTemporaryFiles();
        bool isOpen() const;
        ~AssetDatabase();

        // What changed on disk since the last look (files moved, copied, deleted or edited
        // outside the editor), applied to the database and returned
        std::vector<Change> refresh();

        // What a scan found of the project's links (see the top of this file)
        struct LinkTable;

        // The walk refresh() compares: every tracked file and meta with its time and size.
        // scanDisk() changes nothing and takes the lock only to start, so a background thread
        // runs it (the editor's poll, R23); refresh(scan) applies it, and does nothing for a
        // scan older than the database's last change (the next poll looks again).
        struct DiskScan {
            struct File {
                std::string relative;
                std::filesystem::file_time_type time{};
                uint64_t size = 0;
                bool metaPresent = false;
                std::filesystem::file_time_type metaTime{};
                uint64_t metaSize = 0;
            };
            std::vector<File> assets;
            std::vector<std::string> metas;
            std::shared_ptr<const LinkTable> links;   // none when the project has no links
            uint64_t version = 0;
            bool valid = false;
        };
        DiskScan scanDisk() const;
        std::vector<Change> refresh(const DiskScan& scan);

        // Bumped by every change; listeners (Missing Assets, the dependency index) compare it
        uint64_t getVersion() const;

        // ---- queries (string GUIDs: the prefab code's form)

        // Empty when unknown
        std::filesystem::path getPath(const std::string& guid) const;
        std::string getGuid(const std::filesystem::path& path) const;
        bool contains(const std::string& guid) const;

        std::filesystem::path getPath(const Guid& guid) const;
        Guid getAssetGuid(const std::filesystem::path& path) const;
        bool getInfo(const std::filesystem::path& path, AssetInfo& info) const;
        bool getInfo(const Guid& guid, AssetInfo& info) const;
        void forEachAsset(const std::function<void(const AssetInfo&)>& visitor) const;
        size_t assetCount() const;

        // R10: a live GUID, a former GUID, a sub-asset GUID; without a GUID the asset whose
        // legacyGuid is the hint's, else the file at the hint (preferHint: the file at the
        // hint first, for a path just assigned in the editor); with an unknown GUID and
        // allowPathFallback (non-prefab types), a file of the expected type at the hint.
        Resolution resolve(const Guid& guid, const std::string& projectRelativeHint, AssetType expected,
                           bool allowPathFallback, bool preferHint = false) const;
        // The live asset (or "<container>#<key>" sub-asset) at a path
        // A path under the assets directory (a shader fork's assets-relative spelling)
        Resolution resolveInAssetsDir(const std::string& assetsRelativePath) const;
        Resolution lookupPath(const std::string& projectRelativePath) const;

        std::vector<OrphanMeta> getOrphanMetas() const;
        std::vector<RedirectConflict> getRedirectConflicts() const;
        // Assets in a folder linked in from out of the project whose identity cannot be written there
        // (a read-only target): their GUIDs live in the database, derived from their paths so they
        // are stable, and the Missing Assets panel lists them
        std::vector<std::string> getReadOnlyLinkedAssets() const;
        // The real folders and files the project links to out of itself, with the project path each is
        // tracked under
        std::vector<std::pair<std::filesystem::path, std::string>> getLinkedOutTargets() const;

        // ---- edits

        // A former GUID added to / removed from an asset (its meta or in-file identity is
        // written): references to it then resolve to the asset (R8)
        bool addFormerGuid(const std::filesystem::path& path, const Guid& former, std::string* error = nullptr);
        bool removeFormerGuid(const std::filesystem::path& path, const Guid& former, std::string* error = nullptr);
        // Deletes an orphan meta (to the project trash when trashDir is given)
        bool removeOrphanMeta(const std::string& metaPath, const std::filesystem::path& trashDir, std::string* error = nullptr);

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

        // Where the database knows a path by: project-relative, through the links the last scan found
        std::filesystem::path toRelative(const std::filesystem::path& path) const;
        const std::filesystem::path& getProjectPath() const { return projectPath; }
        // Stops a scan's walk through links after this many entries (the bound of S68 R7; tests)
        static void setLinkedEntryLimitForTesting(size_t limit);

        // The content hashes still being computed in the background (tests and the bench wait)
        void waitForBackgroundWork();
        // Content hashes are computed in the background after a scan (on by default); off,
        // R5 matches a file moved without its meta by name and size only
        void setBackgroundHashing(bool enabled);

    private:
        // One read, edit and write; *superseded: the file changed between the read and the write
        static bool writeInFileIdentityOnce(const std::filesystem::path& fullPath, const Guid& guid, const std::vector<Guid>& formerGuids,
                                            bool* superseded);
        struct Entry {
            std::string path;                 // project-relative, as on disk
            AssetType type = AssetType::Unknown;
            Guid guid;
            std::vector<Guid> formerGuids;
            std::vector<SubAsset> subAssets;
            std::vector<size_t> subAssetIndices;    // cache only: each sub-asset's index among its kind
            std::filesystem::file_time_type writeTime{};
            uint64_t size = 0;
            std::filesystem::file_time_type metaWriteTime{};
            uint64_t metaSize = 0;
            bool hasMeta = false;
            bool hasContentHash = false;
            uint64_t contentHash = 0;
        };

        // The editor saves scenes (and their prefabs) on a background thread too
        mutable std::recursive_mutex mutex;
        std::filesystem::path projectPath;
        std::filesystem::path assetsDir;
        bool caseInsensitive = false;
        std::atomic<uint64_t> version{0};
        // the metas open()'s walk found, while it reads identities (no existence check each)
        const std::unordered_set<std::string>* walkMetas = nullptr;
        // the atomic-write temporaries the last open() walked past (takeTemporaryFiles)
        std::vector<std::filesystem::path> temporaryFiles;

        std::map<std::string, Entry> byPath;                 // key(relative path) -> entry
        std::unordered_map<Guid, std::string> byGuid;        // live guid -> key
        // live guid -> its entry (a std::map node: stable), kept with byGuid: resolve() reads
        // it without a path lookup (R24)
        std::unordered_map<Guid, const Entry*> liveEntries;
        std::unordered_map<Guid, std::string> byFormer;      // former guid -> key
        std::unordered_map<Guid, std::pair<std::string, std::string>> bySubAsset; // sub-asset guid -> key, sub key
        std::map<std::string, OrphanMeta> orphans;           // key(meta path) -> orphan
        std::map<std::string, Entry> orphanLast;             // key(asset path) -> what the cache knew of an orphan's asset
        std::vector<RedirectConflict> conflicts;
        std::shared_ptr<const LinkTable> links;              // read lock-free (std::atomic_load): the background walk uses it too
        std::set<std::string> warnedLinks;                   // each link left out, warned about once
        std::unordered_map<std::string, std::string> hardLinks;   // key(a hard link's path) -> key of the tracked path of that file
        std::set<std::string> readOnlyLinked;                // key -> assets getReadOnlyLinkedAssets lists
        std::set<std::string> warnedReadOnly;                // the linked folders reported in the log

        // background content hashing
        // background content hashes and cache writes: one S06 strand (Background lane, FIFO,
        // one job at a time on the pool's workers); a save asked for runs after the hashes
        // queued before it
        std::unique_ptr<jobs::SerialQueue> hashStrand;
        std::mutex strandMutex;                  // creating / dropping the strand
        std::atomic<bool> hashStop{false};
        std::atomic<bool> hashingEnabled{true};
        bool readOnly = false;
        std::atomic<bool> savePending{false};
        std::atomic<bool> cacheDirty{false};
        jobs::SerialQueue& strand();
        void hashOne(const std::string& relative);
        void saveCacheInBackground();

        std::filesystem::path cacheDir() const;
        std::filesystem::path cacheFile() const;
        void loadCache(std::map<std::string, Entry>& cached) const;
        void saveCache() const;
        // what saveCache() writes, copied under the lock; written without it (the background thread)
        struct CacheSnapshot {
            std::vector<Entry> entries;
            std::vector<Entry> orphanEntries;
            bool caseInsensitive = false;
            std::filesystem::path file;
            std::filesystem::path legacyFile;
        };
        CacheSnapshot snapshotCache() const;
        static void writeCache(const CacheSnapshot& snapshot);
        // saveCache() off the caller's thread when background work runs (the cache is disposable:
        // a crash before the write only costs a rebuild), at once otherwise
        void requestCacheSave();

        std::string key(const std::filesystem::path& path) const;
        std::string relativeKeyOf(const std::string& relative) const;
        bool fileExists(const std::string& relative) const;
        void statFile(const std::string& relative, std::filesystem::file_time_type& time, uint64_t& size) const;
        // The entry a path names: tracked at it, through a link, or a hard link's
        const Entry* findEntry(const std::filesystem::path& path) const;
        std::shared_ptr<const LinkTable> linkTable() const;
        void installLinks(std::shared_ptr<const LinkTable> table);
        // `oldRelative` (and what is under it) is now `newRelative` in the table
        void renameInLinks(const std::string& oldRelative, const std::string& newRelative);
        // `relative` is a name of the table of its own: the entry of a link, as opposed to what it leads to
        bool isLinkAlias(const std::string& relative) const;
        void forgetLink(const std::string& relative);
        // Under a link leading out of the project
        bool isLinkedOut(const std::string& relative) const;
        // Both paths open one file (std::filesystem::equivalent: device and inode, or volume and file index)
        bool sameFile(const std::string& a, const std::string& b) const;
        // The tracked path of the file at `relative` when it is another name of it: the path `guid`
        // has, else (a file without identity, only when it has several hard links) any tracked
        // path of one. "" when it is a file of its own.
        std::string hardLinkOwner(const std::string& relative, const Guid& guid) const;
        // `relative` is an alias of the asset at `owner`: no entry, nothing written
        void addHardLink(const std::string& relative, const std::string& owner);
        // After writeIdentity of a sidecar's meta: `written` false under a link out is a read-only
        // target (reported, and not tried again by every refresh)
        void noteIdentityWrite(Entry& entry, bool written);

        void insert(Entry entry);
        void erase(const std::string& relative);
        void reindexRedirects();
        // The same for the one entry just inserted at `relative` (refresh's loops, which rebuild
        // the whole index once at the end: a checkout of 10,000 files is not 10,000 rebuilds)
        void indexRedirectsOf(const std::string& relative);
        bool guidInUse(const Guid& guid) const;
        Guid freshGuid(const std::string& preferred = std::string()) const;

        // Reads what identity a file has on disk (in-file key, or its meta); false when none
        bool readIdentity(const std::string& relative, AssetType type, Entry& entry, const Entry* cached) const;
        // Writes an entry's identity (in-file line or meta, keeping the meta's other sections)
        bool writeIdentity(const Entry& entry) const;
        // The file's stamp after writeIdentity (`written`: it returned true). An in-file
        // identity is read back: when the file no longer holds it (another program saved it
        // right after the write), the stamp is left unknown so the next refresh reads it again
        // instead of taking that save as identified.
        void stampAfterIdentityWrite(Entry& entry, bool written) const;
        // Sub-assets of a model file matched against the ones it had (§6.4)
        bool refreshSubAssets(Entry& entry, const Entry* previous) const;

        // Tracked files and metas below a path (the path itself when it is a file), relative
        // temporaries, when given, receives the atomic-write temps the walk passes (io::isTemporarySiblingName)
        // table receives the links the walk meets
        void collect(const std::filesystem::path& fullPath, std::vector<std::string>& assets, std::vector<std::string>& metas,
                     LinkTable& table, std::vector<std::filesystem::path>* temporaries = nullptr) const;
        // The links `collect` met (project-relative): each settled in the table, in path order, and
        // the folders leading out of the project walked
        void collectLinks(std::set<std::string>& pending, std::vector<std::string>& assets, std::vector<std::string>& metas,
                          LinkTable& table) const;
        // Files without identity adopt orphan metas that match them (R5); returns the adopted
        std::vector<Change> adoptOrphans(std::vector<std::string>& withoutIdentity);
        // presentKeys: the keys of the assets the walk found (a meta beside one is no orphan, no stat)
        void collectOrphans(const std::vector<std::string>& metas, const std::map<std::string, Entry>& cached,
                            const std::unordered_set<std::string>* presentKeys = nullptr);
        // A new identity for a file that has none (R3), written to disk
        Entry identify(const std::string& relative, AssetType type, const std::string& preferredGuid);

        void queueHash(const std::string& relative);
        void stopHashing();
    };

}
