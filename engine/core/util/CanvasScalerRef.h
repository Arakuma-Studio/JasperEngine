// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef CANVASSCALERREF_H
#define CANVASSCALERREF_H

#include "Scene.h"
#include "Engine.h"
#include "Log.h"

#include <memory>

namespace jasperengine {

    // A scene's Canvas Scaler for scripts (scene:getCanvasScaler()). Reads give what applies to
    // the scene (its own settings, or the project default while useProjectDefault); setting a
    // field of a scene that used the project default makes it override from that default.
    // Changes apply at once (the UI lays out in them from the next frame). Inert once the
    // scene is gone.
    class JASPERENGINE_API CanvasScalerRef {
    private:
        Scene* scene = nullptr;
        std::weak_ptr<char> lifetime;

        Scene* live() const {
            if (!scene || lifetime.expired()){
                Log::warn("CanvasScaler: the scene is gone");
                return nullptr;
            }
            return scene;
        }

        template<typename F>
        void edit(F change) {
            Scene* target = live();
            if (!target)
                return;
            CanvasScalerSettings settings = target->getEffectiveCanvasScaler();
            change(settings);
            target->setCanvasScaler(settings);
            if (target->isCanvasScalerUseProjectDefault())
                target->setCanvasScalerUseProjectDefault(false);
        }

        CanvasScalerSettings current() const {
            Scene* target = live();
            return target ? target->getEffectiveCanvasScaler() : CanvasScalerSettings();
        }

    public:
        CanvasScalerRef() = default;
        explicit CanvasScalerRef(Scene* scene): scene(scene), lifetime(scene ? scene->getLifetime() : std::weak_ptr<char>()) {}

        UIScaleMode getUIScaleMode() const { return current().mode; }
        void setUIScaleMode(UIScaleMode mode) { edit([&](CanvasScalerSettings& s){ s.mode = mode; }); }

        float getScaleFactor() const { return current().scaleFactor; }
        void setScaleFactor(float scaleFactor) { edit([&](CanvasScalerSettings& s){ s.scaleFactor = scaleFactor; }); }

        Vector2 getReferenceResolution() const { return current().referenceResolution; }
        void setReferenceResolution(Vector2 resolution) { edit([&](CanvasScalerSettings& s){ s.referenceResolution = resolution; }); }

        ScreenMatchMode getScreenMatchMode() const { return current().screenMatchMode; }
        void setScreenMatchMode(ScreenMatchMode mode) { edit([&](CanvasScalerSettings& s){ s.screenMatchMode = mode; }); }

        float getMatchWidthOrHeight() const { return current().matchWidthOrHeight; }
        void setMatchWidthOrHeight(float match) { edit([&](CanvasScalerSettings& s){ s.matchWidthOrHeight = match; }); }

        PhysicalUnit getPhysicalUnit() const { return current().physicalUnit; }
        void setPhysicalUnit(PhysicalUnit unit) { edit([&](CanvasScalerSettings& s){ s.physicalUnit = unit; }); }

        float getFallbackScreenDPI() const { return current().fallbackScreenDPI; }
        void setFallbackScreenDPI(float dpi) { edit([&](CanvasScalerSettings& s){ s.fallbackScreenDPI = dpi; }); }

        bool isUseProjectDefault() const {
            Scene* target = live();
            return target ? target->isCanvasScalerUseProjectDefault() : true;
        }
        void setUseProjectDefault(bool useProjectDefault) {
            if (Scene* target = live())
                target->setCanvasScalerUseProjectDefault(useProjectDefault);
        }

        // the UI size now, in UI units (the game canvas under FollowGameCanvas)
        Vector2 getCanvasSize() const {
            Scene* target = live();
            if (!target)
                return Vector2(0, 0);
            const CanvasScalerResult& result = target->getCanvasScale();
            if (!result.scaled)
                return Vector2((float)Engine::getCanvasWidth(), (float)Engine::getCanvasHeight());
            return Vector2(result.uiWidth, result.uiHeight);
        }

        // view pixels per UI unit now
        float getAppliedScaleFactor() const {
            Scene* target = live();
            return target ? target->getCanvasScale().scaleFactor : 1.0f;
        }
    };

}

#endif // CANVASSCALERREF_H
