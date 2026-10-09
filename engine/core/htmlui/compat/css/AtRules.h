// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_CSS_ATRULES_H
#define COMPAT_CSS_ATRULES_H

// The at-rules of phase-2 §3.9.3 and §3.9.9: @supports evaluated statically, @font-face
// source selection, @keyframes (names, keyframe selectors, declarations in keyframe mode,
// reversed copies), @property, the RmlUi extensions written through, and the drops.

#include "htmlui/compat/css/CssAst.h"

#include <string>
#include <vector>

namespace jasperengine::htmlui::compat {

    struct Session;
    struct FlatAtRule;
    class RcssWriter;

    class AtRules {
    public:
        // @supports: true when the condition holds with the current fork features
        // (HTMLUI-AT-0003 Info names the result)
        static bool evaluateSupports(Session& session, const Rule& rule);
        static bool evaluateCondition(Session& session, const std::vector<ComponentValue>& condition, uint32_t file);

        // the at-rules that are dropped (HTMLUI-AT-0001, Warning or Info per Appendix C);
        // false when the at-rule is not one of them
        static bool reportDropped(Session& session, const Rule& rule);

        // @font-face, @keyframes (and prefixed), @decorator, @spritesheet
        static void write(Session& session, const FlatAtRule& rule, RcssWriter& writer);

        // declared names of @font-face (pre-pass)
        static std::string fontFaceFamily(const Rule& rule);
        // the keyframes name as written ("" when invalid)
        static std::string keyframesName(const Rule& rule);
        // a valid RCSS keyframes name ([A-Za-z0-9_-]+)
        static bool validKeyframesName(const std::string& name);
    };

}

#endif //COMPAT_CSS_ATRULES_H
