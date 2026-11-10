// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (camera shake spec 5.5-5.7, 8)
// - Purpose: the static CameraShake API (MilkShake's Shaker.ShakeAll...): shakes on every global
//   Camera Shaker, the preset sources and ShakeInstanceHandle, the handle of the instances one
//   call created (Lua `ShakeInstance`).
// - Global shakers: enabled, active in hierarchy, `global` set, in every scene of Engine's list
//   (child scenes and layers too); each gets its own instance. No such shaker: an invalid handle
//   and a warning (once per process). Shakers of a paused scene (the editor's Play paused, edit
//   mode) take nothing: the shake is dropped with its own warning (once).
// - Presets: a built-in name (no extension), a .shake path (ShakePresetPool) or the values. An
//   unknown name or an unreadable file logs one warning per name and shakes nothing.
// - Handles hold weak references: an instance gone (finished, its shaker destroyed, the scene
//   unloaded) drops out; with none left the handle is invalid and its calls do nothing. On a
//   grouped handle setters apply to every live member and getters read the first live one.
// - Main thread (ECS).

#ifndef CAMERA_SHAKE_H
#define CAMERA_SHAKE_H

#include "Export.h"
#include "camera/ShakePreset.h"
#include "ecs/Entity.h"
#include "math/Vector3.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine{

    class Scene;
    namespace camera{ struct ShakerRuntime; struct ShakerInstanceEntry; }

    // the options of a shake call (all optional)
    struct JASPERENGINE_API ShakeOptions{
        float strength = 1.0f;          // strengthScale
        float roughness = 1.0f;         // roughnessScale
        bool hasSeed = false;
        int seed = 0;
        int unscaled = -1;              // -1: the shaker's unscaledTime; 0: scaled; 1: unscaled
        float fadeIn = -1.0f;           // < 0: the preset's
        float fadeOut = -1.0f;
    };

    // what to shake with: a built-in name or .shake path, or the preset values (inline)
    struct JASPERENGINE_API ShakeSource{
        std::string name;
        bool inlinePreset = false;
        camera::ShakePreset preset;

        ShakeSource() = default;
        ShakeSource(const std::string& name): name(name) {}
        ShakeSource(const char* name): name(name ? name : "") {}
        ShakeSource(const camera::ShakePreset& preset): name("(table)"), inlinePreset(true), preset(preset) {}
    };

    class JASPERENGINE_API ShakeInstanceHandle{
    private:
        struct Member{
            std::weak_ptr<camera::ShakerRuntime> runtime;
            uint64_t id = 0;
        };
        std::vector<Member> members;

        camera::ShakerInstanceEntry* firstLive() const;
        template<typename F> void forEachLive(F&& f) const;

    public:
        ShakeInstanceHandle() = default;

        void add(const std::shared_ptr<camera::ShakerRuntime>& runtime, uint64_t id);
        void merge(const ShakeInstanceHandle& other);

        void stop();
        void stop(float fade);
        void pause();
        void pause(float fade);
        void resume();
        void resume(float fade);

        bool isPaused() const;
        bool isFinished() const;
        bool isValid() const;

        float getStrengthScale() const;
        void setStrengthScale(float scale);
        float getRoughnessScale() const;
        void setRoughnessScale(float scale);
        std::string getState() const;
        int getCount() const;
        std::string toString() const;
    };

    class JASPERENGINE_API CameraShake{
    public:
        // one instance on every global shaker
        static ShakeInstanceHandle shake(const ShakeSource& preset);
        static ShakeInstanceHandle shake(const ShakeSource& preset, const ShakeOptions& options);
        // the same, with the strength and roughness scaled per shaker by max(0, 1 - d / radius)
        // (d: from the shaker's camera, x/y only in 2D); a shaker at d >= radius gets none
        static ShakeInstanceHandle shakeAt(const Vector3& position, float radius, const ShakeSource& preset);
        static ShakeInstanceHandle shakeAt(const Vector3& position, float radius, const ShakeSource& preset, const ShakeOptions& options);
        // An unnamed pixel kick (S48 R22, pixelfx act.js) of `amplitudePx` art pixels decaying over
        // `seconds`, on every global shaker
        static ShakeInstanceHandle kick(float amplitudePx, float seconds);
        static void addTrauma(float amount);
        // every global shaker's shakes fade out over `fade` (default: their fade_out; 0: now)
        static void stopAll();
        static void stopAll(float fade);
        static std::vector<std::string> getPresetNames();

        // A built-in name or a .shake path as a preset; false (and `error`) when it is neither
        static bool resolvePreset(const std::string& nameOrPath, camera::ShakePreset& out, std::string* error = nullptr);
        // resolvePreset for a play request: an unknown name logs one warning per name
        static bool resolveSource(const ShakeSource& source, camera::ShakePreset& out);
        // the shakers the static calls reach, in every scene of the Engine's list. A paused scene
        // (edit mode, the editor's Play paused) is skipped: its shakers are counted in
        // `pausedSkipped` and a shake for them is dropped, not queued
        static void forEachGlobalShaker(const std::function<void(Scene*, Entity)>& callback, int* pausedSkipped = nullptr);
    };

}

#endif //CAMERA_SHAKE_H
