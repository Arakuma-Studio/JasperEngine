// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef FOG_PROFILE_H
#define FOG_PROFILE_H

// The Fog Profile asset (docs/specs/S101-vapor.md R1-R6): a fog look shared by every Fog and Fog Zone
// that links it. Each look field is either set by the profile (an override) or left to whatever
// is beneath it. FogProfileIO reads and writes the .fogprofile JSON, FogProfiles holds the
// built-ins, FogProfileLibrary is the runtime cache by asset path. The resolve that layers the
// profiles is render/FogResolve.h.

#include "Export.h"
#include "component/FogComponent.h"

#include <bitset>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine{

    // The look fields of FogComponent a profile can set (R2), in the canonical order the file
    // writes them. Fields that set cost or technique (type, volumetric, volumetricDistance,
    // lightShafts, qualityOverride) are never in a profile. Append-only is not needed (the file
    // names fields), but the order is the file's.
    enum class FogField : uint8_t{
        Color,
        Density,
        LinearStart,
        LinearEnd,
        ColorMode,
        AttenuationDistance,
        BaseHeight,
        MaximumHeight,
        StartDistance,
        MaxFogDistance,
        MaxOpacity,
        DirectionalInscattering,
        Anisotropy,
        SkyFill,
        SkyFillAngle,
        NoiseIntensity,
        NoiseScale,
        Wind,
        Albedo,
        AmbientMultiplier,
        Emissive,
        Count
    };
    inline constexpr size_t FOG_FIELD_COUNT = static_cast<size_t>(FogField::Count);
    using FogFieldSet = std::bitset<FOG_FIELD_COUNT>;

    // how a field's value is held and blended (R8)
    enum class FogFieldKind : uint8_t{
        Float,          // lerp
        Extinction,     // a distance blended as 1/d (attenuationDistance)
        Color,          // linear RGB, lerp per channel
        Vector3,        // lerp per component (wind)
        Bool,           // switches at w >= 0.5
        Enum            // switches at w >= 0.5 (colorMode)
    };

    struct FogFieldInfo{
        FogField field;
        const char* name;       // the FogComponent member, the reflection key, the JSON key, Lua's name
        const char* label;      // as the inspector shows it
        FogFieldKind kind;
    };

    class JASPERENGINE_API FogFields{
    public:
        static const FogFieldInfo& info(FogField field);
        static const char* name(FogField field);
        // false for a name that is no look field (type, volumetric, ... included)
        static bool find(std::string_view name, FogField& field);

        // to.<field> = from.<field>
        static void copy(FogComponent& to, const FogComponent& from, FogField field);
        // from.<field> == to.<field>, bit for bit
        static bool equal(const FogComponent& a, const FogComponent& b, FogField field);
        // v = lerp(v, target, w) by the field's kind (R8)
        static void blend(FogComponent& value, const FogComponent& target, FogField field, float w);

        // the field as floats (1 for Float, Extinction, Bool, Enum; 3 for Color, Vector3)
        static int width(FogField field);
        static void read(const FogComponent& fog, FogField field, float* out);
        static void write(FogComponent& fog, FogField field, const float* in);

        // a fog of this type reads the field: the distance types their colour, density and linear
        // range; Height every other one, and its colour (diagnostics' profile_no_effect, the
        // Type a link switches to)
        static bool readByType(FogType type, FogField field);

        // "color", "sky", "lit"
        static const char* colorModeName(FogColorMode mode);
        static bool findColorMode(std::string_view name, FogColorMode& mode);
    };

    // A profile's content: which fields it sets and their values (the others in `values` are
    // FogComponent's defaults and mean nothing)
    struct JASPERENGINE_API FogProfileData{
        std::string name;
        std::string description;
        FogFieldSet set;
        FogComponent values;

        bool isSet(FogField field) const { return set.test(static_cast<size_t>(field)); }
        // sets the field from `from` (and marks it set)
        void setFrom(const FogComponent& from, FogField field);
        void clear(FogField field);
        // every look field of `fog`, all set (the inspector's New)
        static FogProfileData fromFog(const FogComponent& fog, const std::string& name);
        // fog's look with this profile's overrides applied at full weight
        void applyTo(FogComponent& fog) const;

        bool operator==(const FogProfileData& other) const;
        bool operator!=(const FogProfileData& other) const { return !(*this == other); }
    };

    // The .fogprofile file (docs/specs/S101-vapor.md §6.2): JSON, {"version", "name", "description"?,
    // "fields": {<field>: <value>}} with fields in FogField order, colours as linear [r, g, b],
    // colorMode as "color" / "sky" / "lit". Its GUID is in its .meta (S04).
    class JASPERENGINE_API FogProfileIO{
    public:
        static constexpr int VERSION = 1;
        static constexpr const char* EXTENSION = ".fogprofile";

        // false + error when the text is not JSON, not an object, of a newer version, or has no
        // version. A field of the wrong type is left unset and named in `problems`; a key that
        // is no look field is left out and named in `unknown`.
        static bool parse(const std::string& json, FogProfileData& out, std::string* error = nullptr,
                          std::vector<std::string>* unknown = nullptr, std::vector<std::string>* problems = nullptr);
        // canonical: the same data always writes the same bytes (LF, 2-space indent)
        static std::string write(const FogProfileData& data);
        // through io::writeFileAtomic (a Document write)
        static bool save(const std::filesystem::path& file, const FogProfileData& data, std::string* error = nullptr);

        static bool isProfilePath(std::string_view path);
    };

    struct FogProfileBuiltin{
        const char* id;          // snake_case: "builtin:<id>"
        FogProfileData data;
    };

    // The built-ins (R4), embedded in the engine: the five looks of AtmospherePresets with the
    // values applyFogPreset writes (so a Fog of type Height linked to one renders as the preset
    // did), then bright_haze, murky, infernal and toxic.
    class JASPERENGINE_API FogProfiles{
    public:
        static constexpr const char* BUILTIN_PREFIX = "builtin:";

        static const std::vector<FogProfileBuiltin>& builtins();
        // by id ("night") or key ("builtin:night"); nullptr when there is none
        static const FogProfileBuiltin* findBuiltin(std::string_view idOrKey);
        static bool isBuiltinKey(std::string_view key);
        // "builtin:<id>"
        static std::string builtinKey(std::string_view id);
        // where the editor copies a built-in on first use: FogProfiles/<id>.fogprofile under the
        // assets directory (assets-relative)
        static std::string builtinCopyPath(std::string_view id);
    };

    // Every profile the game has read, by key (an assets-relative path, "builtin:<id>",
    // "runtime:<n>", or a GUID through AssetMap). Main thread only.
    //
    // A file is read on first use (small, synchronously) and kept until it changes (reload) or
    // the editor sets it; a missing or unreadable file is an empty profile (R6), logged once
    // until it changes. Every user looks its profile up each frame it resolves, so a reload or
    // a set shows on the next frame (R5).
    class JASPERENGINE_API FogProfileLibrary{
    public:
        // nullptr for an empty key, a missing or unreadable file, an unknown built-in
        static std::shared_ptr<const FogProfileData> get(const std::string& key);
        // the key a reference resolves to: its path, else its GUID's path (AssetMap)
        static std::shared_ptr<const FogProfileData> getRef(const AssetRef& ref);
        // the last read failed (missing, unreadable): the empty profile R6 asks for, and why
        static bool isMissing(const std::string& key, std::string* reason = nullptr);

        // the file changed (S05, the editor's own writes): read again on its next use
        static void reload(const std::string& path);
        // replaces a path's profile without reading a file (the editor's edits, tests)
        static void set(const std::string& path, const FogProfileData& data);
        // reads `json` as the path's profile (tests, unsaved previews); false + error when it does not read
        static bool load(const std::string& path, const std::string& json, std::string* error = nullptr);
        // forgets every profile (a project closed)
        static void clear();

        // a mutable profile made at run time (Lua's clone): a "runtime:<n>" key that names it
        // wherever a profile key goes, until resetRuntime
        static std::string addRuntime(std::shared_ptr<FogProfileData> profile);
        static std::shared_ptr<FogProfileData> getRuntime(const std::string& key);
        static bool isRuntimeKey(std::string_view key);
        // drops the runtime profiles (Play stopped)
        static void resetRuntime();

        // changes on every reload, set or load of that key (0: never read)
        static uint32_t version(const std::string& key);
        // changes whenever any entry changes
        static uint32_t generation();

        // an assets-relative path in the library's form ('/' separators, no "./")
        static std::string normalizeKey(const std::string& key);
    };

}

#endif // FOG_PROFILE_H
