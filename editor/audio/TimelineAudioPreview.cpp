// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TimelineAudioPreview.h"

#include "EditorHost.h"
#include "Project.h"
#include "audio/AudioAnalysis.h"
#include "audio/AudioCapture.h"
#include "audio/SoundEvent.h"
#include "audio/SoundEventDocument.h"
#include "audio/SoundEventLibrary.h"
#include "pool/SoundPool.h"
#include "subsystem/AudioSystem.h"
#include "util/AssetDatabase.h"
#include "util/Util.h"

#include "soloud.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    // one analysed clip file: its played length and peak
    struct TimelineClipNumbers {
        std::string stamp;
        bool done = false;
        std::string error;
        double length = 0.0;
        double peakMs = -1.0;
        double headSeconds = 0.0;
    };

    // the analyses, their queue and the worker behind one mutex (AudioWaveform's pattern)
    struct TimelineSoundState {
        std::mutex mutex;
        std::condition_variable wake;
        std::thread thread;
        bool stopping = false;
        fs::path projectDirectory;
        std::map<std::string, TimelineClipNumbers> clips;
        std::deque<std::pair<std::string, fs::path>> queue;
        fs::path cacheDirectory;

        ~TimelineSoundState() {
            {
                std::lock_guard<std::mutex> lock(mutex);
                stopping = true;
                queue.clear();
            }
            wake.notify_all();
            if (thread.joinable()) thread.join();
        }
    };

    TimelineSoundState& timelineSoundState() {
        static TimelineSoundState instance;
        return instance;
    }

    void timelineSoundWork() {
        TimelineSoundState& s = timelineSoundState();
        std::unique_lock<std::mutex> lock(s.mutex);
        while (true) {
            s.wake.wait(lock, [&]() { return s.stopping || !s.queue.empty(); });
            if (s.stopping) return;
            const std::pair<std::string, fs::path> job = s.queue.front();
            s.queue.pop_front();
            const fs::path cache = s.cacheDirectory;
            lock.unlock();
            AudioAnalysis::Json item;
            std::string error;
            TimelineClipNumbers numbers;
            numbers.done = true;
            if (AudioAnalysis::analyzeClip(job.second, cache, item, error)) {
                const AudioAnalysis::Json& lengthOf = item.contains("played") ? item["played"] : item.contains("format") ? item["format"] : item;
                if (lengthOf.is_object() && lengthOf.contains("duration_s") && lengthOf["duration_s"].is_number()) {
                    numbers.length = lengthOf["duration_s"].get<double>();
                }
                const double rate = item.contains("format") && item["format"].is_object() && item["format"].contains("sample_rate") &&
                                    item["format"]["sample_rate"].is_number() ? item["format"]["sample_rate"].get<double>() : 0.0;
                if (rate > 0.0 && item.contains("played") && item["played"].is_object() && item["played"].contains("start_frame") &&
                    item["played"]["start_frame"].is_number()) {
                    numbers.headSeconds = item["played"]["start_frame"].get<double>() / rate;
                }
                if (item.contains("onset") && item["onset"].is_object() && item["onset"].contains("peak_ms") && item["onset"]["peak_ms"].is_number()) {
                    numbers.peakMs = item["onset"]["peak_ms"].get<double>();
                }
            } else {
                numbers.error = error;
            }
            lock.lock();
            if (s.stopping) return;
            auto found = s.clips.find(job.first);
            if (found == s.clips.end()) continue;
            numbers.stamp = found->second.stamp;
            found->second = numbers;
            getEditorHost().enqueueMainThreadTask([]() {});
        }
    }

    std::string fileStamp(const fs::path& file) {
        std::error_code ec;
        const uintmax_t size = fs::file_size(file, ec);
        if (ec) return std::string();
        const auto modified = fs::last_write_time(file, ec);
        return std::to_string(size) + ":" + std::to_string(ec ? 0 : (int64_t)modified.time_since_epoch().count());
    }

    // a clip file's numbers; queued for the worker the first time (and after it changed)
    TimelineClipNumbers clipNumbers(Project* project, const fs::path& file) {
        TimelineSoundState& s = timelineSoundState();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.projectDirectory != project->getProjectPath()) {
            s.clips.clear();
            s.queue.clear();
            s.projectDirectory = project->getProjectPath();
            s.cacheDirectory = AudioAnalysis::cacheDirectory(project->getProjectPath());
        }
        const std::string key = file.lexically_normal().string();
        const std::string stamp = fileStamp(file);
        TimelineClipNumbers missing;
        missing.done = true;
        if (stamp.empty()) {
            missing.error = "the file can't be read";
            return missing;
        }
        auto found = s.clips.find(key);
        if (found != s.clips.end() && found->second.stamp == stamp) return found->second;
        TimelineClipNumbers pending;
        pending.stamp = stamp;
        s.clips[key] = pending;
        s.queue.push_front({key, file.lexically_normal()});
        if (!s.thread.joinable()) s.thread = std::thread(timelineSoundWork);
        s.wake.notify_one();
        return pending;
    }

}

TimelineSoundInfo TimelineSounds::get(Project* project, const std::string& assetPath) {
    TimelineSoundInfo info;
    if (!project || assetPath.empty()) {
        info.pending = false;
        info.error = "no sound";
        return info;
    }
    const fs::path file = project->resolveAssetPath(assetPath);
    if (!Util::isSoundEventFile(assetPath)) {
        const TimelineClipNumbers numbers = clipNumbers(project, file);
        info.pending = !numbers.done;
        info.error = numbers.error;
        info.length = numbers.length;
        info.peakMs = numbers.peakMs;
        info.waveFile = file;
        info.waveOffset = numbers.headSeconds;
        return info;
    }
    info.event = true;
    SoundEventDef def;
    std::string error;
    if (!SoundEventDocument::read(file, def, &error)) {
        info.pending = false;
        info.error = error;
        return info;
    }
    info.stacked = def.clips.size() > 1;
    info.pending = false;
    const bool layer = def.mode == SoundEventMode::Layer;
    for (size_t i = 0; i < def.clips.size(); i++) {
        if (def.clips[i].clip.path.empty()) continue;
        const fs::path clipFile = project->resolveAssetPath(def.clips[i].clip.path);
        const TimelineClipNumbers numbers = clipNumbers(project, clipFile);
        if (!numbers.done) info.pending = true;
        const double delay = layer ? std::max(0.0, (double)def.clips[i].delayMs / 1000.0) : 0.0;
        info.length = std::max(info.length, delay + numbers.length);
        if (info.waveFile.empty()) {
            info.waveFile = clipFile;
            info.waveOffset = numbers.headSeconds - delay;
            info.peakMs = numbers.peakMs >= 0.0 ? numbers.peakMs + delay * 1000.0 : -1.0;
            if (!numbers.error.empty()) info.error = numbers.error;
        }
    }
    if (def.clips.empty()) info.error = "the Sound Event has no clips";
    return info;
}

AudioWaveform::Result TimelineSounds::waveform(Project* project, const TimelineSoundInfo& info) {
    if (info.waveFile.empty()) {
        AudioWaveform::Result none;
        none.pending = false;
        return none;
    }
    return AudioWaveform::get(project, info.waveFile);
}

bool TimelineSounds::peakAlignedTime(const TimelineSoundInfo& info, float playhead, float& time, std::string* why) {
    if (info.pending) {
        if (why) *why = "the sound is still being analysed";
        return false;
    }
    if (info.peakMs < 0.0) {
        if (why) *why = info.error.empty() ? "the sound has no peak (silent)" : info.error;
        return false;
    }
    time = (float)((double)playhead - info.peakMs / 1000.0);
    if (time < 0.0f) {
        if (why) *why = "its peak comes after the playhead: move the playhead later";
        return false;
    }
    return true;
}

bool TimelineSounds::drawBlock(ImDrawList* drawList, Project* project, const TimelineSoundInfo& info, ImVec2 min, ImVec2 max,
                               double secondsPerPixel, ImU32 fill, bool selected, const std::string& label) {
    const float rounding = 2.0f;
    if (info.stacked) {
        // a Sound Event of several clips: the cards behind it
        const float lift = std::min(3.0f, (max.y - min.y) * 0.15f);
        drawList->AddRect(ImVec2(min.x + lift, min.y - lift), ImVec2(max.x + lift, max.y - lift), fill, rounding);
    }
    drawList->AddRectFilled(min, max, (fill & 0x00FFFFFF) | 0xA0000000, rounding);
    bool drawn = false;
    const AudioWaveform::Result wave = waveform(project, info);
    if (wave.data && !wave.data->empty() && secondsPerPixel > 0.0) {
        const AudioWaveform::Data& data = *wave.data;
        const double fileSeconds = data.seconds();
        const float mid = (min.y + max.y) * 0.5f;
        const float half = (max.y - min.y) * 0.45f;
        const ImU32 line = IM_COL32(230, 245, 235, 210);
        drawList->PushClipRect(min, max, true);
        for (float x = min.x; x < max.x; x += 1.0f) {
            const double t0 = info.waveOffset + (double)(x - min.x) * secondsPerPixel;
            const double t1 = t0 + secondsPerPixel;
            if (t1 <= 0.0 || t0 >= fileSeconds || fileSeconds <= 0.0) continue;
            int c0 = (int)std::floor(std::max(0.0, t0) / fileSeconds * AudioWaveform::COLUMNS);
            int c1 = (int)std::floor(std::min(fileSeconds, t1) / fileSeconds * AudioWaveform::COLUMNS);
            c0 = std::clamp(c0, 0, AudioWaveform::COLUMNS - 1);
            c1 = std::clamp(c1, c0, AudioWaveform::COLUMNS - 1);
            int lo = 32767, hi = -32768;
            for (unsigned int channel = 0; channel < data.channels; channel++) {
                for (int c = c0; c <= c1; c++) {
                    lo = std::min(lo, (int)data.min(channel, c));
                    hi = std::max(hi, (int)data.max(channel, c));
                }
            }
            if (hi < lo) continue;
            drawList->AddLine(ImVec2(x + 0.5f, mid - half * (float)hi / 32767.0f), ImVec2(x + 0.5f, mid - half * (float)lo / 32767.0f + 1.0f), line);
        }
        drawList->PopClipRect();
        drawn = true;
    }
    if (info.peakMs >= 0.0 && secondsPerPixel > 0.0) {
        const float x = min.x + (float)(info.peakMs / 1000.0 / secondsPerPixel);
        if (x >= min.x && x <= max.x) drawList->AddLine(ImVec2(x, min.y), ImVec2(x, max.y), IM_COL32(255, 220, 90, 255), 1.5f);
    }
    drawList->AddRect(min, max, selected ? IM_COL32(255, 255, 255, 255) : fill, rounding, 0, selected ? 2.0f : 1.0f);
    if (!label.empty()) {
        drawList->PushClipRect(min, max, true);
        drawList->AddText(ImVec2(min.x + 4.0f, min.y + 1.0f), IM_COL32(255, 255, 255, 230), label.c_str());
        drawList->PopClipRect();
    }
    return drawn;
}

AssetRef TimelineSounds::reference(Project* project, const std::string& assetPath) {
    AssetRef ref(assetPath);
    if (!project || assetPath.empty()) return ref;
    const fs::path relative = project->resolveAssetPath(assetPath).lexically_normal().lexically_relative(project->getProjectPath().lexically_normal());
    ref.guid = project->getAssetDatabase().getAssetGuid(relative);
    return ref;
}

std::vector<std::string> TimelineSounds::projectSounds(Project* project, const std::string& needle) {
    std::vector<std::string> sounds;
    if (!project) return sounds;
    const fs::path root = project->getProjectPath();
    project->getAssetDatabase().forEachAsset([&](const AssetDatabase::AssetInfo& info) {
        if (info.type != AssetType::Audio && info.type != AssetType::SoundEvent) return;
        const fs::path absolute = root / info.path;
        if (!project->isInsideAssetsPath(absolute)) return;
        const std::string path = project->normalizeToAssetsRelative(absolute).generic_string();
        std::string lowered = path;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (!needle.empty() && lowered.find(needle) == std::string::npos) return;
        sounds.push_back(path);
    });
    std::sort(sounds.begin(), sounds.end());
    return sounds;
}

void TimelineSounds::clear() {
    TimelineSoundState& s = timelineSoundState();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.clips.clear();
    s.queue.clear();
    s.projectDirectory.clear();
}

// ---------------------------------------------------------------- the preview

TimelineAudioPreview::~TimelineAudioPreview() {
    stopAll();
}

std::string TimelineAudioPreview::blocker() {
    if (AudioCapture::isActive() || AudioSystem::isDeterministicHeld()) return "Audio preview is off while Play captures audio";
    return std::string();
}

uint32_t TimelineAudioPreview::delayFrames(double seconds, double span, double frameSeconds) {
    if (!(span > 0.0) || !std::isfinite(seconds)) return 0;
    const double rate = AudioSystem::isInited() ? (double)AudioSystem::getSoloud().getBackendSamplerate() : 48000.0;
    const double fraction = std::min(std::max(seconds / span, 0.0), 1.0);
    return (uint32_t)std::llround(fraction * std::max(0.0, frameSeconds) * rate);
}

bool TimelineAudioPreview::play(Project* project, const std::string& assetPath, float volumeDb, float pitch, uint32_t delay, bool exitStop) {
    if (!blocker().empty() || assetPath.empty() || !AudioSystem::init()) return false;
    SoLoud::Soloud& soloud = AudioSystem::getSoloud();
    const unsigned int rate = soloud.getBackendSamplerate();
    // what to start: the clip, or every clip of one pick of the event
    struct Part {
        std::string clip;
        double volumeDb = 0.0;
        double pitchSemitones = 0.0;
        double delayMs = 0.0;
    };
    std::vector<Part> parts;
    if (Util::isSoundEventFile(assetPath)) {
        const SoundEventAsset* event = SoundEventLibrary::find(assetPath);
        if (!event || !event->def || event->def->clips.empty()) return false;
        const uint64_t seed = project ? (uint64_t)project->getAudioSettings().seed : 1;
        const int index = (int)(picks++ % 64);
        const std::vector<SoundEventPick> drawn = SoundEventLibrary::preview(*event->def, event->key, seed, index + 1);
        if (drawn.empty()) return false;
        const SoundEventPick& pick = drawn.back();
        for (int clip : pick.clips) {
            if (clip < 0 || (size_t)clip >= event->def->clips.size()) continue;
            const SoundEventClip& source = event->def->clips[(size_t)clip];
            parts.push_back({source.clip.path, pick.volumeDb + source.volumeDb, pick.pitchSemitones + source.pitchSemitones,
                             std::max(0.0, pick.delayMs + source.delayMs)});
        }
    } else {
        parts.push_back({assetPath, 0.0, 0.0, 0.0});
    }
    bool any = false;
    for (const Part& part : parts) {
        const SoundLoadResult loaded = SoundPool::loadClipNow(part.clip, AudioLoadType::Auto);
        if (loaded.state != ResourceLoadState::Finished || !loaded.data) continue;
        const float volume = (float)std::pow(10.0, ((double)volumeDb + part.volumeDb) / 20.0);
        const unsigned int handle = soloud.play(*loaded.data, volume, 0.0f, true);
        if (handle == 0) continue;
        soloud.setRelativePlaySpeed(handle, (float)((double)pitch * std::pow(2.0, part.pitchSemitones / 12.0)));
        const uint32_t frames = delay + (uint32_t)std::llround(part.delayMs * (double)rate / 1000.0);
        if (frames > 0) soloud.setDelaySamples(handle, frames);
        soloud.setPause(handle, false);
        voices.push_back({handle, loaded.data, exitStop});
        any = true;
    }
    if (any) started++;
    return any;
}

void TimelineAudioPreview::prune() {
    if (!AudioSystem::isInited()) {
        voices.clear();
        return;
    }
    SoLoud::Soloud& soloud = AudioSystem::getSoloud();
    voices.erase(std::remove_if(voices.begin(), voices.end(), [&](const Voice& v) { return !soloud.isValidVoiceHandle(v.handle); }),
                 voices.end());
}

void TimelineAudioPreview::stopExit() {
    prune();
    if (voices.empty()) return;
    SoLoud::Soloud& soloud = AudioSystem::getSoloud();
    for (Voice& voice : voices) {
        if (!voice.exitStop) continue;
        soloud.fadeVolume(voice.handle, 0.0f, 0.01);
        soloud.scheduleStop(voice.handle, 0.01);
    }
}

void TimelineAudioPreview::stopAll() {
    if (AudioSystem::isInited()) {
        SoLoud::Soloud& soloud = AudioSystem::getSoloud();
        for (const Voice& voice : voices) soloud.stop(voice.handle);
    }
    voices.clear();
}

int TimelineAudioPreview::voiceCount() {
    prune();
    return (int)voices.size();
}
