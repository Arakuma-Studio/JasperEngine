// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Small helpers the clip editor's parts share (the Inspector's clip sections, the Animation
// window, the clip scene actions and commands, validation and the converters), so a label or
// a file read is written once. Header-only, no ImGui. Main thread or any thread (pure).

#pragma once

#include "animation/AnimationClip.h"
#include "ecs/Entity.h"
#include "registry/EntityRegistry.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

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

    // the bytes of a file; false when it can't be opened
    inline bool readWholeFile(const std::filesystem::path& path, std::string& content){
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return true;
    }

}
