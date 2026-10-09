// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_CSS_NESTING_H
#define COMPAT_CSS_NESTING_H

// CSS Nesting flattened (phase-2 §3.9.3 step 2): a nested rule's selectors combined with its
// parent's. `&` is replaced by the parent selector; a nested selector without `&` is a
// descendant of the parent (or follows its leading combinator); a parent selector list is
// expanded into the cross product, or kept as :is(parents) once the fork has RootIsWhere.

#include "Export.h"
#include "htmlui/compat/ForkFeatures.h"
#include "htmlui/compat/css/SelectorRewriter.h"

#include <optional>
#include <vector>

namespace jasperengine::htmlui::compat {

    class JASPERENGINE_API Nesting {
    public:
        static constexpr size_t kLimit = 256;

        // nullopt when the product has more than kLimit selectors
        static std::optional<std::vector<ComplexSelector>> combine(const std::vector<ComplexSelector>& parents,
                                                                   const std::vector<ComplexSelector>& children,
                                                                   const ForkFeatures& fork);
        // one child against one parent
        static ComplexSelector substitute(const ComplexSelector& parent, const ComplexSelector& child);
        static bool hasNestingSelector(const ComplexSelector& selector);
    };

}

#endif //COMPAT_CSS_NESTING_H
