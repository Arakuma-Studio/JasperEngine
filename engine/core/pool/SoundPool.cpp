// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SoundPool.h"

#include "Engine.h"
#include "Log.h"
#include "io/Data.h"
#include "io/AssetIOService.h"
#include "thread/ResourceProgress.h"

#include "soloud.h"
#include "soloud_wav.h"

#include <filesystem>
#include <vector>

using namespace jasperengine;

std::unordered_map<std::string, jobs::Task<std::shared_ptr<SoLoud::Wav>>> SoundPool::pendingBuilds;
std::unordered_set<std::string> SoundPool::failedBuilds;
std::mutex SoundPool::cacheMutex;
std::atomic<bool> SoundPool::shutdownRequested{false};

sounds_t& SoundPool::getMap(){
    //To prevent similar problem of static init fiasco but on deinitialization
    //https://isocpp.org/wiki/faq/ctors#static-init-order-on-first-use
    static sounds_t* map = new sounds_t();
    return *map;
};

std::unordered_map<std::string, uint32_t>& SoundPool::getGenerations(){
    static auto* generations = new std::unordered_map<std::string, uint32_t>();
    return *generations;
}

std::shared_ptr<SoLoud::Wav> SoundPool::get(const std::string& id){
    auto it = getMap().find(id);
    return it == getMap().end() ? nullptr : it->second;
}

void SoundPool::replace(const std::string& id, std::shared_ptr<SoLoud::Wav> sound){
    if (!sound){
        return;
    }
    getMap()[id] = std::move(sound);
    getGenerations()[id]++;
    std::lock_guard<std::mutex> lock(cacheMutex);
    failedBuilds.erase(id);
}

uint32_t SoundPool::generation(const std::string& id){
    auto it = getGenerations().find(id);
    return it == getGenerations().end() ? 0 : it->second;
}

std::shared_ptr<SoLoud::Wav> SoundPool::decode(const std::string& id, const std::string& filename){
    return loadSoundInternal(id, filename, false);
}

SoundLoadResult SoundPool::loadFromFile(const std::string& id, const std::string& filename){
    auto& shared = getMap()[id];

    SoundLoadResult result;
    result.id = id;

    if (shared){
        result.state = ResourceLoadState::Finished;
        result.data = shared;
        return result;
    }

    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        if (failedBuilds.count(id)) {
            result.state = ResourceLoadState::Failed;
            return result;
        }
    }

    if (Engine::isAsyncLoading()) {
        std::lock_guard<std::mutex> lock(cacheMutex);

        if (shutdownRequested.load()) {
            throw std::runtime_error("Shutdown requested");
        }

        auto it = pendingBuilds.find(id);
        if (it != pendingBuilds.end()) {
            jobs::Task<std::shared_ptr<SoLoud::Wav>>& task = it->second;
            if (!task.isReady()) {
                result.state = ResourceLoadState::Loading;
                return result;
            }
            try {
                shared = task.get();
                pendingBuilds.erase(it);

                result.state = ResourceLoadState::Finished;
                result.data = shared;
                return result;
            } catch (const std::exception& e) {
                pendingBuilds.erase(it);
                failedBuilds.insert(id);

                result.state = ResourceLoadState::Failed;
                result.errorMessage = e.what();
                return result;
            }
        }

        std::string soundName = std::filesystem::path(filename).filename().string();
        uint64_t buildId = std::hash<std::string>{}(id);

        ResourceProgress::startBuild(buildId, ResourceType::Sound, soundName);

        // read on an asset reader thread, decoded on a Background job
        AssetIOService::ReadRequest request;
        request.path = filename;
        request.priority = IOPriority::Normal;
        request.name = "sound-decode";
        request.onFailure = [buildId, filename](const std::string& error) {
            ResourceProgress::failBuild(buildId);
            if (!AssetIOService::isCancellation(error))
                Log::error("Sound file not loaded: %s (%s)", filename.c_str(), error.c_str());
        };
        pendingBuilds[id] = AssetIOService::instance().submitRead(request,
            [id, filename](std::vector<uint8_t>&& bytes) {
                return decodeSound(id, filename, bytes.data(), (unsigned int)bytes.size(), true);
            }
        );
    } else {
        try {
            shared = loadSoundInternal(id, filename, false);

            result.state = ResourceLoadState::Finished;
            result.data = shared;
            return result;
        } catch (const std::exception& e) {
            {
                std::lock_guard<std::mutex> lock(cacheMutex);
                failedBuilds.insert(id);
            }

            result.state = ResourceLoadState::Failed;
            result.errorMessage = e.what();
            return result;
        }
    }

    result.state = ResourceLoadState::Loading;
    return result;
}

void SoundPool::retry(const std::string& id){
    std::lock_guard<std::mutex> lock(cacheMutex);
    failedBuilds.erase(id);
}

std::shared_ptr<SoLoud::Wav> SoundPool::loadSoundInternal(const std::string& id, const std::string& filename, bool trackProgress){
    uint64_t buildId = std::hash<std::string>{}(id);

    if (trackProgress) {
        if (shutdownRequested.load()) {
            ResourceProgress::failBuild(buildId);
            throw std::runtime_error("Shutdown requested");
        }
        ResourceProgress::updateProgress(buildId, 0.35f);
    }

    Data filedata;

    if (filedata.open(filename.c_str()) != FileErrors::FILEDATA_OK){
        if (trackProgress) {
            ResourceProgress::failBuild(buildId);
        }
        Log::error("Sound file not found: %s", filename.c_str());
        throw std::runtime_error("Sound file not found: " + filename);
    }

    return decodeSound(id, filename, filedata.getMemPtr(), filedata.length(), trackProgress);
}

std::shared_ptr<SoLoud::Wav> SoundPool::decodeSound(const std::string& id, const std::string& filename,
                                                   unsigned char* data, unsigned int size, bool trackProgress){
    uint64_t buildId = std::hash<std::string>{}(id);

    if (trackProgress) {
        if (shutdownRequested.load()) {
            ResourceProgress::failBuild(buildId);
            throw std::runtime_error("Shutdown requested");
        }
        ResourceProgress::updateProgress(buildId, 0.7f);
    }

    auto sound = std::make_shared<SoLoud::Wav>();
    SoLoud::result res = sound->loadMem(data, size, false, false);

    if (res == SoLoud::SOLOUD_ERRORS::FILE_LOAD_FAILED){
        if (trackProgress) {
            ResourceProgress::failBuild(buildId);
        }
        Log::error("Sound file type of '%s' could not be loaded", filename.c_str());
        throw std::runtime_error("Sound file type of '" + filename + "' could not be loaded");
    }else if (res == SoLoud::SOLOUD_ERRORS::OUT_OF_MEMORY){
        if (trackProgress) {
            ResourceProgress::failBuild(buildId);
        }
        Log::error("Out of memory when loading '%s'", filename.c_str());
        throw std::runtime_error("Out of memory when loading '" + filename + "'");
    }else if (res == SoLoud::SOLOUD_ERRORS::UNKNOWN_ERROR){
        if (trackProgress) {
            ResourceProgress::failBuild(buildId);
        }
        Log::error("Unknown error when loading '%s'", filename.c_str());
        throw std::runtime_error("Unknown error when loading '" + filename + "'");
    }

    sound->setSingleInstance(true);
    sound->setVolume(1.0);

    if (trackProgress) {
        if (shutdownRequested.load()) {
            ResourceProgress::failBuild(buildId);
            throw std::runtime_error("Shutdown requested");
        }
        ResourceProgress::updateProgress(buildId, 1.0f);
        ResourceProgress::completeBuild(buildId);
    }

    return sound;
}

void SoundPool::quiescePending(bool cancel){
    for (auto& [id, task] : pendingBuilds) {
        if (cancel && task.cancel()) {
            ResourceProgress::failBuild(std::hash<std::string>{}(id));
        }
        task.wait();
    }
    pendingBuilds.clear();
}

void SoundPool::requestShutdown(){
    std::lock_guard<std::mutex> lock(cacheMutex);
    shutdownRequested = true;
    quiescePending(true);
}

void SoundPool::cancelShutdown(){
    std::lock_guard<std::mutex> lock(cacheMutex);
    shutdownRequested = false;
}

void SoundPool::remove(const std::string& id){
    bool removedPending = false;
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        auto pendingIt = pendingBuilds.find(id);
        if (pendingIt != pendingBuilds.end()) {
            // a load not started never runs; a running one is waited for, so it cannot
            // outlive removal or update a later build that reuses the same resource id
            if (pendingIt->second.cancel()) {
                ResourceProgress::failBuild(std::hash<std::string>{}(id));
            }
            pendingIt->second.wait();
            pendingBuilds.erase(pendingIt);
            removedPending = true;
        }
        failedBuilds.erase(id);
    }

    auto it = getMap().find(id);
    if (it != getMap().end()){
        if (!it->second || it->second.use_count() <= 1){
            getMap().erase(it);
        }
    }else if (!removedPending){
        if (Engine::isViewLoaded()){
            Log::debug("Trying to destroy a non existent sound: %s", id.c_str());
        }
    }
}

void SoundPool::clear(){
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        quiescePending(false);
        failedBuilds.clear();
    }
    getMap().clear();
}

void SoundPool::clearUnused(){
    // Quiesce in-flight loads before sweeping the cache. The worker functions do
    // not acquire cacheMutex, while holding it prevents a replacement load from
    // being registered under the same id during the wait.
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        quiescePending(false);
        failedBuilds.clear();
    }

    std::vector<std::string> ids;
    ids.reserve(getMap().size());
    for (const auto& [id, _] : getMap()) {
        ids.push_back(id);
    }
    for (const auto& id : ids) {
        remove(id);
    }
}
