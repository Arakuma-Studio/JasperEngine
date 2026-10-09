// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_CSS_BASERULES_H
#define COMPAT_CSS_BASERULES_H

// Base rules (phase-2 §3.9.8): RCSS has animation, transition, background layers, the
// individual transforms and text effects only as shorthands, so a rule that sets a longhand
// alone (`.item:nth-child(2) { animation-delay: .1s }`) takes the rest from the last author
// rule whose selector equals its own without the subject's pseudo-classes, attribute and
// structural selectors (`.item`). Textual matching on normalized selectors, always reported
// (HTMLUI-CSS-0017 Info naming the base rule).

#include <functional>
#include <string>

namespace jasperengine::htmlui::compat {

    struct Session;
    struct FlatRule;

    class BaseRules {
    public:
        // indexes every author rule by the CSS text of each of its selectors
        static void index(Session& session);
        // the base rule of `rule` that satisfies `sets` (declares what the caller needs), or nullptr
        static const FlatRule* find(Session& session, const FlatRule& rule, const std::function<bool(const FlatRule&)>& sets);
        // "ui/menu.css:12" of a rule
        static std::string location(Session& session, const FlatRule& rule);
    };

}

#endif //COMPAT_CSS_BASERULES_H
