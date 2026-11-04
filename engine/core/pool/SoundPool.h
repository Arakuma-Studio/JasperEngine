// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SOUNDPOOL_H
#define SOUNDPOOL_H

#include "Engine.h"
#include "component/AudioSourceComponent.h"
#include "jobs/JobSystem.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace SoLoud{
    class AudioSource;
}

namespace jasperengine{

    typedef std::map<std::string, std::shared_ptr<SoLoud::AudioSource>> sounds_t;

    struct SoundLoadResult {
        std::string id;
        ResourceLoadState state = ResourceLoadState::NotStarted;
        std::string errorMessage;
        std::shared_ptr<SoLoud::AudioSource> data = nullptr;
        // what the sample is: Auto resolved by the file size
        AudioLoadType loadType = AudioLoadType::DecompressOnLoad;
    };

    // A cached sample, for the memory checks (tests, get_play_state)
    struct SoundPoolEntry {
        std::string id;
        std::string file;
        AudioLoadType loadType = AudioLoadType::DecompressOnLoad;
        // decoded PCM (Decompress On Load), the file's bytes (Compressed In Memory), 0 (Streaming:
        // each voice reads the file)
        size_t memoryBytes = 0;
        double length = 0;
    };

    // The shared samples, one per file and load type ("<file>#<type>": a decoded and a streamed
    // copy of one file can coexist). Samples are multi-instance: any number of voices play one.
    class JASPERENGINE_API SoundPool{
    private:
        static sounds_t& getMap();

        static std::unordered_map<std::string, jobs::Task<std::shared_ptr<SoLoud::AudioSource>>> pendingBuilds;
        static std::unordered_set<std::string> failedBuilds;
        static std::mutex cacheMutex;
        static std::atomic<bool> shutdownRequested;

        static std::mutex& sizeMutex();
        static sounds_t& getPinned();
        static std::unordered_map<std::string, uint64_t>& fileSizes();

        static std::unordered_map<std::string, uint32_t>& getGenerations();

        static std::shared_ptr<SoLoud::AudioSource> loadSoundInternal(const std::string& id, const std::string& filename, AudioLoadType loadType, bool trackProgress);
        // makes the sample of a load type held in memory from the file's bytes (on a Background
        // job for async loads: the bytes are read on an asset reader thread)
        static std::shared_ptr<SoLoud::AudioSource> decodeSound(const std::string& id, const std::string& filename,
                                                                AudioLoadType loadType, unsigned char* data,
                                                                unsigned int size, bool trackProgress);
        // waits for every pending load (cancelling those not started first when `cancel`)
        // and drops them; cacheMutex held
        static void quiescePending(bool cancel);

    public:
        // Auto streams a file from this size on (spec A8: on disk, compressed)
        static constexpr uint64_t streamingThreshold = 1024 * 1024;

        // Auto -> Streaming from streamingThreshold on, else Decompress On Load; Streaming of a
        // file the platform cannot open for random access (a resource pack) -> Compressed In Memory
        static AudioLoadType resolveLoadType(const std::string& filename, AudioLoadType requested);
        // the size of a file on disk or in the pack (cached), 0 when it does not exist
        static uint64_t fileSize(const std::string& filename);
        // "<file>#<type>" of a resolved type, and "<file>#<type>#i<hash>" while the clip's import
        // settings change its samples (AudioImportTable): those samples are a AudioClipSource
        static std::string key(const std::string& filename, AudioLoadType resolved);
        static const char* loadTypeName(AudioLoadType type);
        // "auto", "decompress_on_load", "compressed_in_memory", "streaming" (also the Unity names); false when unknown
        static bool loadTypeFromName(const std::string& name, AudioLoadType& type);
        // a scene resource entry or a pool id: "<file>" (Auto), "<file>#<type>" or "<file>#<type>#i<hash>"
        static void parseClipRef(const std::string& ref, std::string& filename, AudioLoadType& type);

        // the sample of a file with a load type: Finished (data), Loading (async loading on),
        // Failed (not found, not audio). Synchronous unless Engine::isAsyncLoading.
        static SoundLoadResult loadClip(const std::string& filename, AudioLoadType requested = AudioLoadType::Auto);
        // The clip now, on this thread, also while its asynchronous load is on its way (a Sound
        // Event's pick has to start this frame): Finished or Failed, never Loading
        static SoundLoadResult loadClipNow(const std::string& filename, AudioLoadType requested = AudioLoadType::Auto);
        // A failed file is not tried again until this or remove(); a file name retries every load type
        static void retry(const std::string& idOrFile);

        static void requestShutdown();
        // re-allows async loads after a recreated view
        static void cancelShutdown();

        // drops the cached sample when no one else holds it
        static void remove(const std::string& id);
        // keeps a loaded sample until clear() (AudioSystem.preload): clearUnused leaves it
        static void pin(const std::string& id);

        // The cached sample of `id` ("<file>#<type>"; nullptr when none)
        static std::shared_ptr<SoLoud::AudioSource> get(const std::string& id);
        // The ids cached for a file (each load type it was loaded with)
        static std::vector<std::string> idsOfFile(const std::string& filename);
        // Hot reload (S05 R13): the entry `id` becomes `sound` and its file's generation goes up.
        // Voices playing the old sample finish on it (their sources hold it); new plays take this
        // one (AudioSystem compares the generation). Main thread.
        static void replace(const std::string& id, std::shared_ptr<SoLoud::AudioSource> sound);
        // a file's generation (every hot reload of it adds one, and every change of its import settings)
        static uint32_t generation(const std::string& filename);
        // the clip's next voice takes a new sample (its import settings changed); voices playing keep theirs
        static void invalidate(const std::string& filename);
        // Makes the sample of `id` ("<file>#<type>", a bare file: Decompress On Load) from
        // `filename` as loadClip would; any thread. Throws with the reason.
        static std::shared_ptr<SoLoud::AudioSource> decode(const std::string& id, const std::string& filename);

        // the clip's length in seconds (a Wav, a WavStream or a AudioClipSource's region)
        static double getLength(SoLoud::AudioSource* sample);
        // a voice's loop point in seconds: the component's, else a AudioClipSource's loop start
        static double getLoopPoint(SoLoud::AudioSource* sample, double requested);

        static std::vector<SoundPoolEntry> getEntries();
        static size_t getMemoryBytes();

        // necessary for engine shutdown
        static void clear();
        // removes only entries no one else references (safe while other scenes are alive)
        static void clearUnused();
    };
}

#endif /* SOUNDPOOL_H */
