// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md R31, §6.5, §6.11)
// - Purpose: everything derived from the effect and generator registry (TitleEffectRegistry.h),
//   so no surface keeps a list of its own: the definition checks every entry must pass, the MCP
//   schema fragment of a definition's params, list_title_library's rows and `id` detail (the
//   parameter lines and a ready add_effect op), the field paths exposed parameters and Lua name
//   (`fx:<type>.<param>`, S81), the generated docs page docs/mcp/guide/title-art-effects.md, and
//   the golden case list (every effect at defaults on the standard title, every generator alone).
// - Thread-safety: pure functions of the registry's state (read it from one thread while
//   definitions are registered).

#pragma once

#include "Export.h"
#include "titleart/TitleArtDoc.h"
#include "titleart/TitleEffectRegistry.h"

#include <string>
#include <vector>

namespace jasperengine::titleart {

    // what is wrong with a definition (empty: nothing): ids and keys snake_case, labels present,
    // every param with a description, a default its own check accepts unchanged (inside its
    // range, among its options), a `when` naming another param and values it has, numbers with
    // a unit when they are distances, apply or render present
    JASPERENGINE_API std::vector<std::string> checkTitleEffectDef(const EffectDef& def);
    JASPERENGINE_API std::vector<std::string> checkTitleGeneratorDef(const GeneratorDef& def);
    // the registry as a whole: every definition's problems, ids unique across effects and
    // generators, labels unique per kind
    JASPERENGINE_API std::vector<std::string> checkTitleEffectRegistry(const TitleEffectRegistry& registry);

    // a param's JSON Schema fragment (MCP's closed objects: type, default, minimum/maximum, enum,
    // description; gradients, curves and adjustments as objects their schema kind checks)
    JASPERENGINE_API Json titleParamSchema(const ParamDef& param);
    // the schema of a definition's `params` object: every param a property, no others
    JASPERENGINE_API Json titleParamsSchema(const std::vector<ParamDef>& params);

    // one line per param for list_title_library's detail and the docs:
    // "size px 40 [1..400]: How far the glow reaches past the edge"
    JASPERENGINE_API std::string titleParamLine(const ParamDef& param);

    // list_title_library rows: {id, name, category, cost, adds_light, on_parts, outline,
    // fallback, glyph_safety, live, aliases} for an effect; {id, name, category, cost, follow,
    // fills_canvas, glyph_safety, aliases} for a generator (values at defaults)
    JASPERENGINE_API Json titleEffectRow(const EffectDef& def);
    JASPERENGINE_API Json titleGeneratorRow(const GeneratorDef& def);
    // the `id` detail: the row, its description, `params` (the lines), `schema` and the op that
    // adds it ({"op": "add_effect", "layer": "<layer>", "type": ...} or add_layer for a generator)
    JASPERENGINE_API Json titleEffectDetail(const EffectDef& def);
    JASPERENGINE_API Json titleGeneratorDetail(const GeneratorDef& def);

    // the field paths an exposed parameter (and so S81's Lua) names a definition's params by:
    // "fx:<type>.<key>" for an effect, "gen.<key>" for a generator
    JASPERENGINE_API std::vector<std::string> titleEffectFieldPaths(const EffectDef& def);
    JASPERENGINE_API std::vector<std::string> titleGeneratorFieldPaths(const GeneratorDef& def);

    // docs/mcp/guide/title-art-effects.md, generated from the registry
    JASPERENGINE_API std::string titleEffectDocsMarkdown(const TitleEffectRegistry& registry);
    constexpr const char* TITLE_EFFECT_DOCS_PATH = "docs/mcp/guide/title-art-effects.md";

    // the golden list (AC24): "effect/<type>" for every effect (at defaults on the standard title)
    // and "generator/<type>" for every generator (alone on the canvas), in registry order
    JASPERENGINE_API std::vector<std::string> titleGoldenCases(const TitleEffectRegistry& registry);

}
