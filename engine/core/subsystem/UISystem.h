// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef UISYSTEM_H
#define UISYSTEM_H

#include "SubSystem.h"
#include "System.h"

#include <cstdint>
#include <functional>
#include <vector>

#include "component/RectTransformComponent.h"
#include "component/LayoutGroupComponent.h"
#include "component/SafeAreaComponent.h"
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
#include "component/CanvasGroupComponent.h"
#include "component/PointsComponent.h"

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

    class JASPERENGINE_API UISystem : public SubSystem {

    private:

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
        Entity raycastHit(float x, float y);

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
        // a Safe Area component's anchors: the safe area relative to its anchor reference
        void applySafeArea(SafeAreaComponent& safeArea, RectTransformComponent& layout, const Transform& transform);
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

        void resetButtonStates();

        bool eventOnCharInput(wchar_t codepoint);
        bool eventOnKeyDown(int key, bool repeat, int mods);
        bool eventOnPointerDown(float x, float y);
        bool eventOnPointerUp(float x, float y);
        bool eventOnPointerMove(float x, float y);
        // the last pointer event was over a UI element (hovered or pressed): documents on
        // meshes behind it do not get the pointer (HTML UI, Engine::systemMouseMove)
        bool isPointerOverUI() const;

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