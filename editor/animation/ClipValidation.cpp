// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ClipValidation.h"

#include "animation/AnimationClipIO.h"

using namespace jasperengine;
using namespace jasperengine::editor;

// TODO(T2d): everything below. The stubs report every track and event Unchecked.

bool ClipValidationReport::ok() const{
    for (const TrackReport& track : tracks){
        if (track.status != TrackBindingStatus::Ok && track.status != TrackBindingStatus::DuplicateSiblingName &&
                track.status != TrackBindingStatus::Unchecked && track.status != TrackBindingStatus::NotSampled){
            return false;
        }
    }
    for (const EventReport& event : events){
        if (event.status == EventHandlerStatus::NotFound) return false;
    }
    return true;
}

ClipValidationReport ClipValidation::validate(const ClipData& data, EntityRegistry* registry, Entity root,
                                              const ClipValidationOptions& options){
    (void)registry;
    (void)options;
    ClipValidationReport report;
    report.root = root;
    for (size_t i = 0; i < data.tracks.size(); i++){
        TrackReport track;
        track.track = i;
        track.path = data.tracks[i].path;
        track.channel = data.tracks[i].channel;
        report.tracks.push_back(track);
    }
    for (size_t i = 0; i < data.events.size(); i++){
        EventReport event;
        event.event = i;
        event.function = data.events[i].function;
        report.events.push_back(event);
    }
    return report;
}

ClipValidationReport ClipValidation::validateInScene(Project* project, SceneProject* scene, const ClipData& data, Entity root){
    (void)project;
    (void)scene;
    return validate(data, nullptr, root);
}

std::vector<std::string> ClipValidation::scriptFunctions(Project* project, SceneProject* scene, Entity entity){
    (void)project;
    (void)scene;
    (void)entity;
    return {};
}

std::vector<std::string> ClipValidation::nearestPaths(EntityRegistry* registry, Entity root, const std::string& path, size_t max){
    (void)registry;
    (void)root;
    (void)path;
    (void)max;
    return {};
}

std::string ClipValidation::trackLabel(const ClipTrackData& track){
    return track.path + " : " + anim::channelName(track.channel);
}
