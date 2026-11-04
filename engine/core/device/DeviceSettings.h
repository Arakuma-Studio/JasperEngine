// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef DEVICESETTINGS_H
#define DEVICESETTINGS_H

#include "Export.h"
#include "device/DeviceTypes.h"

#include <array>
#include <optional>
#include <string>

namespace jasperengine {

    // The curated settings a Device Tier overrides (Project Settings > Quality > Device Tiers,
    // docs/device-tiers.md). A tier never raises quality above the project's (or the scene's)
    // value: caps take the smaller, Texture Quality the larger drop, the effect toggles can only
    // turn an effect off; the LOD biases multiply. Stored as ints: append only.
    enum class DeviceOverride : int {
        RenderScale,      // at most (0.5..1), the dynamic resolution's maximum
        MsaaSamples,      // at most (1, 2, 4)
        ShadowQuality,    // at most (ShadowTier: 0 Low .. 3 Ultra)
        ShadowDistance,   // metres at most
        LodBias,          // multiplies the project's LOD Bias (S54; each mesh's lodBias times it)
        ShadowLodBias,    // multiplies the project's Shadow LOD Bias
        LitQuality,       // at most (LitQuality: 0 Standard, 1 High)
        TextureQuality,   // top mips dropped at least (0..3)
        Bloom,            // 0: off
        SSAO,             // 0: off
        Volumetrics,      // 0: no volumetric fog or clouds (Atmosphere Quality Off)
        TargetFrameRate,  // frames per second at most
        MaxPixelLights,   // per-object lights at most
        ParticleBudget    // live particles at most
    };
    constexpr int DEVICE_OVERRIDE_COUNT = 14;

    struct DeviceOverrideInfo {
        enum class Kind { Cap, Multiplier, AtLeast, Off };
        const char* key;      // snake_case, as saved and in MCP keys
        const char* label;    // the Project Settings row
        const char* tooltip;
        Kind kind;
        bool integer;
        float min;
        float max;
        // what a thermal step may change (dynamic settings, never content): render scale and the
        // frame cap at Serious, the shadow and post-processing cells at Critical
        bool thermalCritical;
        // the enum names of an enumerated cell (nullptr: a number)
        const char* const* names;
        int nameCount;
    };

    class JASPERENGINE_API DeviceOverrides {
    public:
        static const DeviceOverrideInfo& info(DeviceOverride field);
        static bool fromKey(const std::string& key, DeviceOverride& field);
        // a value as a person reads it ("0.5", "Low", "Off", "30")
        static std::string format(DeviceOverride field, float value);
        // the value of `text` (a number, or an enumerated cell's name) in range; false with an error
        static bool parse(DeviceOverride field, const std::string& text, float& value, std::string& error);
        // checks a number against the field's range and kind
        static bool validate(DeviceOverride field, float value, std::string& error);
    };

    // One tier's cells: empty (nullopt) inherits the project's value
    struct JASPERENGINE_API DeviceTierOverrides {
        std::array<std::optional<float>, DEVICE_OVERRIDE_COUNT> cells;

        const std::optional<float>& get(DeviceOverride field) const { return cells[(size_t)field]; }
        void set(DeviceOverride field, std::optional<float> value) { cells[(size_t)field] = value; }
        bool empty() const;
        bool operator==(const DeviceTierOverrides& other) const { return cells == other.cells; }
        bool operator!=(const DeviceTierOverrides& other) const { return !(*this == other); }
    };

    // Everything the project says about devices: the tier table per platform group, the LOD
    // biases (contract C4: quality.lod_bias, quality.shadow_lod_bias), the thermal response and
    // the upload budget. One codec for the generated main.cpp and game.json (toJson / fromJson):
    // only what differs from the defaults is written, and a missing key keeps its default, so old
    // projects and old players load unchanged.
    struct JASPERENGINE_API DeviceSettings {
        // [group][tier]
        std::array<std::array<DeviceTierOverrides, DEVICE_TIER_COUNT>, DEVICE_PLATFORM_GROUP_COUNT> tiers;
        float lodBias = 1.0f;
        float shadowLodBias = 1.0f;
        // Project Settings > Quality > Thermal Response: on for mobile only (desktop and web have
        // no thermal signal, macOS reports one without acting on it)
        std::array<bool, DEVICE_PLATFORM_GROUP_COUNT> thermalResponse = {false, true, false};
        // Project Settings > Graphics > Upload Budget: MB of queued texture and buffer uploads a
        // frame runs (SokolCmdQueue)
        std::array<int, DEVICE_PLATFORM_GROUP_COUNT> uploadBudgetMB = {32, 8, 6};

        // the defaults (spec S56 R3): Low halves the render scale, has Low shadows, LOD Bias 0.5,
        // no volumetrics and no SSAO; Medium renders at 0.85; High and Ultra are the project
        DeviceSettings();

        DeviceTierOverrides& tier(DevicePlatformGroup group, DeviceTier tier){ return tiers[(size_t)group][(size_t)tier]; }
        const DeviceTierOverrides& tier(DevicePlatformGroup group, DeviceTier tier) const{ return tiers[(size_t)group][(size_t)tier]; }

        bool operator==(const DeviceSettings& other) const;
        bool operator!=(const DeviceSettings& other) const { return !(*this == other); }

        // compact JSON of what differs from the defaults ("{}" for a default project); a cleared
        // default cell is written as null
        std::string toJson() const;
        static bool fromJson(const std::string& text, DeviceSettings& out, std::string& error);

        static constexpr int MIN_UPLOAD_BUDGET_MB = 1;
        static constexpr int MAX_UPLOAD_BUDGET_MB = 1024;
    };

}

#endif // DEVICESETTINGS_H
