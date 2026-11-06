// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "component/AudioSourceComponent.h"

#if JASPERENGINE_SOUND_LAYER

#include "Audio.h"

#include "Log.h"
#include "audio/MusicPlayer.h"
#include "object/EntityHandle.h"
#include "subsystem/AudioSystem.h"

#include <cmath>
#include <set>

using namespace jasperengine;

PlayOptions& PlayOptions::following(const EntityHandle& gameObject) {
    targetScene = gameObject.getScene();
    target = gameObject.getEntity();
    return *this;
}

SoundHandle Audio::play(const std::string& eventOrClip, const PlayOptions& options) {
    return AudioSystem::playFree(eventOrClip, options, "cpp");
}

int Audio::stopAll(const std::string& group) {
    return AudioSystem::stopFreeAndOneShots(group);
}

// ---- the music (docs/specs/S39-music.md R19): arguments checked here, then MusicPlayer, at once or
// at the frame's audio update when called inside a physics callback

FunctionSubscribe<void(int, int)> Audio::onMusicBeat;
FunctionSubscribe<void(int, const std::string&)> Audio::onMusicSection;

namespace {

    // a refused argument warns once per function and reason, for the process
    bool refuse(const std::string& key, const std::string& message) {
        static auto* warned = new std::set<std::string>();
        if (warned->insert(key).second) Log::warn("%s", message.c_str());
        return false;
    }

    bool validTime(const char* function, const char* name, float value) {
        if (std::isfinite(value) && value >= 0.0f) return true;
        return refuse(std::string(function) + ":" + name, std::string("Audio.") + function + ": " + name + " must be a number of seconds from 0; nothing happens");
    }

    bool validSync(const char* function, const std::string& sync, bool sectionEnd) {
        if (MusicPlayer::isSyncName(sync) && (sectionEnd || sync != "section_end")) return true;
        return refuse(std::string(function) + ":sync:" + sync, std::string("Audio.") + function + ": sync '" + sync + "' is not " +
                      (sectionEnd ? "\"now\", \"beat\", \"bar\" or \"section_end\"" : "\"now\", \"beat\" or \"bar\"") + "; nothing happens");
    }

}

bool Audio::playMusic(const std::string& eventOrClip, const MusicOptions& options) {
    if (!validTime("playMusic", "crossfade", options.crossfade) || !validSync("playMusic", options.sync, true) ||
        (options.snapshotSeconds != -1.0f && !validTime("playMusic", "snapshotSeconds", options.snapshotSeconds)) ||
        !(std::isfinite(options.volume) && options.volume >= 0.0f ? true : refuse("playMusic:volume", "Audio.playMusic: volume must be a number from 0; nothing happens"))) {
        return false;
    }
    if (AudioSystem::isDeferringCalls()) {
        AudioSystem::deferToAudioUpdate([eventOrClip, options]() { MusicPlayer::play(eventOrClip, options); });
        return true;
    }
    return MusicPlayer::play(eventOrClip, options);
}

void Audio::stopMusic(float fade, const std::string& sync) {
    if (!validTime("stopMusic", "fade", fade) || !validSync("stopMusic", sync, true)) return;
    if (AudioSystem::isDeferringCalls()) {
        AudioSystem::deferToAudioUpdate([fade, sync]() { MusicPlayer::stop(fade, sync); });
        return;
    }
    MusicPlayer::stop(fade, sync);
}

bool Audio::advanceMusic() {
    if (AudioSystem::isDeferringCalls()) {
        AudioSystem::deferToAudioUpdate([]() { MusicPlayer::advance(); });
        return !MusicPlayer::current().empty();
    }
    return MusicPlayer::advance();
}

SoundHandle Audio::playStinger(const std::string& eventOrClip, const std::string& sync) {
    if (!validSync("playStinger", sync, false)) return SoundHandle();
    // deferred inside a physics callback by the free voice itself: its boundary is resolved when it starts
    return MusicPlayer::stinger(eventOrClip, sync);
}

std::string Audio::getMusic() {
    return MusicPlayer::current();
}

double Audio::getMusicTime() {
    return MusicPlayer::time();
}

std::optional<MusicPosition> Audio::getMusicPosition() {
    return MusicPlayer::position();
}

#endif // JASPERENGINE_SOUND_LAYER
