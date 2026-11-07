// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SoundPool.h"

#include "Engine.h"
#include "Log.h"
#include "io/Data.h"
#include "io/AssetIOService.h"
#include "thread/ResourceProgress.h"

#include "io/File.h"
#include "io/ResourcePack.h"
#include "audio/AudioFileCheck.h"
#include "component/AudioSourceComponent.h"
#if JASPERENGINE_SOUND_LAYER
#include "audio/AudioImport.h"
#include "audio/ClipSource.h"
#endif
#include "audio/SoloudEngineFile.h"
#include "subsystem/AudioSystem.h"

#include "soloud.h"
#include "soloud_wav.h"
#include "soloud_wavstream.h"

#include <cstdio>
#include <filesystem>
#include <vector>

using namespace jasperengine;

std::unordered_map<std::string, jobs::Task<std::shared_ptr<SoLoud::AudioSource>>> SoundPool::pendingBuilds;
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

std::shared_ptr<SoLoud::AudioSource> SoundPool::get(const std::string& id){
    auto it = getMap().find(id);
    return it == getMap().end() ? nullptr : it->second;
}

std::vector<std::string> SoundPool::idsOfFile(const std::string& filename){
    std::vector<std::string> ids;
    for (const auto& [id, sample] : getMap()) {
        std::string file;
        AudioLoadType type;
        parseClipRef(id, file, type);
        if (file == filename && sample) ids.push_back(id);
    }
    return ids;
}

void SoundPool::replace(const std::string& id, std::shared_ptr<SoLoud::AudioSource> sound){
    if (!sound){
        return;
    }
    std::string file;
    AudioLoadType type;
    parseClipRef(id, file, type);
    getMap()[id] = std::move(sound);
    getGenerations()[file]++;
    std::lock_guard<std::mutex> lock(cacheMutex);
    failedBuilds.erase(id);
}

uint32_t SoundPool::generation(const std::string& filename){
    auto it = getGenerations().find(filename);
    return it == getGenerations().end() ? 0 : it->second;
}

void SoundPool::invalidate(const std::string& filename){
    getGenerations()[filename]++;
}

namespace {

    // the head of an Ogg file, enough to reach its first audio page (AudioClipSource's look at whether
    // its seeks can trust the stream's sample positions)
    constexpr size_t OGG_HEAD_BYTES = 256 * 1024;

    // the sample with the clip's import settings, when they change its samples
    std::shared_ptr<SoLoud::AudioSource> withImportSettings(std::shared_ptr<SoLoud::AudioSource> sample, const std::string& filename,
                                                            const unsigned char* bytes, size_t size){
#if !JASPERENGINE_SOUND_LAYER
        (void)filename;
        (void)bytes;
        (void)size;
        return sample;
#else
        const std::optional<ClipImportSettings> settings = AudioImportTable::find(filename);
        if (!settings || settings->playsAsFile() || !sample){
            return sample;
        }
        const auto* stream = dynamic_cast<SoLoud::WavStream*>(sample.get());
        std::vector<unsigned char> head;
        if (stream && stream->mFiletype == SoLoud::WAVSTREAM_OGG && !bytes){
            SoloudEngineFile file(filename);
            if (file.isOpen()){
                head.resize(OGG_HEAD_BYTES);
                head.resize(file.read(head.data(), (unsigned int)head.size()));
                bytes = head.data();
                size = head.size();
            }
        }
        auto clip = std::make_shared<AudioClipSource>(std::move(sample), *settings, bytes, size);
        clip->setSingleInstance(false);
        return clip;
#endif
    }

}

std::shared_ptr<SoLoud::AudioSource> SoundPool::decode(const std::string& id, const std::string& filename){
    std::string file;
    AudioLoadType type;
    parseClipRef(id, file, type);
    if (type == AudioLoadType::Auto) type = AudioLoadType::DecompressOnLoad;
    return loadSoundInternal(id, filename, type, false);
}

SoundLoadResult SoundPool::loadClip(const std::string& filename, AudioLoadType requested){
    // a game's import settings, before its first clip (the editor registers a project's itself)
#if JASPERENGINE_SOUND_LAYER
    static std::once_flag importsLoaded;
    std::call_once(importsLoaded, [](){ AudioImportTable::load(AudioImportTable::FILE_NAME); });
#endif
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

    // on the deterministic driver a start never waits a number of frames that varies
    if (Engine::isAsyncLoading() && AudioSystem::getOutputMode() != AudioOutputMode::Deterministic) {
        std::lock_guard<std::mutex> lock(cacheMutex);

        if (shutdownRequested.load()) {
            throw std::runtime_error("Shutdown requested");
        }

        auto it = pendingBuilds.find(id);
        if (it != pendingBuilds.end()) {
            jobs::Task<std::shared_ptr<SoLoud::AudioSource>>& task = it->second;
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

        if (loadType == AudioLoadType::Streaming) {
            // a stream opens the file (each voice reads it): a Background job, no bytes read here
            jobs::SubmitOptions options;
            options.lane = jobs::Lane::Background;
            options.name = "sound-stream";
            options.tags = jobs::Tag::Asset;
            pendingBuilds[id] = jobs::submit([id, filename, loadType]() {
                return loadSoundInternal(id, filename, loadType, true);
            }, options);
        } else {
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
                [id, filename, loadType](std::vector<uint8_t>&& bytes) {
                    return decodeSound(id, filename, loadType, bytes.data(), (unsigned int)bytes.size(), true);
                }
            );
        }
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

SoundLoadResult SoundPool::loadClipNow(const std::string& filename, AudioLoadType requested){
    SoundLoadResult result = loadClip(filename, requested);
    if (result.state != ResourceLoadState::Loading) return result;
    try {
        std::shared_ptr<SoLoud::AudioSource> sample = loadSoundInternal(result.id, filename, result.loadType, false);
        getMap()[result.id] = sample;
        result.state = ResourceLoadState::Finished;
        result.data = sample;
    } catch (const std::exception& e) {
        result.state = ResourceLoadState::Failed;
        result.errorMessage = e.what();
    }
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
        std::string problem;
        if (!stream->open(filename, &res, &problem)) {
            if (res == SoLoud::FILE_NOT_FOUND) {
                failBuild("Sound file not found: " + filename);
            }
            if (!problem.empty()) {
                failBuild("Sound file '" + filename + "' cannot be played: " + problem);
            }
        }
        sound = stream;
    } else {
        Data filedata;

        if (filedata.open(filename.c_str()) != FileErrors::FILEDATA_OK){
            failBuild("Sound file not found: " + filename);
        }
        return decodeSound(id, filename, loadType, filedata.getMemPtr(), filedata.length(), trackProgress);
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

    return withImportSettings(std::move(sound), filename, nullptr, 0);
}

std::shared_ptr<SoLoud::AudioSource> SoundPool::decodeSound(const std::string& id, const std::string& filename,
                                                             AudioLoadType loadType, unsigned char* data,
                                                             unsigned int size, bool trackProgress){
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
        ResourceProgress::updateProgress(buildId, 0.7f);
    }

    // what SoLoud's decoders would trust and must not (AudioFileCheck): refused with the reason;
    // a WAV's sizes become the bytes it holds (written to a pipe they say 0xFFFFFFFF)
    AudioFileFormat format;
    std::string problem;
    const AudioDecode decode = loadType == AudioLoadType::CompressedInMemory ? AudioDecode::Stream : AudioDecode::Whole;
    if (!AudioFileCheck::prepare(data, size, decode, format, problem)) {
        failBuild("Sound file '" + filename + "' cannot be played: " + problem);
    }

    std::shared_ptr<SoLoud::AudioSource> sound;
    SoLoud::result res = SoLoud::SO_NO_ERROR;
    if (loadType == AudioLoadType::CompressedInMemory) {
        // the file's bytes (copied), decoded while it plays
        auto stream = std::make_shared<SoLoud::WavStream>();
        res = stream->loadMem(data, size, true, true);
        sound = stream;
    } else {
        auto wav = std::make_shared<SoLoud::Wav>();
        res = wav->loadMem(data, size, false, false);
        sound = wav;
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

    return withImportSettings(std::move(sound), filename, data, size);
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
    const std::string base = filename + "#" + loadTypeName(resolved);
#if JASPERENGINE_SOUND_LAYER
    const std::optional<ClipImportSettings> settings = AudioImportTable::find(filename);
    if (!settings || settings->playsAsFile()) return base;
    char hash[24];
    std::snprintf(hash, sizeof(hash), "#i%016llx", (unsigned long long)settings->hash());
    return base + hash;
#else
    return base;
#endif
}

void SoundPool::parseClipRef(const std::string& ref, std::string& filename, AudioLoadType& type){
    filename = ref;
    type = AudioLoadType::Auto;
    size_t hash = ref.rfind('#');
    if (hash == std::string::npos) return;
    // the import settings' suffix names a sample of the same file and type
    if (ref.compare(hash, 2, "#i") == 0) {
        const size_t before = ref.rfind('#', hash - 1);
        if (before != std::string::npos) {
            AudioLoadType parsed;
            if (hash > 0 && loadTypeFromName(ref.substr(before + 1, hash - before - 1), parsed)) {
                filename = ref.substr(0, before);
                type = parsed;
                return;
            }
        }
    }
    AudioLoadType parsed;
    if (loadTypeFromName(ref.substr(hash + 1), parsed)) {
        filename = ref.substr(0, hash);
        type = parsed;
    }
}

double SoundPool::getLength(SoLoud::AudioSource* sample){
    if (auto* wav = dynamic_cast<SoLoud::Wav*>(sample)) return wav->getLength();
    if (auto* stream = dynamic_cast<SoLoud::WavStream*>(sample)) return stream->getLength();
#if JASPERENGINE_SOUND_LAYER
    if (auto* clip = dynamic_cast<AudioClipSource*>(sample)) return clip->getLength();
#endif
    return 0.0;
}

double SoundPool::getLoopPoint(SoLoud::AudioSource* sample, double requested){
    if (requested != 0.0) return requested;
#if JASPERENGINE_SOUND_LAYER
    auto* clip = dynamic_cast<AudioClipSource*>(sample);
    return clip ? clip->getDefaultLoopPoint() : 0.0;
#else
    (void)sample;
    return 0.0;
#endif
}

std::vector<SoundPoolEntry> SoundPool::getEntries(){
    std::vector<SoundPoolEntry> entries;
    for (const auto& [id, sample] : getMap()) {
        if (!sample) continue;
        SoundPoolEntry entry;
        entry.id = id;
        parseClipRef(id, entry.file, entry.loadType);
        entry.length = getLength(sample.get());
        // what the clip holds is its sample's: the wrapper adds a voice's state, no bytes
        SoLoud::AudioSource* held = sample.get();
#if JASPERENGINE_SOUND_LAYER
        if (auto* clip = dynamic_cast<AudioClipSource*>(held)) held = clip->getSample().get();
#endif
        if (auto* wav = dynamic_cast<SoLoud::Wav*>(held)) {
            entry.memoryBytes = (size_t)wav->mSampleCount * wav->mChannels * sizeof(float);
        } else if (auto* stream = dynamic_cast<SoLoud::WavStream*>(held)) {
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
        quiescePending(false);
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
