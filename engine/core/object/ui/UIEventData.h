// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef UI_EVENT_DATA_H
#define UI_EVENT_DATA_H

#include "Export.h"
#include "Entity.h"
#include "math/Vector2.h"
#include "component/UISelectable.h"

#include <memory>

namespace jasperengine {

    class Scene;

    // The selection events of the component UI (Unity's BaseEventData): what onSelect, onDeselect,
    // onSubmit and onCancel receive (ui-components spec 6.9)
    struct JASPERENGINE_API BaseEventData {
        Entity target = NULL_ENTITY;          // the GameObject receiving this call
        Entity selectedObject = NULL_ENTITY;  // the scene's selection when the event was sent
        // use(): onSubmit skips the Button's own click, onMove its navigation
        bool used = false;

        Scene* scene = nullptr;
        // alive while the scene is: an event kept past the scene's deletion gives no GameObjects
        std::weak_ptr<char> sceneLifetime;

        void use() { used = true; }
        Scene* liveScene() const { return sceneLifetime.expired() ? nullptr : scene; }
    };

    // onMove's event (Unity's AxisEventData): the direction pressed (arrows, d-pad, left stick)
    struct JASPERENGINE_API AxisEventData : BaseEventData {
        MoveDirection moveDir = MoveDirection::NONE;
        Vector2 moveVector = Vector2(0, 0);   // unit, y down (the UI's space)
    };

}

#endif // UI_EVENT_DATA_H
