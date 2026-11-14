// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md R31, §6.5; S83 §6.2-§6.5 fill the values)
// - Purpose: one declarative C++ table per effect and generator, in the engine (a Realtime
//   title in a player needs defaults, units, reach and validation, S81). One definition drives
//   the document's defaults and read-time clamping (TitleArtIO), TitleArtOps' validation and
//   did-you-mean (`fx:` paths), the Inspector rows (S82: one switch over ParamKind honouring
//   `when`, gizmo hints for the canvas), list_title_library and its generated docs page, the
//   exposed-parameter targets and Lua names (S81), the golden case list, and S85's track
//   classes. Adding an effect is one definition plus its pass code.
// - Registration: TitleEffectRegistry::get() builds the table on first use from
//   registerBuiltinTitleEffects (effects/, one file per family) and
//   registerBuiltinTitleGenerators (generators/); tests add and remove their own definitions.
//   Pointers returned stay valid until that definition is removed.
// - The pass code: `apply` / `render` run inside the renderer's job with the contexts the
//   renderer defines (render/: EffectContext, GeneratorContext); `analyse` runs on the job system
//   with S83's AnalysisContext and its result reaches `apply` through the EffectContext. The
//   registry only holds the pointers, so it compiles without the renderer.
// - Thread-safety: as TitleArtSchema (built once, registration at startup or in tests, reads
//   from any thread).

#pragma once

#include "Export.h"
#include "titleart/TitleArtDoc.h"
#include "titleart/TitleArtTypes.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::titleart {

    struct NormalizeContext;
    class TargetRef;
    struct EffectContext;           // render/: the pass context of `apply`
    struct GeneratorContext;        // render/: the pass context of `render`
    struct AnalysisContext;         // S83: the CPU analysis job's input (packed grid, layer box)
    struct AnalysisResult;          // S83: what `analyse` hands `apply` (cached by content hash)

    enum class ParamKind : uint8_t {
        Number, Integer, Bool, Enum, Color, Gradient, Curve, Vec2, Asset, Layer,
        Adjustment,     // an adjustment object (the `adjust` effect), schema kind "adjustment"
        String, Material, RowArray      // for S83 (labels, a material param, rows of params)
    };
    JASPERENGINE_API const char* paramKindName(ParamKind kind);

    struct EnumOption {
        const char* value;          // stored snake_case
        const char* label;          // Inspector label
    };

    // shown while `param` has one of `values` (a hidden param still applies)
    struct When {
        const char* param = nullptr;
        std::vector<const char*> values;
        bool active() const { return param != nullptr; }
    };

    enum class CostClass : uint8_t { C0, C1, C2, C3, C4 };      // S83 §6.4: trivial .. bake only
    enum class FallbackClass : uint8_t { F0, F1, F2 };          // S83 §6.5: RGBA8 agreement class
    enum class LiveKind : uint8_t { None, ScaleSplit, Kernel }; // S83 D10; run by S81's live compose
    enum class FxTier : uint8_t { Desktop, Mobile };
    enum class AnalysisSource : uint8_t { None, Content, Styled };   // what S83's analysis reads
    enum class FollowMode : uint8_t { None, Optional, Required };     // a generator's `follow` layer
    // how an angle param relates to the document's global light G (Photoshop "Use Global Light",
    // Pachislop's lit()): From = the light direction itself (bevels), Away = the direction a flat
    // shadow falls, opposite the light (G 120 -> falls down-right), Floor = a floor cast shadow
    // receding up the screen, Away mirrored vertically (G 120 -> up-right)
    enum class LightRelation : uint8_t { From, Away, Floor };
    // the convention of an angle param: clockwise on screen from +x, or Photoshop's light angle
    // (counter-clockwise from +x, the direction the light comes from)
    enum class AngleConvention : uint8_t { ClockwiseFromRight, CounterClockwiseFromRight };

    JASPERENGINE_API const char* costClassName(CostClass cost);        // "C0" .. "C4"
    JASPERENGINE_API const char* fallbackClassName(FallbackClass fallback);
    JASPERENGINE_API const char* liveKindName(LiveKind kind);          // "none", "scale_split", "kernel"
    JASPERENGINE_API const char* followModeName(FollowMode mode);      // "none", "optional", "required"

    // A canvas gizmo S82 draws for a definition: a point (a Vec2 param), a direction (an angle
    // param, optionally its length), a radius (a size param) or a light (angle + altitude)
    enum class GizmoKind : uint8_t { Point, Direction, Radius, Light };
    struct GizmoHint {
        GizmoKind kind = GizmoKind::Direction;
        const char* param = nullptr;        // Point: the Vec2; Direction / Light: the angle; Radius: the size
        const char* param2 = nullptr;       // Direction: the length (optional); Light: the altitude
    };

    struct ParamDef {
        const char* key = "";           // snake_case: the stored key, the MCP path segment, the Lua name
        const char* label = "";         // Inspector label, Unity-like ("Size", "Use Global Light")
        ParamKind kind = ParamKind::Number;
        ParamUnit unit = ParamUnit::None;
        double min = 0, max = 0, step = 0;      // min == max: unbounded
        Json defaultValue;
        std::vector<EnumOption> options;
        When when;
        const char* description = "";   // one line, for docs, tooltips and list_title_library
        FxAnim anim = FxAnim::Rerender;             // S85's class; S83's tables mark it L/R/S/X
        ParamInterp interp = ParamInterp::Smooth;   // Stepped for enums, bools, ints, seeds
        ParamCombine combine = ParamCombine::Replace;
        bool light = false;                         // a colour of light: S84's schemes map it onto light_ramp
        std::vector<ParamDef> rows;                 // RowArray: the params of one row
        size_t maxRows = 0;                         // RowArray: 0 = no limit
    };

    // What a pass reads besides its input: the layer content without effects, its distance
    // field, S83's analysis
    struct FxInputs {
        bool input = true, content = false, sdf = false;
        AnalysisSource analysis = AnalysisSource::None;
    };
    struct LiveVariant {
        LiveKind kind = LiveKind::None;
        std::vector<const char*> params;        // the params the live kernel animates
        uint32_t program = 0;                   // the TA_PROGRAM_* compiled with TFX_LIVE
    };
    // S84's spectrum schemes: the param that switches, its value outside and inside a spectrum
    struct SpectrumSwitch {
        const char* param = nullptr;
        const char* from = nullptr;
        const char* to = nullptr;
    };
    // the static cost model of S83 §6.4 at one size and tier
    struct FxCost {
        int passes = 0;
        double fre = 0;                 // full-resolution equivalents of fragment work
        int maxTapsPerPixel = 0;
        int instances = 0;
        double cpuMs = 0;
        bool sdf = false, analysis = false, readback = false;
    };
    // the size a cost is estimated at: the layer's region in output pixels
    struct FxSize {
        int width = 0, height = 0;
        float pxScale = 1.0f;
    };
    // the binding withGlobalLight records
    struct GlobalLightBinding {
        const char* angleKey = nullptr;         // null: the definition does not follow the global light
        const char* altitudeKey = nullptr;      // optional elevation param fed by global_light.altitude
        LightRelation relation = LightRelation::From;
        AngleConvention convention = AngleConvention::CounterClockwiseFromRight;
    };

    struct EffectDef;
    struct GeneratorDef;

    // Typed reads of one effect's or generator's params (normalized: every param present). With
    // a GlobalLightBinding and `use_global_light` on, the bound angle (and altitude) read from the
    // document's global light, Pachislop's withGlobalLight.
    class JASPERENGINE_API ParamValues {
    public:
        ParamValues() = default;
        ParamValues(const Json& params, const GlobalLightBinding* binding = nullptr, double lightAngle = 120.0, double lightAltitude = 30.0);

        const Json& json() const;
        bool has(const char* key) const;
        double number(const char* key, double fallback = 0.0) const;
        int integer(const char* key, int fallback = 0) const;
        bool flag(const char* key, bool fallback = false) const;
        std::string string(const char* key) const;
        TitleColor color(const char* key) const;
        TitleVec2 vec2(const char* key) const;
        TitleGradient gradient(const char* key) const;
        TitleCurve curve(const char* key) const;
        TitleAssetRef asset(const char* key) const;
        const Json& raw(const char* key) const;

    private:
        const Json* params = nullptr;
        const GlobalLightBinding* light = nullptr;
        double lightAngle = 120.0, lightAltitude = 30.0;
    };

    // The angle a bound param takes for global light angle `g` (degrees CCW, comes from), in the
    // param's convention, wrapped to (-180, 180]
    JASPERENGINE_API double angleFromGlobalLight(const GlobalLightBinding& binding, double g);

    struct EffectDef {
        const char* type = "";          // registry id, snake_case ("outer_glow")
        const char* label = "";         // UI name ("Outer Glow")
        const char* category = "";      // Add Effect menu group ("Glow", "Shadow & Depth")
        const char* description = "";
        std::vector<ParamDef> params;
        std::vector<GizmoHint> gizmos;
        float (*reach)(const ParamValues&) = nullptr;       // canvas px it may draw outside the ink; null = 0
        float (*gather)(const ParamValues&) = nullptr;      // how far each output pixel reads; < 0 = the whole layer
        bool (*fillsCanvas)(const ParamValues&) = nullptr;  // null = false
        bool (*addsLight)(const ParamValues&) = nullptr;    // null = false
        // its passes read the input's light above 1 (a threshold, an exposure): on the RGBA8 path a
        // group's light reaches it in S83 D14's encoding (EffectContext::inputLight)
        bool readsLight = false;
        bool outline = false;                       // traces ink (draws under light, §6.4)
        bool onParts = true;                        // allowed in part_effects
        // its output's alpha is a function of its input's alpha and its params alone (a glow, a
        // shadow): the effect after it may keep what it computes from that alpha (R42)
        bool alphaFromAlpha = false;
        CostClass costClass = CostClass::C1;        // at defaults; list_title_library's `cost`
        FallbackClass fallback = FallbackClass::F0;
        LiveVariant live;                           // S81's live compose runs it (never title_fx)
        GlyphSafety glyphSafety = GlyphSafety::Approximate;
        std::vector<const char*> aliases;           // searched by menus, search_editor, list_title_library
        FxInputs inputs;
        SpectrumSwitch spectrum;
        GlobalLightBinding globalLight;
        FxCost (*estimate)(const ParamValues&, const FxSize&, FxTier) = nullptr;   // null = one pass of
                                                    // costWeight 1 per output pixel
        void (*analyse)(AnalysisContext&, const ParamValues&, AnalysisResult&) = nullptr;
        const char* presetFile = nullptr;           // editor/resources/titleart/fx-presets/<type>.yaml (S83)
        uint32_t passVersion = 1;                   // bumped with its pass code; part of the bake version
        void (*apply)(EffectContext&, const TargetRef& input, TargetRef& output, const ParamValues&) = nullptr;
    };

    struct GeneratorDef {
        const char* type = "";
        const char* label = "";
        const char* category = "";
        const char* description = "";
        std::vector<ParamDef> params;
        std::vector<GizmoHint> gizmos;
        TitleRect (*bounds)(const ParamValues&) = nullptr;     // document space; empty or null = the canvas
        FollowMode follow = FollowMode::None;
        float (*followReach)(const ParamValues&) = nullptr;    // how far past the followed layer it draws
        bool (*addsLight)(const ParamValues&) = nullptr;
        // its output can hold its light in S83 D14's encoding (GeneratorContext::lightScale)
        bool encodesLight = false;
        CostClass costClass = CostClass::C0;
        FallbackClass fallback = FallbackClass::F0;
        LiveVariant live;
        GlyphSafety glyphSafety = GlyphSafety::Unsafe;
        std::vector<const char*> aliases;
        FxInputs inputs;
        SpectrumSwitch spectrum;
        GlobalLightBinding globalLight;
        FxCost (*estimate)(const ParamValues&, const FxSize&, FxTier) = nullptr;
        void (*analyse)(AnalysisContext&, const ParamValues&, AnalysisResult&) = nullptr;
        const char* presetFile = nullptr;
        uint32_t passVersion = 1;
        void (*render)(GeneratorContext&, TargetRef& target, const ParamValues&) = nullptr;
    };

    class JASPERENGINE_API TitleEffectRegistry {
    public:
        // the table, built on first use from the built-in definitions
        static TitleEffectRegistry& get();

        // false when a definition of that type exists (or the type is empty)
        bool add(EffectDef def);
        bool add(GeneratorDef def);
        // tests
        bool removeEffect(std::string_view type);
        bool removeGenerator(std::string_view type);

        const EffectDef* effect(std::string_view type) const;
        const GeneratorDef* generator(std::string_view type) const;
        // registration order (the Add Effect menu sorts by category and label itself)
        std::vector<const EffectDef*> effects() const;
        std::vector<const GeneratorDef*> generators() const;
        // bumps on every change (caches of list_title_library and the docs)
        uint64_t revision() const { return changes; }

    private:
        TitleEffectRegistry() = default;
        TitleEffectRegistry(const TitleEffectRegistry&) = delete;
        TitleEffectRegistry& operator=(const TitleEffectRegistry&) = delete;
        std::vector<std::unique_ptr<EffectDef>> effectDefs;
        std::vector<std::unique_ptr<GeneratorDef>> generatorDefs;
        uint64_t changes = 0;
    };

    JASPERENGINE_API const EffectDef* findEffect(std::string_view type);
    JASPERENGINE_API const GeneratorDef* findGenerator(std::string_view type);

    // The Photoshop "Use Global Light" decorator (Pachislop's lit()): adds a Bool param
    // `use_global_light` (default true) after `angleKey` and records the binding, so ParamValues
    // reads that angle from the document's global light while it is on. The convention defaults
    // to the relation's: From is a light angle (counter-clockwise), Away and Floor a direction
    // something falls (clockwise).
    JASPERENGINE_API EffectDef withGlobalLight(EffectDef def, const char* angleKey, LightRelation relation,
                                               const char* altitudeKey = nullptr);
    JASPERENGINE_API GeneratorDef withGlobalLight(GeneratorDef def, const char* angleKey, LightRelation relation,
                                                  const char* altitudeKey = nullptr);

    // The built-in definitions, one file per family under effects/ and generators/
    void registerBuiltinTitleEffects(TitleEffectRegistry& registry);
    void registerBuiltinTitleGenerators(TitleEffectRegistry& registry);

    // ---- params in documents ----

    JASPERENGINE_API const ParamDef* findParam(const std::vector<ParamDef>& params, std::string_view key);
    // every param at its default, keys sorted (the canonical order of `params`)
    JASPERENGINE_API Json defaultParams(const std::vector<ParamDef>& params);
    // a stored params object made canonical: every param present (missing ones at their default),
    // each checked and clamped (warnings with JSON paths under `path`), unknown keys kept, keys
    // sorted
    JASPERENGINE_API void normalizeParams(const std::vector<ParamDef>& params, Json& values, const std::string& path, NormalizeContext& context);
    // one value checked against its definition: true when it is valid (possibly clamped, with a
    // warning); false when it cannot be (the caller keeps the default)
    JASPERENGINE_API bool normalizeParamValue(const ParamDef& param, Json& value, const std::string& path, NormalizeContext& context);

    // TITLE_ART_BAKE_VERSION of a document (§6.9): the base plus the passVersion of every
    // effect and generator type it uses (each type once)
    JASPERENGINE_API uint32_t titleArtBakeVersion(const TitleArtDoc& doc);

}
