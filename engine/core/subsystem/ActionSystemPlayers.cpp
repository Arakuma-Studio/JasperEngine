// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// ActionSystem's Animation Players (animation clips spec 3.3, 9.R1/R2/R9/R10/R16-R21): loading
// and binding, states, fades, queue, events, the rest term in the pose blend, edit-mode preview.
// TODO(T1b): everything below is a stub.

#include "ActionSystem.h"

#include "Scene.h"

#include "animation/AnimationPlayerRuntime.h"
#include "animation/ClipBinding.h"
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

void ActionSystem::restorePlayerPreview(anim::ClipPreviewSession& session){
    // complete: what previewPlayer wrote goes back to its rest
    for (const std::pair<Entity, ClipChannel>& entry : session.written){
        auto it = session.rest.find(entry.first);
        if (it == session.rest.end()) continue;
        const anim::ChannelRest& rest = it->second;
        std::vector<float> value;
        switch (entry.second){
            case ClipChannel::Position:
                if (!rest.hasTransform) continue;
                value = {rest.position.x, rest.position.y, rest.position.z};
                break;
            case ClipChannel::Scale:
                if (!rest.hasTransform) continue;
                value = {rest.scale.x, rest.scale.y, rest.scale.z};
                break;
            case ClipChannel::Rotation:
                if (!rest.hasTransform) continue;
                value.resize(4);
                quaternionToXYZW(rest.rotation, value.data());
                break;
            case ClipChannel::Morph:
                if (!rest.hasMorph) continue;
                value = rest.morphWeights;
                break;
            case ClipChannel::Lattice:
                if (!rest.hasLattice) continue;
                for (const Vector3& offset : rest.latticeOffsets) value.insert(value.end(), {offset.x, offset.y, offset.z});
                break;
            case ClipChannel::Property:
                continue;
        }
        anim::writeChannelValue(scene, entry.first, entry.second, {}, value);
    }
    session.reset();
}

void ActionSystem::sampleClip(const AnimationClip& clip, float time, std::vector<anim::TrackSample>& out){
    anim::sampleClipTracks(clip, time, out);
}

void ActionSystem::resetPlayers(){
    // TODO(T1b)
}
