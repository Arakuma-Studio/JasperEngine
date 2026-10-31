// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SoundPool.h"

#include "Engine.h"
#include "Log.h"
#include "io/Data.h"
#include "thread/ResourceProgress.h"
#include "thread/ThreadPoolManager.h"

#include "io/File.h"
#include "io/ResourcePack.h"
#include "audio/SoloudEngineFile.h"

#include "soloud.h"
#include "soloud_wav.h"
#include "soloud_wavstream.h"

#include <filesystem>
#include <vector>

using namespace jasperengine;

std::unordered_map<std::string, std::future<std::shared_ptr<SoLoud::AudioSource>>> SoundPool::pendingBuilds;
std::unordered_set<std::string> SoundPool::failedBuilds;
std::mutex SoundPool::cacheMutex;
std::atomic<bool> SoundPool::shutdownRequested{false};

sounds_t& SoundPool::getMap(){
    //To prevent similar problem of static init fiasco but on deinitialization
    //https://isocpp.org/wiki/faq/ctors#static-init-order-on-first-use
    static sounds_t* map = new sounds_t();
    return *map;
};

SoundLoadResult SoundPool::loadClip(const std::string& filename, AudioLoadType requested){
    const AudioLoadType loadType = resolveLoadType(filename, requested);
    const std::string id = key(filename, loadType);
    auto& shared = getMap()[id];

    SoundLoadResult result;
    result.id = id;
    result.loadType = loadType;

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
            auto& future = it->second;
            if (future.valid() && future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                try {
                    shared = future.get();
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
            } else if (future.valid()) {
                result.state = ResourceLoadState::Loading;
                return result;
            } else {
                pendingBuilds.erase(it);
                result.state = ResourceLoadState::Failed;
                result.errorMessage = "Invalid future";
                return result;
            }
        }

        std::string soundName = std::filesystem::path(filename).filename().string();
        uint64_t buildId = std::hash<std::string>{}(id);

        ResourceProgress::startBuild(buildId, ResourceType::Sound, soundName);

        pendingBuilds[id] = ThreadPoolManager::getInstance().enqueue(
            [id, filename, loadType]() {
                return loadSoundInternal(id, filename, loadType, true);
            }
        );
    } else {
        try {
            shared = loadSoundInternal(id, filename, loadType, false);

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

void SoundPool::retry(const std::string& idOrFile){
    std::lock_guard<std::mutex> lock(cacheMutex);
    failedBuilds.erase(idOrFile);
    // a file name: each of its load types
    const std::string prefix = idOrFile + "#";
    for (auto it = failedBuilds.begin(); it != failedBuilds.end();) {
        if (it->compare(0, prefix.size(), prefix) == 0) it = failedBuilds.erase(it);
        else ++it;
    }
    std::lock_guard<std::mutex> sizeLock(sizeMutex());
    fileSizes().erase(idOrFile);
}

std::shared_ptr<SoLoud::AudioSource> SoundPool::loadSoundInternal(const std::string& id, const std::string& filename, AudioLoadType loadType, bool trackProgress){
    uint64_t buildId = std::hash<std::string>{}(id);

    auto failBuild = [&](const std::string& message) {
        if (trackProgress) {
            ResourceProgress::failBuild(buildId);
        }
        Log::error("%s", message.c_str());
        throw std::runtime_error(message);
    };
    auto checkShutdown = [&]() {
        if (trackProgress && shutdownRequested.load()) {
            ResourceProgress::failBuild(buildId);
            throw std::runtime_error("Shutdown requested");
        }
    };

    if (trackProgress) {
        checkShutdown();
        ResourceProgress::updateProgress(buildId, 0.35f);
    }

    std::shared_ptr<SoLoud::AudioSource> sound;
    SoLoud::result res = SoLoud::SO_NO_ERROR;

    if (loadType == AudioLoadType::Streaming) {
        // each voice reads the file through the engine File (APK assets on Android)
        auto stream = std::make_shared<EngineWavStream>();
        if (!stream->open(filename, &res)) {
            if (res == SoLoud::FILE_NOT_FOUND) {
                failBuild("Sound file not found: " + filename);
            }
        }
        sound = stream;
    } else {
        Data filedata;

        if (filedata.open(filename.c_str()) != FileErrors::FILEDATA_OK){
            failBuild("Sound file not found: " + filename);
        }

        if (trackProgress) {
            checkShutdown();
            ResourceProgress::updateProgress(buildId, 0.7f);
        }

        if (loadType == AudioLoadType::CompressedInMemory) {
            // the file's bytes, decoded while it plays
            auto stream = std::make_shared<SoLoud::WavStream>();
            res = stream->loadMem(filedata.getMemPtr(), filedata.length(), true, true);
            sound = stream;
        } else {
            auto wav = std::make_shared<SoLoud::Wav>();
            res = wav->loadMem(filedata.getMemPtr(), filedata.length(), false, false);
            sound = wav;
        }
    }

    if (res == SoLoud::SOLOUD_ERRORS::FILE_LOAD_FAILED){
        failBuild("Sound file type of '" + filename + "' could not be loaded");
    }else if (res == SoLoud::SOLOUD_ERRORS::OUT_OF_MEMORY){
        failBuild("Out of memory when loading '" + filename + "'");
    }else if (res != SoLoud::SOLOUD_ERRORS::SO_NO_ERROR){
        failBuild("Unknown error when loading '" + filename + "'");
    }

    // shared by every source playing the file: voices of one sample overlap (spec A9)
    sound->setSingleInstance(false);
    sound->setVolume(1.0);

    if (trackProgress) {
        checkShutdown();
        ResourceProgress::updateProgress(buildId, 1.0f);
        ResourceProgress::completeBuild(buildId);
    }

    return sound;
}

std::mutex& SoundPool::sizeMutex(){
    static std::mutex* mutex = new std::mutex();
    return *mutex;
}

std::unordered_map<std::string, uint64_t>& SoundPool::fileSizes(){
    static auto* sizes = new std::unordered_map<std::string, uint64_t>();
    return *sizes;
}

uint64_t SoundPool::fileSize(const std::string& filename){
    {
        std::lock_guard<std::mutex> lock(sizeMutex());
        auto it = fileSizes().find(filename);
        if (it != fileSizes().end()) return it->second;
    }
    uint64_t size = 0;
    if (!ResourcePack::size(filename, size)) {
        File file;
        if (file.open(filename.c_str()) == FileErrors::FILEDATA_OK && file.getFilePtr()) {
            size = file.length();
        }
    }
    std::lock_guard<std::mutex> lock(sizeMutex());
    fileSizes()[filename] = size;
    return size;
}

AudioLoadType SoundPool::resolveLoadType(const std::string& filename, AudioLoadType requested){
    AudioLoadType type = requested;
    if (type == AudioLoadType::Auto) {
        type = fileSize(filename) >= streamingThreshold ? AudioLoadType::Streaming : AudioLoadType::DecompressOnLoad;
    }
    // a packed file has no random access: its bytes stay in memory instead (spec A8)
    if (type == AudioLoadType::Streaming && ResourcePack::contains(filename)) {
        type = AudioLoadType::CompressedInMemory;
    }
    return type;
}

const char* SoundPool::loadTypeName(AudioLoadType type){
    switch (type) {
        case AudioLoadType::Auto: return "auto";
        case AudioLoadType::DecompressOnLoad: return "decompress_on_load";
        case AudioLoadType::CompressedInMemory: return "compressed_in_memory";
        case AudioLoadType::Streaming: return "streaming";
    }
    return "auto";
}

bool SoundPool::loadTypeFromName(const std::string& name, AudioLoadType& type){
    std::string key;
    for (char c : name) {
        if (c == ' ' || c == '_' || c == '-') continue;
        key += (char)std::tolower((unsigned char)c);
    }
    if (key == "auto") type = AudioLoadType::Auto;
    else if (key == "decompressonload" || key == "decompress") type = AudioLoadType::DecompressOnLoad;
    else if (key == "compressedinmemory" || key == "compressed") type = AudioLoadType::CompressedInMemory;
    else if (key == "streaming" || key == "stream") type = AudioLoadType::Streaming;
    else return false;
    return true;
}

std::string SoundPool::key(const std::string& filename, AudioLoadType resolved){
    return filename + "#" + loadTypeName(resolved);
}

void SoundPool::parseClipRef(const std::string& ref, std::string& filename, AudioLoadType& type){
    filename = ref;
    type = AudioLoadType::Auto;
    const size_t hash = ref.rfind('#');
    if (hash == std::string::npos) return;
    AudioLoadType parsed;
    if (loadTypeFromName(ref.substr(hash + 1), parsed)) {
        filename = ref.substr(0, hash);
        type = parsed;
    }
}

double SoundPool::getLength(SoLoud::AudioSource* sample){
    if (auto* wav = dynamic_cast<SoLoud::Wav*>(sample)) return wav->getLength();
    if (auto* stream = dynamic_cast<SoLoud::WavStream*>(sample)) return stream->getLength();
    return 0.0;
}

std::vector<SoundPoolEntry> SoundPool::getEntries(){
    std::vector<SoundPoolEntry> entries;
    for (const auto& [id, sample] : getMap()) {
        if (!sample) continue;
        SoundPoolEntry entry;
        entry.id = id;
        parseClipRef(id, entry.file, entry.loadType);
        entry.length = getLength(sample.get());
        if (auto* wav = dynamic_cast<SoLoud::Wav*>(sample.get())) {
            entry.memoryBytes = (size_t)wav->mSampleCount * wav->mChannels * sizeof(float);
        } else if (auto* stream = dynamic_cast<SoLoud::WavStream*>(sample.get())) {
            entry.memoryBytes = stream->mMemFile ? stream->mMemFile->length() : 0;
        }
        entries.push_back(entry);
    }
    return entries;
}

size_t SoundPool::getMemoryBytes(){
    size_t total = 0;
    for (const SoundPoolEntry& entry : getEntries()) total += entry.memoryBytes;
    return total;
}

void SoundPool::requestShutdown(){
    std::lock_guard<std::mutex> lock(cacheMutex);
    shutdownRequested = true;

    for (auto& [id, future] : pendingBuilds) {
        if (future.valid()) {
            future.wait();
        }
    }
    pendingBuilds.clear();
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
            // ThreadPoolManager uses packaged_task; destroying its future does
            // not cancel the queued task. Wait so it cannot outlive removal or
            // update a later build that reuses the same resource id.
            if (pendingIt->second.valid()) {
                pendingIt->second.wait();
            }
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

sounds_t& SoundPool::getPinned(){
    static sounds_t* map = new sounds_t();
    return *map;
}

void SoundPool::pin(const std::string& id){
    auto it = getMap().find(id);
    if (it != getMap().end() && it->second) getPinned()[id] = it->second;
}

void SoundPool::clear(){
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        for (auto& [id, future] : pendingBuilds) {
            if (future.valid()) {
                future.wait();
            }
        }
        pendingBuilds.clear();
        failedBuilds.clear();
    }
    getPinned().clear();
    getMap().clear();
    std::lock_guard<std::mutex> lock(sizeMutex());
    fileSizes().clear();
}

void SoundPool::clearUnused(){
    // Quiesce in-flight loads before sweeping the cache. The worker functions do
    // not acquire cacheMutex, while holding it prevents a replacement load from
    // being registered under the same id during the wait.
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        for (auto& [id, future] : pendingBuilds) {
            if (future.valid()) {
                future.wait();
            }
        }
        pendingBuilds.clear();
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
