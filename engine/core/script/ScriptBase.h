// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SCRIPT_BASE_H
#define SCRIPT_BASE_H

#include "Scene.h"
#include "Entity.h"
#include "ScriptScheduler.h"

#include <functional>

namespace jasperengine {

    class JASPERENGINE_API ScriptBase {
    protected:
        Scene* scene;
        Entity entity;

    public:
        ScriptBase(Scene* scene, Entity entity);
        // cancels this script's timers (invoke / invokeRepeating)
        ~ScriptBase();

        Scene* getScene() const;
        Entity getEntity() const;

        // Unity isActiveAndEnabled: the GameObject is active in hierarchy and this
        // script's entry is enabled. Engine events registered with
        // REGISTER_ENGINE_EVENT are skipped while it is false.
        bool isActiveAndEnabled() const;

        // Unity's Invoke: calls `fn` once, `delay` seconds from now (scaled by Engine timeScale
        // unless TimeMode::Unscaled), in the update phase after onUpdate. The timer belongs to this
        // script: it keeps running while the script is disabled or its GameObject inactive, and is
        // cancelled when the script is deleted. Returns its id for cancelInvoke.
        TaskId invoke(std::function<void()> fn, float delay, TimeMode mode = TimeMode::Scaled);
        // Unity's InvokeRepeating: first after `delay`, then every `interval` (> 0) seconds, at most
        // once a frame (a late call is rescheduled from now)
        TaskId invokeRepeating(std::function<void()> fn, float delay, float interval, TimeMode mode = TimeMode::Scaled);
        // Cancels one of this script's timers; false when it is not one or already done
        bool cancelInvoke(TaskId id);
        // Cancels every timer of this script
        void cancelAllInvokes();
        // A timer of this script is pending (any, or the one with that id)
        bool isInvoking() const;
        bool isInvoking(TaskId id) const;
    };

}

#endif //SCRIPT_BASE_H