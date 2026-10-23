// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SOUNDPOOL_H
#define SOUNDPOOL_H

#include "Engine.h"
#include "jobs/JobSystem.h"

#include <atomic>
#include <cstdint>
#include <vector>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace SoLoud{
    class Wav;
}

namespace jasperengine{

    typedef std::map<std::string, std::shared_ptr<SoLoud::Wav>> sounds_t;

    struct SoundLoadResult {
        std::string id;
        ResourceLoadState state = ResourceLoadState::NotStarted;
        std::string errorMessage;
        std::shared_ptr<SoLoud::Wav> data = nullptr;

    };

    class JASPERENGINE_API SoundPool{
    private:
        static sounds_t& getMap();

        static std::unordered_map<std::string, jobs::Task<std::shared_ptr<SoLoud::Wav>>> pendingBuilds;
        static std::unordered_set<std::string> failedBuilds;
        static std::mutex cacheMutex;
        static std::atomic<bool> shutdownRequested;

        static std::shared_ptr<SoLoud::Wav> loadSoundInternal(const std::string& id, const std::string& filename, bool trackProgress);
        // decodes the file's bytes (on a Background job for async loads)
        static std::shared_ptr<SoLoud::Wav> decodeSound(const std::string& id, const std::string& filename,
                                                       unsigned char* data, unsigned int size, bool trackProgress);
        // waits for every pending load (cancelling those not started first when `cancel`)
        // and drops them; cacheMutex held
        static void quiescePending(bool cancel);

    public:
        static SoundLoadResult loadFromFile(const std::string& id, const std::string& filename);
        // A failed file is not tried again until this or remove()
        static void retry(const std::string& id);

        static void requestShutdown();
        // re-allows async loads after a recreated view
        static void cancelShutdown();

        static void remove(const std::string& id);

        // necessary for engine shutdown
        static void clear();
        // removes only entries no one else references (safe while other scenes are alive)
        static void clearUnused();
    };
}

#endif /* SOUNDPOOL_H */