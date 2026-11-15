// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUI_EFFECTS_EFFECTTYPES_H
#define HTMLUI_EFFECTS_EFFECTTYPES_H

// The UI effects library (phase 5, docs/html-ui/phase-5-effects.md): what an effect is. An
// effect is one fragment file (engine/shaderlib/effects/<name>.frag, or a project's .frag
// declared with @effect) whose leading comment block is its manifest (3.9): kind, category,
// parameters with types, defaults and ranges, and how the engine drives it.

#include "Export.h"
#include "htmlui/compat/Diagnostics.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::htmlui {

    // paint: an image for the element's box (background-image, mask-image; in filter and
    // backdrop-filter it recolours the content inside its alpha). filter: processes pixels.
    enum class EffectKind : uint8_t { Paint, Filter };
    enum class EffectParamType : uint8_t { Number, Integer, Percentage, Length, Angle, Time, Color, Position, Enum, Boolean, Palette };
    // stateful: an effect with @state (phase 6), measured per frame of its passes plus its draw
    enum class EffectCost : uint8_t { Low, Medium, High, Stateful };
    enum class EffectPointer : uint8_t { None, Position, Trail, Ripples };

    // "size*0.5 + 2px": a length of the effect from one of its parameters (CSS px); param empty =
    // the constant addPx
    struct EffectScaledParam {
        std::string param;
        float scale = 1.f;
        float addPx = 0.f;
    };

    struct EffectParamDef {
        std::string name;            // kebab-case CSS name
        std::string uniform;         // camelCase member of u_fs_effectParams
        EffectParamType type = EffectParamType::Number;
        std::string defaultText;     // the manifest's default (CSS text)
        float minValue = 0, maxValue = 0;   // in normalised units (rad, s, px, 0-1...)
        bool hasRange = false;
        std::vector<std::string> options;   // enum
        std::string description;
        int line = 0;                // manifest line
    };

    // Persistent per-element state (phase 6, docs/html-ui/phase-6-stateful-effects.md 3.2-3.3).
    // Appended only: a value is its bit in EffectStatePool::setUnsupportedFormatsForTesting.
    // RG16F and R16F: half-float two- and one-channel images (velocity, pressure fields)
    enum class EffectStateFormat : uint8_t { RGBA8, RGBA16F, RGBA32F, R32F, RG16F, R16F };

    // @state <name> <format> <w>x<h>|box[*<scale>] [max <px>]|aspect <short> [max <px>] [linear|nearest] [clamp|repeat] [clear|resample]
    struct EffectStateDef {
        std::string name;
        EffectStateFormat format = EffectStateFormat::RGBA16F;
        bool fixedSize = false;      // <w>x<h>
        int width = 1, height = 1;   // fixed size
        float boxScale = 1.f;        // box: the area's CSS px x scale
        int boxMax = 1024;           // box, aspect: the larger side at most this (the aspect kept)
        // aspect <short>: the area's aspect, the shorter side `aspectShort` texels whatever the
        // area's size in px (a grid whose cells stay square: Pavel's getResolution)
        int aspectShort = 0;
        bool linear = true;          // the sampler's filter
        bool repeat = false;         // the sampler's wrap
        bool resample = true;        // box states on a resize: redrawn into the new size (false: restart)
        bool single = false;         // one image (every pass writing it is additive), else a ping-pong pair
        int line = 0;
    };

    // @init <pass> <state> | @step <pass> <state> [x<count>|x<param>] [add] [scatter <state>] [if <param>]
    // | @frame <pass> <state> [x<count>|x<param>] [add] [scatter <state>] [if <param>]
    struct EffectPassDef {
        std::string name;            // JFX_PASS_<NAME> = its index in EffectManifest::passes
        bool init = false;           // @init: once after allocation and every reset
        // @frame: once a drawn frame after that frame's steps, when the state changed (a step ran,
        // a reset, a finished replay): what only the draw reads (a bloom chain, light shafts)
        bool frame = false;
        int state = -1;              // the target, an index in EffectManifest::states
        int repeat = 1;              // x<count>
        std::string repeatParam;     // x<param> (rounded, clamped to [1, 64])
        bool additive = false;       // add: blended ONE, ONE into the cleared target
        int scatterState = -1;       // scatter <state>: one quad per texel of that placement state
        std::string ifParam;         // if <param>: run only while that boolean or number parameter is not 0
        int line = 0;
    };

    // @preset <option> <param> <value>[, <param> <value>...]: the values the option of the enum
    // parameter `preset` gives the parameters an author leaves at their defaults
    struct EffectPresetDef {
        std::string option;
        std::vector<std::pair<std::string, std::string>> values;   // parameter name, CSS value text
        int line = 0;
    };

    struct JASPERENGINE_API EffectManifest {
        std::string name, summary, source, category;
        EffectKind kind = EffectKind::Paint;
        EffectCost cost = EffectCost::Low;
        bool backdrop = false;
        std::vector<std::string> timeParams;   // @time, at most 4 rate parameters
        EffectPointer pointer = EffectPointer::None;
        bool hasPrepass = false;               // @prepass blur <sigma> [threshold <param>]
        EffectScaledParam prepassSigma;
        std::string prepassThreshold;
        EffectScaledParam margin;              // @margin (filter)
        bool boundsGeometry = false;           // @geometry bounds <extent> (paint)
        EffectScaledParam boundsExtent;
        std::vector<std::string> suggestFor;
        std::vector<EffectParamDef> params;
        // phase 6: persistent state and its passes (declaration order); empty for a stateless effect
        std::vector<EffectStateDef> states;
        std::vector<EffectPassDef> passes;
        int warmupSteps = 0;                   // @warmup <steps> <dt>
        float warmupDt = 0.f;
        float rateHz = 60.f;                   // @rate <hz> [max <substeps>]
        int maxSubsteps = 4;
        float replaySeconds = 2.f;             // @replay <seconds>
        std::vector<std::string> reseedParams; // @reseed <param>[, <param>]: a change restarts the simulation
        std::vector<EffectPresetDef> presets;  // @preset (the enum parameter `preset`'s options)
        bool builtin = true;
        // builtin: "effects/<name>.frag" (the library); project: the asset-relative .frag
        std::string sourcePath;
        int line = 0;                          // the manifest's first line

        const EffectParamDef* findParam(const std::string& paramName) const;
        int paramIndex(const std::string& paramName) const;
        // the shader's custom name: "@effects/<name>" (builtin) or the .frag path (project)
        std::string customShaderName() const;
        // phase 6
        bool stateful() const { return !states.empty(); }
        bool hasScatter() const;
        bool hasFramePasses() const;
        const EffectPresetDef* findPreset(const std::string& option) const;
        int stateIndex(const std::string& stateName) const;
        // a hash of the states and passes: a simulation restarts when it changes (hot reload)
        uint64_t stateSignature() const;
    };

    // Phase 2's diagnostic (code, severity, message, suggestion, file, line, column). The
    // effect parsers report positions inside the text they read: column is the 1-based byte
    // offset in it (line 0), length its byte length; the callers map them to the source.
    using EffectDiagnostic = compat::Diagnostic;

    JASPERENGINE_API EffectDiagnostic makeEffectDiagnostic(compat::DiagCode code, std::string message, std::string suggestion = std::string(),
                                                           size_t offset = 0, size_t length = 0);
    JASPERENGINE_API EffectDiagnostic makeEffectDiagnostic(compat::Severity severity, compat::DiagCode code, std::string message,
                                                           std::string suggestion = std::string(), size_t offset = 0, size_t length = 0);

    JASPERENGINE_API const char* effectKindName(EffectKind kind);            // "paint", "filter"
    JASPERENGINE_API const char* effectParamTypeName(EffectParamType type);  // "number", "length", ...
    JASPERENGINE_API const char* effectCostName(EffectCost cost);            // "low", "medium", "high"
    JASPERENGINE_API const char* effectPointerName(EffectPointer pointer);   // "none", "position", ...
    JASPERENGINE_API const char* effectStateFormatName(EffectStateFormat format);   // "rgba8", "rgba16f", "rgba32f", "r32f"
    JASPERENGINE_API int effectStateFormatBytes(EffectStateFormat format);         // bytes per texel
    // JFX_PASS_<NAME>: the pass name upper case, '-' as '_'
    JASPERENGINE_API std::string effectPassMacro(const std::string& passName);
    // kebab-case -> camelCase ("ray-density" -> "rayDensity")
    JASPERENGINE_API std::string effectUniformName(const std::string& kebab);
    // the CSS image and filter functions an effect may not be named after
    JASPERENGINE_API bool isReservedEffectName(const std::string& name);

}

#endif //HTMLUI_EFFECTS_EFFECTTYPES_H
