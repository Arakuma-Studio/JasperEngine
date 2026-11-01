// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef ON_SCREEN_CONTROLS_H
#define ON_SCREEN_CONTROLS_H

#include "Image.h"
#include "component/OnScreenControlComponents.h"

namespace jasperengine{

    // The GameObject wrappers of the on-screen controls (ui-multitouch spec 8): an Image (what
    // pointers hit) with the control's component. The constructor given only a scene creates
    // the GameObject; given an entity it wraps it (adding the component when it has none).
    // Their fields are bound by name (OnScreenStick(scene, entity).deadZone ...).

    class JASPERENGINE_API OnScreenStick: public Image{
    public:
        OnScreenStick(Scene* scene);
        OnScreenStick(Scene* scene, Entity entity);
        virtual ~OnScreenStick();

        OnScreenStickComponent& getStick() const;
    };

    class JASPERENGINE_API OnScreenButton: public Image{
    public:
        OnScreenButton(Scene* scene);
        OnScreenButton(Scene* scene, Entity entity);
        virtual ~OnScreenButton();

        OnScreenButtonComponent& getControl() const;
    };

    class JASPERENGINE_API TouchArea: public Image{
    public:
        TouchArea(Scene* scene);
        TouchArea(Scene* scene, Entity entity);
        virtual ~TouchArea();

        TouchAreaComponent& getArea() const;
    };

}

#endif //ON_SCREEN_CONTROLS_H
