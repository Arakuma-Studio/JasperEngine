// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef UISYSTEM_H
#define UISYSTEM_H

#include "SubSystem.h"
#include "System.h"

#include <functional>
#include <map>
#include <memory>
#include <unordered_map>
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
#include "component/OnScreenControlComponents.h"
#include "PointerEventData.h"
#include "Input.h"

namespace jasperengine{

    enum class ScriptMessage : uint8_t;   // script/ScriptMessages.h

    // One pointer of the component UI's EventSystem (ui-multitouch spec 7.1): the mouse (one
    // state per button: -1 left, -2 right, -3 middle) or a finger (its fingerId). A finger's
    // state lives from its down to its up; the mouse's left state from its first move.
    struct UIPointerState {
        int pointerId = -1;
        bool touch = false;
        int button = 0;                       // D_MOUSE_BUTTON_* (a finger: left)
        bool down = false;
        // the UI holds this press: its moves and up are consumed (the left button or a finger
        // on an element; the other buttons when a script handler took them)
        bool captured = false;
        Vector2 position = Vector2(0, 0);     // canvas px
        Vector2 delta = Vector2(0, 0);
        Vector2 pressPosition = Vector2(0, 0);
        Entity hit = NULL_ENTITY;             // the raycast target of the last event
        std::vector<Entity> enterChain;       // hit and its ancestors: the receivers of enter / exit
        // Unity's PointerEventData targets
        Entity rawPress = NULL_ENTITY;
        Entity press = NULL_ENTITY;
        Entity drag = NULL_ENTITY;
        bool dragging = false;
        bool eligibleForClick = false;
        bool useDragThreshold = true;
        bool used = false;
        int clickCount = 0;
        double clickTime = 0.0;
        // the legacy callbacks and the built-in widgets: the element pressed (left button or a
        // finger), what of it this pointer holds, and the element it hovers
        Entity widget = NULL_ENTITY;
        Entity hover = NULL_ENTITY;
        Entity buttonPressed = NULL_ENTITY;   // a Button this pointer holds pressed
        Entity panelGesture = NULL_ENTITY;    // a Panel this pointer moves or resizes
        Entity scrollbar = NULL_ENTITY;       // a Scrollbar this pointer drags
        Entity inputFieldSelecting = NULL_ENTITY;
        bool legacyDragging = false;
        bool internalGesture = false;         // panel move / resize, scrollbar: no legacy drag, no click
        Vector2 panelSizeAcc = Vector2(0, 0); // float to int accumulation of a resize
        Entity control = NULL_ENTITY;         // the on-screen control this pointer holds
    };

    class JASPERENGINE_API UISystem : public SubSystem {

    private:

        // ---- the EventSystem (UISystemPointer.cpp)
        std::map<int, UIPointerState> pointers;
        // the last click of each pointer id (clickCount; outlives a finger's state)
        struct ClickRecord { Entity element = NULL_ENTITY; double time = -1.0; int count = 0; };
        std::map<int, ClickRecord> clickRecords;
        // the event being delivered to a legacy callback (UIComponent:getPointerEventData); one
        // for every scene: events are delivered one at a time on the main thread
        static const PointerEventData* currentLegacyEvent;
        // the owner keys of the on-screen controls' input sources (stable addresses)
        std::unordered_map<Entity, std::unique_ptr<uint8_t>> controlOwners;

        Entity lastUIFromClick; // the legacy onDoubleClick
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

        // ---- UISystemPointer.cpp
        UIPointerState* findPointer(int pointerId);
        PointerEventData makeEventData(const UIPointerState& state, Entity target) const;
        Vector2 toLocal(Entity entity, const Vector2& point) const;
        // ExecuteEvents.GetEventHandler: scripts defining the message, or a built-in receiver (the
        // on-screen controls take down, drag and up)
        Entity findPointerHandler(Entity from, ScriptMessage m) const;
        // ExecuteEvents.Execute: the scripts of `target`; returns whether any defines it
        bool sendPointerMessage(UIPointerState& state, Entity target, ScriptMessage m,
                                const std::function<void(PointerEventData&)>& adjust = nullptr);
        void updateEnterChain(UIPointerState& state, Entity newHit);
        void setLegacyHover(UIPointerState& state, Entity entity);
        bool isHoveredByOtherPointer(Entity entity, int pointerId) const;
        void refreshButtonPressed(Entity entity);
        void validatePointers();
        // the end of a press: release (true) or cancel; then the touch's exit and its state
        void endPress(int pointerId, bool released);
        void legacyPointerDown(UIPointerState& state);
        void legacyPointerMove(UIPointerState& state, CursorType& cursor);
        void legacyPointerUp(UIPointerState& state, bool released, bool overWidget);
        void callLegacy(FunctionSubscribe<void(float, float)> UIComponent::* event, Entity entity, const UIPointerState& state);
        void applyScrollbarPointerPosition(Entity entity, float x, float y);
        bool isWidgetGestureOwned(Entity entity, int exceptPointer) const;

        // ---- on-screen controls (UISystemPointer.cpp)
        bool controlTakesPointers(Entity entity, Signature signature) const;
        Entity findControl(Entity from) const;
        uintptr_t controlOwner(Entity entity);
        void controlDown(UIPointerState& state, Entity entity);
        void controlMove(UIPointerState& state);
        void controlUp(UIPointerState& state);
        void stickUpdate(Entity entity, OnScreenStickComponent& stick, const Vector2& pointer, bool first);
        void stickRelease(Entity entity);
        void buttonControlPress(Entity entity, int pointerId);
        void buttonControlRelease(Entity entity, int pointerId);
        void touchAreaRelease(Entity entity);
        void setElementOffset(Entity entity, const Vector2& rest, const Vector2& offsetCanvas);
        Vector2 getElementRest(Entity entity) const;
        void updateControls();
        void clearControlSources(Entity entity);

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
        // An anchored element's offset is measured in its nearest layout parent's space,
        // its position in its own parent's space
        Vector2 parentToLayoutSpace(Entity entity, const Vector2& point) const;
        Vector2 layoutToParentSpace(Entity entity, const Vector2& point) const;

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

        // The scene leaves the layers: every pointer state goes without callbacks, buttons
        // unpress, the on-screen controls release and clear their input sources
        void resetButtonStates();

        bool eventOnCharInput(wchar_t codepoint);
        bool eventOnKeyDown(int key, bool repeat, int mods);

        // ---- pointers (ui-multitouch spec 7.1). pointerId: the mouse -1 / -2 / -3 (its left,
        // right and middle button), a finger its fingerId; positions in canvas px. Each returns
        // whether the UI consumed the event (the pointer pressed a UI element).
        bool eventOnPointerDown(int pointerId, int button, float x, float y, bool touch);
        bool eventOnPointerUp(int pointerId, int button, float x, float y, bool touch);
        // the mouse moves -1 (and its other held buttons); a finger its own state
        bool eventOnPointerMove(int pointerId, float x, float y, bool touch);
        // a touch cancelled, focus loss: the press ends without a click (D6)
        bool eventOnPointerCancel(int pointerId);
        // the mouse left the window: its hover goes (the press stays)
        void eventOnPointerLeave(int pointerId);
        // the wheel over the mouse's element: onScroll up the hierarchy; true when a script took it
        bool eventOnScroll(float dx, float dy);
        // every pointer's press ends with callbacks (app pause, Input::releaseAll)
        void cancelAllPointers();
        // C++ callers of the one-pointer API: the mouse's left button
        bool eventOnPointerDown(float x, float y) { return eventOnPointerDown(-1, D_MOUSE_BUTTON_LEFT, x, y, false); }
        bool eventOnPointerUp(float x, float y) { return eventOnPointerUp(-1, D_MOUSE_BUTTON_LEFT, x, y, false); }
        bool eventOnPointerMove(float x, float y) { return eventOnPointerMove(-1, x, y, false); }

        // The hit test of every pointer event, in one place (the contract ui-canvas-group extends
        // with blocksRaycasts): the last element in RectTransform order (children after their
        // parents) that is visible, active in hierarchy with an enabled UIComponent, has an
        // Image, contains the point (clipped to its panel) and does not ignore events. panelHit:
        // the panel the press brings to front (the element's panel, or a Panel it is in).
        Entity raycast(const Vector2& point, Entity* panelHit = nullptr) const;

        // D15: that pointer (or, with INT_MIN, the mouse or any finger) is over a UI element that
        // takes events, or presses one. The last processed event of the pointer counts.
        bool isPointerOverUI(int pointerId) const;
        bool isPointerOverUI() const { return isPointerOverUI(-1); }
        // the pointer pressed a UI element and holds it (its moves and up are the UI's)
        bool isPointerCaptured(int pointerId) const;
        // the pointer states (tests, MCP)
        const std::map<int, UIPointerState>& getPointers() const { return pointers; }
        // the PointerEventData of the legacy callback being called, else null
        static const PointerEventData* getCurrentLegacyEvent() { return currentLegacyEvent; }

        // on-screen controls: the stick's value for its knob position and the axes options
        // (Joystick Pack: dead zone, normalised past 1, snap); input = (pointer - centre) / radius, y down
        static Vector2 stickInput(const OnScreenStickComponent& stick, Vector2 input);

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