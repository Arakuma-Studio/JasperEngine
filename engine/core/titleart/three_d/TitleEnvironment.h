// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S86-title-3d.md R24, R25, R27, §6.5)
// - Purpose: the title environments' data and their CPU twin. A procedural preset is a scene:
//   latitude stops of a sky (linear in elevation), opaque panels (facing the origin), discs and
//   tubes, and additive soft panels (the key softbox at azimuth -95, 70 x 110 degrees), ported as
//   data from Pachislop src/engine/three/environment.ts by SafEight, used with the author's
//   permission (`night`'s horizon lights come from its LCG, seed 1337). title_env.frag evaluates
//   the same scene per cube texel; evaluateTitleEnvironment is the CPU twin the tests compare.
// - Levels: roughness -> mip level is size invariant against a 512 face (6 r + log2(face / 512)
//   from r = 1/6 up; the extra top levels of larger faces sharpen near-mirrors below it), down to
//   8 px faces. titleEnvironmentLevel / titleEnvironmentRoughness are the shader's mapping.
// - Thread-safety: pure; the preset table is built once.

#pragma once

#include "Export.h"
#include "titleart/three_d/Title3DStyle.h"
#include "titleart/three_d/TitleMesh.h"

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::titleart {

    enum class TitleEnvElementKind : uint8_t { Panel = 0, Disc = 1, Tube = 2, SoftPanel = 3 };

    struct TitleEnvElement {
        TitleEnvElementKind kind = TitleEnvElementKind::Panel;
        std::array<float, 3> a{0, 0, 0};    // panel / disc centre, tube start
        std::array<float, 3> b{0, 0, 0};    // tube end
        float width = 0, height = 0;        // panels
        float radius = 0;                   // discs, tubes
        std::array<float, 3> radiance{0, 0, 0};
        float edge = 0;                     // soft panels: the fade, fraction of the half size
    };

    struct TitleEnvScene {
        std::string id;
        std::vector<std::array<float, 4>> sky;      // (elevation -1..1, r, g, b), ascending
        std::vector<TitleEnvElement> elements;
        // the HDRI a photo or softbox preset lights with (its manifest slug), empty for scenes
        std::string hdri;
        bool procedural = true;
    };

    constexpr int TITLE_ENV_MAX_STOPS = 16;
    constexpr int TITLE_ENV_MAX_ELEMENTS = 96;
    constexpr int TITLE_ENV_MIN_FACE = 8;
    // bumped when the builder's output changes (its caches key on it)
    constexpr uint32_t TITLE_ENV_BUILDER_VERSION = 1;

    // the procedural presets' scenes (studio, strips, backlit, under, sunset, neon, gold, night);
    // softbox and the photo presets are HDRIs (their scene holds the key softbox only for softbox)
    JASPERENGINE_API const TitleEnvScene* titleEnvironmentPreset(const std::string& id);
    JASPERENGINE_API bool titleEnvironmentIsPhoto(const std::string& id);

    // linear radiance along a unit direction (the CPU twin of title_env.frag's procedural pass)
    JASPERENGINE_API std::array<float, 3> evaluateTitleEnvironment(const TitleEnvScene& scene, const std::array<float, 3>& direction);
    // the cube texel direction of a face (+X, -X, +Y, -Y, +Z, -Z; st 0..1), title_env.frag's
    JASPERENGINE_API std::array<float, 3> titleEnvFaceDirection(int face, float s, float t);

    // roughness -> level and back for a cube of `face` px
    JASPERENGINE_API float titleEnvironmentLevel(float roughness, int face);
    JASPERENGINE_API float titleEnvironmentRoughness(int level, int face);
    JASPERENGINE_API int titleEnvironmentLevels(int face);
    // the face size a render uses: procedural 1024 (2048 at pxScale >= 2), photo and softbox 512,
    // a custom HDRI width / 4 clamped to 256..1024; Realtime tiers cap it (§6.7)
    JASPERENGINE_API int titleEnvironmentFace(const Environment3D& environment, double pxScale, TitleTier tier, int customWidth = 0);

    // ---- HDRIs (photo and softbox presets) ----
    // an equirectangular HDRI, linear RGBA half floats, rows top first
    struct TitleHdri {
        int width = 0, height = 0;
        std::vector<uint16_t> rgba;
    };
    // where HDRIs come from (the editor reads its bundled ones, editor/resources/titleart/hdri);
    // without a loader photo presets render the studio with `environment_missing`
    using TitleHdriLoader = std::function<std::shared_ptr<const TitleHdri>(const std::string& slug)>;
    JASPERENGINE_API void setTitleHdriLoader(TitleHdriLoader loader);
    // the HDRI of a slug through the loader, kept once loaded; null when none
    JASPERENGINE_API std::shared_ptr<const TitleHdri> loadTitleHdri(const std::string& slug);
    // the yaw of a panorama (Pachislop's spheres: softbox 0, the photos pi / 2)
    JASPERENGINE_API float titleHdriYaw(const std::string& preset);

    // the grade of R27 applied to a radiance (tint, saturation, contrast about 0.5, intensity),
    // the shader's tlGrade
    JASPERENGINE_API std::array<float, 3> gradeTitleEnvironment(const std::array<float, 3>& radiance, const Environment3D& environment);

}
