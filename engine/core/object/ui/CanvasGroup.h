// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Unity's CanvasGroup over a CanvasGroupComponent (ui canvas group spec 7.6): Lua
// `CanvasGroup`, `object:getCanvasGroup()`, and C++ scripts.
// - CanvasGroup(scene, entity) wraps the GameObject's group and ADDS one when it has none
//   (Unity GetComponent ?? AddComponent). CanvasGroup(scene) makes a new GameObject: a
//   RectTransform stretched to its parent with a Canvas Group.
// - alpha is clamped to [0, 1]; a NaN is rejected (the value stays, one warning per GameObject).
// - The fades are AlphaTweens on the group (AlphaTweenTarget::CANVAS_GROUP) from the current
//   alpha, owned by the scene's UISystem: a new fade stops the group's previous one; when one
//   finishes the final alpha is written, then onComplete runs (main thread, in the UISystem
//   update), then the GameObject is deactivated when asked. The tween goes when it finishes,
//   on stopFade, with the group or with the scene.
// - Main thread only (ECS).

#ifndef CANVAS_GROUP_H
#define CANVAS_GROUP_H

#include "object/EntityHandle.h"
#include "object/GameObject.h"
#include "action/AlphaTween.h"
#include "action/Ease.h"
#include "component/CanvasGroupComponent.h"

#include <functional>

namespace jasperengine{

    struct JASPERENGINE_API CanvasGroupFadeOptions{
        EaseType ease = EaseType::LINEAR;
        // advances at time scale 0 (Tween.useUnscaledTime)
        bool unscaled = false;
        // deactivates the GameObject when the fade completes (after onComplete)
        bool deactivate = false;
        std::function<void()> onComplete;
    };

    class JASPERENGINE_API CanvasGroup: public EntityHandle{

    public:
        // a new GameObject with a RectTransform stretched to its parent and a Canvas Group
        CanvasGroup(Scene* scene);
        // the GameObject's Canvas Group, added when it has none
        CanvasGroup(Scene* scene, Entity entity);
        virtual ~CanvasGroup();

        CanvasGroup(const CanvasGroup& rhs);
        CanvasGroup& operator=(const CanvasGroup& rhs);
        CanvasGroup(CanvasGroup&& rhs) noexcept;
        CanvasGroup& operator=(CanvasGroup&& rhs) noexcept;

        void setEnabled(bool enabled);
        bool isEnabled() const;

        void setAlpha(float alpha);
        float getAlpha() const;

        void setInteractable(bool interactable);
        bool isInteractable() const;

        void setBlocksRaycasts(bool blocksRaycasts);
        bool isBlocksRaycasts() const;

        void setIgnoreParentGroups(bool ignoreParentGroups);
        bool isIgnoreParentGroups() const;

        void setRenderMode(CanvasGroupRenderMode renderMode);
        CanvasGroupRenderMode getRenderMode() const;

        // LeanTween.alphaCanvas / DOTween DOFade: from the current alpha to `alpha` in
        // `duration` seconds; returns the running AlphaTween (owned by the group's scene)
        AlphaTween fadeTo(float alpha, float duration);
        AlphaTween fadeTo(float alpha, float duration, const CanvasGroupFadeOptions& options);
        AlphaTween fadeIn(float duration);
        AlphaTween fadeIn(float duration, const CanvasGroupFadeOptions& options);
        AlphaTween fadeOut(float duration);
        AlphaTween fadeOut(float duration, const CanvasGroupFadeOptions& options);
        // stops the running fade; jumpToEnd writes its end alpha and completes it now
        // (onComplete, deactivate)
        void stopFade();
        void stopFade(bool jumpToEnd);
        bool isFading() const;

        GameObject getGameObject() const;
    };
}

#endif //CANVAS_GROUP_H
