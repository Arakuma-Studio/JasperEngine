// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// ActionSystem's Animation Players (animation clips spec 3.3, 9.R1/R2/R9/R10/R16-R21): loading
// and binding, states, fades, queue, events, the rest term in the pose blend, edit-mode preview.
// TODO(T1b): everything below is a stub.

#include "ActionSystem.h"

#include "animation/KeyframeSampling.h"

using namespace jasperengine;

void ActionSystem::updatePlayers(double dt){
    // TODO(T1b)
    (void)dt;
}

void ActionSystem::dispatchPlayerCallbacks(){
    // TODO(T1b)
}

ActionSystem::PlayerPreviewResult ActionSystem::previewPlayer(Entity root, const PlayerPreview& preview, anim::ClipPreviewSession& session){
    // TODO(T1b)
    (void)root;
    (void)preview;
    (void)session;
    return PlayerPreviewResult();
}

void ActionSystem::sampleClip(const AnimationClip& clip, float time, std::vector<anim::TrackSample>& out){
    anim::sampleClipTracks(clip, time, out);
}

void ActionSystem::resetPlayers(){
    // TODO(T1b)
}
