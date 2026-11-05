// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef QUALITYRESOLVER_H
#define QUALITYRESOLVER_H

#include "Export.h"
#include "SceneSettings.h"
#include "device/DeviceSettings.h"
#include "render/AtmosphereQuality.h"
#include "render/shadow/ShadowTiers.h"

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine {

    // One override in effect, for get_device_info and the Stats overlay
    struct AppliedDeviceOverride {
        DeviceOverride field;
        float value;
        // "tier" (the tier's cell), "thermal" (a thermal step), "memory" (memory pressure)
        std::string source;
    };

    // The single source of the quality values a device tier, a thermal step or memory pressure
    // may change (docs/device-tiers.md). Every consumer reads its value through here, with the
    // project's or the scene's value in: render scale and MSAA (RenderSystem's post-processing
    // tiers), the shadow tier and distance, the LOD biases (LOD selection multiplies lodBias()
    // into each mesh's, contract C4), Lit Quality, Texture Quality, bloom, SSAO, volumetrics, the
    // target frame rate, pixel lights and the particle budget. With no tier, no thermal step and
    // no memory pressure every getter returns its input (one branch).
    //
    // Main thread. getRevision() goes up with every change, so a consumer that caches what it
    // resolved (RenderSystem's post-processing tiers, the instance views' LODs) checks it once a
    // frame.
    class JASPERENGINE_API QualityResolver {
    public:
        // ---- what applies

        static void setSettings(const DeviceSettings& settings);
        static const DeviceSettings& getSettings();
        // the platform group whose table applies (the running platform's until the editor
        // simulates another)
        static void setPlatformGroup(DevicePlatformGroup group);
        static DevicePlatformGroup getPlatformGroup();
        static DevicePlatformGroup platformGroupOf(int enginePlatform);
        // the tier whose column applies; none: the project's values (the editor without a
        // simulated tier)
        static void setTier(std::optional<DeviceTier> tier);
        static std::optional<DeviceTier> getTier();

        // ---- thermal and memory response (R6, R7)

        // the platform's state, as Device reads it each frame; the response steps up at once
        // and back down one state per RECOVERY_SECONDS spent below it (hysteresis)
        static void setThermalState(ThermalState reported);
        static ThermalState getReportedThermalState();
        // the state the response acts on (Nominal when the group's Thermal Response is off)
        static ThermalState getThermalResponse();
        // the seconds until the response steps down, 0 when it does not wait
        static double getRecoverySeconds();
        // advances the recovery clock (Device::update, once a frame)
        static void update(double seconds);
        static constexpr double RECOVERY_SECONDS = 30.0;
        // Serious: the render scale's (dynamic resolution's maximum) factor, the frame cap
        static constexpr float THERMAL_SCALE = 0.8f;
        static constexpr int THERMAL_FRAME_CAP = 30;

        // a memory warning: texture streaming keeps one more mip off (at most MAX_MEMORY_DROP)
        static void addMemoryPressure();
        static int getMemoryDrop();
        static constexpr int MAX_MEMORY_DROP = 1;

        // the thermal and memory response back to rest (Play stop, a test)
        static void resetResponse();
        // Saves everything, and puts it back: the export's shader collection resolves the scene
        // under every tier and thermal step a device can reach, then restores the editor's state
        static void pushState();
        static void popState();
        // the response as if the platform reported `state` and it had applied at once
        static void forceThermalResponse(ThermalState state);
        // everything back to its defaults (tests)
        static void reset();

        static uint32_t getRevision(){ return revision.load(std::memory_order_acquire); }

        // ---- the resolved values

        static float renderScale(float sceneScale);
        static int msaaSamples(int sceneSamples);
        static ShadowTier shadowTier(ShadowTier tier);
        static float shadowDistance(float metres);
        static float lodBias();
        static float shadowLodBias();
        static LitQuality litQuality(LitQuality quality);
        static int textureQuality(int projectQuality);
        static bool bloom(bool enabled);
        static bool ssao(bool enabled);
        static AtmosphereQuality volumetrics(AtmosphereQuality quality);
        // -1 (the platform's rate) or a frame rate, as Engine::setTargetFrameRate takes it
        static int targetFrameRate(int fps);
        static int maxPixelLights(int lights);
        static uint32_t particleBudget(uint32_t budget);
        static uint64_t uploadBudgetBytes();

        // the cells in effect now, with where each comes from
        static std::vector<AppliedDeviceOverride> getAppliedOverrides();

    private:
        static std::atomic<uint32_t> revision;
        static void changed();
        static void rebuild();
    };

}

#endif // QUALITYRESOLVER_H
