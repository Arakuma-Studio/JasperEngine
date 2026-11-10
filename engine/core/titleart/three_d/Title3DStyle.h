// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S86-title-3d.md R2, R3, R4, R17, R28, R64, R67, §6.1)
// - Purpose: typed views of the 3D members of a normalized `.titleart` tree (Title3DSchema.h):
//   a layer's 3D style (or R2's derivation of it from the 2D style when `style_3d` is null), its
//   materials, bands and bevels, the document's 3D scene, the lighting a layer or a part renders
//   with (value by value: part <- layer <- document, with the level each value came from), the
//   22 material presets (Pachislop's model/defaults.ts) and the per-character rigs.
// - Presets: applying one sets the members the preset names and keeps the rest (R17).
// - Thread-safety: pure functions of their arguments.

#pragma once

#include "Export.h"
#include "titleart/TitleArtDoc.h"
#include "titleart/geometry/Plate.h"
#include "titleart/three_d/BevelProfile.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::titleart {

    enum class GradientKind3D : uint8_t { Linear, Radial, Reflected };
    enum class GradientSpan3D : uint8_t { Layer, Line, Char, Part };
    enum class SurfaceKind3D : uint8_t { None, Crumple, Hammered, Liquid };
    enum class ToneMapping3D : uint8_t { None, AcesFilmic, AgX, Neutral, Punch };
    enum class LightType3D : uint8_t { Directional, Point, Ambient };
    enum class Mode3D : uint8_t { Extrude, Inflate, Orb };
    enum class BevelPosition3D : uint8_t { Outside, Center, Inside };

    JASPERENGINE_API ToneMapping3D toneMappingFromName(const std::string& name);
    JASPERENGINE_API const char* toneMappingName(ToneMapping3D tone);

    struct TitleMaterial3D {
        std::string preset = "custom";
        TitleColor color{1, 1, 1};
        std::optional<TitleGradient> gradient;
        GradientKind3D gradientKind = GradientKind3D::Linear;
        double gradientAngle = 90, gradientScale = 1, gradientOffset = 0, gradientBend = 0;
        GradientSpan3D gradientSpan = GradientSpan3D::Layer;
        bool gradientDepth = false;         // gradient_axis "depth"
        double metallic = 0, roughness = 0.3, envMultiplier = 1, specularMultiplier = 1;
        double clearcoat = 0, clearcoatRoughness = 0.04;
        double iridescence = 0, iridescenceIor = 1.6;
        std::array<double, 2> iridescenceThicknessNm{180, 520};
        double transmission = 0, ior = 1.5, dispersion = 0, glassDepth = 0;
        double flakes = 0, flakeScale = 0.6;
        Json pattern;                       // S80's pattern object (kind "none" when off)
        SurfaceKind3D surface = SurfaceKind3D::None;
        double surfaceAmount = 0.5, surfaceScale = 20;
        int64_t surfaceSeed = 1;
        TitleAssetRef baseTexture;
        std::array<double, 2> baseTiling{1, 1}, baseOffset{0, 0};
        bool baseWrap = false;              // projection "wrap"
        TitleColor emission{0, 0, 0};
        double emissionIntensity = 0, opacity = 1;
        Json environment = Json::object();  // partial overrides
        std::string link;

        bool hasPattern() const;
    };

    struct Bevel3D {
        double width = 0, depth = 0;        // % of size
        three_d::BevelShape shape;
    };

    struct Band3D {
        std::string id, name;
        bool enabled = true;
        double grow = 3;                    // % of size
        bool roundJoin = false;
        bool fillCounters = false;
        geometry::PlateParams plate;
        double depth = 6, offset = 0;       // % of size
        Bevel3D bevel;
        TitleMaterial3D material;
        std::optional<TitleMaterial3D> side;
    };

    struct Glint3DParams {
        bool enabled = false;
        double threshold = 1, intensity = 1, radius = 6;
        int streaks = 4;
        double length = 40, angle = 0;
        TitleColor tint{1.0f, 0.9568627f, 0.8627451f};
    };

    struct AO3DParams {
        bool enabled = false;
        double radius = 4, intensity = 1;
    };

    struct Camera3D {
        double fov = 30, x = 0, y = 0, zOffset = 0, rotationX = 0, rotationY = 0, rotation = 0;
    };

    struct Environment3D {
        std::string preset = "studio";
        TitleAssetRef hdri, matcap;
        double intensity = 1, rotation = 0;
        TitleColor tint{1, 1, 1};
        double contrast = 1, saturation = 1, blur = 0, repeatX = 1, repeatY = 1;
    };

    struct Light3D {
        LightType3D type = LightType3D::Directional;
        TitleColor color{1, 1, 1};
        double intensity = 2;
        std::array<double, 3> position{-600, 800, 1000};
    };

    struct Inflate3D {
        double height = 12;                 // % of size
        TitleCurve profile;
        double smoothness = 0;
        int detail = 1;
    };

    struct Title3D {
        Mode3D mode = Mode3D::Extrude;
        double depth = 14;                  // % of size
        int curveSegments = 8;
        Bevel3D bevel;
        BevelPosition3D bevelPosition = BevelPosition3D::Outside;
        TitleVec2 skew;
        Inflate3D inflate;
        double faceImage = 1, edgeImage = 0;
        TitleMaterial3D face;
        std::optional<TitleMaterial3D> bevelMaterial, side;
        std::vector<Band3D> bands;
        Glint3DParams glint;
        AO3DParams ao;
        bool envLock = false;
        Json lighting = Json{{"environment", Json::object()}};
        std::optional<Camera3D> camera;
        bool derived = false;               // R2's derivation (style_3d null on a 3D layer)
    };

    struct Scene3D {
        Camera3D camera;
        Environment3D environment;
        std::vector<Light3D> lights;
        ToneMapping3D toneMapping = ToneMapping3D::Neutral;
        double exposure = 1;
        int supersample = 3;
    };

    // the level a lighting value comes from (get_title's sources, the rows' "Inherited")
    enum class LightingLevel : uint8_t { Document, Layer, Part };
    JASPERENGINE_API const char* lightingLevelName(LightingLevel level);

    struct Lighting3D {
        Environment3D environment;
        std::vector<Light3D> lights;
        ToneMapping3D toneMapping = ToneMapping3D::Neutral;
        double exposure = 1;
        // per value key ("preset", "intensity" ... "tone_mapping", "exposure", "lights")
        std::vector<std::pair<std::string, LightingLevel>> sources;
    };

    // ---- decoding ----
    JASPERENGINE_API TitleMaterial3D decodeMaterial3D(const Json& material);
    JASPERENGINE_API Bevel3D decodeBevel3D(const Json& bevel);
    JASPERENGINE_API Environment3D decodeEnvironment3D(const Json& environment);
    JASPERENGINE_API Camera3D decodeCamera3D(const Json& camera);
    JASPERENGINE_API std::vector<Light3D> decodeLights3D(const Json& lights);
    // a layer's 3D style: its `style_3d`, or R2's derivation when null
    JASPERENGINE_API Title3D decodeStyle3D(const Json& layer);
    // the document's scene (every default when `scene_3d` is null)
    JASPERENGINE_API Scene3D decodeScene3D(const Json& root);
    // the effective style_3d object of a layer (normalized; R2's derivation when null)
    JASPERENGINE_API Json effectiveStyle3DJson(const Json& layer);

    // ---- the first switch to 3D (R2) ----
    // the style_3d R2 derives from a layer's 2D style (normalized): the face fill as the face
    // material, each enabled rim a band innermost first, the side at half the face's value; the
    // plain style when the 2D style is S80's default
    JASPERENGINE_API Json deriveStyle3D(const Json& layer);
    JASPERENGINE_API Json plainStyle3D();

    // ---- presets (R17) ----
    // the members a preset sets (empty for "custom" or an unknown id)
    JASPERENGINE_API Json materialPresetValues(const std::string& preset);
    // `material` with the preset's members (and `preset` itself); false for an unknown id
    JASPERENGINE_API bool applyMaterialPreset(Json& material, const std::string& preset);
    // a normalized material_3d object of a preset
    JASPERENGINE_API Json materialPreset(const std::string& preset);
    // the picker's sections: Metals, Gloss and Paint, Glass and Gems, Special
    JASPERENGINE_API const char* materialPresetSection(const std::string& preset);

    // ---- lighting (R3, R28) ----
    // the lighting a layer renders with, or a part with `part` (its environment overrides)
    JASPERENGINE_API Lighting3D resolveLighting(const Scene3D& scene, const Title3D& style, const TitleMaterial3D* part = nullptr);
    // the lights over R28's limit: more than 4 non-ambient lights
    constexpr int TITLE_3D_MAX_LIGHTS = 4;
    JASPERENGINE_API int countNonAmbientLights(const std::vector<Light3D>& lights);

    // ---- per-character 3D (R4) ----
    struct CharRig3D {
        uint32_t charIndex = 0;             // the CharAdjust key
        TitleVec2 offset;                   // % of size
        double dz = 0, tiltX = 0, tiltY = 0, rotation = 0, scale = 1, hScale = 1, vScale = 1;
    };
    // whether a CharAdjust rigs its character: `rig`, or a non-zero dz / tilt_x / tilt_y
    // (`animated` adds S85's tracks)
    JASPERENGINE_API bool charIsRigged(const Json& adjust, bool animated = false);
    JASPERENGINE_API CharRig3D decodeCharRig(uint32_t charIndex, const Json& adjust);
    // whether a CharAdjust gives its character a material block of its own (face_3d, side_3d, bands_3d)
    JASPERENGINE_API bool charHasMaterialBlock(const Json& adjust);

}
