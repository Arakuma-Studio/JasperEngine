// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef UISYSTEM_H
#define UISYSTEM_H

#include "SubSystem.h"
#include "System.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

#include "ecs/ComponentFwd.h"
#include "component/UISelectable.h"
#include "util/STBText.h"
#include "PointerEventData.h"
#include "Input.h"


namespace jasperengine{

    // An element's Canvas Group chain (ui canvas group spec decision 1): the enabled groups
    // from the element up, stopping after the first one with ignoreParentGroups
    struct JASPERENGINE_API CanvasGroupState{
        float alpha = 1.0f;             // product of the alphas (absolute, Unity GetInheritedAlpha)
        bool interactable = true;       // AND of the interactable flags
        bool blocksRaycasts = true;     // AND of the blocksRaycasts flags
        Entity group = NULL_ENTITY;     // the nearest group of the chain, NULL_ENTITY without one
    };

    // Work done by UISystem::syncCanvasGroups since the last reset (tests, budgets)
    struct JASPERENGINE_API CanvasGroupStats{
        uint64_t fullPasses = 0;        // structural passes over every Transform
        uint64_t subtreePasses = 0;     // property passes over one group's branch
        uint64_t visited = 0;           // entities walked by both
    };

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
        // an On-Screen Button (pressWhenPointerEnters) a finger that holds another control (or
        // went down elsewhere on the UI) slid onto: pressed by this pointer besides its control,
        // released when it leaves (releaseWhenPointerLeaves), on the up or the cancel
        Entity enteredButton = NULL_ENTITY;
    };

    class JASPERENGINE_API UISystem : public SubSystem {

    private:

        // the ids onComponentRemoved compares against (it runs for every destroyed entity
        // with a Transform)
        ComponentId uiId, rectTransformId, imageId, transformId;
        ComponentId buttonId, panelId, scrollbarId, progressbarId, inputFieldId, textId;
        ComponentId canvasGroupId, pointsId, actionId, onScreenStickId, onScreenButtonId, touchAreaId;

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

        static bool reducedMotion;

        bool textEffectsHeld = false;
        double lastTextEffectsTime = 0.0;

        // Text Effects after the layout: time, the glyphs from their base, events
        void updateTextEffects(double dt);
        // writes the effect (or the plain text when !effect.playing) into the text's vertices;
        // revealed characters go to `revealedEvents`, a completed cycle sets `completed`
        void applyTextEffect(Entity entity, TextEffectComponent& effect, TextComponent& text, UIComponent& ui,
                             std::vector<int>& revealedEvents, bool& completed);
        float anchorReferenceWidth;
        float anchorReferenceHeight;

        bool pendingImages; // an image waited for its texture in the last update

        // Canvas Groups (syncCanvasGroups)
        struct CanvasGroupEntry{
            float alpha = 1.0f;         // the chain's product (Per Element)
            float layerAlpha = 1.0f;    // relative to the enclosing layered FLATTEN group
            bool interactable = true;
            bool blocks = true;
            bool underRect = false;     // a RectTransform on the entity or above it (UI particles)
            uint8_t layerDepth = 0;     // enclosing layered FLATTEN groups
            Entity group = NULL_ENTITY;
        };
        struct CanvasGroupFade{
            Entity group = NULL_ENTITY;
            Entity tween = NULL_ENTITY;
            float endAlpha = 1.0f;
            bool deactivate = false;
            std::function<void()> onComplete;
            bool finished = false;      // the tween stopped (onCanvasGroupFadeStopped)
            bool completed = false;     // ... at its end (not stopped early)
        };
        bool canvasGroupsStructuralDirty = true;
        uint64_t canvasGroupsRevision = 0;
        // some element may hold a group's state: one reset pass when the last group goes
        bool canvasGroupsApplied = false;
        CanvasGroupStats canvasGroupStats;
        std::vector<CanvasGroupEntry> canvasGroupScratch;
        std::vector<size_t> canvasGroupParents;
        std::vector<Entity> canvasGroupDirty;
        // elements whose effective interactable turned false: their widgets drop presses,
        // focus and gestures in the next update (never inside a draw or a raycast)
        std::vector<Entity> interactabilityLost;
        std::vector<CanvasGroupFade> canvasGroupFades;

        CanvasGroupEntry combineCanvasGroup(const CanvasGroupEntry& parent, Entity entity, Signature signature, CanvasGroupComponent* group, bool warn);
        void applyCanvasGroupEntry(Entity entity, Signature signature, const CanvasGroupEntry& entry);
        bool sanitizeCanvasGroup(Entity entity, CanvasGroupComponent& group);
        void canvasGroupFullPass();
        void canvasGroupSubtreePass(Entity groupEntity);
        void canvasGroupResetPass();
        void processInteractabilityLost();
        void processCanvasGroupFades();
        void finishCanvasGroupFade(CanvasGroupFade fade, bool runCompletion);
        // The hit test of every pointer event, in one place (UISystemPointer.cpp; raycast() is
        // it after syncCanvasGroups): the last element in RectTransform order (children after
        // their parents) that is visible, active in hierarchy with an enabled UIComponent, has
        // an Image, contains the point (clipped to its panel), does not ignore events and whose
        // Canvas Groups block raycasts. panelHit: the panel the press brings to front (the
        // element's panel, or a Panel it is in).
        Entity raycastHit(const Vector2& point, Entity* panelHit = nullptr) const;

        void createOrUpdateUiComponent(RectTransformComponent& layout, Entity entity, Signature signature);
        void getPanelEdges(const PanelComponent& panel, const RectTransformComponent& layout, const Transform& transform, const RectTransformComponent& headerlayout,  Rect& edgeRight, Rect& edgeRightBottom, Rect& edgeBottom, Rect& edgeLeftBottom, Rect& edgeLeft);
        Rect fitOnPanel(Rect uiRect, Entity parentPanel);
        void calculateUIAABB(UIComponent& ui);

        //Image
        bool createImagePatches(ImageComponent& img, UIComponent& ui, RectTransformComponent& layout);

        // Text
        // outline/shadow shader parameters of an SDF text (UIComponent::textParams)
        void updateTextShaderParams(TextComponent& text, UIComponent& ui);
        void hideTextGlyphsAfter(TextComponent& text, UIComponent& ui, size_t count);
        std::string getFontTextureId(const TextComponent& text) const;
        bool isFontAtlasStale(const TextComponent& text) const;
        void syncFontAtlas(TextComponent& text, UIComponent& ui);
        bool loadFontAtlas(TextComponent& text, UIComponent& ui, RectTransformComponent& layout);
        void createText(TextComponent& text, UIComponent& ui, RectTransformComponent& layout);
        // the pivot, clip, visible characters and indices of a text's glyph quads (pivotStep > 0:
        // a bitmap text's font pixel, the pivot offsets stay whole font pixels)
        void finishTextGeometry(TextComponent& text, UIComponent& ui, RectTransformComponent& layout, float ascent, float pivotStep,
                                bool fillColors, std::vector<uint16_t>& indices);
        // bitmap fonts (S51, UISystemBitmapText.cpp)
        bool loadBitmapFont(TextComponent& text, UIComponent& ui);
        void createBitmapText(TextComponent& text, UIComponent& ui, RectTransformComponent& layout);
        // per frame: a reloaded font, a new scale or style, a recycled cache page, a new colour
        void updateBitmapText(TextComponent& text, UIComponent& ui);

        //Button
        // the state shown: Unity's priority, Disabled > Pressed > Selected > Highlighted > Normal
        SelectionState buttonState(const ButtonComponent& button, const UIComponent& ui) const;
        // writes what the transition owns for the current state; animate eases colour and scale
        // over fadeDuration (the Transform's scale is written by the update only)
        void applyButtonVisual(Entity entity, ButtonComponent& button, UIComponent& ui, bool animate);
        void updateButton(Entity entity, ButtonComponent& button, ImageComponent& img, UIComponent& ui, RectTransformComponent& layout);
        // the running colour and scale fades (UI clock); nothing when none runs
        void updateButtonTransitions();
        void applyButtonScale(Entity entity, ButtonComponent& button, float factor);
        // every fade ends at its state and every scale factor goes back to 1
        void endButtonTransitions();
        // ---- Selectables (UISystemNavigation.cpp, ui-components spec 6.4): Unity's EventSystem
        // selection of this scene
        Entity selected = NULL_ENTITY;
        // the earlier selections, oldest first: a lost selection returns to the latest one
        // still navigable (a closed submenu gives it back to its button)
        std::vector<Entity> selectionHistory;
        // the Button a Submit holds pressed, and which key or pad button holds it
        Entity submitButton = NULL_ENTITY;
        int submitSource = 0;
        // a setSelected inside onSelect / onDeselect wins over the one that sent them
        unsigned int selectionRequest = 0;
        // the selected GameObject was destroyed (onComponentRemoved): the history is asked next
        bool selectionDestroyed = false;
        std::vector<Entity> transitioningButtons;
        // the editor's edit mode: no selection, no scale
        bool editMode = false;

        void changeSelection(Entity next, bool animate);
        // the selection is no longer selectable: the latest navigable one of the history, or none
        void loseSelection();
        // the rect navigation measures (the hit test's: world rect, clipped to its panel and the canvas)
        Rect navRect(Entity entity) const;
        // the navigable Buttons in Hierarchy order
        std::vector<Entity> navigationCandidates() const;
        Entity findAutomatic(Entity from, MoveDirection dir, bool wrap) const;
        // the selection messages; hierarchy: up the parents until one handles it. Returns whether
        // a script handled it; used: whether one called use()
        bool sendSelectionMessage(Entity target, ScriptMessage m, MoveDirection dir, bool hierarchy, bool* used);
        // Select On Enable (the Button the update's loop picked) and the selection's loss, once per update
        void updateSelection(Entity pickedOnEnable);

        //Panel
        void updatePanel(Entity entity, PanelComponent& panel, ImageComponent& img, UIComponent& ui, RectTransformComponent& layout);

        //Scrollbar
        void updateScrollbar(ScrollbarComponent& scrollbar, RectTransformComponent& layout);

        //Progressbar
        void updateProgressbar(Entity entity, ProgressbarComponent& progressbar, ImageComponent& img, UIComponent& ui, RectTransformComponent& layout);

        //InputField
        void updateInputField(Entity entity, InputFieldComponent& inputfield, ImageComponent& img, UIComponent& ui, RectTransformComponent& layout);
        void blinkCursorInputField(double dt, InputFieldComponent& inputfield, UIComponent& ui);
        bool handleInputFieldCharInput(InputFieldComponent& inputfield, TextComponent& text, UIComponent& ui, char32_t codepoint);
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
        Entity executeHierarchy(int pointerId, Entity from, ScriptMessage m,
                                const std::function<void(PointerEventData&)>& adjust = nullptr);
        // ExecuteEvents.Execute: the scripts of `target`; returns whether any handled it
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
        // callbacks false: the state, sources and visuals only (a scene being removed)
        void stickRelease(Entity entity, bool callbacks = true);
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
        // a Safe Area component's anchors: the safe area relative to its anchor reference
        void applySafeArea(SafeAreaComponent& safeArea, RectTransformComponent& layout, const Transform& transform);
        void changeFlipY(UIComponent& ui, CameraComponent& camera);

        void destroyText(TextComponent& text);
        void destroyButton(ButtonComponent& button);

        bool isEntityChild(Entity child, Entity parent) const;

        // game canvas pixels (pointer events) to this scene's UI units
        void toUISpace(float& x, float& y) const;

    public:
        // the atlas a text needs now: its raster scale (Canvas Scaler) and, with an outline or a
        // shadow, an SDF padding
        STBTextOptions getTextOptions(const TextComponent& text) const;
        std::string getFontId(const TextComponent& text) const;
        std::string getFontId(const TextComponent& text, const STBTextOptions& options) const;
        // An anchored element's offset is measured in its nearest layout parent's space,
        // its position in its own parent's space
        Vector2 parentToLayoutSpace(Entity entity, const Vector2& point) const;
        Vector2 layoutToParentSpace(Entity entity, const Vector2& point) const;

        UISystem(Scene* scene);
        virtual ~UISystem();

        Rect getAnchorReferenceRect(const RectTransformComponent& layout, const Transform& transform, bool worldPosition);

        // the editor lays a scene out at a size of its choosing (0 = none set)
        void setAnchorReferenceSize(float width, float height);
        void clearAnchorReferenceSize();
        float getAnchorReferenceWidth() const;
        float getAnchorReferenceHeight() const;
        // The size root elements anchor to, in UI units: the reference size above when set,
        // else the scene's Canvas Scaler UI size (Scene::isUIScaled), else the Engine canvas
        Vector2 getCanvasSize() const;
        // the scale glyphs rasterize at: view pixels per UI unit when the Canvas Scaler
        // scales this scene (crisp text), else 1 (the text's own pixel size, as before)
        float getTextRasterScale() const;
        // S51 R7-R8: units per font pixel of a bitmap Text at pixel scale k: k / PPU for world
        // text under a Pixels Per Unit > 1, else d / s with d = max(1, round(k * s)) device pixels
        // (s the canvas raster scale), so every font pixel is whole device pixels
        float getBitmapTextUnit(int pixelScale) const;

        // UI.reducedMotion: Text Effects skip their motion layers (wave, bounce, shake, pulse,
        // grow, unfold) and keep colour and reveal ones. Global, default false.
        static void setReducedMotion(bool reducedMotion);
        static bool isReducedMotion();

        // the editor's edit mode: Text Effects hold still unless the Inspector previews them
        void setTextEffectsHeld(bool held);
        bool isTextEffectsHeld() const;

        bool isInputFieldFocused();

        // the editor's edit mode and Play pause (Project::pauseEngineScene): Select On Enable and
        // pointer selection wait, scale transitions do not apply
        void setEditMode(bool editMode);

        // ---- Selectables (ui-components spec 6.4): Unity's EventSystem.currentSelectedGameObject
        Entity getSelected() const;
        // any selectable Button (interactable or not, any navigation mode, as Unity's Select());
        // NULL_ENTITY deselects and forgets the history. False when it cannot be selected
        bool setSelected(Entity entity);
        // the selection is live (a lost one is first replaced from the history, which may call
        // onSelect)
        bool hasLiveSelection();
        // the same without replacing a lost one (queries: Engine::wantsGamepadNavigation)
        bool hasSelection() const;
        // a focused InputField that takes typing (active, interactable): the typing guard
        bool isInputFieldTyping() const;
        // a live Button that is not an on-screen control
        bool isSelectable(Entity entity) const;
        // selectable, navigation mode not None, interactable, with a non-empty rect on the canvas
        bool isNavigable(Entity entity) const;
        // its navigation mode's target that way (Unity's FindSelectableOnUp...); NULL_ENTITY: none
        Entity findSelectableOnDirection(Entity from, MoveDirection dir) const;
        // the next (or previous) navigable Button in Hierarchy order, wrapping (Tab)
        Entity findNextSelectable(Entity from, bool backwards) const;
        // Unity's Button state of a Button (also while nothing redraws it)
        SelectionState getButtonState(Entity entity) const;
        // a Button's transition writes the UI colour / texture (the Inspector, MCP and Lua redirect
        // or refuse edits of what it overwrites)
        static bool buttonOwnsUIColor(const ButtonComponent& button);
        static bool buttonOwnsUITexture(const ButtonComponent& button);
        static bool transitionWritesColor(ButtonTransition transition);
        static bool transitionWritesTexture(ButtonTransition transition);

        // keyboard, gamepad and TV input of a live selection (Engine routes them); each returns
        // whether it consumed the input
        bool eventOnMove(MoveDirection dir);
        bool eventOnTab(bool backwards);
        // source: the key, or 1000 + pad * 32 + button; a second source while one holds is consumed
        bool eventOnSubmitDown(int source);
        bool eventOnSubmitUp(int source);
        bool eventOnCancel();
        // a Submit's hold ends without a click (onRelease)
        void cancelSubmit();
        // the editor's Play stop (the played scene's snapshot was restored over these Buttons): no
        // selection, no history, every Button's runtime state reset and its authored look applied
        void resetSelection();
        // a primary pointer press (eventOnPointerDown): with a selection, a Button (or its label)
        // is selected, an InputField keeps it, anything else deselects
        void pointerSelects(Entity hit);

        // an image is still waiting for its texture to build its geometry
        bool hasPendingImages() const;

        Vector2 getTextMinSize(TextComponent& text);

        // The scene leaves the layers: every pointer state goes without callbacks, buttons
        // unpress, the on-screen controls release and clear their input sources
        void resetButtonStates();

        // one whole character (Engine::systemCharInput combines UTF-16 pairs)
        bool eventOnCharInput(char32_t codepoint);
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

        // The topmost element under the point a pointer event hits: the last Image in Hierarchy
        // order that contains it, is visible, active, not ignoreEvents and whose Canvas Groups
        // block raycasts (ui canvas group spec 7.4); NULL_ENTITY over nothing
        Entity raycast(Vector2 point);

        // Canvas Groups (ui canvas group spec 7.2). Idempotent, O(number of groups) when nothing
        // changed: a structural change (hierarchy, a group, UI element or Transform added or
        // removed, a scene load) runs one pass over the Transforms, a changed group one pass
        // over its branch. Called by update, draw and every pointer entry point
        void syncCanvasGroups();
        // The chain walked on demand (O(depth)): exact even before the next sync
        CanvasGroupState getCanvasGroupState(Entity entity) const;
        // Unity Selectable.IsInteractable: the groups allow it and the widget's own flag
        // (Button / InputField disabled) is off; the groups' state of the last sync
        bool isInteractable(Entity entity) const;
        const CanvasGroupStats& getCanvasGroupStats() const;
        void resetCanvasGroupStats();

        // Fade helpers (CanvasGroup::fadeTo): `tween` is an AlphaTween on `group`, owned here. A
        // new fade on a group stops its previous one. When the tween finishes, the update
        // destroys it, then calls onComplete, then deactivates the group's GameObject when asked.
        // Destroying the group (or the tween) or unloading the scene drops the fade unfinished
        void startCanvasGroupFade(Entity group, Entity tween, float endAlpha, bool deactivate, std::function<void()> onComplete);
        // jumpToEnd: the end alpha is written and the fade completes now (onComplete, deactivate)
        void stopCanvasGroupFade(Entity group, bool jumpToEnd);
        bool isCanvasGroupFading(Entity group) const;
        // the AlphaTween's ActionComponent stopped (subscribed by startCanvasGroupFade)
        void onCanvasGroupFadeStopped(Entity tween, bool atEnd);

        // basic UIs
        bool createOrUpdatePolygon(PolygonComponent& polygon, UIComponent& ui, RectTransformComponent& layout);
        bool createOrUpdateImage(ImageComponent& img, UIComponent& ui, RectTransformComponent& layout);
        // the canvas scale or the outline/shadow asks for another atlas: flags its rebuild
        void checkTextAtlasOptions(TextComponent& text, UIComponent& ui);
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

        void onComponentAdded(Entity entity, ComponentId componentId) override;
        void onComponentRemoved(Entity entity, ComponentId componentId) override;
    };

}

#endif //UISYSTEM_H