// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PARTICLES_PARTICLEEFFECTCODEC_H
#define PARTICLES_PARTICLEEFFECTCODEC_H

// JSON of particle effects (`.particles` files and inline effects, schema version 1,
// docs/specs/S20-particles.md R1): read, write, validate and merge-patch, all through the
// module field tables (ParticleModules). Writing is canonical (fixed key order, floats in
// their shortest round-trip form, defaults left out), so read -> write gives back the same
// bytes. Paths in diagnostics are field paths: "emission.bursts[0].probability", "duration".

#include "Export.h"
#include "particles/ParticleEffect.h"

#include "json.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace jasperengine { namespace particles {

    class JASPERENGINE_API ParticleEffectCodec {
    public:
        // Parses `text` into `document` (starting from defaults). Syntax and type errors and
        // unknown keys are reported; returns false on any error. Validation runs too.
        static bool read(std::string_view text, ParticleEffectDocument& document, std::vector<Diagnostic>& diagnostics);
        static bool fromJson(const nlohmann::ordered_json& json, ParticleEffectDocument& document, std::vector<Diagnostic>& diagnostics);

        // Canonical JSON (2-space indent, trailing newline)
        static std::string write(const ParticleEffectDocument& document);
        static nlohmann::ordered_json toJson(const ParticleEffectDocument& document);

        // The document's semantic checks (ranges, counts, references between fields). Appends
        // to diagnostics; true when no error was found (warnings allowed).
        static bool validate(const ParticleEffectDocument& document, std::vector<Diagnostic>& diagnostics);

        // The "keyed" form MCP patches use: like toJson, with "modules" an object keyed by
        // module ("modules": {"emission": {"rateOverTime": 20}}) and every present module listed
        static nlohmann::ordered_json toKeyedJson(const ParticleEffectDocument& document);
        // Applies a JSON merge patch (RFC 7386) to the keyed form of `document`; on success the
        // result replaces it. A module set to null is removed (hidden); a module object adds it.
        // A key the format does not have is an error here (reading a file only warns about it).
        static bool applyMergePatch(ParticleEffectDocument& document, const nlohmann::ordered_json& patch, std::vector<Diagnostic>& diagnostics);

        // One MinMaxCurve / MinMaxGradient value in the effect's field form (a plain constant is a
        // number, else {mode, ...} with the members its mode uses): the editor's fields and MCP
        // (docs/specs/S27-editor-tools.md R41)
        static nlohmann::ordered_json minMaxCurveToJson(const MinMaxCurve& value);
        static bool minMaxCurveFromJson(const nlohmann::ordered_json& json, MinMaxCurve& value, std::vector<Diagnostic>& diagnostics);
        static nlohmann::ordered_json minMaxGradientToJson(const MinMaxGradient& value);
        static bool minMaxGradientFromJson(const nlohmann::ordered_json& json, MinMaxGradient& value, std::vector<Diagnostic>& diagnostics);

        // A float as the double its shortest round-trip decimal names, so JSON shows 0.1, not
        // 0.10000000149011612, and reading it back gives the same float
        static double jsonFloat(float value);

        // "path: message" lines
        static std::string format(const std::vector<Diagnostic>& diagnostics);
        static bool hasErrors(const std::vector<Diagnostic>& diagnostics);
    };

}}

#endif // PARTICLES_PARTICLEEFFECTCODEC_H
