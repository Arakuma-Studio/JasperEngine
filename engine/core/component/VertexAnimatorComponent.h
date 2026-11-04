// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (vertex animation spec 3.3-3.7)
// - Purpose: plays a Vertex Animation (.vat) on the GameObject's mesh (Model = "<stem>.vat.glb"),
//   and on each instance of an Instanced Mesh on the same entity. Display "Vertex Animator",
//   Add Component > Animation, Lua VertexAnimator / object:getVertexAnimator(), scene key
//   "vertexanimator".
// - VertexAnimatorSystem (engine/core/subsystem) loads the asset, advances its clock, applies
//   play requests, writes the GPU state (gpu* fields, and InstanceData::vat0/vat1 of the instanced
//   mesh), fires events / finishes, moves attachments, sets MeshComponent::animatedBounds.
//   RenderSystem only reads: asset->getTexture(), gpuState0/1, gpuNow, gpuMode.
// - Thread-safety: an ECS component (main thread). Decoding fills the authored fields only.
// - Invariants: only the authored fields are serialized / cloned / emitted on export; the runtime
//   block is reset on clone and decode; callbacks are cleared with the other script callbacks.

#ifndef VERTEX_ANIMATOR_COMPONENT_H
#define VERTEX_ANIMATOR_COMPONENT_H

#include "Export.h"
#include "animation/VertexAnimation.h"
#include "ecs/Entity.h"
#include "math/Vector4.h"
#include "util/FunctionSubscribe.h"

#include <memory>
#include <string>
#include <vector>

namespace jasperengine{

    struct AnimationEvent;
    namespace vat{ struct VertexAnimatorRuntime; }

    struct JASPERENGINE_API VertexAnimatorAttachment{
        std::string socket;
        Entity entity = NULL_ENTITY;    // a GameObject that follows the socket (single entity mode)

        bool operator==(const VertexAnimatorAttachment& o) const{ return socket == o.socket && entity == o.entity; }
        bool operator!=(const VertexAnimatorAttachment& o) const{ return !(*this == o); }
    };

    // a play request queued by Lua/the editor: applied at once when the asset is loaded and
    // started (VertexAnimatorSystem::applyRequests, also from inside a callback), else at the
    // system's next update
    struct JASPERENGINE_API VertexAnimatorRequest{
        enum class Kind : uint8_t { Play, CrossFade, Blend, Queue, Stop, SetTime, SetSpeed, RandomizeTimes };
        Kind kind = Kind::Play;
        int instance = -1;          // -1 = the entity (and, for Play/CrossFade/Stop/RandomizeTimes with
                                    // `all`, every instance)
        bool all = false;
        std::string clip;
        std::string clipB;          // Blend
        float value = 0.0f;         // fade seconds, blend weight, time, speed or seed
    };

    struct JASPERENGINE_API VertexAnimatorComponent{
        bool enabled = true;

        // authored (serialized)
        std::string animation;          // asset-relative .vat path
        std::string guid;               // the .vat's GUID (editor)
        std::string defaultClip;        // empty = the first clip
        bool playAutomatically = true;
        float speed = 1.0f;
        bool randomStart = true;        // random start phase per instance / entity (seed: entity id + index)
        float defaultFadeTime = 0.2f;
        bool instanceEvents = false;    // fire events/finishes for every instance (cost O(instances))
        std::vector<VertexAnimatorAttachment> attachments;
        // per instance (parallel to InstancedMeshComponent::instances, may be shorter): the clip
        // an instance starts with when playAutomatically (empty or unknown = the default clip)
        // and its own speed multiplier (setInstanceSpeed; missing = 1). Create Crowd writes them
        std::vector<std::string> instanceClips;
        std::vector<float> instanceSpeeds;

        // runtime (not serialized)
        bool preview = false;           // editor: animate in edit mode (Inspector "Preview")
        bool needsLoad = true;
        bool started = false;
        uint64_t loadedGeneration = 0;
        std::string loadError;
        std::shared_ptr<const vat::VertexAnimation> asset;
        vat::VatTrackPair state;                    // the entity's own tracks
        std::vector<vat::VatTrackPair> instanceStates;  // parallel to InstancedMeshComponent::instances
        std::vector<std::string> queued;            // queue(name) for the entity
        std::vector<VertexAnimatorRequest> requests;
        float lastNow = 0.0f;                       // the last update's clock, for the getters (the system
                                                    // crosses events on VertexAnimatorRuntime::lastNow, a double)
        // the system's per-animator bookkeeping (its clock, per-state speeds, holds, queues, pending
        // callbacks: animation/VertexAnimatorRuntime.h); reset on clone and decode
        std::shared_ptr<vat::VertexAnimatorRuntime> runtime;
        // GPU state the RenderSystem reads
        vat::VatMode gpuMode = vat::VatMode::Vertices;
        Vector4 gpuState0 = Vector4(-1, 0, 1, -1);   // vatPackInstance(state)
        Vector4 gpuState1 = Vector4(0, 1, 0, 0);
        float gpuNow = 0.0f;                        // clock for this frame (scene time - epoch)
        bool gpuActive = false;                     // enabled, asset loaded and mesh matches: draw with VAT

        // events: (event) for the entity; (instance, event) per instance when instanceEvents
        FunctionSubscribe<void(const AnimationEvent&)> onEvent;
        FunctionSubscribe<void(const std::string&)> onFinish;
        FunctionSubscribe<void(int, const AnimationEvent&)> onInstanceEvent;
        FunctionSubscribe<void(int, const std::string&)> onInstanceFinish;

        // every event, for the clears at Stop and on tag removal (tools/check_event_clears.py)
        template<typename F> void forEachEvent(F&& f) {
            f(onEvent); f(onFinish); f(onInstanceEvent); f(onInstanceFinish);
        }
    };

}

#endif //VERTEX_ANIMATOR_COMPONENT_H
