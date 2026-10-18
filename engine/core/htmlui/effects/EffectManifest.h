// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUI_EFFECTS_EFFECTMANIFEST_H
#define HTMLUI_EFFECTS_EFFECTMANIFEST_H

#include "EffectTypes.h"
#include "shader/ShaderData.h"

#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::htmlui {

    // The manifest of an effect (phase-5 spec 3.9): the "// @" lines right after #version 450.
    //
    //   // @effect     <name>
    //   // @kind       paint | filter
    //   // @category   gradient | noise | pattern | light | energy | space | interactive | glass | blur | stylize | color | distort | transition
    //   // @summary    <one line, at most 100 characters>
    //   // @source     shaders.com <Name>[ + <Name>] [(adapted: <text>)] | original [(<text>)]
    //   // @cost       low | medium | high
    //   // @backdrop   yes | no                                   (filter kind; default no)
    //   // @time       <param>[, <param>]                          (at most 4 rate parameters)
    //   // @pointer    none | position | trail | ripples           (default none)
    //   // @prepass    blur <scaled> [threshold <param>]           (filter kind)
    //   // @margin     <scaled> | <number>px                       (filter kind; default 0px)
    //   // @geometry   fill | bounds <scaled>                      (paint kind; default fill)
    //   // @suggest-for <word> [<word> ...]
    //   // @param      <name> <type> <default> [<min> <max>] [<opt>|<opt>|...] -- <description>
    //   phase 6 (stateful effects, docs/html-ui/phase-6-stateful-effects.md 3.2; @cost stateful):
    //   // @state      <name> rgba8|rgba16f|rgba32f|r32f <w>x<h>|box[*<scale>] [max <px>] [linear|nearest] [clamp|repeat] [clear|resample]
    //   // @init       <pass> <state>
    //   // @step       <pass> <state> [x<count>|x<param>] [add] [scatter <state>]
    //   // @warmup     <steps> <dt>
    //   // @rate       <hz> [max <substeps>]
    //   // @replay     <seconds>
    //   <scaled> := <param>[*<number>][+<number>px] | <number>px
    //
    // A default with spaces is double-quoted; ranges carry units. Diagnostics are
    // HTMLUI-FX-0014 (effect-manifest) with the file and line set.
    class JASPERENGINE_API EffectManifestParser {
    public:
        // parses the leading "// @" block of a fragment source; false on any error (diags filled)
        static bool parse(const std::string& source, const std::string& path, bool builtin, EffectManifest& out,
                          std::vector<EffectDiagnostic>& diags);
        // the rules beyond the grammar: the "#include main_paint/main_filter" line matches @kind,
        // every default is legal and in range, names are unique and not reserved, the @time,
        // @prepass, @margin and @geometry parameters exist, ...
        static bool validate(const EffectManifest& manifest, const std::string& source, std::vector<EffectDiagnostic>& diags);
        // "<param>[*<number>][+<number>px] | <number>px"
        static bool parseScaled(std::string_view text, EffectScaledParam& out);
        // the manifest block's text: its lines, as they are in the source ("" without one)
        static std::string manifestBlock(const std::string& source);
        // the source starts (after #version 450) with a manifest that begins with "// @effect"
        static bool hasEffectManifest(const std::string& source);
        // The compiled variant against the manifest (the reflection of u_fs_effectParams, by
        // name): a member per parameter of its type (number, integer, percentage, length, angle,
        // time, enum, boolean: float; position: vec2; color: vec4; palette: vec4[8] <p>,
        // vec4[2] <p>Stops, float <p>Count), no other member, and the frame block. Problems are
        // appended as sentences; true when there is none.
        static bool checkReflection(const EffectManifest& manifest, ShaderData& shader, std::vector<std::string>& problems);
    };

}

#endif //HTMLUI_EFFECTS_EFFECTMANIFEST_H
