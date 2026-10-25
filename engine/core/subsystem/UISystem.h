// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef UISYSTEM_H
#define UISYSTEM_H

#include "SubSystem.h"
#include "System.h"

#include <vector>

#include "component/RectTransformComponent.h"
#include "component/LayoutGroupComponent.h"
#include "component/TextComponent.h"
#include "component/ImageComponent.h"
#include "component/UIComponent.h"
#include "component/ButtonComponent.h"
#include "component/PanelComponent.h"
#include "component/ScrollbarComponent.h"
#include "component/ProgressbarComponent.h"
#include "component/PolygonComponent.h"
#include "component/InputFieldComponent.h"
#include "component/Transform.h"
#include "component/CameraComponent.h"

namespace jasperengine{

    class JASPERENGINE_API UISystem : public SubSystem {

    private:

        // the ids onComponentRemoved compares against (it runs for every destroyed entity
        // with a Transform)
        ComponentId uiId, rectTransformId, imageId, transformId;
        ComponentId buttonId, panelId, scrollbarId, progressbarId, inputFieldId, textId;

        Entity lastUIFromPointer;
        Entity lastUIFromPointerHover;
        Entity lastUIFromClick;
        Entity lastPanelFromPointer;
        Entity inputFieldSelecting;
        Vector2 lastPointerDownPos;
        Vector2 lastPointerPos;
        Vector2 panelSizeAcc; // to add accumulation from float to int
        bool pointerDragging;
        bool pointerInternalGesture; // gesture captured by a built-in widget (panel move/resize, scrollbar)
        double lastClickTime; // monotonic wall-clock seconds (Engine::getSystemTime)

        int anchorReferenceWidth;
        int anchorReferenceHeight;

        bool pendingImages; // an image waited for its texture in the last update

        void createOrUpdateUiComponent(RectTransformComponent& layout, Entity entity, Signature signature);
        void getPanelEdges(const PanelComponent& panel, const RectTransformComponent& layout, const Transform& transform, const RectTransformComponent& headerlayout,  Rect& edgeRight, Rect& edgeRightBottom, Rect& edgeBottom, Rect& edgeLeftBottom, Rect& edgeLeft);
        Rect fitOnPanel(Rect uiRect, Entity parentPanel);
        void calculateUIAABB(UIComponent& ui);

        //Image
        bool createImagePatches(ImageComponent& img, UIComponent& ui, RectTransformComponent& layout);

        // Text
        std::string getFontId(const TextComponent& text) const;
        std::string getFontTextureId(const TextComponent& text) const;
        bool isFontAtlasStale(const TextComponent& text) const;
        void syncFontAtlas(TextComponent& text, UIComponent& ui);
        bool loadFontAtlas(TextComponent& text, UIComponent& ui, RectTransformComponent& layout);
        void createText(TextComponent& text, UIComponent& ui, RectTransformComponent& layout);

        //Button
        void applyButtonVisual(ButtonComponent& button, UIComponent& ui);
        void updateButton(Entity entity, ButtonComponent& button, ImageComponent& img, UIComponent& ui, RectTransformComponent& layout);

        //Panel
        void updatePanel(Entity entity, PanelComponent& panel, ImageComponent& img, UIComponent& ui, RectTransformComponent& layout);

        //Scrollbar
        void updateScrollbar(ScrollbarComponent& scrollbar, RectTransformComponent& layout);

        //Progressbar
        void updateProgressbar(Entity entity, ProgressbarComponent& progressbar, ImageComponent& img, UIComponent& ui, RectTransformComponent& layout);

        //InputField
        void updateInputField(Entity entity, InputFieldComponent& inputfield, ImageComponent& img, UIComponent& ui, RectTransformComponent& layout);
        void blinkCursorInputField(double dt, InputFieldComponent& inputfield, UIComponent& ui);
        bool handleInputFieldCharInput(InputFieldComponent& inputfield, TextComponent& text, UIComponent& ui, wchar_t codepoint);
        bool handleInputFieldKeyDown(InputFieldComponent& inputfield, TextComponent& text, UIComponent& ui, int key, bool repeat, int mods);
        void setInputFieldCursorFromLocalX(InputFieldComponent& inputfield, TextComponent& text, ImageComponent& img, float localX, bool extendSelection);
        bool inputFieldHasSelection(const InputFieldComponent& inputfield) const;
        int inputFieldSelectionStart(const InputFieldComponent& inputfield) const;
        int inputFieldSelectionEnd(const InputFieldComponent& inputfield) const;
        void inputFieldClampCursor(InputFieldComponent& inputfield, size_t numCodepoints) const;
        void inputFieldSelectionRects(const TextComponent& text, int start, int end, std::vector<Rect>& rects) const;
        //one caret stop to the left or right on screen, not in the string
        int inputFieldVisualStep(const TextComponent& text, int cursorIndex, int direction) const;
        float inputFieldCursorPixelX(const TextComponent& text, int cursorIndex) const;
        int inputFieldFindCursorIndexFromX(const TextComponent& text, float x) const;
        std::string inputFieldMaskText(const std::string& text, char maskChar) const;
        void inputFieldDeleteSelection(InputFieldComponent& inputfield, TextComponent& text);
        void inputFieldResetBlink(InputFieldComponent& inputfield) const;
        InputFieldComponent* findInputFieldForTextChild(Entity textEntity) const;

        //UI Polygon
        void createUIPolygon(PolygonComponent& polygon, UIComponent& ui, RectTransformComponent& layout);

        // True when the entity is alive and still usable as a UI event target
        bool isLiveUIEntity(Entity entity) const;
        bool isLiveUIEntity(Entity entity, Signature signature) const;
        // Active in hierarchy with an enabled UIComponent (when it has one): drawn and interactive
        bool isUIActive(Entity entity, Signature signature) const;

        void pointerDownOnUI(Entity entity, float x, float y);
        void pointerMoveOnUI(Entity entity, float x, float y, Vector2 pointerDiff, CursorType& cursor);
        void applyScrollbarPointerPosition(Entity entity, float x, float y);

        bool isCoordInside(float x, float y, Transform& transform, RectTransformComponent& layout);

        static Rect transformRectToWorldAxisAligned(const Rect& rect, const Matrix4& matrix);
        Entity findNearestLayoutParent(Entity startParent) const;
        Vector3 mapLayoutPointToParentLocalSpace(float posX, float posY, Entity layoutParent, Entity targetParent) const;
        Vector3 mapParentLocalPointToLayoutSpace(float posX, float posY, Entity layoutParent, Entity sourceParent) const;
        void convertLayoutSpaceToLocalParentSpace(Entity transformParent, Entity layoutParent, float& posX, float& posY) const;
        void convertLocalParentSpaceToLayoutSpace(Entity transformParent, Entity layoutParent, float& posX, float& posY) const;

        void applyAnchorPreset(RectTransformComponent& layout);
        void changeFlipY(UIComponent& ui, CameraComponent& camera);

        void destroyText(TextComponent& text);
        void destroyButton(ButtonComponent& button);

        bool isEntityChild(Entity child, Entity parent) const;

    public:
        UISystem(Scene* scene);
        virtual ~UISystem();

        Rect getAnchorReferenceRect(const RectTransformComponent& layout, const Transform& transform, bool worldPosition);

        void setAnchorReferenceSize(int width, int height);
        void clearAnchorReferenceSize();
        int getAnchorReferenceWidth() const;
        int getAnchorReferenceHeight() const;

        bool isInputFieldFocused();

        // an image is still waiting for its texture to build its geometry
        bool hasPendingImages() const;

        Vector2 getTextMinSize(TextComponent& text);

        void resetButtonStates();

        bool eventOnCharInput(wchar_t codepoint);
        bool eventOnKeyDown(int key, bool repeat, int mods);
        bool eventOnPointerDown(float x, float y);
        bool eventOnPointerUp(float x, float y);
        bool eventOnPointerMove(float x, float y);
        // the last pointer event was over a UI element (hovered or pressed): documents on
        // meshes behind it do not get the pointer (HTML UI, Engine::systemMouseMove)
        bool isPointerOverUI() const;

        // basic UIs
        bool createOrUpdatePolygon(PolygonComponent& polygon, UIComponent& ui, RectTransformComponent& layout);
        bool createOrUpdateImage(ImageComponent& img, UIComponent& ui, RectTransformComponent& layout);
        bool createOrUpdateText(TextComponent& text, UIComponent& ui, RectTransformComponent& layout);

        // advanced UIs
        void createButtonObjects(Entity entity, ButtonComponent& button);
        void createPanelObjects(Entity entity, PanelComponent& panel);
        void createScrollbarObjects(Entity entity, ScrollbarComponent& scrollbar);
        void createProgressbarObjects(Entity entity, ProgressbarComponent& progressbar);
        void createInputFieldObjects(Entity entity, InputFieldComponent& inputfield);

        void load() override;
        void draw() override;
        void destroy() override;
        void update(double dt) override;

        void onComponentRemoved(Entity entity, ComponentId componentId) override;
    };

}

#endif //UISYSTEM_H