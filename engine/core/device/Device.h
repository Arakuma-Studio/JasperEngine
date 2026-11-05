// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef DEVICE_H
#define DEVICE_H

#include "Export.h"
#include "device/DeviceSettings.h"
#include "device/DeviceTypes.h"
#include "device/GpuTierDatabase.h"
#include "util/FunctionSubscribe.h"

#include <optional>
#include <string>

namespace jasperengine {

    // What detection reads (R2): the renderer and backend names, the platform (Engine::Platform
    // as an int), physical memory in MiB (0 unknown) and logical processors
    struct DeviceSpecs {
        std::string gpuName;
        std::string backend;
        int platform = 0;
        int memoryMB = 0;
        int cores = 0;
    };

    struct DeviceDetection {
        bool detected = false;
        DeviceTier tier = DeviceTier::High;
        // "device", "name", "family", "vendor" (a database rule) or "heuristic"
        std::string kind;
        // the rule that matched, or the heuristic's input
        std::string rule;
        std::string vendor;
        // one line, as logged: "Adreno (TM) 650, rule family (adreno 6[3-9]\d), tier Medium"
        std::string reason;
        DeviceSpecs profile;
    };

    // What the editor's Game view simulates in Play (set_device_simulation): each part, when set,
    // replaces what the device says
    struct DeviceSimulation {
        std::optional<DeviceTier> tier;
        std::optional<DevicePlatformGroup> group;
        std::optional<ThermalState> thermal;
        bool operator==(const DeviceSimulation& other) const{ return tier == other.tier && group == other.group && thermal == other.thermal; }
        bool operator!=(const DeviceSimulation& other) const{ return !(*this == other); }
    };

    // The device the game runs on (docs/device-tiers.md, Lua Device): the GPU and the tier it gets,
    // the thermal state and memory pressure, and the overrides of the tier through QualityResolver.
    //
    // A player detects at startup (Engine::systemViewLoaded) and applies the tier once configure()
    // ran (the generated main.cpp and the prebuilt player's game.json do); the editor detects too,
    // but applies a tier only when its Game view simulates one or a script calls setTier, so its
    // views and tests never depend on the machine's GPU.
    class JASPERENGINE_API Device {
    public:
        // ---- detection (R1, R2)

        // the tier of a device: the database's rule, else the platform's heuristic; never fails
        static DeviceDetection detectTier(const DeviceSpecs& profile, const GpuTierDatabase& database);
        // the running device (the render device must exist), logged with its reason
        static void detect();
        static void setDetection(const DeviceDetection& detection);
        static const DeviceDetection& getDetection();

        // the project's device settings; a player applies the detected tier from here on
        static void configure(const DeviceSettings& settings);
        // the same from the JSON of DeviceSettings::toJson (generated main.cpp); a bad text is
        // logged and the defaults apply
        static void configure(const char* settingsJson);
        static bool isConfigured();
        // the editor: the project's settings without applying the detected tier
        static void setProjectSettings(const DeviceSettings& settings);

        // ---- the tier

        // the tier in effect: the simulated one, else setTier's (or --device-tier), else the
        // detected one
        static DeviceTier getTier();
        // whether a tier's overrides apply now (false in the editor without a simulated or set tier)
        static bool isTierApplied();
        // a graphics menu's choice or --device-tier, for the rest of the run (Play in the editor)
        static void setTier(DeviceTier tier);
        static void resetTier();
        static bool isTierSet();
        static std::string getGpuName();

        // ---- signals: the platform layers call these on any thread

        static void reportThermalState(ThermalState state);
        // a low-memory warning (Android onTrimMemory at RUNNING_LOW and above, iOS
        // didReceiveMemoryWarning, the web's heap heuristic)
        static void reportLowMemory();

        static ThermalState getThermalState();
        static int getLowMemoryCount();

        // once a frame on the main thread (Engine::systemDraw): applies the signals since the last
        // frame, then advances the thermal recovery clock
        static void update(double seconds);

        // ---- the editor

        static void setSimulation(const DeviceSimulation& simulation);
        static const DeviceSimulation& getSimulation();
        // Play stop: no set tier, no simulation, the thermal and memory response at rest
        static void resetSession();

        // ---- events (Lua Device.onThermalStateChanged, Device.onLowMemory)

        // the thermal state changed: the new ThermalState
        static FunctionSubscribe<void(int)> onThermalStateChanged;
        // a low-memory warning was handled (after the caches were cleared)
        static FunctionSubscribe<void()> onLowMemory;

    private:
        static void applyTier();
    };

}

#endif // DEVICE_H
