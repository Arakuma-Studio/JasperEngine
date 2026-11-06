// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ALPHATWEEN_COMPONENT_H
#define ALPHATWEEN_COMPONENT_H

#include "Export.h"

namespace jasperengine{

    // What an AlphaTween writes on its target (ui canvas group spec decision 9)
    enum class AlphaTweenTarget{
        // the target's enabled Canvas Group when it has one, else its colour alpha
        AUTO,
        // the colour alpha: UIComponent::color.w and the first submesh's base colour alpha
        COLOR,
        // CanvasGroupComponent::alpha (nothing when the target has no Canvas Group)
        CANVAS_GROUP
    };

    struct JASPERENGINE_API AlphaTweenComponent{
        float endAlpha = 0;
        float startAlpha = 0;
        AlphaTweenTarget alphaTarget = AlphaTweenTarget::AUTO;
    };

}

#endif //ALPHATWEEN_COMPONENT_H