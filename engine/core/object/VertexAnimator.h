// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (vertex animation spec 3.8)
// - Purpose: the VertexAnimator object (Lua `VertexAnimator`, `object:getVertexAnimator()`): plays
//   the clips of a Vertex Animation (.vat) on a GameObject and on each instance of its Instanced
//   Mesh. Calls queue a VertexAnimatorRequest the system applies right away when the asset is
//   loaded (else at its next update, once loaded); reads come from the state the system wrote.
// - The constructor ADDS the component when the entity has none (AnimationPlayer precedent); Lua
//   `object:getVertexAnimator()` returns nil when the GameObject has none.
// - Thread-safety: main thread (ECS).
// - Semantics:
//   * play(name) starts name now (at its end when the speed is negative and it does not loop);
//     play(name, fade) with fade > 0 = crossFade(name, fade); crossFade(name) uses
//     defaultFadeTime. A crossfade moves A to B linearly over `fade`; at its end B is the current
//     clip. Crossfading during a crossfade continues from the dominant clip (weight >= 0.5).
//   * blend(a, b, weight) holds a and b at a fixed weight, b's phase following a's (walk/run);
//     calling it again with the same pair only moves the weight.
//   * queue(name) plays name when the current clip ends (once / clamp_forever) or ends its cycle
//     (loop / ping_pong); with nothing playing it plays now. play / crossFade / stop clear it.
//   * stop() holds the current pose (isPlaying() false); play starts again.
//   * time / normalizedTime / currentClip: the current clip (the one faded to during a
//     crossfade, A in a blend); time is settable (seconds in the clip).
//   * events and finishes reach onEvent / onFinish and the scripts' methods only in play mode and
//     only for a GameObject without an Instanced Mesh; instances fire onInstanceEvent /
//     onInstanceFinish when instanceEvents is on.
//   * instance indices count from 0 like Mesh:getInstance; an unknown index or clip name logs a
//     warning once and does nothing.

#ifndef VERTEXANIMATOR_H
#define VERTEXANIMATOR_H

#include "EntityHandle.h"
#include "component/VertexAnimatorComponent.h"
#include "math/Quaternion.h"
#include "math/Vector3.h"

#include <string>
#include <vector>

namespace jasperengine{

    // A socket's pose in the GameObject's space (getSocketPose); valid false when the socket or
    // the asset is not there
    struct JASPERENGINE_API VertexAnimatorSocketPose{
        Vector3 position = Vector3(0, 0, 0);
        Quaternion rotation;
        bool valid = false;
    };

    class JASPERENGINE_API VertexAnimator: public EntityHandle{
    private:
        void request(VertexAnimatorRequest request);
        // the state of `instance` (-1: the entity), null when out of range
        const vat::VatTrackPair* findState(int instance) const;

    public:
        // adds the Vertex Animator component to the entity when it has none
        VertexAnimator(Scene* scene, Entity entity);
        virtual ~VertexAnimator();

        VertexAnimator(const VertexAnimator& rhs);
        VertexAnimator& operator=(const VertexAnimator& rhs);
        VertexAnimator(VertexAnimator&& rhs) noexcept;
        VertexAnimator& operator=(VertexAnimator&& rhs) noexcept;

        // ---- the GameObject ----

        void play(const std::string& name);
        void play(const std::string& name, float fade);
        void crossFade(const std::string& name);
        void crossFade(const std::string& name, float fade);
        void blend(const std::string& a, const std::string& b, float weight);
        void queue(const std::string& name);
        void stop();
        bool isPlaying() const;
        bool isPlaying(const std::string& name) const;

        void setTime(float time);
        float getTime() const;
        float getNormalizedTime() const;
        // the authored speed multiplying every clip (phases are kept when it changes)
        void setSpeed(float speed);
        float getSpeed() const;
        std::string getCurrentClip() const;

        // the asset's clip names (empty until it is loaded)
        std::vector<std::string> getClipNames() const;
        // seconds; 0 for an unknown clip
        float getClipLength(const std::string& name) const;

        // a baked socket in the GameObject's space at the current time, through crossfades
        VertexAnimatorSocketPose getSocketPose(const std::string& name) const;
        VertexAnimatorSocketPose getSocketPose(const std::string& name, int instance) const;

        // ---- the instances of the Instanced Mesh ----

        void playInstance(int instance, const std::string& name);
        void playInstance(int instance, const std::string& name, float fade);
        void crossFadeInstance(int instance, const std::string& name, float fade);
        void setInstanceSpeed(int instance, float speed);
        void setInstanceTime(int instance, float time);
        std::string getInstanceClip(int instance) const;
        float getInstanceNormalizedTime(int instance) const;
        // the GameObject and every instance
        void playAll(const std::string& name);
        void playAll(const std::string& name, float fade);
        // every instance (and the GameObject) at a seeded time of its clip: the same seed gives
        // the same times
        void randomizeTimes(int seed);
        int getInstanceCount() const;

        // ---- settings ----

        // the .vat (asset-relative)
        void setAnimation(const std::string& path);
        std::string getAnimation() const;
        void setDefaultClip(const std::string& name);
        std::string getDefaultClip() const;
        void setPlayAutomatically(bool playAutomatically);
        bool isPlayAutomatically() const;
        void setRandomStart(bool randomStart);
        bool isRandomStart() const;
        void setDefaultFadeTime(float fade);
        float getDefaultFadeTime() const;
        void setInstanceEvents(bool instanceEvents);
        bool isInstanceEvents() const;
        // editor: animate in edit mode
        void setPreview(bool preview);
        bool isPreview() const;
        void setEnabled(bool enabled);
        bool isEnabled() const;

        // the asset is loaded and plays on this mesh
        bool isLoaded() const;
        // why it does not play ("" when it does): a load error or "mesh_mismatch: ..."
        std::string getLoadError() const;

        FunctionSubscribe<void(const AnimationEvent&)>& getOnEvent();
        FunctionSubscribe<void(const std::string&)>& getOnFinish();
        FunctionSubscribe<void(int, const AnimationEvent&)>& getOnInstanceEvent();
        FunctionSubscribe<void(int, const std::string&)>& getOnInstanceFinish();
    };

}

#endif //VERTEXANIMATOR_H
