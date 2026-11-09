// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (camera shake spec 8)
// - Purpose: the Camera Shaker object (Lua `CameraShaker`, `object:getCameraShaker()`): shakes
//   one shaker, whatever its `global` flag, and reads / writes its settings.
// - The constructor ADDS the component when the entity has none (VertexAnimator precedent); Lua
//   `object:getCameraShaker()` returns nil when the GameObject has none.
// - shake / shakeAt on a disabled shaker add the instance: it plays once the shaker is enabled.
// - offsetPosition / offsetRotation are the offset applied this frame (after the 2D rule, units
//   and gains; rotation in Euler degrees), zero while idle or disabled.
// - Main thread (ECS).

#ifndef CAMERASHAKER_H
#define CAMERASHAKER_H

#include "EntityHandle.h"
#include "camera/CameraShake.h"
#include "component/CameraShakerComponent.h"
#include "math/Vector3.h"

#include <string>

namespace jasperengine{

    class JASPERENGINE_API CameraShaker: public EntityHandle{
    private:
        ShakeInstanceHandle play(const ShakeSource& preset, const ShakeOptions& options, bool fromPoint,
                                 const Vector3& point, float radius);

    public:
        // adds the Camera Shaker component to the entity when it has none
        CameraShaker(Scene* scene, Entity entity);
        virtual ~CameraShaker();

        CameraShaker(const CameraShaker& rhs);
        CameraShaker& operator=(const CameraShaker& rhs);
        CameraShaker(CameraShaker&& rhs) noexcept;
        CameraShaker& operator=(CameraShaker&& rhs) noexcept;

        ShakeInstanceHandle shake(const ShakeSource& preset);
        ShakeInstanceHandle shake(const ShakeSource& preset, const ShakeOptions& options);
        // the strength and roughness scaled by max(0, 1 - d / radius); none at d >= radius
        ShakeInstanceHandle shakeAt(const Vector3& position, float radius, const ShakeSource& preset);
        ShakeInstanceHandle shakeAt(const Vector3& position, float radius, const ShakeSource& preset, const ShakeOptions& options);
        void addTrauma(float amount);
        // every shake fades out over `fade` (default: its fade_out; 0: now)
        void stopAll();
        void stopAll(float fade);
        int getInstanceCount() const;

        void setTrauma(float trauma);
        float getTrauma() const;

        Vector3 getOffsetPosition() const;
        Vector3 getOffsetRotation() const;

        void setEnabled(bool enabled);
        bool isEnabled() const;
        void setGlobal(bool global);
        bool isGlobal() const;
        void setUnscaledTime(bool unscaled);
        bool isUnscaledTime() const;
        // "auto", "3d" or "2d" (another value logs a warning and is ignored)
        void setMode(const std::string& mode);
        std::string getMode() const;
        void setPositionScale(float scale);
        float getPositionScale() const;
        void setRotationScale(float scale);
        float getRotationScale() const;
        void setStartPreset(const std::string& preset);
        std::string getStartPreset() const;

        void setTraumaDecay(float decay);
        float getTraumaDecay() const;
        // clamped to [1, 4]
        void setTraumaPower(float power);
        float getTraumaPower() const;
        void setTraumaMaxOffset(Vector3 offset);
        Vector3 getTraumaMaxOffset() const;
        void setTraumaMaxAngle(Vector3 angle);
        Vector3 getTraumaMaxAngle() const;
        void setTraumaFrequency(float frequency);
        float getTraumaFrequency() const;

        // S48 R22: whole art pixels and no roll; ticks of 1 / tickRate s (0: every frame)
        void setPixelSnap(bool snap);
        bool isPixelSnap() const;
        void setTickRate(float hz);
        float getTickRate() const;

        static std::string modeToString(CameraShakeMode mode);
        static bool modeFromString(const std::string& text, CameraShakeMode& out);
    };

}

#endif //CAMERASHAKER_H
