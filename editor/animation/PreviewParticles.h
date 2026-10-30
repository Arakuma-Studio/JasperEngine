// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S21-animation.md R28)
// - Purpose: the particle-aware animation previews: the Particle Systems on and under a
//   previewed root follow the preview's time (S20 previewSeek / previewAdvance), so an effect
//   keyed to an animation shows where the animation is. A forward step of a playing preview
//   advances them; any other time (a scrub, a loop wrap, the first sync) restarts and simulates
//   to it over frames (pump(), a few milliseconds a frame, the Scene view overlay's budget). Off when the preference scene_view.preview_particles is (stopped and cleared then).
// - Used by the Animator window's preview and the Animation window's clip preview; stop()
//   when their preview ends: the systems it touched stop and clear.
// - Thread-safety: main thread.

#pragma once

#include "ecs/Entity.h"

#include <vector>

namespace jasperengine{
    class Scene;
}

namespace jasperengine::editor{

    class PreviewParticles{
    public:
        // the systems under `root` at `time` (seconds since the preview started); `forward`: the
        // preview played on from the last sync (advance by the difference when time grew)
        void sync(Scene* scene, Entity root, double time, bool forward);
        // per frame while the preview runs (also paused): a seek in progress goes on
        void pump(Scene* scene);
        // the systems it touched stop and clear
        void stop(Scene* scene);
        bool active() const { return !touched.empty(); }

    private:
        std::vector<Entity> touched;
        double lastTime = -1.0;
        bool seeking = false;
    };

}
