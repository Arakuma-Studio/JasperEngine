// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ClipAudio.h"

#include "animation/ClipBinding.h"

#include <filesystem>
#include <vector>

using namespace jasperengine;

bool ClipAudioCue::operator==(const ClipAudioCue& other) const{
    return time == other.time && kind == other.kind && sound == other.sound && path == other.path && group == other.group &&
           snapshot == other.snapshot && name == other.name && spatial == other.spatial && followSpeed == other.followSpeed &&
           unscaled == other.unscaled && volumeDb == other.volumeDb && pitchSemitones == other.pitchSemitones &&
           seconds == other.seconds && stop == other.stop;
}

const char* anim::cueKindName(ClipCueKind kind){
    switch (kind){
        case ClipCueKind::Sound: return "sound";
        case ClipCueKind::Source: return "source";
        case ClipCueKind::Snapshot: return "snapshot";
    }
    return "sound";
}

bool anim::cueKindFromName(const std::string& name, ClipCueKind& out){
    if (name == "sound") out = ClipCueKind::Sound;
    else if (name == "source") out = ClipCueKind::Source;
    else if (name == "snapshot") out = ClipCueKind::Snapshot;
    else return false;
    return true;
}

const char* anim::cueStopName(ClipCueStop stop){
    return stop == ClipCueStop::StateExit ? "state_exit" : "none";
}

bool anim::cueStopFromName(const std::string& name, ClipCueStop& out){
    if (name == "none") out = ClipCueStop::None;
    else if (name == "state_exit") out = ClipCueStop::StateExit;
    else return false;
    return true;
}

std::string anim::validateCue(const ClipAudioCue& cue){
    std::vector<PathSegment> segments;
    switch (cue.kind){
        case ClipCueKind::Sound:
            if (!cue.sound.isSet()) return "a sound cue needs a sound (an audio clip or a .soundevent)";
            if (cue.spatial && !parsePath(cue.path, segments)) return "\"" + cue.path + "\" is not a binding path";
            break;
        case ClipCueKind::Source:
            if (!parsePath(cue.path, segments)) return "\"" + cue.path + "\" is not a binding path";
            break;
        case ClipCueKind::Snapshot:
            if (cue.snapshot.empty()) return "a snapshot cue needs a snapshot name";
            break;
    }
    return "";
}

std::string anim::cueLabel(const ClipAudioCue& cue){
    if (!cue.name.empty()) return cue.name;
    switch (cue.kind){
        case ClipCueKind::Sound: return std::filesystem::path(cue.sound.path).stem().string();
        case ClipCueKind::Source: return cue.path.empty() ? std::string("source") : cue.path;
        case ClipCueKind::Snapshot: return cue.snapshot;
    }
    return "";
}
