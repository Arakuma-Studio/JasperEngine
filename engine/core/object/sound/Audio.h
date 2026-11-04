// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef Audio_h
#define Audio_h

#include "Export.h"
#include "ecs/Entity.h"
#include "math/Vector3.h"
#include "object/sound/SoundHandle.h"

#include <optional>
#include <string>

namespace jasperengine {

    class EntityHandle;
    class Scene;

    // Audio::play's options (docs/specs/S37-sound-events.md R13)
    struct JASPERENGINE_API PlayOptions {
        // 3D, following a GameObject; stops when it is destroyed
        Scene* targetScene = nullptr;
        Entity target = NULL_ENTITY;
        // 3D, at a fixed place
        std::optional<Vector3> position;
        // the owner, with whose pause it holds and with which it stops; default the target's, else the main scene
        Scene* scene = nullptr;
        // linear, as AudioSource.volume
        float volume = 1.0f;
        // relative speed, as AudioSource.pitch
        float pitch = 1.0f;
        // seconds before it starts
        float delay = 0.0f;
        // a group name, as AudioSource.outputGroup; default the event's, else Master
        std::string group;
        // the delay runs on unscaled time
        bool unscaled = false;

        PlayOptions& following(const EntityHandle& gameObject);
    };

    // Free voices (with SoundHandle): sounds a game starts from code with no AudioSource.
    // Inside a physics callback or off the main thread a call defers to the frame's audio update:
    // its handle is valid at once, and invalid from then if nothing started.
    class JASPERENGINE_API Audio {
    public:
        // A Sound Event (GUID or path) or a clip path. An unknown name gives an invalid handle and
        // one warning per name per Play.
        static SoundHandle play(const std::string& eventOrClip, const PlayOptions& options = PlayOptions());
        // Stops the voices started by Audio.play, triggers and one-shots: all, or those routed to
        // `group` or a group below it. The count stopped (0 when deferred). AudioSystem.stopAll
        // stops the sources' main voices too.
        static int stopAll(const std::string& group = "");
    };

}

#endif /* Audio_h */
