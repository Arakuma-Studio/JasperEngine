// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (vertex animation spec 3.3, 3.4, 3.6, 3.7)
// - Purpose: plays the Vertex Animators of a scene. Per animator: its own clock (the scene delta in
//   play mode; in the editor's edit mode only while `preview` is on), rebased every
//   vat::VAT_EPOCH_SECONDS; the asset from VertexAnimationPool (retried every second after a
//   failure, loaded again when the pool's generation of the path moves: a rebake); the mesh
//   check (`gpuActive`, else `loadError` "mesh_mismatch: ..." and the mesh draws at rest);
//   playAutomatically / defaultClip / randomStart; the queued requests; crossfade completion,
//   queues, finishes and clip events (the dominant track, weight >= 0.5); attachments following
//   their sockets; MeshComponent::animatedBounds.
// - Writes only on change: the entity's gpuState0/1 and each instance's InstanceData::vat0/vat1
//   (then InstancedMeshComponent::needUpdateInstances) are written when a state changes, so a
//   crowd of looping characters costs no per-instance work per frame. gpuNow is written every
//   update. Held states (speed 0, stopped) are packed with speed 0 and their local time as the
//   start (vat::vatLocalTime): written once, a paused crowd costs nothing per frame either.
//   Instances written elsewhere (needUpdateInstances seen at the update, or set by user code in
//   the callbacks) are compared and written again. Disabled: the rest state is written once (the
//   entity's and every instance's), gpuActive false; enabled again, every state is written back.
// - Events cross (previous update's now, now] on a double clock (VertexAnimatorRuntime::lastNow):
//   each fires exactly once at any frame rate and clock value.
// - Registered after the Lattice system, before Render. The editor pauses it in edit mode
//   (Project::pauseEngineScene): then only previewing animators advance and no callback fires.
// - User code (onEvent, onFinish, onInstanceEvent, onInstanceFinish, the scripts' event methods)
//   runs at the end of the update, in time order per GameObject. Requests made from it apply
//   right away, at this update's clock (applyRequests, as from any script), and are drawn this
//   frame; the callbacks this update already collected still run. An instance removed by a
//   callback drops its own callbacks still to come, the later instances' are re-indexed.

#ifndef VERTEXANIMATORSYSTEM_H
#define VERTEXANIMATORSYSTEM_H

#include "SubSystem.h"

#include "component/VertexAnimatorComponent.h"

#include <set>
#include <string>
#include <vector>

namespace jasperengine{

    class JASPERENGINE_API VertexAnimatorSystem : public SubSystem {

    private:
        bool updating = false;
        bool dispatching = false;
        std::vector<Entity> withCallbacks;
        std::set<std::string> warnedEventHandlers;

        void updateAnimator(Entity entity, VertexAnimatorComponent& animator, double dt);
        // true when a callback was called
        bool dispatchCallbacks();
        // after the callbacks: instanced meshes they marked (needUpdateInstances) get their
        // instances' states compared and written again (and new instances started)
        void refreshInstances();

    public:
        VertexAnimatorSystem(Scene* scene);

        // Every animator back to its start (clock 0, playAutomatically decided again, requests and
        // callbacks dropped): the editor calls it when Play stops, since play reuses the scene
        void resetAnimators();

        // Edit mode: every animator with Preview on goes back to its start with Preview off (play
        // starts, a save, a scene switch). True when one was previewing
        bool stopPreviews();

        // Applies the animator's queued requests now (at its last update's time) when its asset is
        // loaded and started, so a script reads their effect right away; otherwise (loading, or
        // inside the update) they wait for the next update
        void applyRequests(Entity entity);

        void load() override;
        void draw() override;
        void destroy() override;
        void update(double dt) override;

        void onComponentRemoved(Entity entity, ComponentId componentId) override;
    };

}

#endif //VERTEXANIMATORSYSTEM_H
