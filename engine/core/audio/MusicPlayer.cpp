// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "MusicPlayer.h"

#include "Log.h"
#include "asset/AssetMap.h"
#include "audio/AudioCapture.h"
#include "audio/AudioImport.h"
#include "audio/AudioMixer.h"
#include "audio/ClipSource.h"
#include "audio/MusicSequenceSource.h"
#include "audio/SoundEventLibrary.h"
#include "pool/SoundPool.h"
#include "subsystem/AudioSystem.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <memory>
#include <set>
#include <vector>

using namespace jasperengine;

// docs/specs/S39-music.md. Main thread only: the audio thread's half is MusicSequenceSource, which
// plays the sections prepared here and reports where each began. Positions are track frames (the
// first section's rate, from the track's first frame) and output frames; a voice's output delay is
// counted down by the frames each engine frame mixes, so a switch point, a snapshot and a callback
// land in the frame whose mix holds them.

namespace {

    // the shortest section: preparing the next one then always has frames to spare
    constexpr double SECTION_MIN_SECONDS = 0.1;
    constexpr double HALF_PI = 1.57079632679489661923;

    enum class Sync { Now, Beat, Bar, SectionEnd };

    bool syncFromName(const std::string& name, Sync& sync){
        if (name == "now") sync = Sync::Now;
        else if (name == "beat") sync = Sync::Beat;
        else if (name == "bar") sync = Sync::Bar;
        else if (name == "section_end") sync = Sync::SectionEnd;
        else return false;
        return true;
    }

    const char* syncName(Sync sync){
        switch (sync) {
            case Sync::Beat: return "beat";
            case Sync::Bar: return "bar";
            case Sync::SectionEnd: return "section_end";
            default: return "now";
        }
    }

    std::set<std::string>& warnings(){
        static auto* set = new std::set<std::string>();
        return *set;
    }

    // one warning per key per Play
    void warnOnce(const std::string& key, const std::string& message){
        if (warnings().insert(key).second) Log::warn("%s", message.c_str());
    }

    // the beat grid of a section's clip (R8): frames per beat at the clip's rate, beat 0's frame from
    // the section's first frame
    struct Grid{
        bool valid = false;
        double framesPerBeat = 0.0;
        double firstBeat = 0.0;
        int beatsPerBar = 4;
        double bpm = 0.0;
    };

    // a section the audio thread began
    struct Begun{
        int index = 0;
        int clipIndex = -1;
        uint64_t startFrame = 0;
    };

    struct Track{
        // as playMusic named it, and what it resolved to
        std::string name;
        bool isEvent = false;
        SoundEventAsset event;
        // by event clip index (one for a clip track), held while the track plays
        std::vector<std::shared_ptr<AudioClipSource>> clips;
        std::vector<std::string> paths;
        std::vector<unsigned int> loops;
        std::vector<float> gains;
        std::vector<bool> playable;
        std::vector<Grid> grids;

        std::shared_ptr<MusicSequenceSource> stream;
        unsigned int handle = 0;
        std::string groupId;
        float ownVolume = 1.0f;
        float speed = 1.0f;
        // the volume set last, linear
        float volume = 1.0f;
        float rate = 48000.0f;
        unsigned int outputRate = 48000;
        // the voice's start delay: output frames still to wait, and in seconds as it was set
        int64_t delayLeft = 0;
        double delaySeconds = 0.0;
        // the capture sample of track frame 0
        uint64_t firstSample = 0;
        bool ignoreListenerPause = false;
        bool pauseApplied = false;

        // sections made (the next one's number), the clip of each made and not begun yet, and the
        // next clip of a sequence
        int sectionsMade = 0;
        std::deque<std::pair<int, int>> made;
        int sequenceAt = 0;
        std::deque<Begun> begun;
        // the section playing: from its report, entered in the frame whose mix holds its start
        Begun section;
        bool entered = false;
        // the next beat to call back in it
        int64_t nextBeat = 0;
        // the section numbers Audio.advanceMusic shortened
        int advancedSection = -1;
        // an outgoing track: its frame where the incoming one took over (callbacks stop there)
        uint64_t switchFrame = MusicSequenceSource::NO_STOP;

        // a stop asked for: output frames to its start, its fade, its progress
        bool stopping = false;
        bool stopPending = false;
        int64_t stopLeft = 0;
        double stopFade = 0.0;
        double stopElapsed = 0.0;
        float stopFrom = 1.0f;
    };

    std::unique_ptr<Track> currentTrack;
    // the track a crossfade fades out: never a third
    std::unique_ptr<Track> outgoing;

    // the switch to `current` from `outgoing`: its crossfade and snapshot
    struct Switch{
        bool active = false;
        // the switch point reached (the incoming voice's delay over), and the outgoing track silent
        bool reached = false;
        bool done = false;
        double length = 0.0;
        double elapsed = 0.0;
        float outgoingFrom = 1.0f;
        std::string snapshot;
        float snapshotSeconds = 0.0f;
    };
    Switch pendingSwitch;
    bool previewing = false;

    float dbToLinear(double db){
        return db != 0.0 ? (float)std::pow(10.0, db / 20.0) : 1.0f;
    }

    SoLoud::Soloud& soloud(){
        return AudioSystem::getSoloud();
    }

    bool voiceGone(const Track& track){
        return track.handle == 0 || !AudioSystem::isVoicePlaying(track.handle);
    }

    bool liveTrack(const std::unique_ptr<Track>& track){
        return track && !track->stopping && !voiceGone(*track);
    }

    Grid gridOf(const Track& track, int clipIndex){
        const size_t slot = clipIndex < 0 ? 0 : (size_t)clipIndex;
        return slot < track.grids.size() ? track.grids[slot] : Grid();
    }

    Grid makeGrid(const AudioClipSource& clip){
        Grid grid;
        const ClipImportSettings& settings = clip.getImportSettings();
        if (!(settings.bpm > 0.0f) || !(clip.getSampleRate() > 0.0f)) return grid;
        grid.valid = true;
        grid.bpm = settings.bpm;
        grid.framesPerBeat = (double)clip.getSampleRate() * 60.0 / (double)settings.bpm;
        grid.firstBeat = (double)settings.firstBeatFrame - (double)clip.getStartFrame();
        grid.beatsPerBar = std::max(1, settings.beatsPerBar);
        return grid;
    }

    // the first beat index at or after section frame 0 (and never before beat 0)
    int64_t firstBeatIndex(const Grid& grid){
        if (grid.firstBeat >= 0.0) return 0;
        return (int64_t)std::ceil(-grid.firstBeat / grid.framesPerBeat - 1e-9);
    }

    // the track frames played so far: the next mix plays this one first
    uint64_t playedFrames(const Track& track){
        const double seconds = soloud().getStreamPosition(track.handle) - track.delaySeconds * track.speed;
        return seconds > 0.0 ? (uint64_t)std::llround(seconds * track.rate) : 0;
    }

    // source frames to output frames, and back
    int64_t toOutput(const Track& track, double frames){
        return (int64_t)std::llround(frames * (double)track.outputRate / ((double)track.rate * (double)track.speed));
    }

    double toTrack(const Track& track, int64_t outputFrames){
        return (double)outputFrames * (double)track.rate * (double)track.speed / (double)track.outputRate;
    }

    // a `music` line of the capture (R15); fields(line) adds the action's own (a generic lambda, so a build
    // without the capture compiles none of it)
    template<typename Fields>
    void musicLine(const char* action, const Track* track, Fields&& fields){
#if JASPERENGINE_AUDIO_CAPTURE
        if (!AudioCapture::isActive()) return;
        AudioCapture::Line line("music");
        line.text("action", action);
        if (track) line.text("music", track->name);
        fields(line);
#else
        (void)action;
        (void)track;
        (void)fields;
#endif
    }

    void musicLine(const char* action, const Track* track){
        musicLine(action, track, [](auto&) {});
    }

    // the section the reports say plays at track frame `frame`, and its first frame
    const Begun* sectionAt(const Track& track, uint64_t frame){
        const Begun* found = track.entered ? &track.section : nullptr;
        for (const Begun& begun : track.begun) {
            if (begun.startFrame <= frame) found = &begun;
        }
        return found;
    }

    // R9's boundary B (section frames) of `sync` at section frame `position`; false when the grid
    // cannot give one (no tempo)
    bool boundaryOf(const Track& track, const Begun& section, Sync sync, double position, double& boundary){
        if (sync == Sync::SectionEnd) {
            const size_t slot = section.clipIndex < 0 ? 0 : (size_t)section.clipIndex;
            const AudioClipSource& clip = *track.clips[slot];
            const double firstEnd = (double)clip.getLoopEndFrame() - (double)clip.getStartFrame();
            const double loop = (double)clip.getLoopEndFrame() - (double)clip.getLoopStartFrame();
            const double tail = (double)clip.getEndFrame() - (double)clip.getLoopEndFrame();
            double end = firstEnd;
            if (position > firstEnd && loop > 0.0) end = firstEnd + std::ceil((position - firstEnd) / loop - 1e-9) * loop;
            const unsigned int loops = track.loops[slot];
            const bool shortened = track.advancedSection == section.index;
            if (loops > 0 && !shortened) {
                const double lastEnd = firstEnd + (double)(loops - 1) * loop;
                if (end > lastEnd || position > lastEnd) end = lastEnd + tail;
            } else if (shortened) {
                end += tail;
            }
            boundary = end;
            return true;
        }
        const Grid grid = gridOf(track, section.clipIndex);
        if (!grid.valid) return false;
        const double step = grid.framesPerBeat * (sync == Sync::Bar ? (double)grid.beatsPerBar : 1.0);
        boundary = grid.firstBeat;
        if (position > grid.firstBeat) boundary = grid.firstBeat + std::ceil((position - grid.firstBeat) / step - 1e-9) * step;
        return true;
    }

    // output frames from the next mix's first frame to the playing track's next `sync` boundary (R9);
    // `sync` falls back to now (one warning) without music or tempo. trackFrame: the boundary's
    // track frame.
    int64_t delayTo(const Track* track, Sync& sync, const std::string& who, uint64_t* trackFrame){
        if (sync == Sync::Now || !track) {
            if (sync != Sync::Now) warnOnce("nomusic:" + who, "Audio: " + who + ": no music plays; it happens now");
            sync = Sync::Now;
            if (track && trackFrame) *trackFrame = playedFrames(*track);
            return 0;
        }
        const uint64_t played = playedFrames(*track);
        const Begun* section = sectionAt(*track, played);
        double boundary = 0.0;
        if (!section || !boundaryOf(*track, *section, sync, (double)played - (double)section->startFrame, boundary)) {
            warnOnce("notempo:" + who, "Audio: " + who + ": the music has no tempo (its clip's import settings music.bpm); it happens now");
            sync = Sync::Now;
            if (trackFrame) *trackFrame = played;
            return 0;
        }
        const double position = (double)played - (double)section->startFrame;
        const int64_t ahead = std::max<int64_t>(0, toOutput(*track, boundary - position)) + track->delayLeft;
        if (trackFrame) *trackFrame = section->startFrame + (uint64_t)std::llround(std::max(boundary, position));
        return ahead;
    }

    // the track whose grid a stinger or a switch follows: the incoming one during a crossfade
    Track* gridTrack(){
        return liveTrack(currentTrack) ? currentTrack.get() : nullptr;
    }

    SoundEventClock eventClock(){
        SoundEventClock clock;
        clock.samples = AudioSystem::isDeterministic();
        clock.cursor = AudioSystem::getMixedFrames();
        clock.rate = AudioSystem::isInited() ? soloud().getBackendSamplerate() : 48000;
        clock.seconds = AudioMixer::getClockTime();
        return clock;
    }

    // the next section's clip: a sequence in order from its first clip, else the event's picker (one
    // accepted play per section); -1 when no pick gives a playable clip. pick: the draws of it.
    int pickSection(Track& track, SoundEventPick* drawn){
        const SoundEventDef& def = *track.event.def;
        const int count = (int)def.clips.size();
        if (def.mode == SoundEventMode::Sequence) {
            if (drawn) *drawn = SoundEventLibrary::pick(track.event, eventClock());
            for (int i = 0; i < count; i++) {
                const int clip = track.sequenceAt++ % count;
                if (track.playable[clip]) return clip;
            }
            return -1;
        }
        for (int attempt = 0; attempt < std::max(1, count) * 2; attempt++) {
            const SoundEventPick pick = SoundEventLibrary::pick(track.event, eventClock());
            if (drawn && attempt == 0) *drawn = pick;
            if (pick.clips.empty()) return -1;
            const int clip = pick.clips[0];
            if (clip >= 0 && clip < count && track.playable[clip]) return clip;
        }
        return -1;
    }

    std::unique_ptr<MusicSection> makeSection(Track& track, int clipIndex){
        const size_t slot = clipIndex < 0 ? 0 : (size_t)clipIndex;
        const std::shared_ptr<AudioClipSource>& clip = track.clips[slot];
        auto section = std::make_unique<MusicSection>();
        section->clip = clip;
        section->instance.reset(clip->createInstance());
        section->instance->init(*clip, 0);
        section->instance->mFlags = 0;
        section->instance->mLoopPoint = clip->getDefaultLoopPoint();
        section->index = track.sectionsMade++;
        section->clipIndex = clipIndex;
        track.made.emplace_back(section->index, clipIndex);
        section->loops = track.loops[slot];
        section->gain = track.gains[slot];
        return section;
    }

    bool playlistContinues(const Track& track){
        return track.isEvent && (track.event.def->loop || track.sectionsMade < (int)track.event.def->clips.size());
    }

    // the next section offered as soon as the slot is free, or the end once none follows
    void prepareNext(Track& track){
        if (!track.stream) return;
        track.stream->collectRetired();
        if (track.stopping || !track.stream->wantsNext()) return;
        if (!playlistContinues(track)) {
            track.stream->endAfterCurrent();
            return;
        }
        const int clip = pickSection(track, nullptr);
        if (clip < 0) {
            track.stream->endAfterCurrent();
            return;
        }
        track.stream->offerNext(makeSection(track, clip));
    }

    void checkLoopBeats(const Track& track, int clipIndex){
        const size_t slot = clipIndex < 0 ? 0 : (size_t)clipIndex;
        const Grid grid = gridOf(track, clipIndex);
        if (!grid.valid || track.loops[slot] == 1) return;
        const AudioClipSource& clip = *track.clips[slot];
        const double beats = (double)(clip.getLoopEndFrame() - clip.getLoopStartFrame()) / grid.framesPerBeat;
        if (std::fabs(beats - std::round(beats)) > 1e-3) {
            char text[96];
            std::snprintf(text, sizeof(text), "loop region is %.2f beats at %g bpm: stingers will drift", beats, grid.bpm);
            warnOnce("beats:" + track.paths[slot], "Audio: " + track.paths[slot] + " " + text);
        }
    }

    void drainReports(Track& track){
        if (!track.stream) return;
        MusicSequenceSource::Report report;
        while (track.stream->takeReport(report)) {
            Begun begun;
            begun.index = report.section;
            begun.startFrame = report.startFrame;
            while (!track.made.empty() && track.made.front().first <= report.section) {
                if (track.made.front().first == report.section) begun.clipIndex = track.made.front().second;
                track.made.pop_front();
            }
            track.begun.push_back(begun);
        }
    }

    // the beats of the section playing before track frame `to`, in order, each once
    void fireBeats(Track& track, uint64_t to){
        const Grid grid = gridOf(track, track.section.clipIndex);
        if (!grid.valid || !track.entered) return;
        const int64_t end = (int64_t)to - (int64_t)track.section.startFrame;
        while (true) {
            const double frame = grid.firstBeat + (double)track.nextBeat * grid.framesPerBeat;
            if (!(std::llround(frame) < end)) break;
            const int64_t k = track.nextBeat++;
            Audio::onMusicBeat.call((int)(k / grid.beatsPerBar) + 1, (int)(k % grid.beatsPerBar) + 1);
        }
    }

    void enterSection(Track& track, const Begun& begun){
        track.section = begun;
        track.entered = true;
        track.nextBeat = firstBeatIndex(gridOf(track, begun.clipIndex));
        const size_t slot = begun.clipIndex < 0 ? 0 : (size_t)begun.clipIndex;
        const std::string& clip = track.paths[slot];
        checkLoopBeats(track, begun.clipIndex);
        const int64_t starts = (int64_t)track.firstSample + toOutput(track, (double)begun.startFrame);
        musicLine("section", &track, [&](auto& line) {
            line.integer("section", begun.index + 1).text("clip", clip).integer("starts_at_sample", starts);
        });
        Audio::onMusicSection.call(begun.index + 1, clip);
    }

    // What this frame's mix plays of a track: `outputFrames` of output, less what its delay still
    // holds. The callbacks of the sections and beats it starts, in order, up to `limit` (an outgoing
    // track's switch frame). Returns the output frames of it this mix plays.
    int64_t advanceTrack(Track& track, int64_t outputFrames, uint64_t limit){
        const bool held = soloud().getPause(track.handle);
        if (held || outputFrames <= 0) return 0;
        int64_t audible = outputFrames;
        if (track.delayLeft >= outputFrames) {
            track.delayLeft -= outputFrames;
            return 0;
        }
        audible -= track.delayLeft;
        track.delayLeft = 0;
        const uint64_t from = playedFrames(track);
        uint64_t to = from + (uint64_t)std::llround(toTrack(track, audible));
        to = std::min(to, limit);
        while (!track.begun.empty() && track.begun.front().startFrame < to) {
            const Begun next = track.begun.front();
            track.begun.pop_front();
            fireBeats(track, next.startFrame);
            enterSection(track, next);
        }
        fireBeats(track, to);
        return audible;
    }

    void setVolume(Track& track, float volume, double seconds){
        track.volume = volume;
        // a straight line to the value over the frame: SoLoud's fader is read at each mix call of
        // the frame, so the curve's segments start and end on the engine's frames
        if (seconds > 0.0) soloud().fadeVolume(track.handle, volume, seconds);
        else soloud().setVolume(track.handle, volume);
    }

    // a track whose voice ended by itself, or that the audio closed: nothing to stop
    void dropTrack(std::unique_ptr<Track>& track){
        if (track && track->stream) track->stream->collectRetired();
        track.reset();
    }

    void stopTrack(std::unique_ptr<Track>& track, const char* reason){
        if (!track) return;
        if (!voiceGone(*track)) AudioSystem::stopMusicVoice(track->handle, reason);
        dropTrack(track);
    }

    // the switch is over: the outgoing voice stops (reason music)
    void endSwitch(){
        stopTrack(outgoing, "music");
        if (pendingSwitch.active && pendingSwitch.length > 0.0 && currentTrack) musicLine("crossfade_end", currentTrack.get());
        pendingSwitch = Switch();
    }

    // a track for playMusic: every clip loaded and checked; false with one warning naming why
    bool makeTrack(const std::string& name, Track& track){
        track.name = name;
        const std::string who = "playMusic(\"" + name + "\")";
        if (SoundEventLibrary::isEventName(name)) {
            const SoundEventAsset* event = SoundEventLibrary::find(name);
            if (!event) {
                warnOnce("unknown:" + name, "Audio: " + who + ": Sound Event does not resolve");
                return false;
            }
            const SoundEventDef& def = *event->def;
            if (def.mode == SoundEventMode::Layer) {
                warnOnce("layer:" + name, "Audio: " + who + ": a layer event cannot play as music");
                return false;
            }
            if (def.clips.empty()) {
                warnOnce("empty:" + name, "Audio: " + who + ": the event has no clip");
                return false;
            }
            track.isEvent = true;
            track.event = *event;
            for (const SoundEventClip& clip : def.clips) {
                track.paths.push_back(clip.clip.path);
                track.loops.push_back((unsigned int)std::clamp(clip.loops, 0, 1000));
                track.gains.push_back(dbToLinear(clip.volumeDb));
            }
        } else {
            std::string path = name;
            if (SoundEventLibrary::isGuid(name)) AssetMap::findPath(Guid::parse(name), path);
            // a clip: one section repeating its loop region until advanceMusic
            track.paths.push_back(path);
            track.loops.push_back(0);
            track.gains.push_back(1.0f);
        }

        for (const std::string& path : track.paths) {
            std::shared_ptr<AudioClipSource> clip;
            const SoundLoadResult loaded = path.empty() ? SoundLoadResult() : SoundPool::loadClipNow(path, AudioLoadType::Auto);
            if (loaded.state == ResourceLoadState::Finished && loaded.data) {
                clip = std::dynamic_pointer_cast<AudioClipSource>(loaded.data);
                // a clip the pool plays as its file (no settings, or only a tempo): the section reads it
                // through the clip's settings all the same, for its loop region and grid
                if (!clip) clip = std::make_shared<AudioClipSource>(loaded.data, AudioImportTable::find(path).value_or(ClipImportSettings()));
            }
            if (!clip) {
                warnOnce("missing:" + name + ":" + path, "Audio: " + who + ": clip " + path + " does not load");
                return false;
            }
            if (!track.clips.empty() && (clip->getChannels() != track.clips[0]->getChannels() || clip->getSampleRate() != track.clips[0]->getSampleRate())) {
                char text[200];
                std::snprintf(text, sizeof(text), " is %g Hz, %u channels; %s is %g Hz, %u channels", (double)clip->getSampleRate(), clip->getChannels(),
                              track.paths[0].c_str(), (double)track.clips[0]->getSampleRate(), track.clips[0]->getChannels());
                warnOnce("format:" + name, "Audio: " + who + ": music sections differ in sample rate or channels: " + path + text);
                return false;
            }
            const bool playable = clip->getLength() >= SECTION_MIN_SECONDS;
            if (!playable) warnOnce("short:" + path, "Audio: a music section is shorter than 100 ms: " + path + " is skipped");
            track.grids.push_back(makeGrid(*clip));
            track.clips.push_back(std::move(clip));
            track.playable.push_back(playable);
        }
        if (std::none_of(track.playable.begin(), track.playable.end(), [](bool playable) { return playable; })) {
            warnOnce("noplay:" + name, "Audio: " + who + ": every section is shorter than 100 ms");
            return false;
        }
        return true;
    }

    bool sameTrack(const Track& track, const std::string& name){
        if (track.isEvent) {
            const SoundEventAsset* event = SoundEventLibrary::isEventName(name) ? SoundEventLibrary::find(name) : nullptr;
            return event && event->key == track.event.key;
        }
        return track.name == name;
    }

    // a group by name or id ("" none), falling back as upstream does (unknown: Master, warned once)
    std::string groupIdOf(const std::string& name, const std::string& who){
        if (name.empty()) return "";
        const std::string id = AudioMixer::findGroupId(name);
        if (id.empty()) warnOnce("group:" + name, "Audio: " + who + ": mixer group '" + name + "' is not in the mixer, it plays through Master");
        return id;
    }

    std::string trackGroup(const Track& track, const MusicOptions& options){
        const std::string who = "playMusic(\"" + track.name + "\")";
        if (!options.group.empty()) return groupIdOf(options.group, who);
        if (track.isEvent) {
            const SoundEventDef& def = *track.event.def;
            std::string id = def.outputGroupId.empty() ? std::string() : AudioMixer::findGroupId(def.outputGroupId);
            if (id.empty() && !def.outputGroupName.empty()) id = groupIdOf(def.outputGroupName, who);
            if (!id.empty() || !def.outputGroupName.empty()) return id;
        }
        return groupIdOf(AudioSystem::getConfig().musicGroup, who);
    }

    bool wantsPause(const Track& track){
        return AudioSystem::isMusicHeld() || (AudioSystem::isListenerPaused() && !track.ignoreListenerPause);
    }

}

bool MusicPlayer::isSyncName(const std::string& name){
    Sync sync;
    return syncFromName(name, sync);
}

bool MusicPlayer::play(const std::string& name, const MusicOptions& options){
    Sync sync = Sync::Now;
    syncFromName(options.sync, sync);
    const float crossfade = std::isfinite(options.crossfade) ? std::max(0.0f, options.crossfade) : 0.0f;
    if (liveTrack(currentTrack) && sameTrack(*currentTrack, name)) return true;
    if (name.empty()) {
        warnOnce("unknown:", "Audio: playMusic(\"\"): no clip or Sound Event named");
        return false;
    }
    if (!AudioSystem::init()) return false;

    auto track = std::make_unique<Track>();
    if (!makeTrack(name, *track)) return false;
    track->ignoreListenerPause = options.ignoreListenerPause;

    // the first section and the event's draws, once for the whole track (R7)
    int firstClip = -1;
    track->ownVolume = std::max(0.0f, std::isfinite(options.volume) ? options.volume : 1.0f);
    if (track->isEvent) {
        AudioSystem::getSoundEventSeed();
        SoundEventPick pick;
        firstClip = pickSection(*track, &pick);
        if (firstClip < 0) {
            warnOnce("noplay:" + name, "Audio: playMusic(\"" + name + "\"): no section to play");
            return false;
        }
        track->ownVolume *= dbToLinear(pick.volumeDb);
        track->speed = (float)std::pow(2.0, pick.pitchSemitones / 12.0);
    }
    const AudioClipSource& firstSource = *track->clips[firstClip < 0 ? 0 : (size_t)firstClip];
    track->rate = firstSource.getSampleRate();
    track->outputRate = soloud().getBackendSamplerate();
    track->stream = std::make_shared<MusicSequenceSource>(firstSource.getChannels(), firstSource.getSampleRate());
    track->stream->setFirst(makeSection(*track, firstClip));
    if (track->isEvent) prepareNext(*track);
    else track->stream->endAfterCurrent();
    track->groupId = trackGroup(*track, options);

    // a pending switch whose incoming track was never heard goes: the playing one stays the outgoing
    if (pendingSwitch.active && !pendingSwitch.reached && outgoing && !outgoing->stopping) {
        stopTrack(currentTrack, "music");
        currentTrack = std::move(outgoing);
        currentTrack->switchFrame = MusicSequenceSource::NO_STOP;
        currentTrack->stream->stopAt(MusicSequenceSource::NO_STOP);
        setVolume(*currentTrack, pendingSwitch.outgoingFrom, 0.0);
        pendingSwitch = Switch();
    }

    // the switch point on the playing track
    Track* playing = liveTrack(currentTrack) ? currentTrack.get() : nullptr;
    uint64_t switchFrame = 0;
    const int64_t delay = playing ? delayTo(playing, sync, "playMusic(\"" + name + "\", sync)", &switchFrame) : 0;
    if (!playing) sync = Sync::Now;
    const bool fades = crossfade > 0.0f && playing;
    // the voice's latency where the playing one's differs (another group)
    int64_t latency = 0;
    if (playing) latency = (int64_t)AudioSystem::getDeterministicLatency(playing->groupId) - (int64_t)AudioSystem::getDeterministicLatency(track->groupId);
    const uint32_t delayFrames = (uint32_t)std::max<int64_t>(0, delay + latency);

    track->volume = fades ? 0.0f : track->ownVolume;
    AudioSystem::MusicVoiceRequest request;
    request.groupId = track->groupId;
    request.volume = track->volume;
    request.speed = track->speed;
    request.delayFrames = delayFrames;
    request.ignoreListenerPause = track->ignoreListenerPause;
    request.clip = track->paths[firstClip < 0 ? 0 : (size_t)firstClip];
    if (track->isEvent) {
        request.event = track->event.key;
        request.eventPath = track->event.path;
    }
    const uint64_t now = AudioCapture::cursor();
    track->handle = AudioSystem::startMusicVoice(*track->stream, request);
    if (track->handle == 0) {
        warnOnce("start:" + name, "Audio: playMusic(\"" + name + "\"): its voice did not start (the voice budget is full of protected voices, or no output)");
        return false;
    }
    track->delayLeft = delay;
    track->delaySeconds = (double)delayFrames / (double)track->outputRate;
    track->firstSample = now + (uint64_t)delay;
    track->pauseApplied = wantsPause(*track);

    musicLine("play", track.get(), [&](auto& line) {
        if (playing) line.text("from", playing->name);
        line.number("fade_s", fades ? crossfade : 0.0, 6).text("sync", syncName(sync)).integer("boundary_sample", (int64_t)(now + (uint64_t)delay));
    });

    if (playing) {
        // a crossfade in progress: its outgoing track goes at once, the incoming one fades out from where it is
        stopTrack(outgoing, "music");
        pendingSwitch = Switch();
        playing->switchFrame = switchFrame;
        outgoing = std::move(currentTrack);
        pendingSwitch.active = true;
        pendingSwitch.length = fades ? crossfade : 0.0;
        pendingSwitch.outgoingFrom = outgoing->volume;
        if (!options.snapshot.empty()) {
            pendingSwitch.snapshot = options.snapshot;
            pendingSwitch.snapshotSeconds = options.snapshotSeconds >= 0.0f && std::isfinite(options.snapshotSeconds) ? options.snapshotSeconds : crossfade;
        }
        if (!fades) {
            // a cut: exactly at the switch point
            if (delay == 0) stopTrack(outgoing, "music");
            else outgoing->stream->stopAt(switchFrame);
        }
    } else {
        stopTrack(outgoing, "music");
        pendingSwitch = Switch();
        if (!options.snapshot.empty()) {
            pendingSwitch.active = true;
            pendingSwitch.snapshot = options.snapshot;
            pendingSwitch.snapshotSeconds = options.snapshotSeconds >= 0.0f && std::isfinite(options.snapshotSeconds) ? options.snapshotSeconds : crossfade;
        }
    }
    // a track stopMusic was fading out is replaced: it stops now
    if (currentTrack) stopTrack(currentTrack, "music");
    currentTrack = std::move(track);
    return true;
}

void MusicPlayer::stop(float fadeSeconds, const std::string& syncText){
    Sync sync = Sync::Now;
    syncFromName(syncText, sync);
    const double fade = std::isfinite(fadeSeconds) ? std::max(0.0f, fadeSeconds) : 0.0;
    // a switch not reached yet: the incoming track was never heard, the playing one is what stops
    if (pendingSwitch.active && !pendingSwitch.reached && outgoing && !outgoing->stopping) {
        stopTrack(currentTrack, "music");
        currentTrack = std::move(outgoing);
        currentTrack->switchFrame = MusicSequenceSource::NO_STOP;
        currentTrack->stream->stopAt(MusicSequenceSource::NO_STOP);
    }
    // a crossfade's outgoing track goes at once
    stopTrack(outgoing, "music");
    pendingSwitch = Switch();
    if (!liveTrack(currentTrack)) return;
    uint64_t frame = 0;
    const int64_t delay = delayTo(currentTrack.get(), sync, "stopMusic(sync)", &frame);
    const uint64_t now = AudioCapture::cursor();
    musicLine("stop", currentTrack.get(), [&](auto& line) {
        line.number("fade_s", fade, 6).text("sync", syncName(sync)).integer("boundary_sample", (int64_t)(now + (uint64_t)delay));
    });
    Track& track = *currentTrack;
    track.stopping = true;
    track.stream->endAfterCurrent();
    if (delay == 0 && fade == 0.0) {
        stopTrack(currentTrack, "music");
        return;
    }
    track.stopPending = true;
    track.stopLeft = delay;
    track.stopFade = fade;
    track.stopFrom = track.volume;
    if (fade == 0.0) track.stream->stopAt(frame);
}

bool MusicPlayer::advance(){
    if (!liveTrack(currentTrack)) return false;
    currentTrack->stream->requestAdvance();
    const uint64_t played = playedFrames(*currentTrack);
    if (const Begun* section = sectionAt(*currentTrack, played)) currentTrack->advancedSection = section->index;
    musicLine("advance", currentTrack.get());
    return true;
}

SoundHandle MusicPlayer::stinger(const std::string& eventOrClip, const std::string& syncText){
    Sync sync = Sync::Beat;
    syncFromName(syncText, sync);
    // resolved when the voice starts: the boundary as the music is then, and its group (the music's,
    // unless its event has its own output)
    auto resolve = [eventOrClip, sync](std::string& group) -> uint32_t {
        group = AudioSystem::getConfig().musicGroup;
        std::string stingerGroup = AudioMixer::findGroupId(group);
        if (SoundEventLibrary::isEventName(eventOrClip)) {
            if (const SoundEventAsset* event = SoundEventLibrary::find(eventOrClip)) {
                const SoundEventDef& def = *event->def;
                if (!def.outputGroupId.empty() || !def.outputGroupName.empty()) {
                    group.clear();
                    stingerGroup = AudioMixer::findGroupId(def.outputGroupId.empty() ? def.outputGroupName : def.outputGroupId);
                }
            }
        }
        Sync used = sync;
        Track* track = gridTrack();
        if (track) drainReports(*track);
        const int64_t delay = delayTo(track, used, "playStinger(\"" + eventOrClip + "\", \"" + syncName(sync) + "\")", nullptr);
        // the stinger's path to the output may be shorter or longer than the music's
        int64_t latency = 0;
        if (track && used != Sync::Now) {
            latency = (int64_t)AudioSystem::getDeterministicLatency(track->groupId) - (int64_t)AudioSystem::getDeterministicLatency(stingerGroup);
        }
        const uint64_t now = AudioCapture::cursor();
        musicLine("stinger", track, [&](auto& line) {
            line.text("clip", eventOrClip).text("sync", syncName(used)).integer("boundary_sample", (int64_t)(now + (uint64_t)delay));
        });
        return (uint32_t)std::max<int64_t>(0, delay + latency);
    };
    return AudioSystem::playMusicStinger(eventOrClip, resolve);
}

void MusicPlayer::update(double unscaledFrameSeconds){
    if (!AudioSystem::isInited()) return;
    AudioSystem::updateMusicSystem(unscaledFrameSeconds);
    const int64_t mixFrames = AudioSystem::getFrameMixFrames(unscaledFrameSeconds);

    // what went silent in the last mix stops now: a switch's outgoing track, a stop's
    if (pendingSwitch.done) endSwitch();
    for (std::unique_ptr<Track>* slot : {&outgoing, &currentTrack}) {
        std::unique_ptr<Track>& track = *slot;
        if (track && track->stopPending && track->stopLeft == 0 && track->stopElapsed >= track->stopFade && track->stopElapsed > 0.0) {
            stopTrack(track, "music");
        }
    }
    // tracks whose voice went by itself (a playlist's end, AudioSystem.stopAll)
    if (outgoing && voiceGone(*outgoing)) {
        dropTrack(outgoing);
        if (pendingSwitch.reached) endSwitch();
    }
    if (currentTrack && voiceGone(*currentTrack)) {
        if (!currentTrack->stopping && currentTrack->stream && currentTrack->stream->hasEnded()) musicLine("end", currentTrack.get());
        dropTrack(currentTrack);
        stopTrack(outgoing, "music");
        pendingSwitch = Switch();
    }

    // the hold: the editor's Play pause, AudioListener.pause
    for (Track* track : {outgoing.get(), currentTrack.get()}) {
        if (!track) continue;
        const bool pause = wantsPause(*track);
        if (pause != track->pauseApplied) {
            AudioSystem::setMusicVoicePaused(track->handle, pause);
            track->pauseApplied = pause;
        }
    }

    // what this frame mixes: sections, beats, the switch point
    if (outgoing) {
        drainReports(*outgoing);
        advanceTrack(*outgoing, mixFrames, outgoing->switchFrame);
    }
    int64_t incomingAudible = 0;
    if (currentTrack) {
        drainReports(*currentTrack);
        incomingAudible = advanceTrack(*currentTrack, mixFrames, MusicSequenceSource::NO_STOP);
    }

    // the switch point inside this frame's mix: the snapshot with it, the crossfade from it
    if (pendingSwitch.active && currentTrack && !pendingSwitch.done) {
        bool startsNow = false;
        if (!pendingSwitch.reached && incomingAudible > 0) {
            pendingSwitch.reached = true;
            startsNow = true;
        }
        if (startsNow && !pendingSwitch.snapshot.empty()) {
            AudioMixer::transitionTo(pendingSwitch.snapshot, pendingSwitch.snapshotSeconds, false, "music");
            pendingSwitch.snapshot.clear();
        }
        if (pendingSwitch.reached) {
            if (!outgoing) {
                pendingSwitch = Switch();
            } else if (pendingSwitch.length <= 0.0) {
                // a cut: the outgoing voice stopped itself at the switch frame
                pendingSwitch.done = true;
            } else if (incomingAudible > 0) {
                pendingSwitch.elapsed += startsNow ? (double)incomingAudible / (double)currentTrack->outputRate : unscaledFrameSeconds;
                const double t = std::min(1.0, pendingSwitch.elapsed / pendingSwitch.length);
                setVolume(*outgoing, (float)(pendingSwitch.outgoingFrom * std::cos(HALF_PI * t)), unscaledFrameSeconds);
                setVolume(*currentTrack, (float)(currentTrack->ownVolume * std::sin(HALF_PI * t)), unscaledFrameSeconds);
                pendingSwitch.done = t >= 1.0;
            }
        }
    }

    // a stop's fade, from its sync point
    for (Track* track : {outgoing.get(), currentTrack.get()}) {
        if (!track || !track->stopPending || soloud().getPause(track->handle) || mixFrames <= 0) continue;
        if (track->stopLeft >= mixFrames && track->stopElapsed == 0.0) {
            track->stopLeft -= mixFrames;
            continue;
        }
        const bool startsNow = track->stopElapsed == 0.0;
        track->stopElapsed += startsNow ? (double)(mixFrames - track->stopLeft) / (double)track->outputRate : unscaledFrameSeconds;
        track->stopLeft = 0;
        // a fade of 0: the voice stopped itself at its frame; it goes after this mix
        if (track->stopFade <= 0.0) continue;
        const double t = std::min(1.0, track->stopElapsed / track->stopFade);
        setVolume(*track, (float)(track->stopFrom * (1.0 - t)), unscaledFrameSeconds);
    }

    if (outgoing) prepareNext(*outgoing);
    if (currentTrack) prepareNext(*currentTrack);
}

void MusicPlayer::reset(){
    pendingSwitch = Switch();
    previewing = false;
    if (AudioSystem::isInited()) AudioSystem::stopMusicSystem("scene");
    if (currentTrack && currentTrack->stream) currentTrack->stream->collectRetired();
    if (outgoing && outgoing->stream) outgoing->stream->collectRetired();
    currentTrack.reset();
    outgoing.reset();
    warnings().clear();
    Audio::onMusicBeat.clear();
    Audio::onMusicSection.clear();
}

void MusicPlayer::setPreview(bool preview){
    previewing = preview;
}

bool MusicPlayer::isPreview(){
    return previewing;
}

void MusicPlayer::forget(){
    pendingSwitch = Switch();
    currentTrack.reset();
    outgoing.reset();
}

std::string MusicPlayer::current(){
    return liveTrack(currentTrack) ? currentTrack->name : std::string();
}

double MusicPlayer::time(){
    if (!liveTrack(currentTrack)) return 0.0;
    return (double)playedFrames(*currentTrack) / (double)currentTrack->rate;
}

std::optional<MusicPosition> MusicPlayer::position(){
    if (!liveTrack(currentTrack)) return std::nullopt;
    const Track& track = *currentTrack;
    const uint64_t played = playedFrames(track);
    const Begun* section = sectionAt(track, played);
    if (!section) return std::nullopt;
    const Grid grid = gridOf(track, section->clipIndex);
    if (!grid.valid) return std::nullopt;
    MusicPosition out;
    out.section = section->index + 1;
    out.bpm = grid.bpm;
    out.beatsPerBar = grid.beatsPerBar;
    const double beats = ((double)played - (double)section->startFrame - grid.firstBeat) / grid.framesPerBeat;
    if (beats >= 0.0) {
        const int64_t k = (int64_t)std::floor(beats);
        out.bar = (int)(k / grid.beatsPerBar) + 1;
        out.beat = (int)(k % grid.beatsPerBar) + 1;
        out.beatFraction = beats - (double)k;
    }
    return out;
}

MusicPlayer::Status MusicPlayer::status(){
    Status out;
    if (!liveTrack(currentTrack)) return out;
    const Track& track = *currentTrack;
    out.playing = true;
    out.track = track.name;
    const Begun* section = sectionAt(track, playedFrames(track));
    if (section) {
        out.section = section->index + 1;
        out.clip = track.paths[section->clipIndex < 0 ? 0 : (size_t)section->clipIndex];
    }
    out.timeSeconds = time();
    if (const std::optional<MusicPosition> where = position()) {
        out.bar = where->bar;
        out.beat = where->beat;
        out.bpm = where->bpm;
    }
    out.crossfading = pendingSwitch.active && outgoing != nullptr;
    out.group = track.groupId.empty() ? std::string("Master") : AudioMixer::getGroupName(track.groupId);
    return out;
}
