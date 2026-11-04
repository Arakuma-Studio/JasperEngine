// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "component/AudioSourceComponent.h"

#if JASPERENGINE_SOUND_LAYER

#include "SoundEventLibrary.h"

#include "Log.h"
#include "asset/AssetMap.h"
#include "audio/SoundEventIO.h"
#include "io/Data.h"
#include "io/FileData.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <map>
#include <random>
#include <unordered_map>

using namespace jasperengine;

namespace {

    struct EventRuntime {
        // shuffle: the bag and the next entry to draw
        std::vector<int> bag;
        size_t bagNext = 0;
        // the clip played last (random, shuffle), -1 before the first
        int last = -1;
        uint64_t sequence = 0;
        // accepted plays since the reset: the next play's index
        uint64_t plays = 0;
        bool started = false;
        uint64_t startCursor = 0;
        double startSeconds = 0.0;
    };

    struct LibraryEntry {
        // null: its file did not read (not tried again until it changes)
        std::unique_ptr<SoundEventAsset> asset;
    };

    struct LibraryState {
        std::map<std::string, LibraryEntry> entries;
        // by SoundEventAsset::key, so a reload keeps it
        std::unordered_map<std::string, EventRuntime> runtime;
        uint64_t seed = 0;
        bool seeded = false;
    };

    // made on first use, never destroyed (a game's scenes can outlive static destruction)
    LibraryState& libraryState() {
        static LibraryState* instance = new LibraryState();
        return *instance;
    }

    std::string normalizedPath(const std::string& path) {
        const std::u8string normal = std::filesystem::path(std::u8string(path.begin(), path.end())).lexically_normal().generic_u8string();
        return std::string(normal.begin(), normal.end());
    }

    bool sameClipList(const SoundEventDef& a, const SoundEventDef& b) {
        if (a.clips.size() != b.clips.size()) return false;
        for (size_t i = 0; i < a.clips.size(); i++) {
            if (a.clips[i].clip.path != b.clips[i].clip.path || a.clips[i].clip.guid != b.clips[i].clip.guid) return false;
        }
        return true;
    }

    // The clips by GUID when the project knows it (a moved clip whose hint is stale still plays)
    void resolveClipPaths(SoundEventDef& def) {
        for (SoundEventClip& clip : def.clips) {
            if (clip.clip.guid.isNull()) continue;
            std::string path;
            if (AssetMap::findPath(clip.clip.guid, path)) clip.clip.path = path;
        }
    }

    const SoundEventAsset* install(const std::string& path, SoundEventDef&& def) {
        LibraryState& state = libraryState();
        resolveClipPaths(def);

        LibraryEntry& entry = state.entries[path];
        auto asset = std::make_unique<SoundEventAsset>();
        asset->path = path;
        const Guid guid = AssetMap::findGuid(path);
        asset->key = guid.isNull() ? path : guid.str();
        if (entry.asset && entry.asset->def && (entry.asset->def->mode != def.mode || !sameClipList(*entry.asset->def, def))) {
            // the plays' indices stay (a seed's sequence does not start over), the picks start over
            EventRuntime& runtime = state.runtime[asset->key];
            runtime.bag.clear();
            runtime.bagNext = 0;
            runtime.last = -1;
            runtime.sequence = 0;
        }
        asset->def = std::make_shared<const SoundEventDef>(std::move(def));
        entry.asset = std::move(asset);
        return entry.asset.get();
    }

    const SoundEventAsset* loadFile(const std::string& path) {
        Data data;
        if (data.open(path.c_str()) != FileErrors::FILEDATA_OK) {
            Log::warn("Sound Event '%s': cannot open the file", path.c_str());
            return nullptr;
        }
        std::string error;
        const SoundEventAsset* asset = SoundEventLibrary::load(path, data.readString(), &error);
        if (!asset) Log::warn("Sound Event '%s': %s", path.c_str(), error.c_str());
        return asset;
    }

    void ensureSeed(LibraryState& state) {
        if (state.seeded) return;
        std::random_device device;
        state.seed = ((uint64_t)device() << 32) ^ (uint64_t)device();
        state.seeded = true;
    }

    // random: one draw, the clip whose cumulative weight interval holds u x total, over the clips
    // not left out, in list order
    int pickRandom(const SoundEventDef& def, const EventRuntime& runtime, SoundEventRng& rng) {
        const int count = (int)def.clips.size();
        int weighted = 0;
        for (const SoundEventClip& clip : def.clips) {
            if (clip.weight > 0.0f) weighted++;
        }
        const int excluded = (def.avoidRepeat && weighted > 1) ? runtime.last : -1;

        double total = 0.0;
        for (int i = 0; i < count; i++) {
            if (i != excluded && def.clips[i].weight > 0.0f) total += (double)def.clips[i].weight;
        }
        const double target = rng.uniform() * total;
        double sum = 0.0;
        int chosen = 0;
        for (int i = 0; i < count; i++) {
            if (i == excluded || def.clips[i].weight <= 0.0f) continue;
            sum += (double)def.clips[i].weight;
            chosen = i;
            if (target < sum) break;
        }
        return chosen;
    }

    // shuffle: from the front of the bag; an empty bag is refilled with a Fisher-Yates permutation,
    // and its first two entries swap when the first is the clip played last
    int pickShuffle(const SoundEventDef& def, EventRuntime& runtime, SoundEventRng& rng) {
        const int count = (int)def.clips.size();
        if (runtime.bagNext >= runtime.bag.size() || (int)runtime.bag.size() != count) {
            runtime.bag.resize(count);
            for (int i = 0; i < count; i++) runtime.bag[i] = i;
            for (int i = count - 1; i >= 1; i--) {
                const int j = (int)std::floor(rng.uniform() * (double)(i + 1));
                std::swap(runtime.bag[i], runtime.bag[j]);
            }
            if (count >= 2 && runtime.bag[0] == runtime.last) std::swap(runtime.bag[0], runtime.bag[1]);
            runtime.bagNext = 0;
        }
        return runtime.bag[runtime.bagNext++];
    }

    SoundEventPick pickWith(const SoundEventDef& def, EventRuntime& runtime, const std::string& key, uint64_t seed) {
        SoundEventPick result;
        result.playIndex = runtime.plays;
        SoundEventRng rng(seed, key, runtime.plays);
        const int count = (int)def.clips.size();
        if (count > 0) {
            int clip = 0;
            switch (def.mode) {
                case SoundEventMode::Random:
                    clip = pickRandom(def, runtime, rng);
                    result.clips.push_back(clip);
                    runtime.last = clip;
                    break;
                case SoundEventMode::Shuffle:
                    clip = pickShuffle(def, runtime, rng);
                    result.clips.push_back(clip);
                    runtime.last = clip;
                    break;
                case SoundEventMode::Sequence:
                    clip = (int)(runtime.sequence % (uint64_t)count);
                    runtime.sequence++;
                    result.clips.push_back(clip);
                    runtime.last = clip;
                    break;
                case SoundEventMode::Layer:
                    for (int i = 0; i < count; i++) result.clips.push_back(i);
                    break;
            }
        }
        // exactly three draws, whether or not a range is empty
        result.pitchSemitones = rng.range((double)def.pitchMin, (double)def.pitchMax);
        result.volumeDb = rng.range((double)def.volumeMin, (double)def.volumeMax);
        result.delayMs = rng.range((double)def.delayMin, (double)def.delayMax);
        runtime.plays++;
        return result;
    }

}

bool SoundEventLibrary::isGuid(const std::string& text) {
    return text.size() == 32 && Guid::isValidText(text);
}

bool SoundEventLibrary::isEventName(const std::string& text) {
    if (isGuid(text)) {
        AssetType type = AssetType::Unknown;
        std::string path;
        return AssetMap::findPath(Guid::parse(text), path, &type) && type == AssetType::SoundEvent;
    }
    constexpr const char* extension = ".soundevent";
    constexpr size_t length = 11;
    if (text.size() <= length) return false;
    std::string tail = text.substr(text.size() - length);
    std::transform(tail.begin(), tail.end(), tail.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return tail == extension;
}

const SoundEventAsset* SoundEventLibrary::find(const std::string& guidOrPath) {
    if (guidOrPath.empty()) return nullptr;
    std::string path = guidOrPath;
    if (isGuid(guidOrPath)) {
        AssetType type = AssetType::Unknown;
        if (!AssetMap::findPath(Guid::parse(guidOrPath), path, &type) || type != AssetType::SoundEvent) return nullptr;
    }
    path = normalizedPath(path);
    LibraryState& state = libraryState();
    auto found = state.entries.find(path);
    if (found != state.entries.end()) return found->second.asset.get();
    const SoundEventAsset* asset = loadFile(path);
    // tried once: a missing file costs no more reads until it changes
    if (!asset) state.entries[path].asset.reset();
    return asset;
}

const SoundEventAsset* SoundEventLibrary::load(const std::string& path, const std::string& json, std::string* error) {
    SoundEventDef def;
    if (!SoundEventIO::parse(json, def, error)) return nullptr;
    const std::vector<std::string> problems = SoundEventIO::validate(def);
    if (!problems.empty()) {
        if (error) *error = problems.front();
        return nullptr;
    }
    return install(normalizedPath(path), std::move(def));
}

void SoundEventLibrary::reload(const std::string& path) {
    LibraryState& state = libraryState();
    const std::string normal = normalizedPath(path);
    auto found = state.entries.find(normal);
    // never played: its first use reads it
    if (found == state.entries.end()) return;
    if (!loadFile(normal)) found->second.asset.reset();
}

void SoundEventLibrary::clear() {
    LibraryState& state = libraryState();
    state.entries.clear();
    state.runtime.clear();
    state.seeded = false;
}

void SoundEventLibrary::resetRuntime() {
    LibraryState& state = libraryState();
    state.runtime.clear();
    state.seeded = false;
}

void SoundEventLibrary::setSeed(uint64_t seed) {
    LibraryState& state = libraryState();
    state.seed = seed;
    state.seeded = true;
}

uint64_t SoundEventLibrary::getSeed() {
    LibraryState& state = libraryState();
    ensureSeed(state);
    return state.seed;
}

bool SoundEventLibrary::cooledDown(const SoundEventAsset& event, const SoundEventClock& now) {
    if (!(event.def->cooldownMs > 0.0f)) return true;
    LibraryState& state = libraryState();
    auto found = state.runtime.find(event.key);
    if (found == state.runtime.end() || !found->second.started) return true;
    const EventRuntime& runtime = found->second;
    if (now.samples) {
        // a clock that went back (it restarted) is past any cooldown
        if (now.cursor < runtime.startCursor) return true;
        const long long samples = std::llround((double)event.def->cooldownMs * (double)now.rate / 1000.0);
        return (long long)(now.cursor - runtime.startCursor) >= samples;
    }
    return now.seconds - runtime.startSeconds >= (double)event.def->cooldownMs / 1000.0;
}

SoundEventPick SoundEventLibrary::pick(const SoundEventAsset& event, const SoundEventClock& now) {
    LibraryState& state = libraryState();
    ensureSeed(state);
    EventRuntime& runtime = state.runtime[event.key];
    SoundEventPick result = pickWith(*event.def, runtime, event.key, state.seed);
    runtime.started = true;
    runtime.startCursor = now.cursor;
    runtime.startSeconds = now.seconds;
    return result;
}

std::vector<SoundEventPick> SoundEventLibrary::preview(const SoundEventDef& def, const std::string& key, uint64_t seed, int count) {
    std::vector<SoundEventPick> picks;
    EventRuntime runtime;
    for (int i = 0; i < count; i++) picks.push_back(pickWith(def, runtime, key, seed));
    return picks;
}

#endif // JASPERENGINE_SOUND_LAYER
