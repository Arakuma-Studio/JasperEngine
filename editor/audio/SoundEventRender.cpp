// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SoundEventRender.h"

#include "Project.h"
#include "audio/AudioAnalysis.h"
#include "audio/AudioImportRegistry.h"
#include "audio/AudioMeter.h"
#include "audio/ClipSource.h"
#include "pool/SoundPool.h"
#include "util/FileUtils.h"

#include "soloud.h"
#include "soloud_wav.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <memory>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    constexpr double MATCH_PEAK_LIMIT_DBTP = -1.0;

    // a clip as it plays: its file, with its import settings when it has any (as SoundPool wraps it)
    std::shared_ptr<SoLoud::AudioSource> loadRenderClip(const std::string& clip, const SoundEventRender::Input& input) {
        std::ifstream in((input.assetsRoot / FileUtils::pathFromUtf8(clip)).lexically_normal(), std::ios::binary | std::ios::ate);
        if (!in) return nullptr;
        const std::streamoff size = in.tellg();
        if (size <= 0) return nullptr;
        std::vector<unsigned char> bytes((size_t)size);
        in.seekg(0);
        if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) return nullptr;
        auto wav = std::make_shared<SoLoud::Wav>();
        if (wav->loadMem(bytes.data(), (unsigned int)bytes.size(), true, false) != SoLoud::SO_NO_ERROR) return nullptr;
        const auto settings = input.settings.find(clip);
        if (settings == input.settings.end() || settings->second.isDefault()) return wav;
        return std::make_shared<AudioClipSource>(std::move(wav), settings->second, bytes.data(), bytes.size());
    }

    void measureInto(const std::vector<float>& samples, double& lufs, double& peak) {
        if (samples.empty()) return;
        // a momentary block is 400 ms: a shorter clip is measured with silence after it
        std::vector<float> padded = samples;
        const size_t minimum = (size_t)(0.4 * SoundEventRender::SAMPLE_RATE) * SoundEventRender::CHANNELS;
        if (padded.size() < minimum) padded.resize(minimum, 0.0f);
        const LoudnessResult result = AudioMeter::measure(padded.data(), padded.size() / SoundEventRender::CHANNELS,
                                                          SoundEventRender::SAMPLE_RATE, SoundEventRender::CHANNELS, false);
        lufs = result.momentaryMax.value_or(LoudnessResult::SILENCE_DB);
        peak = result.truePeakDb;
    }

}

SoundEventRender::Input SoundEventRender::prepare(const Project& project, const SoundEventDef& event) {
    Input input;
    input.assetsRoot = fs::absolute(project.getAssetsPath());
    input.cacheDirectory = AudioAnalysis::cacheDirectory(project.getProjectPath());
    for (const SoundEventClip& clip : event.clips) {
        ClipImportSettings settings;
        std::string error;
        const fs::path asset = project.resolveAssetPath(FileUtils::pathFromUtf8(clip.clip.path));
        if (AudioImportRegistry::resolve(project, asset, settings, error)) input.settings[clip.clip.path] = settings;
        input.clipFiles.push_back(asset.lexically_normal());
    }
    return input;
}

bool SoundEventRender::render(const SoundEventDef& event, const std::string& key, const Input& input, int count, uint64_t seed,
                              std::vector<Rendition>& out, std::string& error) {
    out.clear();
    count = std::clamp(count, 1, MAX_COUNT);

    SoLoud::Soloud soloud;
    if (soloud.init(0, SoLoud::Soloud::NULLDRIVER, SAMPLE_RATE, BLOCK_FRAMES, CHANNELS) != SoLoud::SO_NO_ERROR) {
        error = "the offline mixer could not start";
        return false;
    }
    soloud.setPostClipScaler(1.0f);
    soloud.setGlobalVolume(1.0f);

    std::vector<std::shared_ptr<SoLoud::AudioSource>> sources(event.clips.size());
    for (size_t i = 0; i < event.clips.size(); i++) sources[i] = loadRenderClip(event.clips[i].clip.path, input);

    const size_t maxFrames = (size_t)std::llround(MAX_SECONDS * SAMPLE_RATE);
    std::vector<float> block((size_t)BLOCK_FRAMES * CHANNELS);
    for (const SoundEventPick& pick : SoundEventLibrary::preview(event, key, seed, count)) {
        Rendition rendition;
        rendition.playIndex = pick.playIndex;
        std::vector<SoLoud::handle> playing;
        size_t passEnd = 0;
        for (size_t layer = 0; layer < pick.clips.size(); layer++) {
            const int index = pick.clips[layer];
            const SoundEventClip& clip = event.clips[index];
            Voice voice;
            voice.variation = index;
            voice.layer = (int)layer;
            voice.clip = clip.clip.path;
            voice.pitchSemitones = pick.pitchSemitones + (double)clip.pitchSemitones;
            voice.volumeDb = pick.volumeDb + (double)clip.volumeDb;
            voice.delayMs = std::max(pick.delayMs + (double)clip.delayMs, 0.0);
            if (!sources[index]) {
                voice.error = "the clip did not load";
                rendition.voices.push_back(voice);
                continue;
            }
            const double speed = std::pow(2.0, voice.pitchSemitones / 12.0);
            const unsigned int delay = (unsigned int)std::llround(voice.delayMs * SAMPLE_RATE / 1000.0);
            const SoLoud::handle handle = soloud.play(*sources[index], (float)std::pow(10.0, voice.volumeDb / 20.0), 0.0f, true);
            soloud.setRelativePlaySpeed(handle, (float)speed);
            soloud.setLooping(handle, event.loop);
            soloud.setLoopPoint(handle, SoundPool::getLoopPoint(sources[index].get(), 0.0));
            if (delay > 0) soloud.setDelaySamples(handle, delay);
            soloud.setPause(handle, false);
            voice.played = true;
            passEnd = std::max(passEnd, (size_t)delay + (size_t)std::llround(SoundPool::getLength(sources[index].get()) / speed * SAMPLE_RATE));
            playing.push_back(handle);
            rendition.voices.push_back(voice);
        }

        size_t mixed = 0;
        const size_t limit = event.loop ? std::min(passEnd, maxFrames) : maxFrames;
        while (true) {
            bool any = false;
            for (SoLoud::handle handle : playing) any = any || soloud.isValidVoiceHandle(handle);
            if (!any) break;
            if (mixed >= limit) {
                rendition.truncated = !event.loop;
                break;
            }
            const size_t frames = std::min((size_t)BLOCK_FRAMES, limit - mixed);
            soloud.mix(block.data(), (unsigned int)frames);
            rendition.samples.insert(rendition.samples.end(), block.begin(), block.begin() + (std::ptrdiff_t)(frames * CHANNELS));
            mixed += frames;
        }
        soloud.stopAll();
        measureInto(rendition.samples, rendition.lufs, rendition.peakDbtp);
        out.push_back(std::move(rendition));
    }

    // the sources go before the instance they played in (~AudioSource calls back into it)
    soloud.stopAll();
    sources.clear();
    soloud.deinit();
    return true;
}

SoundEventRender::ClipLevel SoundEventRender::measureClip(const std::string& clip, const Input& input) {
    ClipLevel level;
    std::shared_ptr<SoLoud::AudioSource> wav = loadRenderClip(clip, input);
    if (!wav) return level;
    SoLoud::Soloud soloud;
    if (soloud.init(0, SoLoud::Soloud::NULLDRIVER, SAMPLE_RATE, BLOCK_FRAMES, CHANNELS) != SoLoud::SO_NO_ERROR) return level;
    soloud.setPostClipScaler(1.0f);
    const SoLoud::handle handle = soloud.play(*wav);
    std::vector<float> samples;
    std::vector<float> block((size_t)BLOCK_FRAMES * CHANNELS);
    const size_t maxFrames = (size_t)std::llround(MAX_SECONDS * SAMPLE_RATE);
    while (soloud.isValidVoiceHandle(handle) && samples.size() / CHANNELS < maxFrames) {
        soloud.mix(block.data(), BLOCK_FRAMES);
        samples.insert(samples.end(), block.begin(), block.end());
    }
    soloud.stopAll();
    wav.reset();
    soloud.deinit();
    level.loaded = true;
    measureInto(samples, level.lufs, level.peakDbtp);
    return level;
}

bool SoundEventRender::matchLevels(SoundEventDef& event, const Input& input, std::string& error) {
    if (event.mode == SoundEventMode::Layer) {
        error = "Match Levels is for one clip at a time: a layer event's clips sound together";
        return false;
    }
    std::vector<ClipLevel> levels;
    double sum = 0.0;
    int measured = 0;
    for (const SoundEventClip& clip : event.clips) {
        ClipLevel level = measureClip(clip.clip.path, input);
        levels.push_back(level);
        if (level.loaded && level.lufs > -100.0) {
            sum += level.lufs;
            measured++;
        }
    }
    if (measured == 0) {
        error = "no clip could be measured";
        return false;
    }
    // their average, lowered as a whole when a clip would pass -1 dBTP there: they stay matched
    double target = sum / measured;
    for (const ClipLevel& level : levels) {
        if (level.loaded && level.lufs > -100.0) target = std::min(target, level.lufs + MATCH_PEAK_LIMIT_DBTP - level.peakDbtp);
    }
    for (size_t i = 0; i < event.clips.size(); i++) {
        const ClipLevel& level = levels[i];
        if (!level.loaded || level.lufs <= -100.0) continue;
        event.clips[i].volumeDb = (float)std::clamp(target - level.lufs, -60.0, 12.0);
    }
    return true;
}
