// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef POINTER_EVENT_DATA_H
#define POINTER_EVENT_DATA_H

#include "Export.h"
#include "Entity.h"
#include "math/Vector2.h"

#include <memory>

namespace jasperengine {

    class Scene;

    // One pointer's event of the component UI's EventSystem (Unity's PointerEventData,
    // ui-multitouch spec 7.2): what the script callbacks onPointerDown/Up/Click/Enter/Exit,
    // onInitializePotentialDrag, onBeginDrag/onDrag/onEndDrag, onDrop and onScroll receive.
    struct JASPERENGINE_API PointerEventData {
        // -1 / -2 / -3: the mouse's left / right / middle button (Unity); a finger: its
        // Touch.fingerId (>= 0)
        int pointerId = -1;
        int button = 0;                       // Input.MOUSE_BUTTON_LEFT/RIGHT/MIDDLE (a finger: left)
        bool isTouch = false;

        // canvas px, the space of Input.getMousePosition and Touch.position (y down)
        Vector2 position = Vector2(0, 0);
        Vector2 delta = Vector2(0, 0);        // since this pointer's last event
        Vector2 pressPosition = Vector2(0, 0);
        // the receiving element's local units (what the legacy x, y callbacks get)
        Vector2 localPosition = Vector2(0, 0);
        // the scene's UI units (equal to canvas px without a canvas scaler)
        Vector2 uiPosition = Vector2(0, 0);
        Vector2 uiDelta = Vector2(0, 0);
        Vector2 scrollDelta = Vector2(0, 0);  // onScroll: the wheel offsets

        int clickCount = 0;                   // clicks of this pointer on the same element within 0.3 s
        double clickTime = 0.0;               // seconds, unscaled system time of the last click

        bool dragging = false;
        bool eligibleForClick = false;
        bool canceled = false;                // the press ended without a release (touch cancel, focus loss, disable)
        bool useDragThreshold = true;         // false in onInitializePotentialDrag: the drag starts at once
        bool used = false;                    // use(): a used down stops the built-in widgets' handling of it

        // the GameObjects of the event (NULL_ENTITY: none)
        Entity pointerEnter = NULL_ENTITY;    // under the pointer (the raycast hit)
        Entity pointerPress = NULL_ENTITY;    // the press handler: gets up and click
        Entity rawPointerPress = NULL_ENTITY; // the element pressed, handler or not
        Entity pointerDrag = NULL_ENTITY;     // the drag handler
        Entity target = NULL_ENTITY;          // the GameObject receiving this call

        Scene* scene = nullptr;
        // alive while the scene is (Scene::getLifetime): an event a script keeps past the
        // scene's deletion gives no GameObjects (liveScene() is null)
        std::weak_ptr<char> sceneLifetime;

        void use() { used = true; }
        Scene* liveScene() const { return sceneLifetime.expired() ? nullptr : scene; }
    };

}

#endif // POINTER_EVENT_DATA_H
