// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef AUDIO_SNAPSHOT_COMPONENT_H
#define AUDIO_SNAPSHOT_COMPONENT_H

#include "Export.h"

#include <string>

namespace jasperengine{

    // A Mixer Snapshot action of the legacy action timeline (docs/specs/S40-timeline-audio.md R18):
    // on an action entity (with an ActionComponent) referenced by an Animation's frame, the mixer
    // transitions to `snapshot` when the action starts and, with `revert`, back to the snapshot
    // active before when it stops (only if the mixer still shows its own). Clip cues need no
    // component: a `.anim` snapshot cue does the same.
    struct JASPERENGINE_API AudioSnapshotComponent{
        // a snapshot of the project's mixer; "" = none: the action does nothing
        std::string snapshot;
        // the transition time (AudioMixer::transitionTo)
        float seconds = 0.0f;
        // an unscaled transition; false: the mixer's update mode
        bool unscaled = false;
        bool revert = true;
        // -1: the same as seconds
        float revertSeconds = -1.0f;

        // runtime, never serialized (EntityCloner clears them): the snapshot the mixer showed when
        // the action started, and whether it transitioned
        std::string previousSnapshot;
        bool applied = false;
    };

}

#endif //AUDIO_SNAPSHOT_COMPONENT_H
