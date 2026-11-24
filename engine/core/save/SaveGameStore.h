// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SAVE_SAVEGAMESTORE_H
#define SAVE_SAVEGAMESTORE_H

// The SaveGame store (docs/saved-data.md, docs/specs/savegame.md): named slots of namespaces,
// each namespace one JLV1 payload. A slot is kept in memory as encoded sections once read or
// written; a write swaps one section and hands an immutable snapshot to a serial job queue, which
// writes it as a new generation file `<slot>.g<N>.jsave` (writeFileAtomic, Document) and prunes
// the generations beyond Backups Kept + 1. Per slot at most one write is in flight and one waits
// (a later write replaces the waiting snapshot). A read uses the newest generation that parses,
// checks and decodes; an invalid newer one is renamed `.corrupt`. A file that can't be read now
// (not damage) fails the read and is left alone. A write at a newer data version migrates the
// slot's other namespaces first (setMigrator). Opening a folder deletes the temporary files a
// killed write left.
//
// Main thread only, but the jobs. Nothing exists before the first instance() call: the engine's
// pause, shutdown and Play-stop hooks test existing().

#include "Export.h"
#include "save/SaveFormat.h"
#include "save/SaveValue.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace jasperengine::jobs { class SerialQueue; }

namespace jasperengine::savegame {

    // Project Settings > Player > Saved Data (editor SaveGameSettings, game.json saveGame)
    struct SaveSettings {
        int backups = 2;        // Backups Kept, 0..9
        int maxSlotMiB = 8;     // Max Slot Size (MiB), 1..64
        bool operator==(const SaveSettings& other) const { return backups == other.backups && maxSlotMiB == other.maxSlotMiB; }
        bool operator!=(const SaveSettings& other) const { return !(*this == other); }
    };
    // Applies at the next write; values are clamped to their ranges. Creates no store
    JASPERENGINE_API void configure(const SaveSettings& settings);
    JASPERENGINE_API SaveSettings settings();

    // The game's SaveGame.version (the Lua binding sets it; 1 by default): reads of a slot above
    // it report newer_version
    JASPERENGINE_API void setGameDataVersion(uint32_t version);
    JASPERENGINE_API uint32_t gameDataVersion();

    // The engine's pause, shutdown and Play-stop hooks: flushes every slot (2 s limit) when a
    // store exists, logging what was abandoned ("before the app paused"); one pointer test otherwise
    JASPERENGINE_API void flushForLifecycle(const char* when);

    struct SlotInfo {
        std::string slot;
        uint32_t version = 0;
        double savedAt = 0;         // UTC seconds
        uint64_t size = 0;          // bytes of the current generation
        uint64_t generation = 0;    // the current generation
        int generations = 0;        // generation files on disk
        std::vector<std::string> namespaces;
        std::string meta;           // payloads, empty when absent
        std::string requirements;
        // ok, recovered, corrupt, newer_version, newer_format
        std::string status = "ok";
        bool pending = false;       // a write in flight or waiting
        bool writtenThisSession = false;
    };

    struct ReadInfo {
        uint64_t generation = 0;
        uint64_t recoveredFrom = 0;     // the generation used when newer ones were invalid
        uint32_t version = 0;
        int64_t savedAtMs = 0;
        std::vector<std::string> problems;
        std::vector<std::string> namespaces;
        std::string meta;
        std::string requirements;
    };

    // Every namespace of a slot, as stored or migrated
    struct SlotData {
        ReadInfo info;
        std::vector<SaveSection> sections;
    };

    struct BackupInfo {
        uint64_t generation = 0;
        double savedAt = 0;
        uint32_t version = 0;
        uint64_t size = 0;
        bool valid = false;
        std::string problem;
        std::string file;           // file name
    };

    struct WriteOptions {
        uint32_t version = 1;       // the data version stamped
        bool force = false;         // write over a newer_version / newer_format slot
        bool replaceAll = false;    // drop every other namespace
        bool hasMeta = false;       // meta = nil keeps the previous metadata
        std::string meta;
        bool hasRequirements = false;
        std::string requirements;
    };

    struct StorageInfo {
        int64_t used = -1;
        int64_t quota = -1;
        int64_t free = -1;
    };

    enum class CorruptMode { Truncate, Flip, Header };

    using WriteCallback = std::function<void(const SaveError&)>;

    // The web's IndexedDB syncs (FS.syncfs) and the writes waiting for one: a landed write is
    // confirmed by the result of a sync that started after it landed (each sync takes the files
    // as they are when it starts), so a later sync's QuotaExceededError reaches its own writes
    struct JASPERENGINE_API SyncWaiters {
        struct Delivery {
            std::vector<WriteCallback> callbacks;
            SaveError error;
        };
        // A write reached the in-memory file system with `result` (ok)
        void landed(std::vector<WriteCallback> callbacks, const SaveError& result);
        // A sync started: it covers every write landed until now
        void started(uint64_t sync);
        // That sync ended: the callbacks it covered, with its result
        std::vector<Delivery> finished(uint64_t sync, bool ok, std::string_view errorName, std::string_view message);
        void clear();
        size_t waiting() const;

    private:
        std::vector<Delivery> unsynced;
        std::map<uint64_t, std::vector<Delivery>> syncing;
    };

    // Runs the game's migrations on a slot's namespaces (data.info.version) up to `version`,
    // leaving the migrated sections in data.sections (the Lua binding registers SaveGame.addMigration's)
    using Migrator = std::function<SaveError(const std::string& slot, SlotData& data, uint32_t version)>;
    JASPERENGINE_API void setMigrator(Migrator migrator);

    // Test seam: called before the store reads ("read") or deletes ("remove") a file of the saves
    // folder; a non-empty error code makes that step fail with it. Empty function: no-op
    using FileFaultInjector = std::function<std::error_code(std::string_view step, const std::filesystem::path& path)>;
    JASPERENGINE_API void setFileFaultInjector(FileFaultInjector injector);

    class JASPERENGINE_API SaveGameStore {
    public:
        static constexpr size_t MEMORY_LIMIT = 32u * 1024u * 1024u;

        // Created on first use
        static SaveGameStore& instance();
        // Null until then
        static SaveGameStore* existing();

        // Where the slots live: data://saves (the web: data://<storageId>/saves) unless set (tests)
        void setFolder(const std::filesystem::path& folder);
        std::filesystem::path folder();

        // Encoded payload of `nameSpace` ("" = main). Fails at once for name, too_large,
        // newer_version and newer_format (the slot's newest data); otherwise the write is queued
        // and `onComplete` runs on the main thread in a later frame
        SaveError write(std::string_view slot, std::string_view nameSpace, std::string payload,
                        const WriteOptions& options, WriteCallback onComplete);
        // One namespace (read-your-writes: pending writes included)
        SaveError read(std::string_view slot, std::string_view nameSpace, std::string& payload, ReadInfo& info);
        // Every namespace; generation 0: the slot's current data, else that generation's file
        SaveError readSlot(std::string_view slot, SlotData& out, uint64_t generation = 0);
        // Read only (the Saved Data window's view): the slot's image when loaded, else its newest
        // valid generation on disk with the skipped ones in info.problems; nothing is installed,
        // set aside or deleted
        SaveError peekSlot(std::string_view slot, SlotData& out);
        // The file read and checksum on the queue, `done` on the main thread
        void readSlotAsync(std::string_view slot, std::function<void(const SaveError&, const SlotData&)> done);
        // The image takes migrated sections at `version`; the files change at the next write (R9)
        SaveError markMigrated(std::string_view slot, uint32_t version, std::vector<SaveSection> sections);

        // Every slot, newest first; headers only for slots not in memory
        std::vector<SlotInfo> list();
        SaveError info(std::string_view slot, SlotInfo& out);
        bool exists(std::string_view slot);
        // Every generation and set-aside file of the slot, after its queued writes landed (`limit`)
        SaveError remove(std::string_view slot, std::chrono::milliseconds limit = std::chrono::seconds(2));
        SaveError backups(std::string_view slot, std::vector<BackupInfo>& out);
        // That generation's sections, metadata and version written as a new newest generation
        SaveError restore(std::string_view slot, uint64_t generation, WriteCallback onComplete);
        // Damages the newest generation file (testing recovery); `file` gets its name
        SaveError corrupt(std::string_view slot, CorruptMode mode, std::string& file);

        // Blocks until the queued writes (of `slot`, or all) landed or `limit` passed; what is left
        // then is logged ("was not written before <when>") and reported
        SaveError flush(std::string_view slot = {}, std::chrono::milliseconds limit = std::chrono::seconds(2),
                        const char* when = nullptr);
        // Drops images without pending writes (they reload on the next read)
        void unload(std::string_view slot = {});
        // Flushes, then forgets everything (project switch, tests); the folder is resolved again
        void reset();
        // The editor's Play stopped: no slot counts as written this session any more
        void clearSessionMarks();

        // The files of a slot (generations and set-aside), for the editor's undo
        std::vector<std::filesystem::path> slotFiles(std::string_view slot);
        // The highest generation number on disk, 0 when none
        uint64_t newestGeneration(std::string_view slot);
        // The generation the slot's queued writes will publish, 0 when none is queued
        uint64_t pendingGeneration(std::string_view slot);
        // Changes when a file of the slot does (names, sizes, times; no file is read)
        uint64_t slotSignature(std::string_view slot);
        // Bumped by every change of what list() shows (the Saved Data window refreshes on it)
        uint64_t changeCount() const { return changes.load(); }

        StorageInfo storage();
        // The web layer: navigator.storage.estimate()
        void setStorageEstimate(int64_t used, int64_t quota);
        // The web layer: an IndexedDB sync started, and that sync ended
        void fileSystemSyncStarted(uint64_t sync);
        void fileSystemSynced(uint64_t sync, bool ok, std::string_view errorName, std::string_view message);

        // Tests: while held, queued writes wait before they start (flush still writes them)
        void setHold(bool held);
        // Generation files written by this store
        size_t fileWrites() const { return writes.load(); }
        // Whole save files read (any store; header reads are not counted)
        static size_t fileReads();
        // Bytes of the images in memory
        size_t memoryBytes() const;

        struct Impl;

    private:
        SaveGameStore();
        ~SaveGameStore();

        std::unique_ptr<Impl> impl;
        std::atomic<uint64_t> changes{0};
        std::atomic<size_t> writes{0};
    };

    JASPERENGINE_API std::string slotFileName(std::string_view slot, uint64_t generation);
    // "slot1.g12.jsave" -> slot1, 12; ".corrupt" names too (corrupt set)
    JASPERENGINE_API bool parseSlotFileName(std::string_view name, std::string& slot, uint64_t& generation, bool& corrupt);

}

#endif // SAVE_SAVEGAMESTORE_H
