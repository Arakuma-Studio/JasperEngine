// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "save/SaveGameStore.h"

#include "Engine.h"
#include "Log.h"
#include "PlatformConfig.h"
#include "System.h"
#include "io/AtomicFile.h"
#include "jobs/JobSystem.h"
#include "jobs/SerialQueue.h"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <set>

#ifndef JASPERENGINE_VERSION_STRING
#define JASPERENGINE_VERSION_STRING ""
#endif

#if defined(__EMSCRIPTEN__)
// engine/platform/emscripten/JasperEngineWeb.cpp
extern "C" {
    int jasper_web_idb_enabled();
    void jasper_web_storage_persist();
    void jasper_web_storage_estimate();
}
#endif

namespace fs = std::filesystem;
using namespace jasperengine;

namespace jasperengine::savegame {

namespace {

    // configure() and the game's data version: plain values, so nothing is allocated before a
    // store exists (R30)
    std::atomic<int> configuredBackups{2};
    std::atomic<int> configuredMaxSlotMiB{8};
    std::atomic<uint32_t> dataVersion{1};

    SaveGameStore* storeInstance = nullptr;

    int64_t nowMs(){
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    const char* platformName(){
        switch (Engine::getPlatform()){
            case Platform::MacOS: return "macos";
            case Platform::iOS: return "ios";
            case Platform::Web: return "web";
            case Platform::Android: return "android";
            case Platform::Linux: return "linux";
            case Platform::Windows: return "windows";
        }
        return "";
    }

    std::string mib(uint64_t bytes){
        char text[32];
        std::snprintf(text, sizeof(text), "%.1f MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
        return text;
    }

    std::string saveStorePathText(const fs::path& path){
        return path.generic_string();
    }

    std::atomic<size_t> fullReads{0};

    // setFileFaultInjector's (tests); read on the store's jobs too
    std::mutex& saveFaultMutex(){
        static std::mutex mutex;
        return mutex;
    }
    FileFaultInjector& saveFaultInjector(){
        static FileFaultInjector injector;
        return injector;
    }
    std::error_code injectedSaveFault(std::string_view step, const fs::path& path){
        std::lock_guard<std::mutex> lock(saveFaultMutex());
        return saveFaultInjector() ? saveFaultInjector()(step, path) : std::error_code();
    }

    Migrator& registeredMigrator(){
        static Migrator migrator;
        return migrator;
    }

    // A file of the saves folder deleted (false with `ec` when it is still there)
    bool removeSaveFile(const fs::path& path, std::error_code& ec){
        ec = injectedSaveFault("remove", path);
        if (ec) return false;
        return fs::remove(path, ec) || !ec;
    }

    // What reading a whole file gave: its bytes, nothing (it went since the folder was listed: a
    // prune or a delete), an I/O failure (a sharing violation, permissions: not damage, the file is
    // left alone), or a size above the cap (damage)
    enum class ReadOutcome { Ok, Missing, Unreadable, TooLarge };

    // Whole file, refusing above `cap` before allocating
    ReadOutcome readSaveFile(const fs::path& path, size_t cap, std::string& out, std::string& problem){
        if (const std::error_code fault = injectedSaveFault("read", path)){
            problem = "cannot read (" + fault.message() + ")";
            return ReadOutcome::Unreadable;
        }
        std::error_code ec;
        const uintmax_t size = fs::file_size(path, ec);
        if (ec){
            if (ec == std::errc::no_such_file_or_directory) return ReadOutcome::Missing;
            problem = "cannot read (" + ec.message() + ")";
            return ReadOutcome::Unreadable;
        }
        if (size > cap){ problem = "file of " + std::to_string(size) + " bytes is above the 64 MiB cap"; return ReadOutcome::TooLarge; }
        {
            std::ifstream in(path, std::ios::binary);
            if (!in){
                std::error_code existsError;
                if (!fs::exists(path, existsError) && !existsError) return ReadOutcome::Missing;
                problem = "cannot open";
                return ReadOutcome::Unreadable;
            }
            out.resize(static_cast<size_t>(size));
            if (size > 0 && !in.read(out.data(), static_cast<std::streamsize>(size))){ problem = "cannot read"; return ReadOutcome::Unreadable; }
        }
        // counted once the file is closed: a test may delete it as soon as it sees the count
        fullReads.fetch_add(1);
        return ReadOutcome::Ok;
    }

    bool readFile(const fs::path& path, size_t cap, std::string& out, std::string& problem){
        return readSaveFile(path, cap, out, problem) == ReadOutcome::Ok;
    }

    bool readPrefix(const fs::path& path, size_t count, std::string& out){
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        out.resize(count);
        in.read(out.data(), static_cast<std::streamsize>(count));
        out.resize(static_cast<size_t>(in.gcount()));
        return true;
    }

    std::string sectionNames(const std::vector<SaveSection>& sections){
        std::string out;
        for (const SaveSection& section : sections) out += (out.empty() ? "" : ", ") + section.name;
        return out.empty() ? "none" : out;
    }

    // The files of one slot in a folder listing
    struct SlotFiles {
        std::map<uint64_t, fs::path> generations;              // ascending
        std::map<uint64_t, fs::path> corrupt;
        std::map<uint64_t, uint64_t> sizes;
    };

    std::map<std::string, SlotFiles> scanFolder(const fs::path& folder, std::string_view only = {}){
        std::map<std::string, SlotFiles> out;
        std::error_code ec;
        for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)){
            std::string slot;
            uint64_t generation = 0;
            bool corrupt = false;
            const std::string name = it->path().filename().string();
            if (!parseSlotFileName(name, slot, generation, corrupt)) continue;
            if (!only.empty() && slot != only) continue;
            SlotFiles& files = out[slot];
            if (corrupt) files.corrupt[generation] = it->path();
            else {
                files.generations[generation] = it->path();
                std::error_code sizeError;
                files.sizes[generation] = it->file_size(sizeError);
            }
        }
        return out;
    }

}

}  // namespace jasperengine::savegame

namespace jasperengine::savegame {

    struct WriteJob {
        std::string slot;
        SaveFile file;
        fs::path folder;
        int backups = 2;
        uint64_t epoch = 0;

        // 0 queued, 1 running, 2 done: the worker or a flush claims it
        std::atomic<int> state{0};
        std::mutex mutex;
        std::condition_variable done;
        SaveError result;
        uint64_t size = 0;

        // main thread only
        std::vector<WriteCallback> callbacks;
        bool landed = false;

        bool claim(){
            int expected = 0;
            return state.compare_exchange_strong(expected, 1);
        }
    };

    struct Slot {
        bool loaded = false;
        uint32_t version = 1;
        uint64_t generation = 0;        // of the image's data on disk (0: not written yet)
        uint64_t highest = 0;           // the highest generation seen or given out
        int64_t savedAtMs = 0;
        std::string meta;
        std::string requirements;
        std::vector<SaveSection> sections;     // by name
        uint64_t recoveredFrom = 0;
        std::vector<std::string> problems;
        std::string status = "ok";
        SaveErrorCode blocked = SaveErrorCode::None;   // NewerFormat: the files are of a newer engine
        std::string blockedMessage;
        bool warned = false;
        bool written = false;
        uint64_t lastUse = 0;
        uint64_t currentSize = 0;
        std::shared_ptr<WriteJob> inFlight;
        std::shared_ptr<WriteJob> waiting;

        bool pending() const { return inFlight || waiting; }
        size_t bytes() const {
            size_t total = meta.size() + requirements.size();
            for (const SaveSection& section : sections) total += section.name.size() + (section.payload ? section.payload->size() : 0);
            return total;
        }
    };

    struct SaveGameStore::Impl {
        SaveGameStore* owner = nullptr;
        fs::path folder;
        bool folderSet = false;
        std::map<std::string, Slot, std::less<>> slots;
        // per slot name: bumped by every change of its data or files from this store (an async
        // read installs what it read only when nothing changed meanwhile)
        std::map<std::string, uint64_t, std::less<>> edits;
        fs::path swept;                 // the folder whose leftover temporary files were deleted
        std::unique_ptr<jobs::SerialQueue> queue;
        jobs::JobSystem* queueSystem = nullptr;     // the job system the queue was made on
        uint64_t epoch = 1;
        uint64_t useCounter = 0;

        std::mutex holdMutex;
        std::condition_variable holdChanged;
        bool held = false;

        // the web: callbacks of landed writes waiting for the IndexedDB sync
        SyncWaiters syncWaiters;
        bool persistRequested = false;
        StorageInfo webStorage;

        const fs::path& resolveFolder(){
            if (!folderSet){
                std::string base = System::hasInstance() ? System::instance().getUserDataPath() : std::string(".");
#if defined(__EMSCRIPTEN__)
                // games of one origin share the IndexedDB mount: a folder per storageId (D9)
                base += "/" + platformConfig().storageId;
#endif
                folder = fs::path(base) / "saves";
                folderSet = true;
            }
            // the first use of a folder: no write of this store is on its way yet
            if (swept != folder){
                swept = folder;
                sweepTemporaries(folder, {});
            }
            return folder;
        }

        void edited(const std::string& name){ edits[name]++; }

        // Temporary files of slot generations that a killed write left (a crash between the
        // temporary and its rename): the generation was never published, so they go. Those of
        // another running game stay. Called when no write of `only` (or of any slot) is on its way
        void sweepTemporaries(const fs::path& dir, std::string_view only){
            std::vector<fs::path> leftovers;
            std::error_code ec;
            for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)){
                const std::string name = it->path().filename().string();
                if (!io::isTemporarySiblingName(name)) continue;
                std::string slot;
                uint64_t generation = 0;
                bool corrupt = false;
                if (!parseSlotFileName(io::temporarySiblingTarget(it->path()).filename().string(), slot, generation, corrupt)) continue;
                if (!only.empty() && slot != only) continue;
                const int64_t writer = io::temporarySiblingWriter(name);
                if (writer != io::currentProcessId() && io::isProcessRunning(writer)) continue;
                leftovers.push_back(it->path());
            }
            for (const fs::path& path : leftovers){
                std::error_code removeError;
                if (!removeSaveFile(path, removeError))
                    Log::warn("SaveGame: could not delete the leftover %s: %s", saveStorePathText(path).c_str(), removeError.message().c_str());
            }
        }

        // The slot's namespaces brought to `version` before a write at that version keeps them
        // (R9: a slot never mixes data of two versions). The game's migrations run through the
        // registered migrator; without one the write is refused
        SaveError migrateOthers(const std::string& name, Slot& slot, uint32_t version, const std::string& others){
            const Migrator& migrator = registeredMigrator();
            if (!migrator){
                return makeError(SaveErrorCode::Migration, "slot \"" + name + "\" is version " + std::to_string(slot.version) +
                    " and this write is version " + std::to_string(version) + ": its other namespaces (" + others +
                    ") would be labelled version " + std::to_string(version) + " without their migrations. Read the slot in the "
                    "game first (SaveGame.read runs SaveGame.addMigration's functions), or write every namespace (replaceAll).");
            }
            SlotData data;
            data.info = infoOf(slot);
            data.sections = slot.sections;
            const SaveError error = migrator(name, data, version);
            if (!error.ok()) return error;
            // the Lua binding hands its result to markMigrated itself
            if (slot.version != version){
                std::sort(data.sections.begin(), data.sections.end(), [](const SaveSection& a, const SaveSection& b){ return a.name < b.name; });
                slot.sections = std::move(data.sections);
                slot.version = version;
                edited(name);
                changed();
            }
            return {};
        }

        // The SlotData of one parsed generation file
        static void fillSlotData(const SaveFile& file, SlotData& out){
            SaveMetadata metadata;
            splitMetadata(file.metadata, metadata);
            out.info = ReadInfo();
            out.info.generation = file.generation;
            out.info.version = file.dataVersion;
            out.info.savedAtMs = file.savedAtMs;
            out.info.meta = metadata.meta;
            out.info.requirements = metadata.requirements;
            out.sections = file.sections;
            std::sort(out.sections.begin(), out.sections.end(), [](const SaveSection& a, const SaveSection& b){ return a.name < b.name; });
            for (const SaveSection& section : out.sections) out.info.namespaces.push_back(section.name);
        }

        void changed(){ owner->changes.fetch_add(1); }

        Slot& slotFor(std::string_view name){
            auto it = slots.find(name);
            if (it == slots.end()) it = slots.emplace(std::string(name), Slot()).first;
            it->second.lastUse = ++useCounter;
            return it->second;
        }

        // ------------------------------------------------------------ reading

        // The slot's newest valid generation into the image. NotFound without files, Corrupt
        // without a valid one (the files stay), NewerFormat for a newer engine's file
        SaveError load(const std::string& name, Slot& slot){
            if (slot.loaded) return {};
            const fs::path& dir = resolveFolder();
            auto all = scanFolder(dir, name);
            SlotFiles& files = all[name];
            for (const auto& entry : files.corrupt) slot.highest = std::max(slot.highest, entry.first);
            if (files.generations.empty()){
                return makeError(SaveErrorCode::NotFound, notFoundMessage(name));
            }
            slot.highest = std::max(slot.highest, files.generations.rbegin()->first);
            std::vector<std::string> problems;
            std::vector<fs::path> invalid;
            for (auto it = files.generations.rbegin(); it != files.generations.rend(); ++it){
                std::string bytes, problem;
                SaveFile file;
                SaveError error;
                const std::string fileName = it->second.filename().string();
                const ReadOutcome outcome = readSaveFile(it->second, MAX_FILE_BYTES, bytes, problem);
                if (outcome == ReadOutcome::Missing) continue;
                if (outcome == ReadOutcome::Unreadable){
                    // not damage: nothing is set aside or started afresh over it
                    return makeError(SaveErrorCode::Io, "Couldn't read slot \"" + name + "\": " + fileName + ": " + problem +
                                     ". Its files are left as they are; try again.");
                }
                if (outcome == ReadOutcome::TooLarge){
                    problems.push_back(fileName + ": " + problem);
                    invalid.push_back(it->second);
                    continue;
                }
                if (!parseSaveFile(bytes, file, error)){
                    if (error.code == SaveErrorCode::NewerFormat){
                        // never skipped past, never written over (R8)
                        slot.blocked = SaveErrorCode::NewerFormat;
                        slot.blockedMessage = "slot \"" + name + "\" was " + error.message +
                                              ": it is left as it is (pass force = true to write over it)";
                        slot.status = "newer_format";
                        slot.generation = it->first;
                        slot.currentSize = files.sizes[it->first];
                        return makeError(SaveErrorCode::NewerFormat, slot.blockedMessage);
                    }
                    problems.push_back(fileName + ": " + error.message);
                    invalid.push_back(it->second);
                    continue;
                }
                install(slot, file, files.sizes[it->first]);
                slot.problems = problems;
                slot.recoveredFrom = problems.empty() ? 0 : it->first;
                slot.status = problems.empty() ? "ok" : "recovered";
                if (!invalid.empty()) setAside(name, invalid, files);
                if (!problems.empty() && !slot.warned){
                    slot.warned = true;
                    std::string list;
                    for (const std::string& p : problems) list += (list.empty() ? "" : "; ") + p;
                    Log::warn("SaveGame: slot \"%s\" recovered from generation %llu (%s)", name.c_str(),
                              static_cast<unsigned long long>(it->first), list.c_str());
                }
                if (slot.status == "ok" && slot.version > dataVersion.load()) slot.status = "newer_version";
                changed();
                return {};
            }
            slot.status = "corrupt";
            slot.problems = problems;
            std::string list;
            for (const std::string& p : problems) list += (list.empty() ? "" : "; ") + p;
            return makeError(SaveErrorCode::Corrupt, "slot \"" + name + "\" has no readable generation (" + list + ")");
        }

        void install(Slot& slot, const SaveFile& file, uint64_t size){
            SaveMetadata metadata;
            splitMetadata(file.metadata, metadata);
            slot.loaded = true;
            slot.version = file.dataVersion;
            slot.generation = file.generation;
            slot.highest = std::max(slot.highest, file.generation);
            slot.savedAtMs = file.savedAtMs;
            slot.meta = metadata.meta;
            slot.requirements = metadata.requirements;
            slot.sections = file.sections;
            std::sort(slot.sections.begin(), slot.sections.end(), [](const SaveSection& a, const SaveSection& b){ return a.name < b.name; });
            slot.currentSize = size;
            slot.blocked = SaveErrorCode::None;
            slot.blockedMessage.clear();
        }

        // The newest invalid file becomes `<name>.corrupt` (the only set-aside file of the slot)
        void setAside(const std::string& name, const std::vector<fs::path>& invalid, const SlotFiles& files){
            std::error_code ec;
            for (const auto& entry : files.corrupt) fs::remove(entry.second, ec);
            for (size_t i = 0; i < invalid.size(); i++){
                if (i == 0){
                    fs::path target = invalid[i];
                    target += ".corrupt";
                    if (!io::renameWithRetry(invalid[i], target, ec))
                        Log::warn("SaveGame: could not set aside %s: %s", saveStorePathText(invalid[i]).c_str(), ec.message().c_str());
                } else {
                    fs::remove(invalid[i], ec);
                }
            }
            (void)name;
        }

        std::string notFoundMessage(const std::string& name){
            std::string list;
            for (const auto& entry : scanFolder(resolveFolder())){
                if (entry.first == name || entry.second.generations.empty()) continue;
                list += (list.empty() ? "" : ", ") + entry.first;
            }
            return "No slot \"" + name + "\". Slots: " + (list.empty() ? std::string("none") : list) + ".";
        }

        ReadInfo infoOf(const Slot& slot) const {
            ReadInfo info;
            info.generation = slot.generation;
            info.recoveredFrom = slot.recoveredFrom;
            info.version = slot.version;
            info.savedAtMs = slot.savedAtMs;
            info.problems = slot.problems;
            for (const SaveSection& section : slot.sections) info.namespaces.push_back(section.name);
            info.meta = slot.meta;
            info.requirements = slot.requirements;
            return info;
        }

        // The new sections become the image and a snapshot is queued (too_large refused first)
        SaveError commit(const std::string& name, Slot& slot, std::vector<SaveSection> sections, const WriteOptions& options,
                         WriteCallback onComplete){
            std::sort(sections.begin(), sections.end(), [](const SaveSection& a, const SaveSection& b){ return a.name < b.name; });
            SaveMetadata metadata;
            metadata.meta = options.hasMeta ? options.meta : slot.meta;
            metadata.requirements = options.hasRequirements ? options.requirements : slot.requirements;
            appendString(metadata.engine, JASPERENGINE_VERSION_STRING);
            appendString(metadata.platform, platformName());
            if (options.hasMeta && options.meta.size() > MAX_METADATA_BYTES)
                return makeError(SaveErrorCode::TooLarge, "meta of " + std::to_string(options.meta.size()) + " bytes is above 4 KiB");
            std::string metadataBytes = buildMetadata(metadata);
            if (metadataBytes.size() > MAX_METADATA_BYTES)
                return makeError(SaveErrorCode::TooLarge, "meta and requires of " + std::to_string(metadataBytes.size()) + " bytes are above 4 KiB");

            uint64_t total = HEADER_FIXED_BYTES + metadataBytes.size() + FOOTER_BYTES;
            for (const SaveSection& section : sections) total += 6 + section.name.size() + section.payload->size();
            const uint64_t cap = static_cast<uint64_t>(configuredMaxSlotMiB.load()) * 1024u * 1024u;
            if (total > cap){
                std::vector<const SaveSection*> largest;
                for (const SaveSection& section : sections) largest.push_back(&section);
                std::sort(largest.begin(), largest.end(), [](const SaveSection* a, const SaveSection* b){ return a->payload->size() > b->payload->size(); });
                std::string names;
                for (size_t i = 0; i < largest.size() && i < 3; i++)
                    names += (i ? ", " : "") + largest[i]->name + " " + mib(largest[i]->payload->size());
                return makeError(SaveErrorCode::TooLarge, "slot \"" + name + "\" would be " + mib(total) + ", above Max Slot Size (" +
                                 std::to_string(configuredMaxSlotMiB.load()) + " MiB); largest namespaces: " + names +
                                 ". Split the data into more slots or save less.");
            }

            // the image takes the write now: reads see it before it reaches the disk (R4)
            edited(name);
            slot.sections = std::move(sections);
            slot.meta = std::move(metadata.meta);
            slot.requirements = std::move(metadata.requirements);
            slot.version = options.version;
            slot.savedAtMs = nowMs();
            slot.loaded = true;
            slot.blocked = SaveErrorCode::None;
            slot.blockedMessage.clear();
            slot.status = slot.version > dataVersion.load() ? "newer_version" : "ok";
            slot.written = true;

            SaveFile file;
            file.savedAtMs = slot.savedAtMs;
            file.dataVersion = slot.version;
            file.metadata = std::move(metadataBytes);
            file.sections = slot.sections;

            if (!slot.inFlight){
                auto job = std::make_shared<WriteJob>();
                job->slot = name;
                job->file = std::move(file);
                if (onComplete) job->callbacks.push_back(std::move(onComplete));
                dispatch(slot, job);
            } else {
                if (!slot.waiting){
                    slot.waiting = std::make_shared<WriteJob>();
                    slot.waiting->slot = name;
                }
                slot.waiting->file = std::move(file);
                if (onComplete) slot.waiting->callbacks.push_back(std::move(onComplete));
            }
            changed();
            trimMemory();
            return {};
        }

        // One generation file, parsed whole
        SaveError readGeneration(const std::string& name, uint64_t generation, SaveFile& file){
            const fs::path path = resolveFolder() / slotFileName(name, generation);
            std::error_code ec;
            if (!fs::exists(path, ec)){
                std::string generations;
                auto all = scanFolder(resolveFolder(), name);
                for (const auto& entry : all[name].generations)
                    generations += (generations.empty() ? "" : ", ") + std::to_string(entry.first);
                return makeError(SaveErrorCode::NotFound, "slot \"" + name + "\" has no generation " + std::to_string(generation) +
                                 " (generations: " + (generations.empty() ? std::string("none") : generations) + ")");
            }
            std::string bytes, problem;
            if (!readFile(path, MAX_FILE_BYTES, bytes, problem))
                return makeError(SaveErrorCode::Io, path.filename().string() + ": " + problem);
            SaveError error;
            parseSaveFile(bytes, file, error);
            return error;
        }

        // ------------------------------------------------------------ writing

        void ensureQueue(){
            // a new engine run in the same process (a recreated Android activity) has a new job
            // system; the shut-down one cancels what its queues are given
            jobs::JobSystem* current = &jobs::system();
            if (queue && queueSystem == current) return;
            queue = std::make_unique<jobs::SerialQueue>("savegame", jobs::Lane::Background, jobs::GateClass::Default, current);
            queueSystem = current;
        }

        void dispatch(Slot& slot, const std::shared_ptr<WriteJob>& job){
            job->file.generation = ++slot.highest;
            job->folder = resolveFolder();
            job->backups = configuredBackups.load();
            job->epoch = epoch;
            slot.inFlight = job;
            ensureQueue();
            Impl* self = this;
            queue->push([self, job]{
                {
                    std::unique_lock<std::mutex> lock(self->holdMutex);
                    self->holdChanged.wait(lock, [self]{ return !self->held; });
                }
                if (!job->claim()) return;
                self->run(*job);
                jobs::postToMain([self, job]{
                    if (job->epoch == self->epoch) self->land(job->slot, job);
                });
            }, "SaveGame write");
        }

        // The file of a claimed job (a worker or a flush)
        void run(WriteJob& job){
            const std::string bytes = buildSaveFile(job.file);
            const fs::path target = job.folder / slotFileName(job.slot, job.file.generation);
            const io::WriteResult result = io::writeFileAtomic(target, bytes);
            SaveError error;
            if (!result.written()){
                error = makeError(SaveErrorCode::Io, "Couldn't save \"" + job.slot + "\": " + io::describeWriteError(result));
            } else {
                owner->writes.fetch_add(1);
                prune(job);
            }
            {
                std::lock_guard<std::mutex> lock(job.mutex);
                job.result = error;
                job.size = bytes.size();
                job.state.store(2);
            }
            job.done.notify_all();
        }

        // Generations beyond Backups Kept + 1 go, oldest first; a crash before this leaves one
        // extra file that the next write prunes
        void prune(const WriteJob& job){
            auto all = scanFolder(job.folder, job.slot);
            const auto& generations = all[job.slot].generations;
            const size_t keep = static_cast<size_t>(job.backups) + 1;
            if (generations.size() <= keep) return;
            size_t extra = generations.size() - keep;
            std::error_code ec;
            for (const auto& entry : generations){
                if (extra == 0) break;
                if (entry.first >= job.file.generation) break;
                fs::remove(entry.second, ec);
                extra--;
            }
        }

        void deliver(std::vector<WriteCallback> callbacks, const SaveError& error){
            for (WriteCallback& callback : callbacks){
                if (!callback) continue;
                jobs::postToMain([callback = std::move(callback), error]{ callback(error); });
            }
        }

        void land(const std::string& name, const std::shared_ptr<WriteJob>& job){
            if (job->landed) return;
            job->landed = true;
            auto it = slots.find(name);
            if (it != slots.end() && it->second.inFlight == job){
                Slot& slot = it->second;
                slot.inFlight.reset();
                if (job->result.ok()){
                    slot.generation = std::max(slot.generation, job->file.generation);
                    slot.currentSize = job->size;
                    slot.recoveredFrom = 0;
                    slot.problems.clear();
                    slot.status = slot.version > dataVersion.load() ? "newer_version" : "ok";
                }
                if (slot.waiting){
                    std::shared_ptr<WriteJob> next = std::move(slot.waiting);
                    slot.waiting.reset();
                    dispatch(slot, next);
                }
            }
            completeWrite(std::move(job->callbacks), job->result);
            changed();
            trimMemory();
        }

        void completeWrite(std::vector<WriteCallback> callbacks, const SaveError& error){
#if defined(__EMSCRIPTEN__)
            if (error.ok()){
                if (!jasper_web_idb_enabled()){
                    deliver(std::move(callbacks), makeError(SaveErrorCode::Unavailable,
                        "the browser's storage (IndexedDB) is unavailable: saves last until the page closes"));
                    return;
                }
                if (!persistRequested){
                    persistRequested = true;
                    jasper_web_storage_persist();
                }
                syncWaiters.landed(std::move(callbacks), error);
                if (System::hasInstance()) System::instance().flushFileSystem();
                return;
            }
#endif
            deliver(std::move(callbacks), error);
        }

        // Waits for (or writes) the slot's in-flight and waiting snapshots; false at the deadline
        bool flushSlot(const std::string& name, std::chrono::steady_clock::time_point deadline, SaveError& error){
            for (;;){
                auto it = slots.find(name);
                if (it == slots.end()) return true;
                Slot& slot = it->second;
                if (!slot.inFlight && slot.waiting){
                    std::shared_ptr<WriteJob> next = std::move(slot.waiting);
                    slot.waiting.reset();
                    dispatch(slot, next);
                }
                std::shared_ptr<WriteJob> job = slot.inFlight;
                if (!job) return true;
                if (job->claim()){
                    run(*job);
                } else {
                    std::unique_lock<std::mutex> lock(job->mutex);
                    if (!job->done.wait_until(lock, deadline, [&]{ return job->state.load() == 2; })) return false;
                }
                if (!job->result.ok()) error = job->result;
                land(name, job);
            }
        }

        void trimMemory(){
            size_t total = 0;
            for (const auto& entry : slots) total += entry.second.bytes();
            while (total > MEMORY_LIMIT){
                Slot* oldest = nullptr;
                for (auto& entry : slots){
                    Slot& slot = entry.second;
                    if (!slot.loaded || slot.pending()) continue;
                    if (!oldest || slot.lastUse < oldest->lastUse) oldest = &slot;
                }
                if (!oldest) return;
                total -= oldest->bytes();
                drop(*oldest);
            }
        }

        void drop(Slot& slot){
            slot.loaded = false;
            slot.sections.clear();
            slot.sections.shrink_to_fit();
            slot.meta.clear();
            slot.requirements.clear();
            slot.status = "ok";
        }
    };

}

// ================================================================ free functions

namespace jasperengine::savegame {

void configure(const SaveSettings& settings){
    configuredBackups.store(std::clamp(settings.backups, 0, 9));
    configuredMaxSlotMiB.store(std::clamp(settings.maxSlotMiB, 1, 64));
}

SaveSettings settings(){
    SaveSettings out;
    out.backups = configuredBackups.load();
    out.maxSlotMiB = configuredMaxSlotMiB.load();
    return out;
}

void setGameDataVersion(uint32_t version){
    dataVersion.store(std::max<uint32_t>(version, 1));
}

uint32_t gameDataVersion(){
    return dataVersion.load();
}

void flushForLifecycle(const char* when){
    if (!storeInstance) return;
    storeInstance->flush({}, std::chrono::seconds(2), when);
}

std::string slotFileName(std::string_view slot, uint64_t generation){
    return std::string(slot) + ".g" + std::to_string(generation) + ".jsave";
}

bool parseSlotFileName(std::string_view name, std::string& slot, uint64_t& generation, bool& corrupt){
    corrupt = false;
    constexpr std::string_view corruptSuffix = ".corrupt";
    if (name.size() > corruptSuffix.size() && name.substr(name.size() - corruptSuffix.size()) == corruptSuffix){
        corrupt = true;
        name.remove_suffix(corruptSuffix.size());
    }
    constexpr std::string_view suffix = ".jsave";
    if (name.size() <= suffix.size() || name.substr(name.size() - suffix.size()) != suffix) return false;
    name.remove_suffix(suffix.size());
    const size_t dot = name.rfind(".g");
    if (dot == std::string_view::npos || dot + 2 >= name.size()) return false;
    const std::string_view digits = name.substr(dot + 2);
    if (digits.size() > 19) return false;
    uint64_t value = 0;
    for (char c : digits){
        if (c < '0' || c > '9') return false;
        value = value * 10 + static_cast<uint64_t>(c - '0');
    }
    const std::string_view slotName = name.substr(0, dot);
    if (!isValidName(slotName, MAX_SLOT_NAME)) return false;
    slot.assign(slotName.data(), slotName.size());
    generation = value;
    return true;
}

// ================================================================ the store

SaveGameStore& SaveGameStore::instance(){
    if (!storeInstance) storeInstance = new SaveGameStore();
    return *storeInstance;
}

SaveGameStore* SaveGameStore::existing(){
    return storeInstance;
}

SaveGameStore::SaveGameStore(): impl(std::make_unique<Impl>()){
    impl->owner = this;
}

SaveGameStore::~SaveGameStore() = default;

void SaveGameStore::setFolder(const fs::path& folder){
    reset();
    impl->folder = folder;
    impl->folderSet = !folder.empty();
}

fs::path SaveGameStore::folder(){
    return impl->resolveFolder();
}

namespace {

    SaveError nameError(std::string_view slot, std::string_view nameSpace){
        if (!isValidName(slot, MAX_SLOT_NAME)){
            return makeError(SaveErrorCode::Name, "Slot names use a-z, 0-9, '_' and '-' (got \"" + std::string(slot) +
                             "\"); try \"" + suggestName(slot, MAX_SLOT_NAME) + "\".");
        }
        if (!nameSpace.empty() && !isValidName(nameSpace, MAX_SECTION_NAME)){
            return makeError(SaveErrorCode::Name, "Namespace names use 1-32 characters of a-z, 0-9, '_' and '-' (got \"" +
                             std::string(nameSpace) + "\"); try \"" + suggestName(nameSpace, MAX_SECTION_NAME) + "\".");
        }
        return {};
    }

}

SaveError SaveGameStore::write(std::string_view slotName, std::string_view nameSpace, std::string payload,
                               const WriteOptions& options, WriteCallback onComplete){
    if (nameSpace.empty()) nameSpace = "main";
    SaveError error = nameError(slotName, nameSpace);
    if (!error.ok()) return error;
    const std::string name(slotName);
    Slot& slot = impl->slotFor(name);
    if (!slot.loaded){
        const SaveError loaded = impl->load(name, slot);
        // a slot without a readable generation is started afresh; its files stay until pruned
        if (!loaded.ok() && loaded.code != SaveErrorCode::NotFound && loaded.code != SaveErrorCode::Corrupt
            && !(loaded.code == SaveErrorCode::NewerFormat && options.force))
            return loaded;
    }
    if (slot.blocked == SaveErrorCode::NewerFormat && !options.force) return makeError(SaveErrorCode::NewerFormat, slot.blockedMessage);
    if (slot.loaded && slot.version > options.version && !options.force && !slot.sections.empty()){
        return makeError(SaveErrorCode::NewerVersion, "slot \"" + name + "\" is version " + std::to_string(slot.version) +
                         " and this game writes " + std::to_string(options.version) +
                         ": pass force = true to overwrite it (its newer data would be lost)");
    }

    if (!options.replaceAll && !options.force && slot.loaded && slot.version < options.version){
        std::string others;
        for (const SaveSection& section : slot.sections)
            if (section.name != nameSpace) others += (others.empty() ? "" : ", ") + section.name;
        if (!others.empty()){
            error = impl->migrateOthers(name, slot, options.version, others);
            if (!error.ok()) return error;
        }
    }

    std::vector<SaveSection> sections;
    if (!options.replaceAll) sections = slot.sections;
    auto it = std::find_if(sections.begin(), sections.end(), [&](const SaveSection& s){ return s.name == nameSpace; });
    auto shared = std::make_shared<const std::string>(std::move(payload));
    if (it != sections.end()) it->payload = shared;
    else {
        if (sections.size() >= MAX_SECTIONS)
            return makeError(SaveErrorCode::TooLarge, "slot \"" + name + "\" already has " + std::to_string(MAX_SECTIONS) +
                             " namespaces (" + sectionNames(sections) + ")");
        sections.push_back({std::string(nameSpace), shared});
    }
    return impl->commit(name, slot, std::move(sections), options, std::move(onComplete));
}

SaveError SaveGameStore::read(std::string_view slotName, std::string_view nameSpace, std::string& payload, ReadInfo& info){
    if (nameSpace.empty()) nameSpace = "main";
    SaveError error = nameError(slotName, nameSpace);
    if (!error.ok()) return error;
    const std::string name(slotName);
    Slot& slot = impl->slotFor(name);
    error = impl->load(name, slot);
    if (!error.ok()) return error;
    info = impl->infoOf(slot);
    if (slot.version > dataVersion.load()){
        return makeError(SaveErrorCode::NewerVersion, "slot \"" + name + "\" is version " + std::to_string(slot.version) +
                         " and SaveGame.version is " + std::to_string(dataVersion.load()) + ": it is left as it is");
    }
    for (const SaveSection& section : slot.sections){
        if (section.name == nameSpace){
            payload = *section.payload;
            return {};
        }
    }
    return makeError(SaveErrorCode::NotFound, "slot \"" + name + "\" has no namespace \"" + std::string(nameSpace) +
                     "\" (namespaces: " + sectionNames(slot.sections) + ")");
}

SaveError SaveGameStore::readSlot(std::string_view slotName, SlotData& out, uint64_t generation){
    SaveError error = nameError(slotName, {});
    if (!error.ok()) return error;
    const std::string name(slotName);
    if (generation == 0){
        Slot& slot = impl->slotFor(name);
        error = impl->load(name, slot);
        if (!error.ok()) return error;
        out.info = impl->infoOf(slot);
        out.sections = slot.sections;
        return {};
    }
    SaveFile file;
    error = impl->readGeneration(name, generation, file);
    if (!error.ok()){
        if (error.code == SaveErrorCode::Corrupt)
            error.message = "generation " + std::to_string(generation) + " of \"" + name + "\" is corrupt (" + error.message + ")";
        return error;
    }
    Impl::fillSlotData(file, out);
    return {};
}

SaveError SaveGameStore::peekSlot(std::string_view slotName, SlotData& out){
    SaveError error = nameError(slotName, {});
    if (!error.ok()) return error;
    const std::string name(slotName);
    auto it = impl->slots.find(name);
    if (it != impl->slots.end() && it->second.loaded){
        out.info = impl->infoOf(it->second);
        out.sections = it->second.sections;
        return {};
    }
    auto all = scanFolder(impl->resolveFolder(), name);
    const SlotFiles& files = all[name];
    if (files.generations.empty()) return makeError(SaveErrorCode::NotFound, impl->notFoundMessage(name));
    std::vector<std::string> problems;
    for (auto generation = files.generations.rbegin(); generation != files.generations.rend(); ++generation){
        const std::string fileName = generation->second.filename().string();
        std::string bytes, problem;
        const ReadOutcome outcome = readSaveFile(generation->second, MAX_FILE_BYTES, bytes, problem);
        if (outcome == ReadOutcome::Missing) continue;
        if (outcome == ReadOutcome::Unreadable)
            return makeError(SaveErrorCode::Io, "Couldn't read slot \"" + name + "\": " + fileName + ": " + problem + ".");
        SaveFile file;
        SaveError parseError;
        if (outcome == ReadOutcome::Ok && parseSaveFile(bytes, file, parseError)){
            Impl::fillSlotData(file, out);
            out.info.recoveredFrom = problems.empty() ? 0 : generation->first;
            out.info.problems = problems;
            return {};
        }
        if (outcome == ReadOutcome::Ok && parseError.code == SaveErrorCode::NewerFormat)
            return makeError(SaveErrorCode::NewerFormat, "slot \"" + name + "\" was " + parseError.message);
        problems.push_back(fileName + ": " + (outcome == ReadOutcome::Ok ? parseError.message : problem));
    }
    std::string list;
    for (const std::string& p : problems) list += (list.empty() ? "" : "; ") + p;
    return makeError(SaveErrorCode::Corrupt, "slot \"" + name + "\" has no readable generation (" + list + ")");
}

void SaveGameStore::readSlotAsync(std::string_view slotName, std::function<void(const SaveError&, const SlotData&)> done){
    const std::string name(slotName);
    SaveError error = nameError(slotName, {});
    auto it = impl->slots.find(name);
    if (!error.ok() || (it != impl->slots.end() && it->second.loaded)){
        SlotData data;
        if (error.ok()) error = readSlot(name, data);
        jobs::postToMain([done = std::move(done), error, data = std::move(data)]{ done(error, data); });
        return;
    }
    // the reads and checksums on the queue; the image installed (and recovery applied) on the
    // main thread, where the files are looked at again only when a write landed meanwhile
    struct Loaded { fs::path path; std::string bytes; SaveFile file; SaveError error; bool ok = false; };
    auto loaded = std::make_shared<Loaded>();
    const fs::path dir = impl->resolveFolder();
    const uint64_t epoch = impl->epoch;
    const uint64_t edit = impl->edits[name];
    Impl* self = impl.get();
    impl->ensureQueue();
    impl->queue->push([self, name, dir, loaded, epoch, edit, done = std::move(done)]() mutable {
        auto all = scanFolder(dir, name);
        const auto& generations = all[name].generations;
        if (!generations.empty()){
            std::string problem;
            loaded->path = generations.rbegin()->second;
            if (readFile(loaded->path, MAX_FILE_BYTES, loaded->bytes, problem))
                loaded->ok = parseSaveFile(loaded->bytes, loaded->file, loaded->error);
            loaded->bytes.clear();
        }
        jobs::postToMain([self, name, loaded, epoch, edit, done = std::move(done)]{
            if (epoch != self->epoch) return;
            Slot& slot = self->slotFor(name);
            // a write, delete or corrupt since the job read the file: what it read is stale
            if (!slot.loaded && loaded->ok && self->edits[name] == edit){
                std::error_code ec;
                self->install(slot, loaded->file, fs::file_size(loaded->path, ec));
                slot.status = slot.version > dataVersion.load() ? "newer_version" : "ok";
                self->changed();
            }
            SlotData data;
            // the newest file invalid or gone: the synchronous path does the recovery
            const SaveError error = self->owner->readSlot(name, data);
            done(error, data);
        });
    }, "SaveGame read");
}

SaveError SaveGameStore::markMigrated(std::string_view slotName, uint32_t version, std::vector<SaveSection> sections){
    auto it = impl->slots.find(slotName);
    if (it == impl->slots.end() || !it->second.loaded)
        return makeError(SaveErrorCode::NotFound, "slot \"" + std::string(slotName) + "\" is not loaded");
    Slot& slot = it->second;
    std::sort(sections.begin(), sections.end(), [](const SaveSection& a, const SaveSection& b){ return a.name < b.name; });
    slot.sections = std::move(sections);
    slot.version = version;
    slot.status = slot.version > dataVersion.load() ? "newer_version" : "ok";
    impl->edited(std::string(slotName));
    impl->changed();
    return {};
}

std::vector<SlotInfo> SaveGameStore::list(){
    std::vector<SlotInfo> out;
    auto all = scanFolder(impl->resolveFolder());
    for (auto& entry : impl->slots){
        if (entry.second.loaded && !all.count(entry.first)) all[entry.first];
    }
    for (auto& entry : all){
        const std::string& name = entry.first;
        SlotFiles& files = entry.second;
        auto loaded = impl->slots.find(name);
        if (files.generations.empty() && (loaded == impl->slots.end() || !loaded->second.loaded || loaded->second.sections.empty()))
            continue;
        SlotInfo info;
        info.slot = name;
        info.generations = static_cast<int>(files.generations.size());
        if (loaded != impl->slots.end() && loaded->second.loaded){
            const Slot& slot = loaded->second;
            info.version = slot.version;
            info.savedAt = static_cast<double>(slot.savedAtMs) / 1000.0;
            info.generation = slot.generation;
            info.size = slot.currentSize;
            for (const SaveSection& section : slot.sections) info.namespaces.push_back(section.name);
            info.meta = slot.meta;
            info.requirements = slot.requirements;
            info.status = slot.status;
            info.pending = slot.pending();
            info.writtenThisSession = slot.written;
        } else {
            if (loaded != impl->slots.end()) info.writtenThisSession = loaded->second.written;
            info.status = "corrupt";
            bool first = true;
            for (auto it = files.generations.rbegin(); it != files.generations.rend(); ++it, first = false){
                std::string bytes;
                SaveFile file;
                SaveError error;
                if (!readPrefix(it->second, MAX_HEADER_BYTES, bytes)) continue;
                if (!parseSaveHeader(bytes, file, error)){
                    // a format field above this engine's: a newer engine's file only when its footer
                    // checks (rare: read whole), else a damaged one, skipped as the read does
                    std::string whole, problem;
                    SaveFile full;
                    SaveError fullError;
                    if (error.code == SaveErrorCode::NewerFormat && readFile(it->second, MAX_FILE_BYTES, whole, problem)
                        && !parseSaveFile(whole, full, fullError) && fullError.code == SaveErrorCode::NewerFormat){
                        info.status = "newer_format";
                        info.generation = it->first;
                        info.size = files.sizes[it->first];
                        break;
                    }
                    continue;
                }
                SaveMetadata metadata;
                splitMetadata(file.metadata, metadata);
                info.version = file.dataVersion;
                info.savedAt = static_cast<double>(file.savedAtMs) / 1000.0;
                info.generation = it->first;
                info.size = files.sizes[it->first];
                info.meta = metadata.meta;
                info.requirements = metadata.requirements;
                info.status = first ? "ok" : "recovered";
                // the names of the sections follow the header: read with the header's bytes when there
                const size_t headerSize = HEADER_FIXED_BYTES + file.metadata.size();
                std::string_view rest(bytes);
                size_t pos = headerSize;
                for (size_t i = 0; i < file.sections.size() && pos + 2 <= rest.size(); i++){
                    const size_t length = static_cast<unsigned char>(rest[pos]) | (static_cast<size_t>(static_cast<unsigned char>(rest[pos + 1])) << 8);
                    if (length == 0 || length > MAX_SECTION_NAME || pos + 2 + length + 4 > rest.size()) break;
                    info.namespaces.push_back(std::string(rest.substr(pos + 2, length)));
                    const size_t payload = static_cast<unsigned char>(rest[pos + 2 + length]) |
                        (static_cast<size_t>(static_cast<unsigned char>(rest[pos + 3 + length])) << 8) |
                        (static_cast<size_t>(static_cast<unsigned char>(rest[pos + 4 + length])) << 16) |
                        (static_cast<size_t>(static_cast<unsigned char>(rest[pos + 5 + length])) << 24);
                    pos += 6 + length + payload;
                }
                break;
            }
            // a set-aside file above the newest valid one: an earlier read recovered
            if (info.status == "ok" && !files.corrupt.empty() && files.corrupt.rbegin()->first > info.generation)
                info.status = "recovered";
            // damage first: a recovered slot of a newer version shows as recovered
            if (info.status == "ok" && info.version > dataVersion.load()) info.status = "newer_version";
        }
        out.push_back(std::move(info));
    }
    std::sort(out.begin(), out.end(), [](const SlotInfo& a, const SlotInfo& b){
        if (a.savedAt != b.savedAt) return a.savedAt > b.savedAt;
        return a.slot < b.slot;
    });
    return out;
}

SaveError SaveGameStore::info(std::string_view slotName, SlotInfo& out){
    SaveError error = nameError(slotName, {});
    if (!error.ok()) return error;
    for (SlotInfo& info : list()){
        if (info.slot == slotName){
            out = std::move(info);
            return {};
        }
    }
    return makeError(SaveErrorCode::NotFound, impl->notFoundMessage(std::string(slotName)));
}

bool SaveGameStore::exists(std::string_view slotName){
    if (!isValidName(slotName, MAX_SLOT_NAME)) return false;
    auto it = impl->slots.find(slotName);
    if (it != impl->slots.end() && it->second.loaded && !it->second.sections.empty()) return true;
    auto all = scanFolder(impl->resolveFolder(), slotName);
    auto files = all.find(std::string(slotName));
    return files != all.end() && !files->second.generations.empty();
}

SaveError SaveGameStore::remove(std::string_view slotName, std::chrono::milliseconds limit){
    SaveError error = nameError(slotName, {});
    if (!error.ok()) return error;
    const std::string name(slotName);
    auto it = impl->slots.find(name);
    if (it != impl->slots.end()){
        // the write on its way lands first; the waiting one is dropped only once that happened
        std::shared_ptr<WriteJob> waiting = std::move(it->second.waiting);
        it->second.waiting.reset();
        SaveError ignored;
        if (!impl->flushSlot(name, std::chrono::steady_clock::now() + limit, ignored)){
            // deleting now would let that write publish its generation afterwards: nothing is deleted
            auto again = impl->slots.find(name);
            if (again != impl->slots.end()) again->second.waiting = std::move(waiting);
            return makeError(SaveErrorCode::Io, "Couldn't delete slot \"" + name + "\": it is still being written (a write took longer than " +
                             std::to_string(limit.count()) + " ms). Try again.");
        }
        if (waiting)
            impl->deliver(std::move(waiting->callbacks), makeError(SaveErrorCode::Cancelled, "slot \"" + name + "\" was deleted before this write"));
    }
    std::error_code ec;
    for (const fs::path& path : slotFiles(name)){
        if (!removeSaveFile(path, ec)){
            impl->edited(name);
            impl->slots.erase(name);
            impl->changed();
            return makeError(SaveErrorCode::Io, "Couldn't delete " + path.filename().string() + ": " + ec.message());
        }
    }
    impl->sweepTemporaries(impl->resolveFolder(), name);
    impl->edited(name);
    impl->slots.erase(name);
    impl->changed();
    return {};
}

SaveError SaveGameStore::backups(std::string_view slotName, std::vector<BackupInfo>& out){
    out.clear();
    SaveError error = nameError(slotName, {});
    if (!error.ok()) return error;
    const std::string name(slotName);
    auto all = scanFolder(impl->resolveFolder(), name);
    const SlotFiles& files = all[name];
    for (auto it = files.generations.rbegin(); it != files.generations.rend(); ++it){
        BackupInfo backup;
        backup.generation = it->first;
        backup.file = it->second.filename().string();
        std::string bytes, problem;
        SaveFile file;
        SaveError parseError;
        if (!readFile(it->second, MAX_FILE_BYTES, bytes, problem)){
            backup.problem = problem;
        } else {
            backup.size = bytes.size();
            backup.valid = parseSaveFile(bytes, file, parseError);
            if (!backup.valid) backup.problem = parseError.message;
            else {
                backup.version = file.dataVersion;
                backup.savedAt = static_cast<double>(file.savedAtMs) / 1000.0;
            }
        }
        out.push_back(std::move(backup));
    }
    if (out.empty() && !exists(name)) return makeError(SaveErrorCode::NotFound, impl->notFoundMessage(name));
    return {};
}

SaveError SaveGameStore::restore(std::string_view slotName, uint64_t generation, WriteCallback onComplete){
    SaveError error = nameError(slotName, {});
    if (!error.ok()) return error;
    const std::string name(slotName);
    SaveFile file;
    error = impl->readGeneration(name, generation, file);
    if (!error.ok()){
        if (error.code == SaveErrorCode::Corrupt || error.code == SaveErrorCode::NewerFormat)
            error.message = "Couldn't restore \"" + name + "\" generation " + std::to_string(generation) +
                            ": the file is corrupt (" + error.message + ").";
        return error;
    }
    flush(name);
    Slot& slot = impl->slotFor(name);
    impl->load(name, slot);
    SaveMetadata metadata;
    splitMetadata(file.metadata, metadata);
    WriteOptions options;
    options.version = file.dataVersion;
    options.force = true;
    options.hasMeta = true;
    options.meta = metadata.meta;
    options.hasRequirements = true;
    options.requirements = metadata.requirements;
    return impl->commit(name, slot, file.sections, options, std::move(onComplete));
}

SaveError SaveGameStore::corrupt(std::string_view slotName, CorruptMode mode, std::string& file){
    SaveError error = nameError(slotName, {});
    if (!error.ok()) return error;
    const std::string name(slotName);
    flush(name);
    auto all = scanFolder(impl->resolveFolder(), name);
    const SlotFiles& files = all[name];
    if (files.generations.empty()) return makeError(SaveErrorCode::NotFound, impl->notFoundMessage(name));
    const fs::path path = files.generations.rbegin()->second;
    std::string bytes, problem;
    if (!readFile(path, MAX_FILE_BYTES, bytes, problem)) return makeError(SaveErrorCode::Io, path.filename().string() + ": " + problem);
    switch (mode){
        case CorruptMode::Truncate:
            bytes.resize(bytes.size() / 2);
            break;
        case CorruptMode::Flip:
            if (!bytes.empty()) bytes[bytes.size() / 2] = static_cast<char>(bytes[bytes.size() / 2] ^ 0xFF);
            break;
        case CorruptMode::Header:
            for (size_t i = 0; i < bytes.size() && i < 16; i++) bytes[i] = 'X';
            break;
    }
    const io::WriteResult result = io::writeFileAtomic(path, bytes);
    if (!result.written()) return makeError(SaveErrorCode::Io, "Couldn't corrupt " + path.filename().string() + ": " + io::describeWriteError(result));
    file = path.filename().string();
    impl->edited(name);
    auto it = impl->slots.find(name);
    if (it != impl->slots.end() && !it->second.pending()){
        impl->drop(it->second);
        it->second.warned = false;
    }
    impl->changed();
    return {};
}

SaveError SaveGameStore::flush(std::string_view slotName, std::chrono::milliseconds limit, const char* when){
    const auto deadline = std::chrono::steady_clock::now() + limit;
    SaveError result;
    std::vector<std::string> names;
    if (!slotName.empty()) names.emplace_back(slotName);
    else for (const auto& entry : impl->slots) if (entry.second.pending()) names.push_back(entry.first);
    std::string abandoned;
    for (const std::string& name : names){
        SaveError error;
        if (!impl->flushSlot(name, deadline, error)){
            abandoned += (abandoned.empty() ? "\"" : ", \"") + name + "\"";
            continue;
        }
        if (!error.ok()) result = error;
    }
    if (!abandoned.empty()){
        const double seconds = static_cast<double>(limit.count()) / 1000.0;
        char limitText[32];
        std::snprintf(limitText, sizeof(limitText), "%g s", seconds);
        const std::string message = "SaveGame: " + abandoned + (names.size() > 1 && abandoned.find(',') != std::string::npos ? " were" : " was") +
                                    " not written before " + (when ? when : "the flush ended") + " (" + limitText + ")";
        Log::warn("%s", message.c_str());
        result = makeError(SaveErrorCode::Io, message.substr(10));
    }
    return result;
}

void SaveGameStore::unload(std::string_view slotName){
    for (auto& entry : impl->slots){
        if (!slotName.empty() && entry.first != slotName) continue;
        if (!entry.second.pending()) impl->drop(entry.second);
    }
    impl->changed();
}

void SaveGameStore::reset(){
    flush({}, std::chrono::seconds(2), "the store was reset");
    for (auto& entry : impl->slots){
        if (entry.second.waiting)
            impl->deliver(std::move(entry.second.waiting->callbacks), makeError(SaveErrorCode::Cancelled, "the store was reset"));
    }
    impl->slots.clear();
    impl->epoch++;
    impl->folderSet = false;
    impl->folder.clear();
    impl->swept.clear();
    impl->edits.clear();
    impl->syncWaiters.clear();
    impl->changed();
}

void SaveGameStore::clearSessionMarks(){
    for (auto& entry : impl->slots) entry.second.written = false;
    impl->changed();
}

std::vector<fs::path> SaveGameStore::slotFiles(std::string_view slotName){
    std::vector<fs::path> out;
    if (!isValidName(slotName, MAX_SLOT_NAME)) return out;
    auto all = scanFolder(impl->resolveFolder(), slotName);
    for (const auto& entry : all[std::string(slotName)].generations) out.push_back(entry.second);
    for (const auto& entry : all[std::string(slotName)].corrupt) out.push_back(entry.second);
    return out;
}

uint64_t SaveGameStore::newestGeneration(std::string_view slotName){
    if (!isValidName(slotName, MAX_SLOT_NAME)) return 0;
    auto all = scanFolder(impl->resolveFolder(), slotName);
    const auto& generations = all[std::string(slotName)].generations;
    return generations.empty() ? 0 : generations.rbegin()->first;
}

uint64_t SaveGameStore::pendingGeneration(std::string_view slotName){
    auto it = impl->slots.find(slotName);
    if (it == impl->slots.end()) return 0;
    const Slot& slot = it->second;
    // a waiting write is given the next number when the one on its way lands
    if (slot.waiting) return slot.highest + 1;
    return slot.inFlight ? slot.inFlight->file.generation : 0;
}

uint64_t SaveGameStore::slotSignature(std::string_view slotName){
    if (!isValidName(slotName, MAX_SLOT_NAME)) return 0;
    uint64_t hash = 1469598103934665603ull;
    auto mix = [&hash](uint64_t value){
        for (int i = 0; i < 8; i++){
            hash ^= (value >> (8 * i)) & 0xFF;
            hash *= 1099511628211ull;
        }
    };
    for (const fs::path& path : slotFiles(slotName)){
        for (char c : path.filename().string()) mix(static_cast<unsigned char>(c));
        std::error_code ec;
        mix(static_cast<uint64_t>(fs::file_size(path, ec)));
        mix(static_cast<uint64_t>(fs::last_write_time(path, ec).time_since_epoch().count()));
    }
    return hash;
}

StorageInfo SaveGameStore::storage(){
#if defined(__EMSCRIPTEN__)
    return impl->webStorage;
#else
    StorageInfo out;
    std::error_code ec;
    const fs::path& dir = impl->resolveFolder();
    const fs::path probe = fs::exists(dir, ec) ? dir : dir.parent_path();
    const fs::space_info space = fs::space(probe, ec);
    if (!ec) out.free = static_cast<int64_t>(space.available);
    int64_t used = 0;
    for (const auto& entry : scanFolder(dir)){
        for (const auto& size : entry.second.sizes) used += static_cast<int64_t>(size.second);
    }
    // temporary files of slot writes (one on its way, or one a kill left) take space too
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)){
        const std::string name = it->path().filename().string();
        std::string slot;
        uint64_t generation = 0;
        bool corrupt = false;
        if (!io::isTemporarySiblingName(name) ||
            !parseSlotFileName(io::temporarySiblingTarget(it->path()).filename().string(), slot, generation, corrupt)) continue;
        std::error_code sizeError;
        const uintmax_t size = it->file_size(sizeError);
        if (!sizeError) used += static_cast<int64_t>(size);
    }
    out.used = used;
    return out;
#endif
}

void SaveGameStore::setStorageEstimate(int64_t used, int64_t quota){
    impl->webStorage.used = used;
    impl->webStorage.quota = quota;
    impl->webStorage.free = used >= 0 && quota >= 0 ? quota - used : -1;
}

void SaveGameStore::fileSystemSyncStarted(uint64_t sync){
    impl->syncWaiters.started(sync);
}

void SaveGameStore::fileSystemSynced(uint64_t sync, bool ok, std::string_view errorName, std::string_view message){
    for (SyncWaiters::Delivery& delivery : impl->syncWaiters.finished(sync, ok, errorName, message))
        impl->deliver(std::move(delivery.callbacks), delivery.error);
#if defined(__EMSCRIPTEN__)
    jasper_web_storage_estimate();
#endif
}

size_t SaveGameStore::fileReads(){
    return fullReads.load();
}

// ================================================================ web syncs

void SyncWaiters::landed(std::vector<WriteCallback> callbacks, const SaveError& result){
    unsynced.push_back({std::move(callbacks), result});
}

void SyncWaiters::started(uint64_t sync){
    if (unsynced.empty()) return;
    std::vector<Delivery>& covered = syncing[sync];
    for (Delivery& delivery : unsynced) covered.push_back(std::move(delivery));
    unsynced.clear();
}

std::vector<SyncWaiters::Delivery> SyncWaiters::finished(uint64_t sync, bool ok, std::string_view errorName, std::string_view message){
    auto it = syncing.find(sync);
    if (it == syncing.end()) return {};
    std::vector<Delivery> out = std::move(it->second);
    syncing.erase(it);
    if (!ok){
        SaveError error;
        if (errorName == "QuotaExceededError")
            error = makeError(SaveErrorCode::Quota, std::string(message) + "; free space in the browser's site data or save less");
        else
            error = makeError(SaveErrorCode::Io, "the browser could not store the save (" + std::string(errorName) + ": " + std::string(message) + ")");
        for (Delivery& delivery : out) delivery.error = error;
    }
    return out;
}

void SyncWaiters::clear(){
    unsynced.clear();
    syncing.clear();
}

size_t SyncWaiters::waiting() const{
    size_t total = unsynced.size();
    for (const auto& entry : syncing) total += entry.second.size();
    return total;
}

// ================================================================ test seams and hooks

void setMigrator(Migrator migrator){
    registeredMigrator() = std::move(migrator);
}

void setFileFaultInjector(FileFaultInjector injector){
    std::lock_guard<std::mutex> lock(saveFaultMutex());
    saveFaultInjector() = std::move(injector);
}

void SaveGameStore::setHold(bool held){
    {
        std::lock_guard<std::mutex> lock(impl->holdMutex);
        impl->held = held;
    }
    impl->holdChanged.notify_all();
}

size_t SaveGameStore::memoryBytes() const{
    size_t total = 0;
    for (const auto& entry : impl->slots) total += entry.second.bytes();
    return total;
}

}  // namespace jasperengine::savegame
