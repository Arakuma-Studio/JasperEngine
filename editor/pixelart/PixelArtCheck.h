// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The pixel-art validator (docs/specs/S42-pixel-art-mode.md R9-R14, D3, D5, D9): one checker for
// check_pixel_art (MCP), the Pixel Art Check window, the Console summaries and `check
// --pixel-art` (CLI). Its input is plain data taken from a Project (live, unsaved edits
// included: PixelArtInput.cpp) and its rules are pure functions of it plus S41's layout and snap
// rule, so the same project state gives the same JSON byte for byte. A finding names what it
// measured, what was expected and the changes that fix it (the first option the default); a
// finding no automatic change can fix names why a person must look (ritmo's "never a confident
// bad fix"). The family's later specs add rules (S46's cleanup_outputs) through registerRule.

#include "pixelart/PixelArtSettings.h"
#include "pixelart/PixelProblem.h"
#include "pixelart/Rgba8.h"
#include "texture/TextureImportSettings.h"

#include "math/Quaternion.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "pixelart/PixelPerfectLayout.h"
#include "render/Render.h"

#include "json.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine {
    enum class Scaling;   // Engine.h
}

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::pixelart {

    enum class RuleStatus : uint8_t { Pass, Warn, Fail };
    // "PASS", "WARN", "FAIL"
    const char* ruleStatusName(RuleStatus status);

    struct FixChange {
        std::string target;            // "project", "scenes/Main.scene#/Player", "sprites/hero.png"
        std::string field;             // "graphics.canvas_width", "mesh.submeshes[0].material.baseColorTexture.min_filter"
        nlohmann::json from, to;
    };
    struct FixOption {
        std::string id, label;         // "default" (the first), "on_grid" ...
        std::vector<FixChange> changes;
    };
    struct Finding {
        std::string id;                // F1, F2 ... in rule order, then target order
        std::string key;               // <rule>:<target>[:<field>], stable across runs
        std::string rule;
        RuleStatus status = RuleStatus::Warn;
        nlohmann::json target;         // {kind, path, scene, entity, field}
        nlohmann::json measured, expected;
        std::string message;           // one sentence
        std::string needsPerson;       // why no change fixes it (set when options is empty)
        std::vector<FixOption> options;
    };
    struct RuleResult {
        std::string id;
        RuleStatus status = RuleStatus::Pass;
        int checked = 0;
        int findings = 0;
    };
    struct CheckResult {
        Verdict verdict = Verdict::Clean;
        std::vector<RuleResult> rules;
        std::vector<Finding> findings;
        nlohmann::json project, cameras;
        // R11: rules, findings, project, cameras, verdict and counts, keys sorted
        nlohmann::json toJson() const;
        int count(RuleStatus status) const;
        const Finding* find(const std::string& keyOrId) const;
    };

    // ---- the input (D3): what the rules read, nothing else

    struct PixelArtTexture {
        std::string path;                      // assets-relative "sprites/hero.png"
        bool exists = false;
        bool hasSettings = false;              // the meta has an import.textureImporter section
        TextureImportSettings settings;
        int width = 0, height = 0;             // the source image's (0: unknown)
        bool hasAlpha = true;
        // a generated page: the sheet it belongs to (an .aseprite file, an atlas), else ""
        std::string sheetSource;
        // .aseprite / .ase: its importer's texture compression ("none" or "project")
        bool aseprite = false;
        std::string asepriteCompression = "none";
        // an atlas page (.spriteatlas): the atlas's own compression ("none", "project")
        bool atlas = false;
        std::string atlasCompression = "none";
        bool pixelArt = false;                 // resolved: settings.pixelArt, or a pixel-art drawable uses it
        bool paletteSwap = false;              // a Palette Swap material draws it (S43 R15)
        std::string paletteSwapSource;         // the swap's source palette (assets-relative), when named
    };

    enum class DrawableKind : uint8_t { Sprite, Tilemap, Image, Text, Particles };
    const char* drawableKindName(DrawableKind kind);    // "sprite", "tilemap", "image", "text", "particles"

    struct PixelArtDrawable {
        DrawableKind kind = DrawableKind::Sprite;
        std::string scene;                     // project-relative scene file
        std::string entity;                    // S31 entity path "/Player"
        uint32_t entityId = 0;
        std::string texture;                   // assets-relative, "" when none
        std::string textureField;              // the property that holds it
        TextureFilter minFilter = TextureFilter::LINEAR, magFilter = TextureFilter::LINEAR;
        bool sheet = false;                    // a sheet sprite (its pages are the textures)
        std::string sheetPath;
        // the Transform (local values, and the world values from the parent chain)
        bool hasTransform = false;
        Vector3 position, scale = Vector3(1, 1, 1);
        Quaternion rotation;
        Vector3 worldPosition, worldScale = Vector3(1, 1, 1);
        Quaternion worldRotation;
        Vector3 parentWorldScale = Vector3(1, 1, 1);
        Quaternion parentWorldRotation;
        // Sprite / Tilemap: their own Pixels Per Unit (0: the project's), pixel size and pivot
        float pixelsPerUnit = 0.0f;
        float width = 0.0f, height = 0.0f;
        Vector2 pivot = Vector2(0, 0);         // the pivot in the quad's pixels from its bottom-left
        PixelRotation pixelRotation = PixelRotation::CAMERA;
        // Image: the rect's size and nine-slice margins
        float rectWidth = 0.0f, rectHeight = 0.0f;
        bool sliced = false;
        // Text
        std::string font;                      // "" the engine's default font
        unsigned int fontSize = 0;
        // Particles
        std::string particleRenderMode;        // "billboard", "stretched_billboard" ...
        std::vector<float> particleSizes;      // constant start sizes (world units)
        bool particleEffectAsset = false;      // its effect is a .particles asset (path in particleEffect)
        std::string particleEffect;
        // Palette Swap on its material
        bool paletteSwap = false;
        std::string paletteSwapSource;
        // library:sprite material identity (features, blend) for sprite_variants
        std::string spriteMaterial;
    };

    // a Pixel Perfect Camera's settings, as the rules read them
    struct PixelArtCameraSettings {
        bool enabled = true;
        unsigned int referenceWidth = 0, referenceHeight = 0;
        PixelUpscaleMode upscaleMode = PixelUpscaleMode::INTEGER;
        bool upscaleRenderTexture = true;
        bool pixelSnapping = true;
        PixelRotation rotationMode = PixelRotation::STEPS_90;
        bool subpixelSmoothing = false;
    };

    struct PixelArtCamera {
        bool exists = false;                   // a saved camera GameObject (else the scene's default view)
        std::string entity;
        uint32_t entityId = 0;
        bool perspective = false;
        float roll = 0.0f;                     // degrees about Z
        Vector3 rotationEuler;                 // degrees, the Inspector's
        bool hasComponent = false;             // a Pixel Perfect Camera
        PixelArtCameraSettings component;
    };

    struct PixelArtScene {
        std::string path;                      // project-relative ("scenes/Main.scene"), "" unsaved
        std::string name;
        bool is2D = false;
        bool pixelArt = false;                 // R9: a pixel-art scene
        bool drawsSprites = false;             // a 2D scene with sprites or tilemaps (pixel_art_mode's content)
        bool fixedResolution = false;
        int fixedWidth = 0, fixedHeight = 0;
        PixelArtCamera camera;
        // post-processing of the main camera
        bool fxaa = false;
        int msaaSamples = 1;
        bool blur = false;
        float renderScale = 1.0f;
        bool dynamicResolution = false;
        std::vector<PixelArtDrawable> drawables;
        // library:sprite variants (sprite_variants): material identity -> distinct variants
        std::map<std::string, std::set<std::string>> spriteVariants;
    };

    // a Device Tier whose Render Scale cap is below 1 (S56): its devices draw the frame smaller
    // and stretch it back bilinear
    struct PixelArtRenderScaleCap {
        std::string group, tier;               // "desktop", "low"
        float scale = 1.0f;
    };

    struct PixelArtProjectInfo {
        bool modeEnabled = false;
        bool assumeMode = false;               // read as Pixel Art Mode would see it (Scope::assumeModeOn)
        int canvasWidth = 0, canvasHeight = 0;
        Scaling scaling{};
        int pixelsPerUnit = 1;
        TextureFilterMode defaultFilterMode = TextureFilterMode::Bilinear;
        int windowWidth = 0, windowHeight = 0;
        std::vector<PixelArtScreen> screens;
        PixelUpscaleMode upscaleMode = PixelUpscaleMode::INTEGER;
        std::string defaultPalette;            // assets-relative .palette, "" none
        // Project Settings > Textures (max sizes, qualities, power-of-two strategy) the import resolves against
        TextureProjectSettings textureSettings;
        // Project Settings > Quality > Device Tiers: the Render Scale caps below 1, by group then tier
        std::vector<PixelArtRenderScaleCap> renderScaleCaps;
    };

    // A font a Text draws or `paths` names (S51's analyzeFontForPixelArt plugs in through
    // setFontAnalyzer; before it every font reads as a vector font)
    struct PixelArtFontInfo {
        std::string font;
        std::string kind = "vector";           // "vector", "bitmap"
        unsigned int size = 0;
        int nativeSize = 0;                    // bitmap fonts: their native pixel size
        float pixelScale = 0.0f;
        bool antialiased = true;
        float partialAlphaShare = -1.0f;       // < 0: not measured
        std::vector<std::string> vectorFallbacks;
        bool pixelGridDetected = false;
        bool loadError = false;
        std::string error;
    };

    struct PixelArtCheckInput {
        PixelArtProjectInfo project;
        std::vector<PixelArtScene> scenes;
        std::map<std::string, PixelArtTexture> textures;    // by assets-relative path
        std::vector<std::string> fontPaths;    // fonts `paths` names
        std::filesystem::path assetsRoot;      // absolute, to read pixels (palette rule)
        // the palettes the palette rule measures against (the Default Palette, Palette Swap
        // sources), by assets-relative path
        std::map<std::string, std::vector<jasperengine::pixelart::Rgba8>> palettes;
        // what a later spec adds (S46's cleanup outputs, S50's Tilemap Layers): its rule reads
        // its own part by name
        nlohmann::json extensions = nlohmann::json::object();
    };

    struct Scope {
        std::vector<std::string> scenes;       // project-relative scene paths or names; empty: build list + open scenes
        std::vector<std::string> paths;        // textures and fonts; empty: every pixel-art texture
        bool selectionOnly = false;            // the window's Selection scope
        // the window's Project scope: every scene file of the project (listed in the build or not)
        // and every Aseprite sheet, besides every pixel-art texture
        bool allScenes = false;
        // no scene at all, only `paths` (check --hook after images alone changed)
        bool noScenes = false;
        // `paths` that do not resolve to pixel art are left out (check --hook: a changed normal
        // map or photo is not the pixel-art rules' business)
        bool pixelArtPathsOnly = false;
        // the project as Pixel Art Mode would see it (the preset's plan before the mode is on):
        // every 2D scene and every sprite or UI texture is pixel art
        bool assumeModeOn = false;
        std::vector<std::pair<uint32_t, uint32_t>> selection;   // (scene id, entity)
    };

    // ---- the rules

    // R10's ids in order; registered rules (registerRule) appear in their table position
    const std::vector<std::string>& ruleIds();
    // a rule of this editor (built in or registered)
    bool isKnownRule(const std::string& id);

    using RuleFn = std::function<RuleResult(const PixelArtCheckInput&, std::vector<Finding>&)>;
    // A later spec's rule (S46's cleanup_outputs), checked in R10's table position; a known id
    // replaces the built-in (its tests do that)
    void registerRule(std::string id, RuleFn rule);
    void unregisterRule(const std::string& id);

    // S51's font analysis (D9: read when present); null: every font is a vector font
    using FontAnalyzer = std::function<PixelArtFontInfo(const std::filesystem::path& assetsRoot, const std::string& font, unsigned int size)>;
    void setFontAnalyzer(FontAnalyzer analyzer);

    // rules: ids to run (empty: all); unknown ids are skipped (the callers validate them)
    CheckResult run(const PixelArtCheckInput& input, const std::vector<std::string>& rules = {});

    // ---- fixes (R13)

    struct FixPlan {
        std::vector<std::pair<std::string, FixOption>> fixes;   // (finding key, chosen option)
        std::vector<std::string> unknown;      // selectors that matched no finding
        std::vector<std::string> badOptions;   // "<key>@<option>" whose option does not exist
        std::vector<std::string> needsPerson;  // keys selected that have no fix
        nlohmann::json toJson() const;         // fix_plan: [{key, option, changes}]
        size_t changeCount() const;
    };
    // selection: finding keys, F<n>, rule ids or "all", each optionally with "@<option>"
    FixPlan plan(const CheckResult& result, const std::vector<std::string>& selection);

    // ---- helpers the rules, the preset and the tests share

    // the effective rotation policy of a sprite under its scene's camera
    PixelRotation effectiveRotation(const PixelArtDrawable& drawable, const PixelArtCamera& camera);
    // a project canvas that is a pixel-art base resolution (both sides <= 640 x 360, so the
    // smallest screen checked still scales it at least 2x)
    bool isPixelArtBase(int width, int height);
    // a number as the JSON prints it (textureCanonicalNumber's rule, R14)
    nlohmann::json number(double value);

}
