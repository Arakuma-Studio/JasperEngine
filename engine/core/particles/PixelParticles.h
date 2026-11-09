// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PARTICLES_PIXELPARTICLES_H
#define PARTICLES_PIXELPARTICLES_H

// The ember entry point of pixel particles (docs/specs/S49-pixel-particles.md R21a): one particle
// of an effect at each given world position, this frame. S48's dissolve embers call it with the
// texels that crossed the burn front. Each (scene, effect) gets one engine-owned emitter (a system
// entity, never saved), made on first use: nothing exists and nothing runs until someone calls it.

#include "Export.h"
#include "math/Vector3.h"

#include <string>
#include <vector>

namespace jasperengine {

    class Scene;

    namespace particles {

        class JASPERENGINE_API PixelParticles {
        public:
            // `effect`: a .particles asset path, or an inline effect's JSON. Velocities (world, per
            // second) pair with the positions; missing ones keep the effect's. False with `error`
            // when the effect cannot be read, or is Analytic (analytic_no_events: it emits nothing
            // on request).
            static bool emitAt(Scene* scene, const std::string& effect, const std::vector<Vector3>& positions,
                               const std::vector<Vector3>& velocities = {}, std::string* error = nullptr);
            // the emitter emitAt made for (scene, effect), or NULL_ENTITY
            static uint32_t emitterOf(Scene* scene, const std::string& effect);
            // forgets a scene's emitters (its teardown destroys them)
            static void forget(Scene* scene);
        };

    }

}

#endif // PARTICLES_PIXELPARTICLES_H
