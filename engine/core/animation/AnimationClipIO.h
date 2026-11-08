// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (animation clips spec 2.1, 9.R8/R20/R35-R38)
// - Purpose: the one .anim parser and canonical writer, shared by the engine (AnimationClipPool),
//   the editor document, MCP, the CLI checks and the tests.
// - Thread-safety: pure functions of their arguments (no engine state): any thread.
// - Format: UTF-8 JSON {"format": "jasperengine.anim", "version": 1 | 2 | 3, "guid", "frame_rate", "wrap",
//   "length", "additive_reference" (only when not 0), "humanoid" (only when set), "tracks", "events",
//   "audio" (only when not empty), "preview", "source", unknown keys...}. Version 3 (S40) is written
//   only for a clip with audio cues (ClipAudio.h); version 2 (anim-retarget 6.1) only for a clip
//   with humanoid tracks ({"channel": "humanoid" | "humanoid_position", "bone", "keys"}: no path);
//   every other clip is written as version 1, byte for byte as before. Humanoid keys accept
//   {"angles": [dof1, dof2, twist]} degrees for the 22 slots (written back as quaternions).
// - Invariants:
//   * parseClip fails (data empty, error set) only for: text that is not JSON (error.line/column
//     of the syntax error), a non-object root, a missing or different `format`, a missing or
//     newer `version`, and Unity YAML clips (error.message = unityYamlRefusal()). Everything
//     else is lenient: the bad key/track/event is dropped (or the value repaired) with a
//     warning carrying its JSON path, the rest loads.
//   * a parsed ClipData satisfies the AnimationClip.h invariants (normalize() is applied).
//   * writeClip is deterministic and canonical: 2-space indent, fixed key order, one key per
//     line, values with %.6g of the float, times with at most 6 decimals, tracks sorted by
//     path then channel in ClipChannel enum order, events by (time, function), quaternions
//     renormalized only when
//     |‖q‖ - 1| > 1e-5, tangent objects only for keys with tangents, every lattice point in
//     every key, unknown keys written back. writeClip(parseClip(writeClip(d))) == writeClip(d)
//     byte for byte, and parse + write of a canonical file is a no-op.
// - Canonical details: members in the order of the Format line; "guid", "length"
//   (explicit only), "preview" and "source" only when set, "tracks" and "events" always (empty:
//   []); a track is {"path", "channel", "relative"?, "loop"?, "keys"} with one key per line,
//   [time, value] / [time, value, ease] (ease only when not linear) or the object form for keys
//   with tangents; lattice values {"points": [[flat, [x, y, z]], ...]} in ascending point order;
//   events {"time", "function", "args"?}, number args with %.15g; property tracks and unknown
//   keys as compact JSON (nlohmann dump), property tracks with their path/component/property
//   rewritten from the data. Times live on the 1e-6 s grid (normalize rounds them), keys on the
//   same grid point collapse (the later one wins). The writer decides quaternion hemispheres on
//   the written (%.6g) values, so a reader sees the same signs.
// - Euler keys ({"euler": [x, y, z]} degrees) use edit_animation's order: Quaternion(x, y, z).

#ifndef ANIMATION_CLIP_IO_H
#define ANIMATION_CLIP_IO_H

#include "Export.h"
#include "animation/AnimationClip.h"

#include <optional>
#include <string>
#include <vector>

namespace jasperengine::anim{

    constexpr const char* ANIM_FORMAT = "jasperengine.anim";
    // the newest version this engine reads and writes (clips with audio cues); a humanoid clip
    // without cues is written as version 2, any other clip as version 1
    constexpr int ANIM_VERSION = 3;
    // humanoid track bones: the 22 slots, then the 33 extra bones (ClipTrackData::humanBone)
    constexpr int HUMANOID_TRACK_BONE_COUNT = 22 + 33;
    constexpr const char* ANIM_EXTENSION = ".anim";
    // frame_rate bounds and key time resolution (seconds)
    constexpr float ANIM_MIN_FRAME_RATE = 1.0f;
    constexpr float ANIM_MAX_FRAME_RATE = 240.0f;
    constexpr float ANIM_TIME_EPSILON = 1e-6f;

    struct JASPERENGINE_API ClipParseError{
        int line = 0;           // 1-based; 0 when the error is not at a text position
        int column = 0;         // 1-based
        std::string message;

        bool empty() const { return message.empty(); }
        // "line:col: message" (or just the message)
        std::string toString() const;
    };

    struct JASPERENGINE_API ClipParseResult{
        std::optional<ClipData> data;           // empty when the file can't be read as a clip
        std::vector<ClipWarning> warnings;
        ClipParseError error;
        // keys of slerp tracks whose sign the file flips from the key before (path, time), which
        // normalize takes out: a canonical file has none. check --anims reports them (S77 R25)
        std::vector<std::pair<std::string, float>> rotationSignFlips;

        bool ok() const { return data.has_value(); }
    };

    JASPERENGINE_API ClipParseResult parseClip(const std::string& text);
    // the canonical text of `data` (normalized first; the input is not changed)
    JASPERENGINE_API std::string writeClip(const ClipData& data);
    // Restores the AnimationClip.h invariants on hand-built data (sort and merge tracks and keys,
    // pad widths, renormalize quaternions with hemisphere continuity, clamp frame rate...);
    // what it changed is appended to `warnings` (JSON paths of the input)
    JASPERENGINE_API void normalize(ClipData& data, std::vector<ClipWarning>* warnings = nullptr);

    // {"euler": [x, y, z]} degrees as a quaternion, in the order edit_animation uses
    // (Quaternion(x, y, z): qz * qy * qx), always in degrees: unlike Quaternion's Euler
    // constructor it does not read Engine::isUseDegrees(), so it is pure (any thread)
    JASPERENGINE_API Quaternion eulerDegreesToQuaternion(float x, float y, float z);

    // Unity's YAML .anim (%YAML header or an "AnimationClip:" document)
    JASPERENGINE_API bool isUnityYamlClip(const std::string& text);
    // "This is a Unity .anim (YAML). JasperEngine clips are JSON {"format": "jasperengine.anim"}:
    // create one with Assets > Create > Animation Clip or edit_animation {clip, create}."
    JASPERENGINE_API const char* unityYamlRefusal();

    // GUIDs (32 lowercase hex digits, AssetDatabase's form)
    JASPERENGINE_API std::string generateClipGuid();
    JASPERENGINE_API bool isClipGuid(const std::string& text);
    // The top-level "guid" of a clip text without a full parse (AssetDatabase), empty without one
    JASPERENGINE_API std::string readClipGuid(const std::string& text);
    // The text with its top-level "guid" set (inserted right after "version" when missing), the
    // rest byte-identical; false when the text is not a clip (it is left unchanged)
    JASPERENGINE_API bool replaceClipGuid(std::string& text, const std::string& guid);

    // File names of the enums ("position", "ping_pong", "quad_out"...); false for unknown names
    JASPERENGINE_API const char* channelName(ClipChannel channel);
    JASPERENGINE_API bool channelFromName(const std::string& name, ClipChannel& out);
    // a humanoid track's bone name: "left_upper_arm", "left_index_proximal" (humanoid), "hips",
    // "left_foot", "right_foot" (humanoid_position); "" when out of range
    JASPERENGINE_API const char* humanoidTrackBoneName(ClipChannel channel, int humanBone);
    JASPERENGINE_API bool humanoidTrackBoneFromName(ClipChannel channel, const std::string& name, int& out);
    // "default", "once", "loop", "ping_pong", "clamp_forever"
    JASPERENGINE_API const char* wrapName(ClipWrapMode wrap);
    JASPERENGINE_API bool wrapFromName(const std::string& name, ClipWrapMode& out);
    // "linear", "step", "<quad|cubic|quart|quint|sine|expo|circ|back|elastic|bounce>_<in|out|in_out>"
    JASPERENGINE_API const char* easeName(EaseType ease);
    // "clamped_auto", "auto", "free", "linear", "constant", "flat", "broken" ("" for None)
    JASPERENGINE_API const char* tangentModeName(ClipTangentMode mode);
    JASPERENGINE_API bool tangentModeFromName(const std::string& name, ClipTangentMode& out);
    // The tangents of `track`'s keys that have a computed mode (ClampedAuto, Auto, Linear, Flat,
    // Constant), from their neighbours' values and times (value units per second, componentwise;
    // a rotation's neighbours on the key's hemisphere); a Free or Broken key without tangents
    // starts from ClampedAuto's; a key without a mode in a track where any key has one gets the
    // straight lines' (Linear). Constant keys get the step ease. Nothing when no key has a mode.
    // Clamped Auto is Unity's: 0 at an extremum and at the ends, else the slope through the
    // neighbours clamped to 3x the smaller side slope (no overshoot, Fritsch-Carlson).
    JASPERENGINE_API void applyTangentModes(ClipTrackData& track);
    JASPERENGINE_API bool easeFromName(const std::string& name, EaseType& out);

    // The sign of a rotation key value an editor writes into a track (x, y, z, w): negated when
    // its dot product with `reference` is < 0. The reference is the key it replaces, or the previous
    // key when it is inserted. Normalize leaves the signs of a track with tangents as they are (a
    // per-component Hermite spline), so an edit that flips a key's sign (an euler converted to +q
    // next to stored -q keys) would make the segments into and out of it pass near zero; aligned,
    // the same rotation samples the same. No-op unless both have 4 values.
    JASPERENGINE_API void alignQuaternionKey(std::vector<float>& value, const std::vector<float>& reference);

    // the event function rule: a Lua identifier that is not a keyword ("end", "function"...);
    // the loader drops other events
    JASPERENGINE_API bool isLuaIdentifier(const std::string& name);

}

#endif //ANIMATION_CLIP_IO_H
