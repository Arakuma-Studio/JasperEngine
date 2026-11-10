// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S85-title-animation.md R1-R10, R59, R60, §6.1, §6.8)
// - Purpose: the JSON of title animations, always built. (1) The codec of the authoring form:
//   the `.titleart`'s `animation` member as JSON objects (the module's TitleAnimIO adds what
//   needs the document: ids, layer and field checks, orphans, track order) and the `.anim` key
//   grammar `[t, v]`, `[t, v, ease]`, `{time, value, ease, in, out, tangent_mode}`. (2)
//   `<file>.anim.json` (format `jasperengine.titleanim`, version 1), which the bake writes beside
//   the PNGs and a Baked player reads with S81's sidecar, compiling its title-live tracks here.
// - Lenient (R59): a bad animation, track, key or marker is dropped with a warning carrying its
//   JSON path ("animation.animations[1].tracks[3].keys[2]: expected [time, value]"); limits are
//   clamped; unknown members are kept and written back after the known ones.
// - Canonical writing: every member, defaults included, in §6.1's order, except stagger, drive,
//   reseed, preset_source, from_preset and sound (only when present); numbers rounded to float
//   precision (7 significant digits).
// - A colour key keeps its "#rrggbb" text (TitleKey::stepped) beside its sRGB channels.
// - Thread-safety: pure functions.

#ifndef TITLE_ANIM_DATA_IO_H
#define TITLE_ANIM_DATA_IO_H

#include "Export.h"
#include "json.hpp"
#include "titleanim/TitleAnimTypes.h"

#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::titleanim{

    using AnimJson = nlohmann::ordered_json;

    constexpr const char* TITLE_ANIM_DATA_FORMAT = "jasperengine.titleanim";
    constexpr int TITLE_ANIM_DATA_VERSION = 1;
    constexpr const char* TITLE_ANIM_DATA_SUFFIX = ".anim.json";

    struct JASPERENGINE_API TitleAnimWarning{
        std::string path;
        std::string message;
        std::string toString() const;
    };

    // ---- values and keys ----

    // a number to float precision, as the file stores it
    JASPERENGINE_API double roundToFloat(double value);
    // "#rrggbb" or "#rgb" -> sRGB channels 0..1
    JASPERENGINE_API bool parseHexColor(std::string_view text, float* rgb);
    JASPERENGINE_API std::string formatHexColor(const float* rgb);
    // a numeric value: a number, an array of numbers, or "#rrggbb"; writes up to 3 floats and
    // their count
    JASPERENGINE_API bool numericFromJson(const AnimJson& value, float* out, size_t& width);
    // a key value as JSON (the colour text when the key has one)
    JASPERENGINE_API AnimJson keyValueJson(const TitleKey& key);
    // one key in the .anim grammar; false with `error` ("expected [time, value]")
    JASPERENGINE_API bool decodeKey(const AnimJson& value, TitleKey& out, std::string& error);
    JASPERENGINE_API AnimJson encodeKey(const TitleKey& key);

    // ---- the authoring form ----

    // the whole member; `path` prefixes the warnings ("animation")
    JASPERENGINE_API void decodeAnimationSet(const AnimJson& value, const std::string& path, TitleAnimationSet& out,
                                             std::vector<TitleAnimWarning>* warnings);
    JASPERENGINE_API AnimJson encodeAnimationSet(const TitleAnimationSet& set);
    // false when the value cannot be an animation (dropped)
    JASPERENGINE_API bool decodeAnimation(const AnimJson& value, const std::string& path, TitleAnimation& out,
                                          std::vector<TitleAnimWarning>* warnings);
    JASPERENGINE_API AnimJson encodeAnimation(const TitleAnimation& animation);
    JASPERENGINE_API bool decodeTrack(const AnimJson& value, const std::string& path, TitleTrack& out, std::vector<TitleAnimWarning>* warnings);
    JASPERENGINE_API AnimJson encodeTrack(const TitleTrack& track);
    JASPERENGINE_API bool decodeMarker(const AnimJson& value, const std::string& path, TitleMarker& out, std::vector<TitleAnimWarning>* warnings);
    JASPERENGINE_API AnimJson encodeMarker(const TitleMarker& marker);
    JASPERENGINE_API bool decodeStagger(const AnimJson& value, const std::string& path, TitleStagger& out, std::vector<TitleAnimWarning>* warnings);
    JASPERENGINE_API AnimJson encodeStagger(const TitleStagger& stagger);
    JASPERENGINE_API bool decodeDrive(const AnimJson& value, const std::string& path, TitleDrive& out, std::vector<TitleAnimWarning>* warnings);
    JASPERENGINE_API AnimJson encodeDrive(const TitleDrive& drive);
    // a valid animation name: 1..32 characters, any Unicode but '/' and control characters
    JASPERENGINE_API bool isAnimationName(std::string_view name);
    // markers in (time, name) order
    JASPERENGINE_API void sortMarkers(std::vector<TitleMarker>& markers);

    // ---- title-live tracks (the Baked player's compile) ----

    // A title-level track (layer "") compiled: its virtual field (and component, "tint.r"), keys
    // or drive, rest and range. False with `error` when its field is not a title field.
    JASPERENGINE_API bool compileTitleTrack(const TitleTrack& track, CompiledTitleTrack& out, std::string* error);
    // "transform.position.x" -> (Position, 0); "tint" style names without a component: -1
    JASPERENGINE_API bool splitComponent(std::string_view field, std::string& base, int& component);
    // The animation's settings, markers and title tracks compiled (the other tracks are left
    // out); the duration resolved (automatic: the last key with its delays and the last marker)
    JASPERENGINE_API void compileTitleOnly(const TitleAnimation& animation, CompiledTitleAnimation& out, std::vector<TitleAnimWarning>* warnings);
    // R1: the automatic duration from compiled tracks and markers (>= 0.01)
    JASPERENGINE_API double resolveDuration(const TitleAnimation& animation, const CompiledTitleAnimation& compiled);
    JASPERENGINE_API void compileMarkers(const TitleAnimation& animation, CompiledTitleAnimation& out);

    // ---- <file>.anim.json ----

    struct JASPERENGINE_API TitleAnimDataEntry{
        TitleAnimation authored;            // settings, title tracks and markers as authored
        CompiledTitleAnimation compiled;    // with the flipbook and the Baked figures
    };

    struct JASPERENGINE_API TitleAnimData{
        std::string sourceHash;
        uint32_t bakeVersion = 0;
        std::string defaultName;
        std::vector<TitleAnimDataEntry> animations;
        const TitleAnimDataEntry* find(std::string_view name) const;
    };

    // false with `error` (one sentence) for text that is not an animation data file or a newer
    // version ("needs a newer JasperEngine (version 2; this build reads 1)")
    JASPERENGINE_API bool readAnimData(std::string_view text, TitleAnimData& out, std::vector<std::string>* warnings, std::string* error);
    JASPERENGINE_API bool loadAnimData(const std::string& path, TitleAnimData& out, std::vector<std::string>* warnings, std::string* error);
    JASPERENGINE_API std::string writeAnimData(const TitleAnimData& data);

    // "assets/logo.titleart" -> "assets/logo.titleart.anim.json"
    JASPERENGINE_API std::string titleAnimDataPath(const std::string& titlePath);
    // a flipbook page's file name beside the document: "logo.titleart.7c1e0a42.flip0.png"
    JASPERENGINE_API std::string titleFlipbookPageFile(const std::string& titlePath, const std::string& animationId, int page);

}

#endif //TITLE_ANIM_DATA_IO_H
