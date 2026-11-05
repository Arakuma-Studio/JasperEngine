// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TimelineAudio.h"

#include "component/AudioSourceComponent.h"

#if JASPERENGINE_SOUND_LAYER

#include "Engine.h"
#include "Log.h"
#include "Scene.h"
#include "animation/ClipAudio.h"
#include "animation/ClipBinding.h"
#include "asset/AssetMap.h"
#include "audio/AudioCapture.h"
#include "audio/AudioMixer.h"
#include "audio/SoundEvent.h"
#include "audio/SoundEventLibrary.h"
#include "object/sound/Audio.h"
#include "pool/SoundPool.h"
#include "subsystem/AudioSystem.h"
#include "soloud.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <unordered_map>

using namespace jasperengine;

namespace {

    std::set<std::string>& timelineWarnings() {
        static auto* warned = new std::set<std::string>();
        return *warned;
    }

    struct TimelineLength {
        uint32_t generation = 0;
        double seconds = 0.0;
    };

    std::unordered_map<std::string, TimelineLength>& timelineLengths() {
        static auto* lengths = new std::unordered_map<std::string, TimelineLength>();
        return *lengths;
    }

    // "Audio: cue 2 of anim/run.anim: Body/LeftFoot binds nothing", once per cue and problem per Play
    void warnCue(const TimelineCueInfo& info, const std::string& problem) {
        const std::string what = std::string(info.timeline) == "actions"
                                     ? "action frame " + std::to_string(info.cue) + " of Animation '" + info.clip + "'"
                                     : "cue " + std::to_string(info.cue) + " of " + info.clip;
        if (timelineWarnings().insert(what + "|" + problem).second) Log::warn("Audio: %s: %s", what.c_str(), problem.c_str());
    }

    void cueLine(const TimelineCueInfo& info, const char* kind, const std::string& name, uint32_t delay) {
#if JASPERENGINE_AUDIO_CAPTURE
        if (!AudioCapture::isActive()) return;
        AudioCapture::Line line("cue");
        line.text("timeline", info.timeline).text("clip", info.clip).integer("cue", info.cue).text("name", name).text("kind", kind);
        line.scene(info.scene).entity(info.scene, (uint32_t)info.player, "player", "player_id");
        line.text("state", info.state).integer("layer", info.layer).number("position", info.position, 6);
        line.integer("delay_samples", delay);
#else
        (void)info;
            (void)name;
        (void)delay;
#endif
    }

    // the path a reference plays: its GUID's file where the asset map knows it, else its path
    std::string referencePath(const AssetRef& ref) {
        std::string path;
        if (!ref.guid.isNull() && AssetMap::findPath(ref.guid, path)) return path;
        return ref.path;
    }

    double clipLength(const std::string& path) {
        if (path.empty()) return 0.0;
        // a missing clip is asked once per Play too (an auto-length frame asks every update)
        const uint32_t generation = SoundPool::generation(path);
        auto found = timelineLengths().find(path);
        if (found != timelineLengths().end() && found->second.generation == generation) return found->second.seconds;
        const SoundLoadResult loaded = SoundPool::loadClipNow(path, AudioLoadType::Auto);
        TimelineLength& cached = timelineLengths()[path];
        cached.generation = generation;
        cached.seconds = loaded.state == ResourceLoadState::Finished && loaded.data ? SoundPool::getLength(loaded.data.get()) : 0.0;
        return cached.seconds;
    }

    AudioSourceComponent* sourceOf(Scene* scene, Entity entity) {
        return scene && entity != NULL_ENTITY && scene->isEntityCreated(entity) ? scene->findComponent<AudioSourceComponent>(entity) : nullptr;
    }

}

uint32_t TimelineAudio::delayFrames(double fraction, double span) {
    if (!std::isfinite(fraction)) return 0;
    double frame = std::max(0.0, (double)Engine::getUnscaledDeltatime());
    if (frame <= 0.0 && span > 0.0 && Engine::getTimeScale() > 0.0f) frame = span / (double)Engine::getTimeScale();
    const double rate = AudioSystem::isInited() ? (double)AudioSystem::getSoloud().getBackendSamplerate() : 48000.0;
    const double frames = std::min(std::max(fraction, 0.0), 1.0) * frame * rate;
    return (uint32_t)std::llround(frames);
}

uint32_t TimelineAudio::delayFramesAt(double seconds, double span) {
    return span > 0.0 ? delayFrames(seconds / span, span) : 0;
}

TimelineVoice TimelineAudio::fireCue(Scene* scene, Entity root, const ClipAudioCue& cue, uint32_t delay, float stateSpeed,
                                     const TimelineCueInfo& info) {
    TimelineVoice voice;
    const std::string label = anim::cueLabel(cue);
    if (cue.kind == ClipCueKind::Snapshot) {
        cueLine(info, "snapshot", label, 0);
        transition(cue.snapshot, cue.seconds, cue.unscaled, info);
        return voice;
    }

    Entity bound = root;
    if (cue.kind == ClipCueKind::Source || cue.spatial) {
        bound = anim::resolvePath(scene, root, cue.path);
        if (bound == NULL_ENTITY) {
            warnCue(info, (cue.path.empty() ? std::string("the root") : cue.path) + " binds nothing");
            return voice;
        }
    }
    const float speed = cue.followSpeed && std::isfinite(stateSpeed) ? std::fabs(stateSpeed) : 1.0f;

    if (cue.kind == ClipCueKind::Source) {
        AudioSourceComponent* audio = sourceOf(scene, bound);
        if (!audio) {
            warnCue(info, (cue.path.empty() ? std::string("the root") : cue.path) + " has no AudioSource");
            return voice;
        }
        cueLine(info, "source", label, delay);
        startSource(scene, bound, delay, speed, NULL_ENTITY, info);
        if (cue.stop == ClipCueStop::StateExit) voice.source = bound;
        return voice;
    }

    const std::string path = referencePath(cue.sound);
    const bool isEvent = SoundEventLibrary::isEventName(path);
    if (path.empty() || (isEvent ? SoundEventLibrary::find(path) == nullptr : !SoundPool::loadClipNow(path, AudioLoadType::Auto).data)) {
        warnCue(info, "sound '" + (cue.sound.path.empty() ? cue.sound.guid.str() : cue.sound.path) + "' does not resolve");
        return voice;
    }
    PlayOptions options;
    options.scene = scene;
    if (cue.spatial) {
        options.targetScene = scene;
        options.target = bound;
    }
    options.volume = (float)std::pow(10.0, (double)cue.volumeDb / 20.0);
    options.pitch = (float)(std::pow(2.0, (double)cue.pitchSemitones / 12.0) * (double)speed);
    options.group = cue.group;
    cueLine(info, "sound", label, delay);
    const SoundHandle handle = AudioSystem::playFreeFromTimeline(path, options, delay);
    if (cue.stop == ClipCueStop::StateExit && handle.isValid()) {
        voice.id = handle.getId();
        voice.generation = handle.getGeneration();
    }
    return voice;
}

void TimelineAudio::logStart(const TimelineCueInfo& info, const char* kind, const std::string& name, uint32_t delay) {
    cueLine(info, kind, name, delay);
}

void TimelineAudio::stop(Scene* scene, const TimelineVoice& voice) {
    if (voice.id != 0) AudioSystem::freeStopFaded(voice.id, voice.generation, anim::CUE_EXIT_FADE_SECONDS, "cue_exit");
    if (voice.source != NULL_ENTITY) stopSource(scene, voice.source);
}

bool TimelineAudio::startSource(Scene* scene, Entity source, uint32_t delay, float speedScale, Entity target, const TimelineCueInfo& info) {
    AudioSourceComponent* audio = sourceOf(scene, source);
    if (!audio) return false;
    if (audio->filename.empty() && audio->event.empty()) warnCue(info, "its AudioSource has no clip or Sound Event");
    audio->timelineStart = true;
    audio->timelineDelayFrames = delay;
    audio->timelineSpeedScale = std::isfinite(speedScale) && speedScale > 0.0f ? speedScale : 1.0f;
    audio->timelineTarget = target;
    audio->timelineStopFade = false;
    audio->stopTrigger = false;
    audio->pauseTrigger = false;
    audio->startTrigger = true;
    return true;
}

void TimelineAudio::pauseSource(Scene* scene, Entity source) {
    if (AudioSourceComponent* audio = sourceOf(scene, source)) {
        audio->startTrigger = false;
        audio->pauseTrigger = true;
    }
}

void TimelineAudio::stopSource(Scene* scene, Entity source) {
    if (AudioSourceComponent* audio = sourceOf(scene, source)) {
        audio->startTrigger = false;
        audio->timelineStart = false;
        audio->timelineStopFade = true;
        audio->stopTrigger = true;
    }
}

bool TimelineAudio::isSourcePlaying(Scene* scene, Entity source) {
    const AudioSourceComponent* audio = sourceOf(scene, source);
    return audio && (audio->startTrigger || audio->state != AudioSourceState::Stopped);
}

bool TimelineAudio::transition(const std::string& snapshot, float seconds, std::optional<bool> unscaled, const TimelineCueInfo& info) {
    const std::vector<std::string> names = AudioMixer::getSnapshotNames();
    if (std::find(names.begin(), names.end(), snapshot) == names.end()) {
        warnCue(info, "the mixer has no snapshot '" + snapshot + "'");
        return false;
    }
    const AudioMixer::CaptureCause cause("animation");
    return AudioMixer::transitionTo(snapshot, std::max(0.0f, std::isfinite(seconds) ? seconds : 0.0f), unscaled.value_or(false));
}

double TimelineAudio::soundLength(const std::string& eventOrClip) {
    if (!SoundEventLibrary::isEventName(eventOrClip)) {
        std::string path = eventOrClip;
        if (SoundEventLibrary::isGuid(path)) AssetMap::findPath(Guid::parse(eventOrClip), path);
        return clipLength(path);
    }
    const SoundEventAsset* event = SoundEventLibrary::find(eventOrClip);
    if (!event || !event->def) return 0.0;
    double longest = 0.0;
    for (const SoundEventClip& clip : event->def->clips) {
        const double length = clipLength(referencePath(clip.clip));
        const double delay = event->def->mode == SoundEventMode::Layer ? std::max(0.0, (double)clip.delayMs / 1000.0) : 0.0;
        longest = std::max(longest, delay + length);
    }
    return longest;
}

void TimelineAudio::resetRuntime() {
    timelineWarnings().clear();
    timelineLengths().clear();
}

#endif // JASPERENGINE_SOUND_LAYER
