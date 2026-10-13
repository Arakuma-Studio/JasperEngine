// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SCENESETTINGS_H
#define SCENESETTINGS_H

#include "render/Render.h"
#include "shader/ShaderUniforms.h"
#include "texture/Texture.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include <string>
#include <vector>

namespace jasperengine{

    enum class LightState {
        OFF,
        ON,
        AUTO
    };

    // PCF filter quality of shadow edges, uniform-driven (no shader rebuild).
    // 3D shadows (shadowQuality): NONE = 1 tap, LOW = 3x3, MEDIUM = 5x5, HIGH = 7x7.
    // 2D shadows (shadow2DQuality): NONE = 1 tap, LOW = 5, MEDIUM = 9, HIGH = 13 taps
    // along the 1D polar map, smoothing the penumbra set by Light2D shadowSoftness.
    enum class ShadowQuality {
        NONE,   // 1 tap, no filtering (hard edges)
        LOW,
        MEDIUM,
        HIGH
    };

    // Resolution of the scene-colour copy sampled by shaders requiring scenecolor.
    enum class SceneColorScale {
        FULL,   // the camera pass's resolution
        HALF    // half of it (cheaper; mobile)
    };

    // Tone mapping curve of the post-processing colour (PostProcessingSettings::tonemap), baked
    // with the exposure and the colour grade into the colour LUT
    enum class TonemapMode : int {
        NONE,       // clamp: today's output
        ACES,       // Narkowicz's ACES fitted curve
        NEUTRAL,    // Khronos PBR Neutral (base colours stay as authored)
        REINHARD,   // c / (1 + c)
        AGX         // AgX approximation (minimal AgX, as Filament and three.js)
    };

    // Grayscale or sepia (PostProcessingSettings::monochromeMode), a colour transform baked
    // into the colour LUT
    enum class MonochromeMode : int {
        GRAYSCALE,
        SEPIA
    };

    // Fast post-processing of the main camera (shader library phase 4). Every effect has an
    // enable flag (or a neutral value: exposure 0, tonemap NONE, render scale 1, MSAA 1) and
    // costs nothing while off; a default-constructed struct draws exactly what the engine drew
    // before it existed. Tiers (docs/superpowers/specs/2026-10-10-shader-library-phase4-design.md):
    // T0 (exposure, tonemap, grade, monochrome, vignette, dither) runs at the end of every mesh,
    // library, sky, points and lines shader (no pass); T1 (FXAA, chromatic aberration, pixelate,
    // CRT, edge detect, film grain, sharpen, render scale) adds one fullscreen pass, the uber
    // pass, which then takes the grade, vignette and dither too; T2 (bloom, blur) adds a small
    // quarter-resolution chain composited by the uber pass. Colours are sRGB (as the colour
    // pickers show them). RenderSystem::getPostProcessingTiers tells what a set turns on.
    struct PostProcessingSettings {
        // colour (T0): exposure in stops (0 = none), then the tone mapping curve
        float exposure = 0.0f;
        TonemapMode tonemap = TonemapMode::NONE;

        // colour grade after the tone mapping, in this order: white balance (temperature, tint:
        // -1..1, on the linear colour), then on the display-encoded colour contrast around mid
        // grey, lift / gamma / gain per channel (lift adds to the shadows, gain scales, gamma
        // bends the midtones), saturation, and the LUT strip (lutTexture: a project image laid
        // out N*N x N, Unity's layout with green 0 at the bottom row, display colour in and out)
        // mixed in by lutContribution
        bool colorGradingEnabled = false;
        float temperature = 0.0f;
        float tint = 0.0f;
        float contrast = 1.0f;
        float saturation = 1.0f;
        Vector3 lift = Vector3(0.0f, 0.0f, 0.0f);
        Vector3 gamma = Vector3(1.0f, 1.0f, 1.0f);
        Vector3 gain = Vector3(1.0f, 1.0f, 1.0f);
        std::string lutTexture;
        float lutContribution = 1.0f;
        // size of the engine's colour LUT (16 or 32 per side): 16 is lighter on low-end GPUs and
        // bands under strong grades
        int lutSize = 32;

        // grayscale / sepia, mixed in by intensity (a colour transform: baked into the LUT, so a
        // T0 effect although it reads like a filter)
        bool monochromeEnabled = false;
        MonochromeMode monochromeMode = MonochromeMode::GRAYSCALE;
        float monochromeIntensity = 1.0f;

        // vignette: darkens (towards vignetteColor) away from the centre. intensity 0..1, the
        // amount at the corners; smoothness 0..1, the falloff width; roundness 0..1, 1 a circle
        // whatever the aspect, 0 follows the screen's shape
        bool vignetteEnabled = false;
        float vignetteIntensity = 0.35f;
        float vignetteSmoothness = 0.5f;
        float vignetteRoundness = 1.0f;
        Vector3 vignetteColor = Vector3(0.0f, 0.0f, 0.0f);

        // +-0.5/255 noise before the 8-bit write, against banding in gradients
        bool ditherEnabled = false;

        // T1 (the uber pass)
        bool fxaaEnabled = false;                   // FXAA 3.11 console quality, luma from green

        bool chromaticAberrationEnabled = false;
        float chromaticAberrationIntensity = 0.5f;  // 0..1, red and blue shift at the screen edge

        bool pixelateEnabled = false;
        float pixelateSize = 4.0f;                  // block size in output pixels

        bool crtEnabled = false;
        float crtScanlines = 0.5f;                  // 0..1 scanline darkening
        float crtMask = 0.3f;                       // 0..1 aperture-grille mask
        float crtCurvature = 0.2f;                  // 0..1 barrel curvature of the screen
        float crtVignette = 0.3f;                   // 0..1 darkening of the tube's edges

        bool edgeDetectEnabled = false;             // Sobel on luma, aspect-correct
        float edgeDetectThreshold = 0.1f;           // 0..1 luma gradient where edges start
        float edgeDetectIntensity = 1.0f;           // 0..1 mix of the edge colour
        Vector3 edgeDetectColor = Vector3(0.0f, 0.0f, 0.0f);

        bool filmGrainEnabled = false;
        float filmGrainIntensity = 0.2f;            // 0..1
        float filmGrainResponse = 0.8f;             // 0..1, less grain in bright areas

        bool sharpenEnabled = false;
        float sharpenIntensity = 0.5f;              // 0..1

        // T2: dual-filter bloom from quarter resolution, composited by the uber pass. threshold
        // and knee on the display colour (0..1), intensity, tint, scatter 0..1 (radius)
        bool bloomEnabled = false;
        float bloomThreshold = 0.8f;
        float bloomKnee = 0.5f;
        float bloomIntensity = 1.0f;
        Vector3 bloomTint = Vector3(1.0f, 1.0f, 1.0f);
        float bloomScatter = 0.7f;

        // T2: the same chain without threshold: a blurred copy of the scene (UI backgrounds can
        // sample it), shown on screen by blurIntensity (0 = only the texture, 1 = full screen blur)
        bool blurEnabled = false;
        float blurRadius = 1.0f;
        float blurIntensity = 1.0f;

        // resolution of the 3D scene, 0.5..1 of the output, upscaled by the uber pass (the UI
        // stays at the output's resolution)
        float renderScale = 1.0f;
        // multisampling of the main camera's scene target (1, 2 or 4, clamped to what the device
        // has): resolved at the end of the scene pass into the target the effects read; with
        // nothing else offscreen an exported game blits that target to the screen (one pass)
        int msaaSamples = 1;

        bool operator==(const PostProcessingSettings& o) const;
        bool operator!=(const PostProcessingSettings& o) const { return !(*this == o); }

        // the field table (name, type, accessor) every consumer iterates: serialization,
        // exported scene code, the editor's Catalog paths (post_processing.<snake name>) and MCP
        enum class FieldType { Bool, Int, Float, Vector3, Enum, String };
        struct Field {
            const char* name;       // camelCase, the member and YAML key
            const char* snakeName;  // snake_case, the Catalog path after "post_processing."
            FieldType type;
            void* (*ref)(PostProcessingSettings& settings);
            const char* const* enumNames;   // Enum: lower-case value names, null-terminated
            const char* enumType;           // Enum: the C++ type (TonemapMode)
            const char* const* enumValues;  // Enum: the C++ value names (NONE, ACES...)
            float minValue;                 // Int / Float: the valid range
            float maxValue;
            const char* help;
        };
        static const std::vector<Field>& getFields();
        static const Field* findField(const std::string& name); // camelCase or snake_case
        const void* getFieldRef(const Field& field) const { return field.ref(const_cast<PostProcessingSettings&>(*this)); }
        // clamps the Int / Float fields to their ranges (lutSize to 16 or 32, msaaSamples to 1, 2, 4)
        void clamp();
    };

    // One user post-process pass (tier T3 of phase 4): a fullscreen shader run after the scene
    // colour (and SSR), before the engine's uber pass. It reads the previous pass's colour
    // (u_sceneColorTexture), and may declare u_originalSceneTexture (the scene before the chain),
    // u_depthTexture, u_gbufferTexture, u_ssaoTexture and its own textures. Uniforms are keyed by
    // the name reflected from the shader's u_fs_postParams block, so a renamed or removed member
    // just drops and an unset one takes the manifest's default (zero without one).
    struct PostProcessPass {
        // a project fork base path, "library:<name>" (a library post-process shader,
        // @shader postprocess), or empty: the built-in passthrough
        std::string shader;
        bool enabled = true;
        ShaderUniformValues uniforms;
        // the shader's textures by uniform (its manifest's @texture, or any it declares when it
        // has no manifest); one it declares and has none of binds the manifest default (white)
        std::vector<std::pair<std::string, Texture>> textures;
        // the pass's output resolution, a fraction of the chain's (0.1..1): the next pass (or a
        // copy to the destination, after the last) samples it bilinearly
        float scale = 1.0f;

        // applied by Scene::setPostProcessPasses (Scene::setPostProcessUniform edits a live pass)
        void setUniform(const std::string& name, const Vector4& value) { ShaderUniforms::set(uniforms, name, value); }
        void setUniform(const std::string& name, const Vector3& value) { setUniform(name, Vector4(value.x, value.y, value.z, 0.0f)); }
        void setUniform(const std::string& name, const Vector2& value) { setUniform(name, Vector4(value.x, value.y, 0.0f, 0.0f)); }
        void setUniform(const std::string& name, float value) { setUniform(name, Vector4(value, 0.0f, 0.0f, 0.0f)); }
        Vector4 getUniform(const std::string& name) const { return ShaderUniforms::get(uniforms, name); }
        bool removeUniform(const std::string& name) { return ShaderUniforms::remove(uniforms, name); }
        // a texture by uniform from a project image path (empty: removed, the default binds)
        bool setTexture(const std::string& uniform, const std::string& path);
        std::string getTexture(const std::string& uniform) const;
        // the scale as the engine applies it (0.1..1)
        float getAppliedScale() const { return scale < 0.1f ? 0.1f : (scale > 1.0f ? 1.0f : scale); }
    };

    // Plain-data scene configuration whose member initializers are the engine's factory
    // defaults. This is the single source of truth for scene-setting defaults: Scene embeds
    // one and delegates its getters/setters to it (keeping side effects and color-space
    // conversions in Scene), and the editor reads a default-constructed SceneSettings to show
    // and reset "default" values without building a full Scene (which owns a physics runtime).
    struct SceneSettings {
        Vector4 backgroundColor = Vector4(0.0, 0.0, 0.0, 1.0); // sRGB
        ShadowQuality shadowQuality = ShadowQuality::LOW;

        // off builds no levels at all; a detail level is drawn once its error projects below this many pixels
        bool meshLodEnabled = false;
        float meshLodThreshold = 1.0f;

        // depth-only pass before the opaque draws; pays off with heavy overdraw
        bool depthPrepassEnabled = false;

        // resolution of the scene-colour copy that shaders requiring scenecolor sample (shader
        // library phase 3): FULL = the camera pass's resolution, HALF = half of it (default; mobile)
        SceneColorScale sceneColorScale = SceneColorScale::HALF;

        LightState lightState = LightState::AUTO;
        Vector3 globalIllumColor = Vector3(1.0, 1.0, 1.0);    // stored linear
        float globalIllumIntensity = 0.1f;

        // lights evaluated per object (1..16); the rest of the lights reaching it are folded
        // into its spherical-harmonics ambient
        int maxLightsPerObject = 8;

        // default 1.0 keeps 2D scenes visually unchanged until the user dims it
        Vector3 ambientLight2DColor = Vector3(1.0, 1.0, 1.0); // stored linear
        float ambientLight2DIntensity = 1.0f;
        ShadowQuality shadow2DQuality = ShadowQuality::LOW;

        bool ssaoEnabled = false;
        float ssaoRadius = 0.5f;
        float ssaoIntensity = 1.0f;
        float ssaoBias = 0.025f;
        bool ssaoDebug = false;

        bool ssrEnabled = false;
        float ssrMaxDistance = 8.0f;   // max ray length in view-space units
        float ssrThickness = 0.5f;     // depth-compare tolerance (view-space units)
        int ssrMaxSteps = 48;          // linear march sample count
        float ssrIntensity = 1.0f;     // reflection strength multiplier
        float ssrBlur = 0.0f;          // glossy blur amount [0..1] (0 = sharp/mirror)
        int ssrDebugMode = 0;          // 0=off,1=reflection,2=normal,3=roughness,4=metallic,5=albedo,6=IBL specular

        // Initial physics gravity. The live value is owned by PhysicsSystem (it must be pushed
        // to the Box2D/Jolt worlds on change); this is the default it initializes from.
        Vector2 gravity2D = Vector2(0.0f, -9.81f);
        Vector3 gravity3D = Vector3(0.0f, -9.81f, 0.0f);

        // fixed game resolution: when enabled and this scene is the Engine main scene, the main
        // camera renders into an offscreen framebuffer of this size, upscaled to the view rect
        // by RenderSystem
        bool fixedResolutionEnabled = false;
        unsigned int fixedResolutionWidth = 640;
        unsigned int fixedResolutionHeight = 360;
        TextureFilter fixedResolutionFilter = TextureFilter::NEAREST;

        // scene-wide custom shaders, used when a component's customShader is empty
        // (project-relative base or "a.vert|b.frag"; empty = engine built-in)
        std::string defaultMeshShader;
        std::string defaultUIShader;
        std::string defaultSkyShader;
        std::string defaultPointsShader;
        std::string defaultLinesShader;

        // ordered user post-process chain (main camera only), applied after SSR
        std::vector<PostProcessPass> postProcess;

        // the engine's fast post-processing (main camera only; phase 4)
        PostProcessingSettings postProcessing;
    };

}

#endif //SCENESETTINGS_H
