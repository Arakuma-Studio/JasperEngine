// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef RECTTRANSFORM_H
#define RECTTRANSFORM_H

#include "GameObject.h"
#include "component/RectTransformComponent.h"

namespace jasperengine{
    class JASPERENGINE_API RectTransform: public GameObject{

    public:

        // component checkbox (Unity Behaviour.enabled): a disabled UI is skipped by its system
        void setEnabled(bool enabled);
        bool isEnabled() const;
        RectTransform(Scene* scene);
        RectTransform(Scene* scene, Entity entity);
        virtual ~RectTransform();

        RectTransform(const RectTransform& rhs);
        RectTransform& operator=(const RectTransform& rhs);
        RectTransform(RectTransform&& rhs) noexcept;
        RectTransform& operator=(RectTransform&& rhs) noexcept;

        void setSize(unsigned int width, unsigned int height);
        void setWidth(unsigned int width);
        void setHeight(unsigned int height);

        unsigned int getWidth() const;
        unsigned int getHeight() const;

        void setAnchorPoints(float left, float top, float right, float bottom);
        void setAnchorPointLeft(float left);
        void setAnchorPointTop(float top);
        void setAnchorPointRight(float right);
        void setAnchorPointBottom(float bottom);

        float getAnchorPointLeft() const;
        float getAnchorPointTop() const;
        float getAnchorPointRight() const;
        float getAnchorPointBottom() const;

        void setAnchorOffsets(int left, int top, int right, int bottom);
        void setAnchorOffsetLeft(int left);
        void setAnchorOffsetTop(int top);
        void setAnchorOffsetRight(int right);
        void setAnchorOffsetBottom(int bottom);

        int getAnchorOffsetLeft() const;
        int getAnchorOffsetTop() const;
        int getAnchorOffsetRight() const;
        int getAnchorOffsetBottom() const;

        void setPositionXOffset(float xOffset);
        float getPositionXOffset() const;
        void setPositionYOffset(float yOffset);
        float getPositionYOffset() const;
        void setPositionOffset(Vector2 positionOffset);
        Vector2 getPositionOffset() const;

        void setPivot(Vector2 pivot);
        void setPivot(float x, float y);
        Vector2 getPivot() const;

        void setAnchorPreset(AnchorPreset anchorPreset);
        AnchorPreset getAnchorPreset() const;

        void setUsingAnchors(bool usingAnchors);
        bool isUsingAnchors() const;

        void setIgnoreScissor(bool ignoreScissor);
        bool isIgnoreScissor() const;

        // Canvas Groups over this element, the chain walked now (exact before the next sync):
        // Unity CanvasRenderer.GetInheritedAlpha, CanvasGroup-allowed interaction and raycasts
        float getInheritedAlpha() const;
        bool getGroupsAllowInteraction() const;
        bool getGroupsBlockRaycasts() const;
        // Unity Selectable.IsInteractable: the groups allow it and the Button / InputField's
        // own disabled flag is off
        bool isInteractable() const;
    };
}

#endif //RECTTRANSFORM_H