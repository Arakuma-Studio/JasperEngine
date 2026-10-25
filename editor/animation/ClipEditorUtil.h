// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Small helpers the clip editor's parts share (the Inspector's clip sections, the Animation
// window, the clip scene actions and commands, validation and the converters), so a label or
// a file read is written once. Header-only, no ImGui. Main thread or any thread (pure).

#pragma once

#include "animation/AnimationClip.h"
#include "animation/AnimationClipIO.h"
#include "ecs/Entity.h"
#include "registry/EntityRegistry.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace jasperengine::editor::clipui{

    // The name of a channel in the editor UI (the Inspector's track list, the window's rows):
    // "Position", "Rotation", "Scale", "Blend Shapes", "Lattice", "Property"
    inline const char* channelLabel(ClipChannel channel){
        switch (channel){
            case ClipChannel::Position: return "Position";
            case ClipChannel::Rotation: return "Rotation";
            case ClipChannel::Scale: return "Scale";
            case ClipChannel::Morph: return "Blend Shapes";
            case ClipChannel::Lattice: return "Lattice";
            case ClipChannel::Property: return "Property";
        }
        return "";
    }

    // What a track of the root ("" path) is called where no GameObject is bound
    inline const char* rootLabel(){
        return "(Root)";
    }

    // the last '/' segment of a clip path (the path itself without one)
    inline std::string lastSegment(const std::string& path){
        const size_t slash = path.rfind('/');
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }

    // "'Name'": a GameObject in messages
    inline std::string quotedName(EntityRegistry* registry, Entity entity){
        return "'" + registry->getEntityName(entity) + "'";
    }

    // ---- time texts (one place; each form has its own use) ----

    // a duration or time in the Inspector: "1.25 s"
    inline std::string formatSeconds(float seconds){
        char text[32];
        std::snprintf(text, sizeof(text), "%.2f s", std::isfinite(seconds) ? seconds : 0.0f);
        return text;
    }

    // a time in messages, as short as it is exact: "0.5", "1.125" (validation, event texts)
    inline std::string formatSecondsPlain(float seconds){
        char text[32];
        std::snprintf(text, sizeof(text), "%g", seconds);
        return text;
    }

    // the Animation window's ruler label: the frame number, or seconds
    inline std::string formatTime(float time, float fps, bool frames){
        char text[48];
        if (frames){
            std::snprintf(text, sizeof(text), "%d", (int)std::lround(time * fps));
        }else{
            std::snprintf(text, sizeof(text), "%.2f", time);
        }
        return text;
    }

    // both forms, the shown one first: "frame 21 (0.700 s)" / "0.700 s (frame 21)"
    inline std::string formatBoth(float time, float fps, bool frames){
        char text[64];
        const long frame = std::lround(time * fps);
        if (frames) std::snprintf(text, sizeof(text), "frame %ld (%.3f s)", frame, time);
        else std::snprintf(text, sizeof(text), "%.3f s (frame %ld)", time, frame);
        return text;
    }

    // The value a key written at `time` into `track` gets (Record, K, Add Key, paste): a rotation
    // on the side of the key it replaces there, else of the key before `time` (the first key when
    // none is), by anim::alignQuaternionKey, so a Hermite segment never flips. Other channels:
    // `value` as it is. `tolerance`: how close a key replaced at `time` is (the caller's own rule).
    inline std::vector<float> alignedRotationKeyValue(const ClipTrackData& track, float time, std::vector<float> value,
                                                      float tolerance = anim::ANIM_TIME_EPSILON){
        if (track.channel != ClipChannel::Rotation || track.keys.empty()) return value;
        const ClipKey* reference = &track.keys.front();
        for (const ClipKey& key : track.keys){
            if (std::fabs(key.time - time) <= tolerance){
                reference = &key;
                break;
            }
            if (key.time < time) reference = &key;
        }
        anim::alignQuaternionKey(value, reference->value);
        return value;
    }

    // the bytes of a file; false when it can't be opened
    inline bool readWholeFile(const std::filesystem::path& path, std::string& content){
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return true;
    }

}
