// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The Outline of a GameObject (docs/specs/S94-rendering-anim-gaps.md F22): Lua `Outline(scene,
// entity)` and C++ scripts. It ADDS an Outline when the GameObject has none. Main thread only (ECS).

#ifndef OUTLINE_H
#define OUTLINE_H

#include "object/EntityHandle.h"
#include "component/OutlineComponent.h"
#include "math/Vector4.h"

namespace jasperengine{

    class JASPERENGINE_API Outline: public EntityHandle{

    public:
        Outline(Scene* scene, Entity entity);
        virtual ~Outline();

        Outline(const Outline& rhs);
        Outline& operator=(const Outline& rhs);
        Outline(Outline&& rhs) noexcept = default;
        Outline& operator=(Outline&& rhs) noexcept = default;

        void setEnabled(bool enabled);
        bool isEnabled() const;
        void setMode(OutlineMode mode);
        OutlineMode getMode() const;
        // sRGB with straight alpha, as the Inspector shows it (stored linear)
        void setColor(Vector4 color);
        Vector4 getColor() const;
        // in the width units; a negative or non-finite width logs an error and changes nothing
        void setWidth(float width);
        float getWidth() const;
        void setWidthUnits(OutlineWidthUnits units);
        OutlineWidthUnits getWidthUnits() const;
        // the Meshes below it too (skinned model children, nested model nodes)
        void setIncludeChildren(bool includeChildren);
        bool isIncludeChildren() const;
    };

}

#endif //OUTLINE_H
