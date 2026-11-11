// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (camera shake spec 6)
// - Purpose: the Shake Preset (MilkShake's ShakePreset / ShakeParameters) and the one `.shake`
//   parser and canonical writer, shared by the engine (ShakePresetPool), the editor, MCP and tests.
// - Thread-safety: pure functions of their arguments: any thread.
// - Format: UTF-8 JSON {"format": "jasperengine.shake", "version": 1, "guid"?, "type", "strength",
//   "roughness", "fade_in", "fade_out", "position_influence": [x, y, z],
//   "rotation_influence": [x, y, z], "units"?, "tick"?, unknown keys...}. "units" ("view" or
//   "pixels", S48 R22) and "tick" (Hz) are written only when they differ from their defaults
//   ("view", 0), so a file without them re-saves byte for byte.
// - Lenient: parseShakePreset fails (file empty, error set) only for text that is not JSON (with
//   the line/column), a non-object root, a missing or different "format", a missing or newer
//   "version". A bad value is repaired (negative strength, roughness or fade -> 0; an unknown
//   type -> one_shot; a bad vector -> the default) and a missing key takes its default, each with
//   a warning naming the key. Unknown keys are kept, verbatim, and written back.
// - writeShakePreset is canonical and deterministic: 2-space indent, the key order above, one
//   key per line, %.6g numbers; writeShakePreset(parse(writeShakePreset(f))) == writeShakePreset(f).

#ifndef SHAKE_PRESET_H
#define SHAKE_PRESET_H

#include "Export.h"
#include "math/Vector3.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::camera{

    constexpr const char* SHAKE_FORMAT = "jasperengine.shake";
    constexpr int SHAKE_VERSION = 1;
    constexpr const char* SHAKE_EXTENSION = ".shake";

    // MilkShake's ShakeType
    enum class ShakeType : uint8_t{
        // fades in, then fades out right away
        OneShot,
        // fades in, then holds until stopped
        Sustained,
        // S48 R22, pixelfx's hit shake (act.js): a decaying integer offset over fade_out seconds,
        // k = strength * remaining / fade_out, f = floor(remaining * tick) (tick 0: 60),
        // offset (round(sin(2.4 f) k), round(cos(3.7 f) k 0.7)); no noise, no rotation
        Kick
    };

    // what a preset's strength and position influence are measured in (S48 R22)
    enum class ShakeUnits : uint8_t{
        // tenths of the view height in 2D, world units in 3D (the camera shake spec)
        View,
        // art pixels: the S41 grid's (1 / pixels per unit world units) with a Pixel Perfect
        // Camera, else the camera's screen pixels
        Pixels
    };

    // MilkShake's shake parameters (the .shake defaults)
    struct JASPERENGINE_API ShakePreset{
        ShakeType type = ShakeType::OneShot;
        float strength = 1.0f;          // amplitude multiplier (>= 0)
        float roughness = 1.0f;         // noise speed (>= 0): higher is jerkier
        float fadeIn = 0.0f;            // seconds, 0 = instant
        float fadeOut = 0.0f;           // seconds, 0 = instant
        Vector3 positionInfluence = Vector3(1, 1, 1);   // per-axis amplitude in units
        Vector3 rotationInfluence = Vector3(1, 1, 1);   // per-axis amplitude in degrees
        ShakeUnits units = ShakeUnits::View;
        // the offset changes only on ticks of 1 / tick s of the shake's clock (0: every frame);
        // a kick's step rate (0: 60)
        float tick = 0.0f;

        bool operator==(const ShakePreset& other) const;
        bool operator!=(const ShakePreset& other) const { return !(*this == other); }
    };

    // a .shake file: the preset plus what the file carries besides it
    struct JASPERENGINE_API ShakePresetFile{
        std::string guid;               // 32 lowercase hex digits, or empty
        ShakePreset preset;
        // unknown top-level keys in file order: name, compact JSON
        std::vector<std::pair<std::string, std::string>> unknownKeys;
    };

    struct JASPERENGINE_API ShakeParseResult{
        std::optional<ShakePresetFile> file;    // empty when the text can't be read as a preset
        std::vector<std::string> warnings;      // "key: what was done"
        std::string error;
        int line = 0;                           // 1-based position of a JSON syntax error, else 0
        int column = 0;

        bool ok() const { return file.has_value(); }
        // "line:col: error" (or the error)
        std::string errorText() const;
    };

    JASPERENGINE_API ShakeParseResult parseShakePreset(const std::string& text);
    JASPERENGINE_API std::string writeShakePreset(const ShakePresetFile& file);
    JASPERENGINE_API std::string writeShakePreset(const ShakePreset& preset);

    // "one_shot", "sustained", "kick"; false for other names
    JASPERENGINE_API const char* shakeTypeName(ShakeType type);
    JASPERENGINE_API bool shakeTypeFromName(const std::string& name, ShakeType& out);
    // "view", "pixels"; false for other names
    JASPERENGINE_API const char* shakeUnitsName(ShakeUnits units);
    JASPERENGINE_API bool shakeUnitsFromName(const std::string& name, ShakeUnits& out);
    // An unnamed pixel kick (CameraShake.kick, HitStop's {amplitude, seconds}): `amplitudePx`
    // art pixels decaying over `seconds`
    JASPERENGINE_API ShakePreset makeKickPreset(float amplitudePx, float seconds);

    // Clamps negative strength, roughness and fades to 0 (non-finite values to the defaults);
    // what it changed is appended to `warnings` ("strength: ..."), when given
    JASPERENGINE_API void sanitizeShakePreset(ShakePreset& preset, std::vector<std::string>* warnings = nullptr);

    // The span the editor graph and the MCP peaks sample: fade_in + 1 s + fade_out, or 3 s when
    // both fades are 0 (a sustained shake is stopped after fade_in + 1 s)
    JASPERENGINE_API float shakePreviewDuration(const ShakePreset& preset);

}

#endif //SHAKE_PRESET_H
