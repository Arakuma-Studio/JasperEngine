// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef DEVICETYPES_H
#define DEVICETYPES_H

#include "Export.h"

namespace jasperengine {

    // The quality tier a device gets (docs/device-tiers.md): which column of Project Settings >
    // Quality > Device Tiers applies. Stored and sent as ints: append only.
    enum class DeviceTier : int {
        Low,
        Medium,
        High,
        Ultra
    };
    constexpr int DEVICE_TIER_COUNT = 4;

    // The platform's thermal state (iOS NSProcessInfoThermalState, Android PowerManager's
    // thermal status mapped onto it); desktops and the web stay Nominal
    enum class ThermalState : int {
        Nominal,
        Fair,
        Serious,
        Critical
    };
    constexpr int THERMAL_STATE_COUNT = 4;

    // The platform groups the device settings are made for, in the order of the other per-group
    // project settings (texture quality, atmosphere quality): desktop, mobile, web
    enum class DevicePlatformGroup : int {
        Desktop,
        Mobile,
        Web
    };
    constexpr int DEVICE_PLATFORM_GROUP_COUNT = 3;

    struct JASPERENGINE_API DeviceNames {
        // lower case, as saved and as MCP and the command line take them: low, medium, high, ultra
        static const char* tier(DeviceTier tier);
        static bool tierFromName(const char* name, DeviceTier& tier);
        // the label people read: Low, Medium, High, Ultra
        static const char* tierLabel(DeviceTier tier);
        // nominal, fair, serious, critical
        static const char* thermal(ThermalState state);
        static bool thermalFromName(const char* name, ThermalState& state);
        static const char* thermalLabel(ThermalState state);
        // desktop, mobile, web
        static const char* group(DevicePlatformGroup group);
        static bool groupFromName(const char* name, DevicePlatformGroup& group);
        static const char* groupLabel(DevicePlatformGroup group);
    };

}

#endif // DEVICETYPES_H
