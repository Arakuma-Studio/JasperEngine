// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "component/AudioSourceComponent.h"

#if JASPERENGINE_SOUND_LAYER

#include "AudioImport.h"

#include "Log.h"
#include "io/Data.h"
#include "pool/SoundPool.h"

#include "json.hpp"

#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

using namespace jasperengine;

bool ClipImportSettings::operator==(const ClipImportSettings& other) const{
    return gainDb == other.gainDb &&
           trimHeadFrames == other.trimHeadFrames &&
           trimTailFrames == other.trimTailFrames &&
           fadeInFrames == other.fadeInFrames &&
           fadeOutFrames == other.fadeOutFrames &&
           loopStartFrame == other.loopStartFrame &&
           loopEndFrame == other.loopEndFrame &&
           bpm == other.bpm &&
           beatsPerBar == other.beatsPerBar &&
           firstBeatFrame == other.firstBeatFrame;
}

bool ClipImportSettings::isDefault() const{
    return *this == ClipImportSettings();
}

bool ClipImportSettings::playsAsFile() const{
    return gainDb == 0.0f && trimHeadFrames == 0 && trimTailFrames == 0 && fadeInFrames == 0 && fadeOutFrames == 0 &&
           loopStartFrame == -1 && loopEndFrame == -1;
}

uint64_t ClipImportSettings::hash() const{
    uint64_t h = 14695981039346656037ull;
    auto add = [&h](const void* data, size_t size){
        const unsigned char* bytes = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; i++){
            h ^= bytes[i];
            h *= 1099511628211ull;
        }
    };
    // by value through fixed-width integers, so the byte order of the machine shows nowhere
    auto addInteger = [&add](uint64_t value){
        unsigned char bytes[8];
        for (int i = 0; i < 8; i++) bytes[i] = (unsigned char)(value >> (8 * i));
        add(bytes, sizeof(bytes));
    };
    auto addFloat = [&addInteger](float value){
        uint32_t bits = 0;
        // -0 and 0 are one gain
        const float normal = value == 0.0f ? 0.0f : value;
        std::memcpy(&bits, &normal, sizeof(bits));
        addInteger(bits);
    };
    addFloat(gainDb);
    addInteger(trimHeadFrames);
    addInteger(trimTailFrames);
    addInteger(fadeInFrames);
    addInteger(fadeOutFrames);
    addInteger((uint64_t)loopStartFrame);
    addInteger((uint64_t)loopEndFrame);
    addFloat(bpm);
    addInteger((uint64_t)(int64_t)beatsPerBar);
    addInteger((uint64_t)firstBeatFrame);
    return h;
}

float jasperengine::clipFadeWeight(uint32_t k, uint32_t n, bool fadeIn){
    if (k >= n){
        return 1.0f;
    }
    const double pi = 3.14159265358979323846;
    if (fadeIn){
        return (float)(0.5 * (1.0 - std::cos(pi * (double)k / (double)n)));
    }
    return (float)(0.5 * (1.0 + std::cos(pi * (double)(k + 1) / (double)n)));
}

namespace {

    std::mutex importTableMutex;
    std::map<std::string, ClipImportSettings> importTable;
    // an empty table costs a look-up nothing: the pool asks on every clip it loads
    std::atomic<bool> importTableEmpty{true};

}

bool AudioImportTable::parse(const std::string& text, std::string* error){
    auto fail = [error](const std::string& why){
        if (error) *error = why;
        return false;
    };
    const nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) return fail("not JSON");
    if (root.value("format", std::string()) != FORMAT) return fail(std::string("its format is not ") + FORMAT);
    if (root.value("version", 0) > VERSION) return fail("written by a newer engine (version " + std::to_string(root.value("version", 0)) + ")");
    const auto clips = root.find("clips");
    if (clips == root.end() || !clips->is_object()) return fail("no clips");

    std::map<std::string, ClipImportSettings> parsed;
    for (auto it = clips->begin(); it != clips->end(); ++it){
        const nlohmann::json& clip = it.value();
        if (!clip.is_object()) return fail("the clip " + it.key() + " is not an object");
        ClipImportSettings settings;
        settings.gainDb = clip.value("gain_db", 0.0f);
        settings.trimHeadFrames = clip.value("trim_head_frames", (uint32_t)0);
        settings.trimTailFrames = clip.value("trim_tail_frames", (uint32_t)0);
        settings.fadeInFrames = clip.value("fade_in_frames", (uint32_t)0);
        settings.fadeOutFrames = clip.value("fade_out_frames", (uint32_t)0);
        settings.loopStartFrame = clip.value("loop_start_frame", (int64_t)-1);
        settings.loopEndFrame = clip.value("loop_end_frame", (int64_t)-1);
        settings.bpm = clip.value("bpm", 0.0f);
        settings.beatsPerBar = clip.value("beats_per_bar", 4);
        settings.firstBeatFrame = clip.value("first_beat_frame", (int64_t)0);
        parsed[it.key()] = settings;
    }
    std::lock_guard<std::mutex> lock(importTableMutex);
    importTable = std::move(parsed);
    importTableEmpty.store(importTable.empty());
    return true;
}

bool AudioImportTable::load(const std::string& assetPath){
    Data data;
    if (data.open(assetPath.c_str()) != FileErrors::FILEDATA_OK){
        return true;
    }
    std::string text;
    text.assign(reinterpret_cast<const char*>(data.getMemPtr()), data.length());
    std::string error;
    if (!parse(text, &error)){
        Log::error("%s: %s", assetPath.c_str(), error.c_str());
        return false;
    }
    return true;
}

void AudioImportTable::set(const std::string& clip, const ClipImportSettings& settings){
    {
        std::lock_guard<std::mutex> lock(importTableMutex);
        auto it = importTable.find(clip);
        if (settings.isDefault()){
            if (it == importTable.end()) return;
            importTable.erase(it);
        }else{
            if (it != importTable.end() && it->second == settings) return;
            importTable[clip] = settings;
        }
        importTableEmpty.store(importTable.empty());
    }
    SoundPool::invalidate(clip);
}

void AudioImportTable::clear(){
    std::vector<std::string> clips;
    {
        std::lock_guard<std::mutex> lock(importTableMutex);
        for (const auto& [clip, settings] : importTable) clips.push_back(clip);
        importTable.clear();
        importTableEmpty.store(true);
    }
    for (const std::string& clip : clips) SoundPool::invalidate(clip);
}

std::optional<ClipImportSettings> AudioImportTable::find(const std::string& clip){
    if (importTableEmpty.load()) return std::nullopt;
    std::lock_guard<std::mutex> lock(importTableMutex);
    auto it = importTable.find(clip);
    if (it == importTable.end()) return std::nullopt;
    return it->second;
}

bool AudioImportTable::isEmpty(){
    return importTableEmpty.load();
}

#endif // JASPERENGINE_SOUND_LAYER
