// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (camera shake spec 5, 7.1)
// - Purpose: the Camera Shaker (MilkShake's Shaker, Cinemachine's Impulse Listener): plays shake
//   instances from Shake Presets and a trauma signal (Eiserloh) on its GameObject.
//   * With a CameraComponent the shake is a view offset (CameraComponent::hasViewOffset...): the
//     Transform is never written, so follow cameras and script-moved cameras are never fought.
//   * Without one (transform mode) it moves the Transform additively over whatever else writes it
//     (PoseBlendBone's base rule); a GameObject with a Rigidbody or Rigidbody2D is refused.
// - CameraControlSystem steps it each frame after the scene's scripts and animations; disabled or
//   inactive, its instances hold (no time passes for them) and its offset is cleared.
// - Only the authored fields are serialized, cloned and emitted on export; `runtime` is not.

#ifndef CAMERA_SHAKER_COMPONENT_H
#define CAMERA_SHAKER_COMPONENT_H

#include "Export.h"
#include "math/Vector3.h"

#include <cstdint>
#include <memory>
#include <string>

namespace jasperengine{

    namespace camera{ struct ShakerRuntime; }

    enum class CameraShakeMode{
        // 2D for orthographic and UI cameras (and UI GameObjects in transform mode), else 3D
        Auto,
        Mode3D,
        // position x/y in tenths of the view height and roll only
        Mode2D
    };

    struct JASPERENGINE_API CameraShakerComponent{
        bool enabled = true;
        // receives the CameraShake.* static calls (MilkShake's global shakers)
        bool global = true;
        // the shakes advance with the unscaled frame time (a shake option overrides it per shake)
        bool unscaledTime = false;
        CameraShakeMode mode = CameraShakeMode::Auto;
        // gains of every position and rotation offset (>= 0)
        float positionScale = 1.0f;
        float rotationScale = 1.0f;
        // a built-in name or a .shake path played on start and on each enable ("" = none)
        std::string startPreset;

        // trauma (Eiserloh, "Juicing Your Cameras With Math")
        float traumaDecay = 1.0f;           // trauma per second, linear
        float traumaPower = 2.0f;           // amplitude = trauma^power, [1, 4]
        Vector3 traumaMaxOffset = Vector3(0.1f, 0.1f, 0.0f);   // units at trauma 1 (2D: tenths of the view height)
        Vector3 traumaMaxAngle = Vector3(3.0f, 3.0f, 5.0f);    // degrees at trauma 1
        float traumaFrequency = 15.0f;      // noise speed (MilkShake roughness units)

        // S48 R22, pixel art: the applied offset is rounded to whole art pixels (the S41 grid
        // with a Pixel Perfect Camera, else the camera's screen pixels) and the roll is dropped;
        // inert on a 3D (perspective) shaker
        bool pixelSnap = false;
        // Hz: the applied offset changes only on ticks of 1 / tickRate s of the shake's clock
        // (0: every frame)
        float tickRate = 0.0f;

        // runtime (not serialized): instances, trauma, applied offset, transform-mode base
        std::shared_ptr<camera::ShakerRuntime> runtime;
    };

}

#endif //CAMERA_SHAKER_COMPONENT_H
