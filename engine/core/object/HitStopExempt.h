// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Hit Stop Exempt of a GameObject (docs/specs/S48-pixel-fx.md R21): Lua
// `HitStopExempt(scene, entity)` and C++ scripts. It wraps the GameObject's component and ADDS
// one when it has none. Main thread only (ECS).

#ifndef HIT_STOP_EXEMPT_H
#define HIT_STOP_EXEMPT_H

#include "object/EntityHandle.h"
#include "component/HitStopExemptComponent.h"

namespace jasperengine{

    class JASPERENGINE_API HitStopExempt: public EntityHandle{
    public:
        HitStopExempt(Scene* scene, Entity entity);
        virtual ~HitStopExempt();

        HitStopExempt(const HitStopExempt& rhs);
        HitStopExempt& operator=(const HitStopExempt& rhs);

        void setEnabled(bool enabled);
        bool isEnabled() const;
        // its descendants run through a hit-stop too
        void setIncludeChildren(bool includeChildren);
        bool isIncludeChildren() const;
    };

}

#endif //HIT_STOP_EXEMPT_H
