// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT
//
// The ranges of the Physics Placement settings: what the panel lets a value take, and what a
// value read from workspace.yaml is clamped to (a hand-edited 1000 substeps or a 10^6 s settle
// limit would freeze the editor, a zero size would refuse everything).

#pragma once

#include "PlacementSettings.h"
#include "util/Angle.h"

#include <algorithm>
#include <cmath>

namespace jasperengine::editor {

    struct PlacementSettingRange {
        float min;
        float max;
    };

    namespace PlacementSettingLimits {
        constexpr PlacementSettingRange gravityComponent{-100.0f, 100.0f};   // m/s², each axis
        constexpr PlacementSettingRange gravityStrength{0.0f, 50.0f};        // the panel's slider
        constexpr PlacementSettingRange snapOffset{-1.0f, 1.0f};             // m
        constexpr PlacementSettingRange regionMargin{0.0f, 100.0f};          // m
        constexpr PlacementSettingRange maxMoverSize{0.1f, 10000.0f};        // m
        // Advanced; these four are also "from the Feel preset" as a negative value
        constexpr PlacementSettingRange maxSpeed{0.1f, 100.0f};              // m/s
        constexpr PlacementSettingRange contactSpeed{0.01f, 50.0f};          // m/s
        constexpr PlacementSettingRange maxAngularSpeedDeg{1.0f, 5000.0f};   // deg/s (rad/s stored)
        constexpr PlacementSettingRange substeps{1.0f, 16.0f};
        constexpr PlacementSettingRange iterations{1.0f, 50.0f};             // velocity and position
        constexpr PlacementSettingRange teleportDistance{0.1f, 1000.0f};     // m
        constexpr PlacementSettingRange sleepLinear{0.0f, 1.0f};             // m/s
        constexpr PlacementSettingRange sleepAngularDeg{0.0f, 90.0f};        // deg/s
        constexpr PlacementSettingRange sleepSeconds{0.0f, 5.0f};            // s
        constexpr PlacementSettingRange maxSettleSeconds{0.1f, 60.0f};       // s
        constexpr PlacementSettingRange killYDepth{0.0f, 10000.0f};          // m
    }

    // Every value into its range; the preset-or-value fields keep "from the preset" (-1) for any
    // negative value. Non-finite values were already replaced by defaults when read.
    inline void clampPhysicsPlacementSettings(PhysicsPlacementSettings& s) {
        namespace L = PlacementSettingLimits;
        auto clampTo = [](float value, const PlacementSettingRange& range) {
            return std::clamp(value, range.min, range.max);
        };
        auto presetOr = [&](float value, const PlacementSettingRange& range) {
            return value < 0.0f ? -1.0f : clampTo(value, range);
        };
        auto presetOrInt = [](int value, const PlacementSettingRange& range) {
            return value < 0 ? -1 : std::clamp(value, (int)range.min, (int)range.max);
        };
        s.gravity = Vector3(clampTo(s.gravity.x, L::gravityComponent), clampTo(s.gravity.y, L::gravityComponent),
                            clampTo(s.gravity.z, L::gravityComponent));
        s.snapOffset = clampTo(s.snapOffset, L::snapOffset);
        s.regionMargin = clampTo(s.regionMargin, L::regionMargin);
        s.maxMoverSize = clampTo(s.maxMoverSize, L::maxMoverSize);
        s.maxSpeed = presetOr(s.maxSpeed, L::maxSpeed);
        s.contactSpeed = presetOr(s.contactSpeed, L::contactSpeed);
        s.maxAngularSpeed = s.maxAngularSpeed < 0.0f ? -1.0f
            : std::clamp(s.maxAngularSpeed, Angle::degToRad(L::maxAngularSpeedDeg.min), Angle::degToRad(L::maxAngularSpeedDeg.max));
        s.substeps = presetOrInt(s.substeps, L::substeps);
        s.velocityIterations = presetOrInt(s.velocityIterations, L::iterations);
        s.positionIterations = presetOrInt(s.positionIterations, L::iterations);
        s.teleportDistance = clampTo(s.teleportDistance, L::teleportDistance);
        s.sleepLinear = clampTo(s.sleepLinear, L::sleepLinear);
        s.sleepAngularDeg = clampTo(s.sleepAngularDeg, L::sleepAngularDeg);
        s.sleepSeconds = clampTo(s.sleepSeconds, L::sleepSeconds);
        s.maxSettleSeconds = clampTo(s.maxSettleSeconds, L::maxSettleSeconds);
        s.killYDepth = clampTo(s.killYDepth, L::killYDepth);
    }

}
