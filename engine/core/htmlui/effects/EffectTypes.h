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
    enum class EffectCost : uint8_t { Low, Medium, High };
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

    struct EffectManifest {
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
        bool builtin = true;
        // builtin: "effects/<name>.frag" (the library); project: the asset-relative .frag
        std::string sourcePath;
        int line = 0;                          // the manifest's first line

        const EffectParamDef* findParam(const std::string& paramName) const;
        int paramIndex(const std::string& paramName) const;
        // the shader's custom name: "@effects/<name>" (builtin) or the .frag path (project)
        std::string customShaderName() const;
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
    // kebab-case -> camelCase ("ray-density" -> "rayDensity")
    JASPERENGINE_API std::string effectUniformName(const std::string& kebab);
    // the CSS image and filter functions an effect may not be named after
    JASPERENGINE_API bool isReservedEffectName(const std::string& name);

}

#endif //HTMLUI_EFFECTS_EFFECTTYPES_H
