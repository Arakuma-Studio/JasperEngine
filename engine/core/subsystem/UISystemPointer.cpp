// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The component UI's EventSystem (ui-multitouch spec 7.1-7.4): every pointer (the mouse's
// buttons, each finger) has its own state, hover, press, drag and click; the Unity script
// callbacks (onPointerDown(e) ...) go up the hierarchy like ExecuteEvents.ExecuteHierarchy; the
// legacy UIComponent callbacks (x, y) and the built-in widgets (Button, Panel, Scrollbar,
// InputField) follow the pointer that pressed them; the on-screen controls feed Input actions.

#include "UISystem.h"
#include "component/ButtonComponent.h"
#include "component/ImageComponent.h"
#include "component/InputFieldComponent.h"
#include "component/OnScreenControlComponents.h"
#include "component/PanelComponent.h"
#include "component/ScrollbarComponent.h"
#include "component/TextComponent.h"
#include "component/Transform.h"
#include "component/UIComponent.h"

#include "Scene.h"
#include "Engine.h"
#include "Input.h"
#include "System.h"
#include "input/InputActions.h"
#include "script/ScriptMessages.h"
#include "script/ScriptRuntime.h"
#include "script/LuaBinding.h"
#include "util/StringUtils.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <functional>

using namespace jasperengine;

const PointerEventData* UISystem::currentLegacyEvent = nullptr;

static constexpr float UI_DRAG_START_DISTANCE = 4.0f;
static constexpr double UI_DOUBLE_CLICK_TIME = 0.3;

namespace {

    bool isMousePointer(int pointerId){
        return pointerId < 0;
    }

    void callCppPointerMessage(ScriptBase& script, ScriptMessage m, PointerEventData& e){
        switch (m){
            case ScriptMessage::PointerDown: script.onPointerDown(e); break;
            case ScriptMessage::PointerUp: script.onPointerUp(e); break;
            case ScriptMessage::PointerClick: script.onPointerClick(e); break;
            case ScriptMessage::PointerEnter: script.onPointerEnter(e); break;
            case ScriptMessage::PointerExit: script.onPointerExit(e); break;
            case ScriptMessage::InitializePotentialDrag: script.onInitializePotentialDrag(e); break;
            case ScriptMessage::BeginDrag: script.onBeginDrag(e); break;
            case ScriptMessage::Drag: script.onDrag(e); break;
            case ScriptMessage::EndDrag: script.onEndDrag(e); break;
            case ScriptMessage::Drop: script.onDrop(e); break;
            case ScriptMessage::Scroll: script.onScroll(e); break;
            default: break;
        }
    }

    bool containsEntity(const std::vector<Entity>& list, Entity entity){
        return std::find(list.begin(), list.end(), entity) != list.end();
    }

    // Joystick Pack's SnapFloat (Joystick.cs): with both axes, the angle to up decides
    float snapAxis(float value, const Vector2& inputUp, bool horizontal, bool bothAxes){
        if (value == 0.0f)
            return value;
        if (bothAxes){
            const float length = inputUp.length();
            float angle = 0.0f;
            if (length > 0.0f){
                float cosine = inputUp.y / length;
                cosine = std::max(-1.0f, std::min(1.0f, cosine));
                angle = std::acos(cosine) * 180.0f / 3.14159265358979f;
            }
            if (horizontal){
                if (angle < 22.5f || angle > 157.5f)
                    return 0.0f;
                return (value > 0.0f) ? 1.0f : -1.0f;
            }
            if (angle > 67.5f && angle < 112.5f)
                return 0.0f;
            return (value > 0.0f) ? 1.0f : -1.0f;
        }
        return (value > 0.0f) ? 1.0f : -1.0f;
    }

}

// ---- the hit test

Entity UISystem::raycastHit(const Vector2& point, Entity* panelHit) const{
    Entity hit = NULL_ENTITY;
    Entity panel = NULL_ENTITY;

    auto layouts = scene->getComponentArray<RectTransformComponent>();
    for (int i = 0; i < layouts->size(); i++){
        const RectTransformComponent& layout = layouts->getComponentFromIndex(i);
        const Entity entity = layouts->getEntity(i);
        const Signature signature = scene->getSignature(entity);
        if (!signature.test(scene->getComponentId<Transform>()) || !signature.test(scene->getComponentId<ImageComponent>()))
            continue;
        const Transform& transform = scene->getComponent<Transform>(entity);
        if (!transform.visible || !isUIActive(entity, signature) || layout.ignoreEvents)
            continue;
        // a Canvas Group that does not block raycasts lets the pointer through (ui-canvas-group 7.4)
        if (signature.test(scene->getComponentId<UIComponent>()) && !scene->getComponent<UIComponent>(entity).groupBlocksRaycasts)
            continue;

        Rect uirect(transform.worldPosition.x, transform.worldPosition.y, layout.width * transform.worldScale.x, layout.height * transform.worldScale.y);
        if (layout.panel != NULL_ENTITY)
            uirect = const_cast<UISystem*>(this)->fitOnPanel(uirect, layout.panel);

        //TODO: inside to polygon
        if (uirect.contains(point)){
            hit = entity;
            panel = layout.panel;
            if (signature.test(scene->getComponentId<PanelComponent>()))
                panel = entity;
        }
    }

    if (panelHit)
        *panelHit = panel;
    return isLiveUIEntity(hit) ? hit : NULL_ENTITY;
}

// ---- pointer states

UIPointerState* UISystem::findPointer(int pointerId){
    auto it = pointers.find(pointerId);
    return (it != pointers.end()) ? &it->second : nullptr;
}

Vector2 UISystem::toLocal(Entity entity, const Vector2& point) const{
    const Transform* transform = scene->findComponent<Transform>(entity);
    if (!transform)
        return point;
    return Vector2(point.x - transform->worldPosition.x, point.y - transform->worldPosition.y);
}

PointerEventData UISystem::makeEventData(const UIPointerState& state, Entity target) const{
    PointerEventData e;
    e.pointerId = state.pointerId;
    e.button = state.button;
    e.isTouch = state.touch;
    e.position = state.position;
    e.delta = state.delta;
    e.pressPosition = state.pressPosition;
    e.localPosition = toLocal(target, state.position);
    // ui-scaling-text gives the scene's UI units; without a canvas scaler they are canvas px
    e.uiPosition = state.position;
    e.uiDelta = state.delta;
    e.clickCount = state.clickCount;
    e.clickTime = state.clickTime;
    e.dragging = state.dragging;
    e.eligibleForClick = state.eligibleForClick;
    e.useDragThreshold = state.useDragThreshold;
    e.used = state.used;
    e.pointerEnter = state.hit;
    e.pointerPress = state.press;
    e.rawPointerPress = state.rawPress;
    e.pointerDrag = state.drag;
    e.target = target;
    e.scene = scene;
    e.sceneLifetime = scene->getLifetime();
    return e;
}

// A GameObject handles a message when a script of it defines the method, or (down, up and the
// drag messages) when it is an on-screen control that takes pointers
Entity UISystem::findPointerHandler(Entity from, ScriptMessage m) const{
    const bool controlMessage = m == ScriptMessage::PointerDown || m == ScriptMessage::PointerUp ||
                                m == ScriptMessage::InitializePotentialDrag || m == ScriptMessage::BeginDrag ||
                                m == ScriptMessage::Drag || m == ScriptMessage::EndDrag;
    for (Entity current = from; current != NULL_ENTITY && scene->isEntityCreated(current);){
        if (scene->isEntityActiveInHierarchy(current)){
            if (controlMessage && controlTakesPointers(current, scene->getSignature(current)))
                return current;
            if (ScriptMessages::defines(scene, current, m))
                return current;
        }
        const Transform* transform = scene->findComponent<Transform>(current);
        current = transform ? transform->parent : NULL_ENTITY;
    }
    return NULL_ENTITY;
}

// ExecuteEvents.ExecuteHierarchy: sent from the element up until a GameObject handles it (an
// on-screen control for the control messages, or a script whose method ran: a C++ script that
// does not override it is passed, ScriptMessages tells); the handler, or NULL_ENTITY
Entity UISystem::executeHierarchy(int pointerId, Entity from, ScriptMessage m,
                                  const std::function<void(PointerEventData&)>& adjust){
    for (Entity current = from; current != NULL_ENTITY && scene->isEntityCreated(current);){
        const Entity handler = findPointerHandler(current, m);
        if (handler == NULL_ENTITY)
            return NULL_ENTITY;
        UIPointerState* state = findPointer(pointerId);
        if (!state)
            return NULL_ENTITY;
        const bool control = controlTakesPointers(handler, scene->getSignature(handler));
        if (sendPointerMessage(*state, handler, m, adjust) || control)
            return handler;
        const Transform* transform = scene->findComponent<Transform>(handler);
        current = transform ? transform->parent : NULL_ENTITY;
    }
    return NULL_ENTITY;
}

bool UISystem::sendPointerMessage(UIPointerState& state, Entity target, ScriptMessage m,
                                  const std::function<void(PointerEventData&)>& adjust){
    if (target == NULL_ENTITY || !scene->isEntityCreated(target))
        return false;

    PointerEventData data = makeEventData(state, target);
    if (adjust)
        adjust(data);

    // every Lua script of the GameObject gets the same event object, read back after the calls
    lua_State* L = LuaBinding::getLuaState();
    int luaRef = LUA_NOREF;
    PointerEventData* luaData = nullptr;
    const int called = ScriptRuntime::sendMessage(scene, target, m,
        [&](lua_State* Lpush) -> int {
            if (luaRef == LUA_NOREF){
                if (!luabridge::push(Lpush, data))
                    lua_pushnil(Lpush);
                auto pushed = luabridge::get<PointerEventData*>(Lpush, -1);
                luaData = pushed ? pushed.value() : nullptr;
                lua_pushvalue(Lpush, -1);
                luaRef = luaL_ref(Lpush, LUA_REGISTRYINDEX);
            }else{
                lua_rawgeti(Lpush, LUA_REGISTRYINDEX, luaRef);
            }
            return 1;
        },
        [&](ScriptBase& script){ callCppPointerMessage(script, m, data); },
        DeliverRequireActive | DeliverRequireEnabled);

    if (luaData){
        data.used = data.used || luaData->used;
        if (luaData->useDragThreshold != state.useDragThreshold)
            data.useDragThreshold = luaData->useDragThreshold;
    }
    if (L && luaRef != LUA_NOREF)
        luaL_unref(L, LUA_REGISTRYINDEX, luaRef);

    if (data.used)
        state.used = true;
    if (m == ScriptMessage::InitializePotentialDrag)
        state.useDragThreshold = data.useDragThreshold;
    return called > 0;
}

void UISystem::callLegacy(FunctionSubscribe<void(float, float)> UIComponent::* event, Entity entity, const UIPointerState& state){
    UIComponent* ui = scene->findComponent<UIComponent>(entity);
    Transform* transform = scene->findComponent<Transform>(entity);
    if (!ui || !transform)
        return;
    const PointerEventData data = makeEventData(state, entity);
    const PointerEventData* previous = currentLegacyEvent;
    currentLegacyEvent = &data;
    (ui->*event).call(state.position.x - transform->worldPosition.x, state.position.y - transform->worldPosition.y);
    currentLegacyEvent = previous;
}

// Unity's HandlePointerExitAndEnter: exit to the old chain's GameObjects the new one does not
// have (deepest first), then enter to the new ones (root first)
void UISystem::updateEnterChain(UIPointerState& state, Entity newHit){
    std::vector<Entity> chain;
    for (Entity current = newHit; current != NULL_ENTITY && scene->isEntityCreated(current);){
        chain.push_back(current);
        const Transform* transform = scene->findComponent<Transform>(current);
        current = transform ? transform->parent : NULL_ENTITY;
    }
    state.hit = newHit;
    if (chain == state.enterChain)
        return;

    const std::vector<Entity> old = state.enterChain;
    state.enterChain = chain;
    const int pointerId = state.pointerId;

    for (Entity entity : old){
        if (!containsEntity(chain, entity)){
            UIPointerState* current = findPointer(pointerId);
            if (!current) return;
            sendPointerMessage(*current, entity, ScriptMessage::PointerExit);
        }
    }
    for (auto it = chain.rbegin(); it != chain.rend(); ++it){
        if (!containsEntity(old, *it)){
            UIPointerState* current = findPointer(pointerId);
            if (!current) return;
            sendPointerMessage(*current, *it, ScriptMessage::PointerEnter);
        }
    }
}

bool UISystem::isHoveredByOtherPointer(Entity entity, int pointerId) const{
    for (const auto& pair : pointers){
        if (pair.first != pointerId && pair.second.hover == entity)
            return true;
    }
    return false;
}

// The legacy hover (onPointerEnter / onPointerLeave (x, y), Button.hovered): the first pointer
// entering, the last leaving
void UISystem::setLegacyHover(UIPointerState& state, Entity entity){
    if (state.hover == entity)
        return;
    const Entity old = state.hover;
    const int pointerId = state.pointerId;
    state.hover = entity;
    const UIPointerState snapshot = state;

    if (isLiveUIEntity(old) && !isHoveredByOtherPointer(old, pointerId)){
        if (ButtonComponent* button = scene->findComponent<ButtonComponent>(old)){
            button->hovered = false;
            if (UIComponent* ui = scene->findComponent<UIComponent>(old))
                applyButtonVisual(*button, *ui);
        }
        callLegacy(&UIComponent::onPointerLeave, old, snapshot);
    }
    if (isLiveUIEntity(entity) && !isHoveredByOtherPointer(entity, pointerId)){
        if (ButtonComponent* button = scene->findComponent<ButtonComponent>(entity)){
            button->hovered = true;
            if (UIComponent* ui = scene->findComponent<UIComponent>(entity))
                applyButtonVisual(*button, *ui);
        }
        callLegacy(&UIComponent::onPointerEnter, entity, snapshot);
    }
}

// A Button is pressed while a pointer holds it (or its On-Screen Button is pressed): onPress on
// the first, onRelease on the last (D2)
void UISystem::refreshButtonPressed(Entity entity){
    ButtonComponent* button = scene->findComponent<ButtonComponent>(entity);
    UIComponent* ui = scene->findComponent<UIComponent>(entity);
    if (!button || !ui)
        return;
    bool held = false;
    for (const auto& pair : pointers){
        if (pair.second.buttonPressed == entity)
            held = true;
    }
    if (const OnScreenButtonComponent* control = scene->findComponent<OnScreenButtonComponent>(entity))
        held = held || control->pressed;
    if (held == button->pressed)
        return;
    button->pressed = held;
    applyButtonVisual(*button, *ui);
    if (held)
        button->onPress.call();
    else
        button->onRelease.call();
}

bool UISystem::isWidgetGestureOwned(Entity entity, int exceptPointer) const{
    for (const auto& pair : pointers){
        if (pair.first == exceptPointer)
            continue;
        const UIPointerState& other = pair.second;
        if (other.panelGesture == entity || other.scrollbar == entity || other.inputFieldSelecting == entity)
            return true;
    }
    return false;
}

// A pressed, dragged or held element that went (destroyed, deactivated, its UIComponent
// disabled): the press is cancelled while the element still exists (D6)
void UISystem::validatePointers(){
    std::vector<int> cancelled;
    std::vector<std::pair<Entity, int>> releasedEntered;
    for (auto& pair : pointers){
        UIPointerState& state = pair.second;
        if (!state.down)
            continue;
        // an entered On-Screen Button that cannot take pointers any more lets go of this one only
        if (state.enteredButton != NULL_ENTITY){
            const Entity entered = state.enteredButton;
            if (!scene->isEntityCreated(entered) || !controlTakesPointers(entered, scene->getSignature(entered))){
                state.enteredButton = NULL_ENTITY;
                if (scene->isEntityCreated(entered))
                    releasedEntered.push_back({entered, pair.first});
            }
        }
        bool lost = false;
        for (Entity entity : {state.widget, state.press, state.drag, state.control, state.buttonPressed}){
            if (entity == NULL_ENTITY)
                continue;
            if (!scene->isEntityCreated(entity)){
                lost = true;
                continue;
            }
            const Signature signature = scene->getSignature(entity);
            if (!scene->isEntityActiveInHierarchy(entity))
                lost = true;
            else if (entity == state.widget && signature.test(scene->getComponentId<UIComponent>()) && !isUIActive(entity, signature))
                lost = true;
            else if (entity == state.control && !controlTakesPointers(entity, signature))
                lost = true;
            else if (entity == state.buttonPressed && !isInteractable(entity))
                lost = true;   // Button.disabled (or its Canvas Group) set mid-press
        }
        if (lost)
            cancelled.push_back(pair.first);
    }
    for (const auto& entered : releasedEntered)
        buttonControlRelease(entered.first, entered.second);
    for (int pointerId : cancelled)
        endPress(pointerId, false);
}

// ---- legacy callbacks and built-in widgets of the pressed element

// The built-in handling of a press on the hit element (before multi-touch: pointerDownOnUI).
// Every user callback here can create or destroy entities, which invalidates the component
// references
void UISystem::legacyPointerDown(UIPointerState& state){
    const Entity entity = state.widget;
    const int pointerId = state.pointerId;
    const float x = state.position.x;
    const float y = state.position.y;
    if (!isLiveUIEntity(entity))
        return;

    Signature signature = scene->getSignature(entity);
    UIComponent* ui = scene->findComponent<UIComponent>(entity);
    ui->pointerMoved = false;

    // a Button: this pointer holds it (the first one presses it)
    if (signature.test(scene->getComponentId<ButtonComponent>())){
        ButtonComponent* button = scene->findComponent<ButtonComponent>(entity);
        if (button && isInteractable(entity)){
            state.buttonPressed = entity;
            refreshButtonPressed(entity);
            UIPointerState* current = findPointer(pointerId);
            if (!current || !scene->isEntityCreated(entity))
                return;
            signature = scene->getSignature(entity);
        }
    }

    UIPointerState* current = findPointer(pointerId);
    Transform* transform = scene->findComponent<Transform>(entity);
    RectTransformComponent* layout = scene->findComponent<RectTransformComponent>(entity);
    if (!current || !transform || !layout)
        return;

    // a non-interactable widget (its own flag or a Canvas Group) takes no gesture and no focus
    const bool interactable = isInteractable(entity);
    const bool inputFieldBlocked = signature.test(scene->getComponentId<InputFieldComponent>()) && !interactable;

    if (signature.test(scene->getComponentId<InputFieldComponent>())){
        InputFieldComponent& inputfield = scene->getComponent<InputFieldComponent>(entity);
        if (interactable && !isWidgetGestureOwned(entity, pointerId)){
            TextComponent* text = scene->findComponent<TextComponent>(inputfield.text);
            ImageComponent* img = scene->findComponent<ImageComponent>(entity);
            if (text && img){
                float localX = x - transform->worldPosition.x;
                bool extendSelection = (Input::getModifiers() & D_MODIFIER_SHIFT) != 0;
                if (text->text.empty()){
                    inputfield.cursorIndex = 0;
                    if (!extendSelection){
                        inputfield.selectionAnchor = 0;
                    }
                    inputFieldResetBlink(inputfield);
                    inputfield.needUpdateInputField = true;
                }else{
                    setInputFieldCursorFromLocalX(inputfield, *text, *img, localX, extendSelection);
                }
                current->inputFieldSelecting = entity;

                bool hadInvalid = false;
                std::wstring wideText = StringUtils::utf8ToWString(text->text, hadInvalid);
                System::instance().showVirtualKeyboard(wideText);
            }
        }
    }else{
        System::instance().hideVirtualKeyboard();
    }

    // a Scrollbar: its bar follows this pointer only (D2)
    if (interactable && signature.test(scene->getComponentId<ScrollbarComponent>()) && !isWidgetGestureOwned(entity, pointerId)){
        ScrollbarComponent& scrollbar = scene->getComponent<ScrollbarComponent>(entity);
        if (scrollbar.bar != NULL_ENTITY &&
            scene->findComponent<Transform>(scrollbar.bar) &&
            scene->findComponent<RectTransformComponent>(scrollbar.bar)){
            Transform& bartransform = scene->getComponent<Transform>(scrollbar.bar);
            RectTransformComponent& barlayout = scene->getComponent<RectTransformComponent>(scrollbar.bar);

            if (isCoordInside(x, y, bartransform, barlayout)){
                scrollbar.barPointerDown = true;
                current->scrollbar = entity;
                current->internalGesture = true;
                if (scrollbar.type == ScrollbarType::VERTICAL){
                    scrollbar.barPointerPos = y - transform->worldPosition.y - (bartransform.position.y * bartransform.worldScale.y);
                }else if (scrollbar.type == ScrollbarType::HORIZONTAL){
                    scrollbar.barPointerPos = x - transform->worldPosition.x - (bartransform.position.x * bartransform.worldScale.x);
                }
            }else if (scrollbar.barSize < 1.0){
                scrollbar.barPointerDown = true;
                current->scrollbar = entity;
                current->internalGesture = true;
                if (scrollbar.type == ScrollbarType::VERTICAL){
                    float innerHeight = std::max(0.0f, (float)layout->height - scrollbar.barMarginTop - scrollbar.barMarginBottom);
                    scrollbar.barPointerPos = innerHeight * scrollbar.barSize / 2.0f;
                }else if (scrollbar.type == ScrollbarType::HORIZONTAL){
                    float innerWidth = std::max(0.0f, (float)layout->width - scrollbar.barMarginLeft - scrollbar.barMarginRight);
                    scrollbar.barPointerPos = innerWidth * scrollbar.barSize / 2.0f;
                }
                applyScrollbarPointerPosition(entity, x, y);
            }
        }
    }

    current = findPointer(pointerId);
    if (!current || !scene->isEntityCreated(entity))
        return;
    signature = scene->getSignature(entity);
    if (!signature.test(scene->getComponentId<Transform>()) ||
        !signature.test(scene->getComponentId<RectTransformComponent>()) ||
        !signature.test(scene->getComponentId<UIComponent>()))
        return;
    transform = &scene->getComponent<Transform>(entity);
    layout = &scene->getComponent<RectTransformComponent>(entity);

    // a Panel: moved or resized by this pointer only (D2)
    if (interactable && signature.test(scene->getComponentId<PanelComponent>()) && !isWidgetGestureOwned(entity, pointerId)){
        PanelComponent& panel = scene->getComponent<PanelComponent>(entity);
        RectTransformComponent defaultHeaderLayout;
        const RectTransformComponent& headerlayout = panel.headercontainer != NULL_ENTITY && scene->findComponent<RectTransformComponent>(panel.headercontainer)
            ? scene->getComponent<RectTransformComponent>(panel.headercontainer)
            : defaultHeaderLayout;

        Rect edgeRight;
        Rect edgeRightBottom;
        Rect edgeBottom;
        Rect edgeLeftBottom;
        Rect edgeLeft;
        getPanelEdges(panel, *layout, *transform, headerlayout, edgeRight, edgeRightBottom, edgeBottom, edgeLeftBottom, edgeLeft);

        if (panel.canResize){
            current->panelSizeAcc = Vector2(0, 0);
            if (edgeRight.contains(Vector2(x, y))){
                panel.edgePointerDown = PanelEdge::RIGHT;
            }else if (edgeRightBottom.contains(Vector2(x, y))){
                panel.edgePointerDown = PanelEdge::RIGHT_BOTTOM;
            }else if (edgeBottom.contains(Vector2(x, y))){
                panel.edgePointerDown = PanelEdge::BOTTOM;
            }else if (edgeLeftBottom.contains(Vector2(x, y))){
                panel.edgePointerDown = PanelEdge::LEFT_BOTTOM;
            }else if (edgeLeft.contains(Vector2(x, y))){
                panel.edgePointerDown = PanelEdge::LEFT;
            }else{
                panel.edgePointerDown = PanelEdge::NONE;
            }

            if (panel.edgePointerDown != PanelEdge::NONE){
                current->panelGesture = entity;
                current->internalGesture = true;
            }
        }

        if (panel.canMove && panel.headercontainer != NULL_ENTITY){
            Transform* headertransform = scene->findComponent<Transform>(panel.headercontainer);
            RectTransformComponent* headerlayoutPtr = scene->findComponent<RectTransformComponent>(panel.headercontainer);
            if (headertransform && headerlayoutPtr && isCoordInside(x, y, *headertransform, *headerlayoutPtr)){
                panel.headerPointerDown = true;
                current->panelGesture = entity;
                current->internalGesture = true;
            }
        }
    }

    callLegacy(&UIComponent::onPointerDown, entity, *current);

    ui = scene->findComponent<UIComponent>(entity);
    if (!ui)
        return;

    if (!ui->focused && !inputFieldBlocked){
        ui->focused = true;
        ui->onGetFocus.call();
        ui = scene->findComponent<UIComponent>(entity);
        if (!ui)
            return;
    }

    if (scene->findComponent<InputFieldComponent>(entity))
        scene->getComponent<InputFieldComponent>(entity).needUpdateInputField = true;
}

// Every user callback here can create or destroy entities, which invalidates the component
// references, so they are fetched again after each call
void UISystem::legacyPointerMove(UIPointerState& state, CursorType& cursor){
    const Entity entity = state.widget;
    const int pointerId = state.pointerId;
    const float x = state.position.x;
    const float y = state.position.y;
    const Vector2 pointerDiff = state.delta;

    Transform* transform = scene->findComponent<Transform>(entity);
    UIComponent* ui = scene->findComponent<UIComponent>(entity);
    if (!transform || !ui)
        return;

    float localX = x - transform->worldPosition.x;
    float localY = y - transform->worldPosition.y;

    callLegacy(&UIComponent::onPointerMove, entity, state);
    ui = scene->findComponent<UIComponent>(entity);
    UIPointerState* current = findPointer(pointerId);
    if (!ui || !current)
        return;
    ui->pointerMoved = true;

    if (!current->internalGesture){
        if (!current->legacyDragging && Vector2(x, y).squaredDistance(current->pressPosition) >= UI_DRAG_START_DISTANCE * UI_DRAG_START_DISTANCE){
            current->legacyDragging = true;
            callLegacy(&UIComponent::onDragStart, entity, *current);
            ui = scene->findComponent<UIComponent>(entity);
            current = findPointer(pointerId);
            if (!ui || !current)
                return;
        }

        if (current->legacyDragging){
            callLegacy(&UIComponent::onDrag, entity, *current);
            ui = scene->findComponent<UIComponent>(entity);
            current = findPointer(pointerId);
            if (!ui || !current)
                return;
        }

        if (current->inputFieldSelecting == entity){
            InputFieldComponent* inputfield = scene->findComponent<InputFieldComponent>(entity);
            if (inputfield && isInteractable(entity)){
                TextComponent* text = scene->findComponent<TextComponent>(inputfield->text);
                ImageComponent* img = scene->findComponent<ImageComponent>(entity);
                if (text && img){
                    setInputFieldCursorFromLocalX(*inputfield, *text, *img, localX, true);
                }
            }
        }
    }
    (void)localY;

    // scrollbar and panel gestures stop with the widget's interactability
    if (!isInteractable(entity))
        return;

    if (current->scrollbar == entity)
        applyScrollbarPointerPosition(entity, x, y);

    current = findPointer(pointerId);
    transform = scene->findComponent<Transform>(entity);
    RectTransformComponent* layout = scene->findComponent<RectTransformComponent>(entity);
    PanelComponent* panel = scene->findComponent<PanelComponent>(entity);
    if (!current || !transform || !layout || !panel || current->panelGesture != entity)
        return;

    if (panel->headerPointerDown){
        transform->position += Vector3(pointerDiff.x / transform->worldScale.x, pointerDiff.y / transform->worldScale.y, 0);
        transform->needUpdate = true;

        panel->onMove.call();
        current = findPointer(pointerId);
        transform = scene->findComponent<Transform>(entity);
        layout = scene->findComponent<RectTransformComponent>(entity);
        panel = scene->findComponent<PanelComponent>(entity);
        if (!current || !transform || !layout || !panel)
            return;
    }

    if (panel->edgePointerDown != PanelEdge::NONE){
        Vector2& panelSizeAcc = current->panelSizeAcc;
        panelSizeAcc += Vector2(pointerDiff.x / transform->worldScale.x, pointerDiff.y / transform->worldScale.y);
        if (panel->edgePointerDown == PanelEdge::RIGHT){
            layout->width += (int)panelSizeAcc.x;
            layout->needUpdateSizes = true;
            cursor = CursorType::RESIZE_EW;
        }else if (panel->edgePointerDown == PanelEdge::RIGHT_BOTTOM){
            layout->width += (int)panelSizeAcc.x;
            layout->height += (int)panelSizeAcc.y;
            layout->needUpdateSizes = true;
            cursor = CursorType::RESIZE_NWSE;
        }else if (panel->edgePointerDown == PanelEdge::BOTTOM){
            layout->height += (int)panelSizeAcc.y;
            layout->needUpdateSizes = true;
            cursor = CursorType::RESIZE_NS;
        }else if (panel->edgePointerDown == PanelEdge::LEFT_BOTTOM){
            transform->position += Vector3(pointerDiff.x / transform->worldScale.x, 0, 0);
            transform->needUpdate = true;
            layout->width -= (int)panelSizeAcc.x;
            layout->height += (int)panelSizeAcc.y;
            layout->needUpdateSizes = true;
            cursor = CursorType::RESIZE_NESW;
        }else if (panel->edgePointerDown == PanelEdge::LEFT){
            transform->position += Vector3(pointerDiff.x / transform->worldScale.x, 0, 0);
            transform->needUpdate = true;
            layout->width -= (int)panelSizeAcc.x;
            layout->needUpdateSizes = true;
            cursor = CursorType::RESIZE_EW;
        }
        if (layout->width < panel->minWidth){
            layout->width = panel->minWidth;
        }
        if (layout->height < panel->minHeight){
            layout->height = panel->minHeight;
        }
        panelSizeAcc -= Vector2((int)panelSizeAcc.x, (int)panelSizeAcc.y);

        panel->onResize.call(layout->width, layout->height);
    }
}

// The end of this pointer's press of its element: the widgets it held let go, then the legacy
// up (only on the element it pressed, D3), drag end or click, double click
void UISystem::legacyPointerUp(UIPointerState& state, bool released, bool overWidget){
    const Entity entity = state.widget;
    const int pointerId = state.pointerId;

    const Entity buttonEntity = state.buttonPressed;
    state.buttonPressed = NULL_ENTITY;
    if (buttonEntity != NULL_ENTITY)
        refreshButtonPressed(buttonEntity);

    UIPointerState* current = findPointer(pointerId);
    if (!current)
        return;
    if (current->scrollbar != NULL_ENTITY){
        if (ScrollbarComponent* scrollbar = scene->findComponent<ScrollbarComponent>(current->scrollbar))
            scrollbar->barPointerDown = false;
        current->scrollbar = NULL_ENTITY;
    }
    if (current->panelGesture != NULL_ENTITY){
        if (PanelComponent* panel = scene->findComponent<PanelComponent>(current->panelGesture)){
            panel->headerPointerDown = false;
            panel->edgePointerDown = PanelEdge::NONE;
        }
        current->panelGesture = NULL_ENTITY;
    }
    current->inputFieldSelecting = NULL_ENTITY;

    if (!isLiveUIEntity(entity))
        return;

    callLegacy(&UIComponent::onPointerUp, entity, *current);
    current = findPointer(pointerId);
    if (!current || !isLiveUIEntity(entity))
        return;

    if (current->legacyDragging){
        callLegacy(&UIComponent::onDragEnd, entity, *current);
    }else if (released && !current->internalGesture && overWidget){
        bool canClick = true;
        // interactable when released (Unity Button.OnPointerClick)
        if (scene->findComponent<ButtonComponent>(entity))
            canClick = isInteractable(entity);

        if (canClick){
            callLegacy(&UIComponent::onClick, entity, *current);
            current = findPointer(pointerId);
            if (current && isLiveUIEntity(entity)){
                double clickTime = Engine::getSystemTime();
                if (lastUIFromClick == entity && clickTime - lastClickTime <= UI_DOUBLE_CLICK_TIME){
                    callLegacy(&UIComponent::onDoubleClick, entity, *current);
                    lastUIFromClick = NULL_ENTITY;
                    lastClickTime = -1.0;
                }else{
                    lastUIFromClick = entity;
                    lastClickTime = clickTime;
                }
            }
        }
    }
}

void UISystem::applyScrollbarPointerPosition(Entity entity, float x, float y){
    Transform* transform = scene->findComponent<Transform>(entity);
    RectTransformComponent* layout = scene->findComponent<RectTransformComponent>(entity);
    ScrollbarComponent* scrollbar = scene->findComponent<ScrollbarComponent>(entity);
    if (!transform || !layout || !scrollbar || !scrollbar->barPointerDown || scrollbar->barSize >= 1.0)
        return;
    if (!scene->findComponent<Transform>(scrollbar->bar) || !scene->findComponent<RectTransformComponent>(scrollbar->bar))
        return;

    Entity bar = scrollbar->bar;

    float innerHeight = std::max(0.0f, (float)layout->height - scrollbar->barMarginTop - scrollbar->barMarginBottom);
    float innerWidth = std::max(0.0f, (float)layout->width - scrollbar->barMarginLeft - scrollbar->barMarginRight);

    float trackStartNorm = 0;
    float trackEndNorm = 1;
    float halfBarParent = 0;
    float pos = 0;

    if (scrollbar->type == ScrollbarType::VERTICAL){
        float barSizePixel = innerHeight * scrollbar->barSize;
        float barLocalY = (y - transform->worldPosition.y) / transform->worldScale.y;
        float posAlongInner = innerHeight > 0 ? (barLocalY - scrollbar->barMarginTop + (barSizePixel / 2.0) - scrollbar->barPointerPos) / innerHeight : 0;
        trackStartNorm = layout->height > 0 ? scrollbar->barMarginTop / (float)layout->height : 0;
        trackEndNorm = layout->height > 0 ? 1.0f - scrollbar->barMarginBottom / (float)layout->height : 1;
        halfBarParent = layout->height > 0 ? (barSizePixel / 2.0f) / layout->height : 0;
        pos = trackStartNorm + posAlongInner * (trackEndNorm - trackStartNorm);
    }else if (scrollbar->type == ScrollbarType::HORIZONTAL){
        float barSizePixel = innerWidth * scrollbar->barSize;
        float barLocalX = (x - transform->worldPosition.x) / transform->worldScale.x;
        float posAlongInner = innerWidth > 0 ? (barLocalX - scrollbar->barMarginLeft + (barSizePixel / 2.0) - scrollbar->barPointerPos) / innerWidth : 0;
        trackStartNorm = layout->width > 0 ? scrollbar->barMarginLeft / (float)layout->width : 0;
        trackEndNorm = layout->width > 0 ? 1.0f - scrollbar->barMarginRight / (float)layout->width : 1;
        halfBarParent = layout->width > 0 ? (barSizePixel / 2.0f) / layout->width : 0;
        pos = trackStartNorm + posAlongInner * (trackEndNorm - trackStartNorm);
    }

    float movableStart = trackStartNorm + halfBarParent;
    float movableEnd = trackEndNorm - halfBarParent;
    if (pos < movableStart){
        pos = movableStart;
    }else if (pos > movableEnd){
        pos = movableEnd;
    }

    float newStep = movableEnd > movableStart ? (pos - movableStart) / (movableEnd - movableStart) : 0;

    if (newStep != scrollbar->step){
        scrollbar->step = newStep;
        scrollbar->onChange.call(newStep);
        scrollbar = scene->findComponent<ScrollbarComponent>(entity);
    }

    RectTransformComponent* barlayout = scene->findComponent<RectTransformComponent>(bar);
    if (scrollbar && barlayout){
        if (scrollbar->type == ScrollbarType::VERTICAL){
            barlayout->anchorPointTop = pos;
            barlayout->anchorPointBottom = pos;
        }else if (scrollbar->type == ScrollbarType::HORIZONTAL){
            barlayout->anchorPointLeft = pos;
            barlayout->anchorPointRight = pos;
        }
    }
}

// ---- the entry points

bool UISystem::eventOnPointerDown(int pointerId, int button, float x, float y, bool touch){
    // canvas pixels to UI units (the scene's Canvas Scaler; the same point without one)
    toUISpace(x, y);
    // a script may have changed a Canvas Group since the last frame (blocksRaycasts, interactable)
    syncCanvasGroups();
    validatePointers();

    const Vector2 point(x, y);
    // a down without an up (a lost release): that press ends first
    if (UIPointerState* old = findPointer(pointerId)){
        if (old->down)
            endPress(pointerId, false);
    }

    UIPointerState& created = pointers[pointerId];
    created.pointerId = pointerId;
    created.touch = touch;
    created.button = touch ? D_MOUSE_BUTTON_LEFT : button;
    created.delta = Vector2(0, 0);
    created.position = point;
    created.pressPosition = point;
    created.down = true;
    created.captured = false;
    created.dragging = false;
    created.legacyDragging = false;
    created.internalGesture = false;
    created.eligibleForClick = true;
    created.useDragThreshold = true;
    created.used = false;
    created.press = created.rawPress = created.drag = NULL_ENTITY;
    created.widget = NULL_ENTITY;
    created.control = NULL_ENTITY;

    Entity panelHit = NULL_ENTITY;
    const Entity hit = raycastHit(point, &panelHit);
    const bool primary = touch || button == D_MOUSE_BUTTON_LEFT;

    // a press elsewhere takes the keyboard focus from the focused element (left button, a finger)
    if (primary){
        auto layouts = scene->getComponentArray<RectTransformComponent>();
        std::vector<Entity> focused;
        for (int i = 0; i < layouts->size(); i++){
            const Entity entity = layouts->getEntity(i);
            if (UIComponent* ui = scene->findComponent<UIComponent>(entity)){
                if (ui->focused && entity != hit)
                    focused.push_back(entity);
            }
        }
        for (Entity entity : focused){
            UIComponent* ui = scene->findComponent<UIComponent>(entity);
            if (!ui || !ui->focused)
                continue;
            ui->focused = false;
            ui->onLostFocus.call();
            if (InputFieldComponent* inputfield = scene->findComponent<InputFieldComponent>(entity))
                inputfield->needUpdateInputField = true;
        }
    }

    // a finger has no hover: it enters first (D5); the mouse's left button entered on its moves
    UIPointerState* state = findPointer(pointerId);
    if (!state)
        return false;
    if (touch || pointerId == -1){
        updateEnterChain(*state, hit);
    }else{
        state->hit = hit;
        state->enterChain.clear();
    }
    state = findPointer(pointerId);
    if (!state)
        return false;
    if (touch)
        setLegacyHover(*state, hit);
    state = findPointer(pointerId);
    if (!state)
        return false;

    // Unity's ProcessPress: the down handler is pointerPress (else the click handler), the drag
    // handler gets onInitializePotentialDrag
    state->rawPress = hit;
    Entity downHandler = executeHierarchy(pointerId, hit, ScriptMessage::PointerDown);
    state = findPointer(pointerId);
    if (!state)
        return true;
    if (downHandler == NULL_ENTITY){
        downHandler = findPointerHandler(hit, ScriptMessage::PointerClick);
    }

    const double now = Engine::getSystemTime();
    ClickRecord& record = clickRecords[pointerId];
    if (downHandler != NULL_ENTITY && record.element == downHandler && now - record.time < UI_DOUBLE_CLICK_TIME)
        state->clickCount = record.count + 1;
    else
        state->clickCount = 1;
    state->clickTime = now;
    state->press = downHandler;

    Entity dragHandler = findPointerHandler(hit, ScriptMessage::Drag);
    if (dragHandler == NULL_ENTITY)
        dragHandler = findPointerHandler(hit, ScriptMessage::BeginDrag);
    state->drag = dragHandler;
    if (dragHandler != NULL_ENTITY){
        sendPointerMessage(*state, dragHandler, ScriptMessage::InitializePotentialDrag);
        state = findPointer(pointerId);
        if (!state)
            return true;
    }

    // an on-screen control on the way up takes the pointer (D14: the first pointer wins)
    const Entity control = findControl(hit);
    if (control != NULL_ENTITY && primary){
        controlDown(*state, control);
        state = findPointer(pointerId);
        if (!state)
            return true;
    }

    // the built-in widgets and the legacy callbacks: the left button or a finger, not a used down
    if (primary && hit != NULL_ENTITY && !state->used){
        state->widget = hit;
        legacyPointerDown(*state);
        state = findPointer(pointerId);
        if (!state)
            return true;
    }else if (primary && hit == NULL_ENTITY){
        System::instance().hideVirtualKeyboard();
    }

    if (primary){
        if (PanelComponent* panel = scene->findComponent<PanelComponent>(panelHit)){
            // a non-interactable panel is not brought to the front
            if (panel->canBringToFront && isInteractable(panelHit))
                scene->moveChildToTop(panelHit);
        }
    }

    // the left button or a finger on an element is the UI's; the other buttons only when a
    // script took them (a right click on a HUD still reaches the game otherwise)
    state = findPointer(pointerId);
    const bool captured = primary ? hit != NULL_ENTITY : (state && (state->press != NULL_ENTITY || state->drag != NULL_ENTITY));
    if (state)
        state->captured = captured;
    return captured;
}

bool UISystem::eventOnPointerMove(int pointerId, float x, float y, bool touch){
    toUISpace(x, y);
    syncCanvasGroups();
    validatePointers();

    const Vector2 point(x, y);
    // the mouse's other held buttons follow it (one position)
    std::vector<int> moved;
    if (isMousePointer(pointerId) && !touch){
        moved.push_back(-1);
        for (int other : {-2, -3}){
            if (findPointer(other))
                moved.push_back(other);
        }
        if (!findPointer(-1)){
            UIPointerState& mouse = pointers[-1];
            mouse.pointerId = -1;
            mouse.button = D_MOUSE_BUTTON_LEFT;
            mouse.position = point;
        }
    }else{
        // a finger that went down elsewhere (a document, the game) does not hover
        if (!findPointer(pointerId))
            return false;
        moved.push_back(pointerId);
    }

    CursorType cursor = CursorType::ARROW;
    Entity panelHit = NULL_ENTITY;
    const Entity hit = raycastHit(point, &panelHit);
    bool consumed = false;

    for (int id : moved){
        UIPointerState* state = findPointer(id);
        if (!state)
            continue;
        state->delta = point - state->position;
        state->position = point;

        // enter / exit: the left mouse state and the fingers; the legacy hover too
        if (id == -1 || state->touch){
            updateEnterChain(*state, hit);
            state = findPointer(id);
            if (!state) continue;
            if (!state->touch || state->down){
                setLegacyHover(*state, hit);
                state = findPointer(id);
                if (!state) continue;
            }
        }else{
            state->hit = hit;
        }

        if (!state->down)
            continue;

        // an On-Screen Button a finger slides onto or off (spec 12): a finger without a control
        // takes it as its control; one holding a control (a stick, an area, another button)
        // presses it besides (enteredButton)
        if (state->touch){
            const Entity entered = findControl(hit);
            const OnScreenButtonComponent* enteredControl = entered != NULL_ENTITY
                ? scene->findComponent<OnScreenButtonComponent>(entered) : nullptr;
            const bool pressesOnEnter = enteredControl && enteredControl->pressWhenPointerEnters;
            // the entered button left
            if (state->enteredButton != NULL_ENTITY && entered != state->enteredButton){
                const Entity left = state->enteredButton;
                const OnScreenButtonComponent* leftControl = scene->findComponent<OnScreenButtonComponent>(left);
                if (!leftControl || leftControl->releaseWhenPointerLeaves || pressesOnEnter){
                    state->enteredButton = NULL_ENTITY;
                    buttonControlRelease(left, id);
                    state = findPointer(id);
                    if (!state) continue;
                }
            }
            if (pressesOnEnter && entered != state->control && entered != state->enteredButton){
                if (state->control == NULL_ENTITY){
                    controlDown(*state, entered);
                    if (UIPointerState* again = findPointer(id))
                        again->captured = true;
                }else{
                    state->enteredButton = entered;
                    buttonControlPress(entered, id);
                }
                state = findPointer(id);
                if (!state) continue;
            }
        }
        if (state->control != NULL_ENTITY){
            controlMove(*state);
            state = findPointer(id);
            if (!state) continue;
        }

        // Unity's ProcessDrag
        if (state->drag != NULL_ENTITY){
            if (!state->dragging){
                const bool past = !state->useDragThreshold ||
                    point.squaredDistance(state->pressPosition) >= UI_DRAG_START_DISTANCE * UI_DRAG_START_DISTANCE;
                if (past){
                    state->dragging = true;
                    state->eligibleForClick = false;
                    sendPointerMessage(*state, state->drag, ScriptMessage::BeginDrag);
                    state = findPointer(id);
                    if (!state) continue;
                    // the press handler is not the drag handler: it is released (no click)
                    if (state->press != NULL_ENTITY && state->press != state->drag){
                        sendPointerMessage(*state, state->press, ScriptMessage::PointerUp);
                        state = findPointer(id);
                        if (!state) continue;
                        state->press = NULL_ENTITY;
                        state->rawPress = NULL_ENTITY;
                    }
                }
            }
            if (state->dragging && (state->delta.x != 0.0f || state->delta.y != 0.0f)){
                sendPointerMessage(*state, state->drag, ScriptMessage::Drag);
                state = findPointer(id);
                if (!state) continue;
            }
        }

        if (state->widget != NULL_ENTITY && isLiveUIEntity(state->widget)){
            legacyPointerMove(*state, cursor);
            state = findPointer(id);
            if (!state) continue;
        }
        consumed = consumed || state->captured;
    }

    // the mouse's cursor from what it is over
    if (!touch && isLiveUIEntity(hit) && isInteractable(hit)){
        const Signature signature = scene->getSignature(hit);
        if (signature.test(scene->getComponentId<InputFieldComponent>())){
            if (cursor == CursorType::ARROW) cursor = CursorType::IBEAM;
        }else if (signature.test(scene->getComponentId<PanelComponent>()) && cursor == CursorType::ARROW){
            PanelComponent& panel = scene->getComponent<PanelComponent>(hit);
            RectTransformComponent& layout = scene->getComponent<RectTransformComponent>(hit);
            Transform& transform = scene->getComponent<Transform>(hit);
            RectTransformComponent defaultHeaderLayout;
            const RectTransformComponent& headerlayout = panel.headercontainer != NULL_ENTITY && scene->findComponent<RectTransformComponent>(panel.headercontainer)
                ? scene->getComponent<RectTransformComponent>(panel.headercontainer)
                : defaultHeaderLayout;
            Rect edgeRight, edgeRightBottom, edgeBottom, edgeLeftBottom, edgeLeft;
            getPanelEdges(panel, layout, transform, headerlayout, edgeRight, edgeRightBottom, edgeBottom, edgeLeftBottom, edgeLeft);
            if (panel.canResize){
                if (edgeRight.contains(point)) cursor = CursorType::RESIZE_EW;
                else if (edgeRightBottom.contains(point)) cursor = CursorType::RESIZE_NWSE;
                else if (edgeBottom.contains(point)) cursor = CursorType::RESIZE_NS;
                else if (edgeLeftBottom.contains(point)) cursor = CursorType::RESIZE_NESW;
                else if (edgeLeft.contains(point)) cursor = CursorType::RESIZE_EW;
            }
        }
    }
    if (!touch)
        Engine::setMouseCursor(cursor);

    return consumed;
}

bool UISystem::eventOnPointerUp(int pointerId, int button, float x, float y, bool touch){
    (void)button;
    (void)touch;
    toUISpace(x, y);
    syncCanvasGroups();
    validatePointers();

    UIPointerState* state = findPointer(pointerId);
    if (!state || !state->down){
        return false;
    }
    const Vector2 point(x, y);
    state->delta = point - state->position;
    state->position = point;
    state->hit = raycastHit(point);

    const bool consumed = state->captured;
    endPress(pointerId, true);

    if (consumed && !touch)
        Engine::setMouseCursor(CursorType::ARROW);
    return consumed;
}

// Unity's ProcessRelease (released) or the D6 cancel: up to the press handler (canceled), the
// click when the release is over it, the drop and the drag end; the legacy up and the widgets;
// a finger then exits and its state goes
void UISystem::endPress(int pointerId, bool released){
    UIPointerState* state = findPointer(pointerId);
    if (!state || !state->down)
        return;

    const Entity hit = released ? state->hit : NULL_ENTITY;

    // the on-screen control lets go, and a button the finger slid onto besides
    if (state->control != NULL_ENTITY){
        controlUp(*state);
        state = findPointer(pointerId);
        if (!state) return;
    }
    if (state->enteredButton != NULL_ENTITY){
        const Entity entered = state->enteredButton;
        state->enteredButton = NULL_ENTITY;
        buttonControlRelease(entered, pointerId);
        state = findPointer(pointerId);
        if (!state) return;
    }

    if (!released)
        state->eligibleForClick = false;
    auto canceled = [released](PointerEventData& data){ data.canceled = !released; };

    if (state->press != NULL_ENTITY){
        const Entity press = state->press;
        const bool clickable = released && state->eligibleForClick &&
                               findPointerHandler(hit, ScriptMessage::PointerClick) == press;
        sendPointerMessage(*state, press, ScriptMessage::PointerUp, canceled);
        state = findPointer(pointerId);
        if (!state) return;
        if (clickable){
            sendPointerMessage(*state, press, ScriptMessage::PointerClick);
            state = findPointer(pointerId);
            if (!state) return;
            ClickRecord& record = clickRecords[pointerId];
            record.element = press;
            record.time = state->clickTime;
            record.count = state->clickCount;
        }
    }

    if (state->drag != NULL_ENTITY && state->dragging){
        if (released){
            executeHierarchy(pointerId, hit, ScriptMessage::Drop);
            state = findPointer(pointerId);
            if (!state) return;
        }
        sendPointerMessage(*state, state->drag, ScriptMessage::EndDrag, canceled);
        state = findPointer(pointerId);
        if (!state) return;
        state->dragging = false;
    }

    // the legacy callbacks and the widgets of the element it pressed
    if (state->widget != NULL_ENTITY || state->buttonPressed != NULL_ENTITY ||
        state->scrollbar != NULL_ENTITY || state->panelGesture != NULL_ENTITY){
        const bool overWidget = released && hit == state->widget;
        legacyPointerUp(*state, released, overWidget);
        state = findPointer(pointerId);
        if (!state) return;
    }

    state->down = false;
    state->captured = false;
    state->press = state->rawPress = state->drag = NULL_ENTITY;
    state->widget = NULL_ENTITY;
    state->buttonPressed = NULL_ENTITY;
    state->dragging = false;
    state->legacyDragging = false;
    state->internalGesture = false;
    state->eligibleForClick = false;

    // a finger leaves with its release (no sticky hover on touch); the mouse's other buttons
    // have no state between presses
    if (state->touch){
        setLegacyHover(*state, NULL_ENTITY);
        state = findPointer(pointerId);
        if (!state) return;
        updateEnterChain(*state, NULL_ENTITY);
        pointers.erase(pointerId);
    }else if (pointerId != -1){
        pointers.erase(pointerId);
    }
}

bool UISystem::eventOnPointerCancel(int pointerId){
    UIPointerState* state = findPointer(pointerId);
    if (!state)
        return false;
    const bool consumed = state->down && state->captured;
    if (state->down){
        endPress(pointerId, false);
    }else if (state->touch){
        pointers.erase(pointerId);
    }
    return consumed;
}

void UISystem::eventOnPointerLeave(int pointerId){
    UIPointerState* state = findPointer(pointerId);
    if (!state)
        return;
    setLegacyHover(*state, NULL_ENTITY);
    state = findPointer(pointerId);
    if (!state)
        return;
    updateEnterChain(*state, NULL_ENTITY);
}

bool UISystem::eventOnScroll(float dx, float dy){
    UIPointerState* state = findPointer(-1);
    if (!state)
        return false;
    const bool called = executeHierarchy(-1, state->hit, ScriptMessage::Scroll,
        [dx, dy](PointerEventData& data){ data.scrollDelta = Vector2(dx, dy); }) != NULL_ENTITY;
    return called;
}

void UISystem::cancelAllPointers(){
    std::vector<int> ids;
    for (const auto& pair : pointers)
        ids.push_back(pair.first);
    for (int pointerId : ids)
        eventOnPointerCancel(pointerId);
}

bool UISystem::isPointerOverUI(int pointerId) const{
    for (const auto& pair : pointers){
        if (pointerId != INT_MIN){
            // the mouse is one pointer for D15: any of its button states
            const bool same = isMousePointer(pointerId) ? isMousePointer(pair.first) : pair.first == pointerId;
            if (!same)
                continue;
        }
        const UIPointerState& state = pair.second;
        if (isLiveUIEntity(state.hit) || (state.down && state.captured))
            return true;
    }
    return false;
}

bool UISystem::isPointerCaptured(int pointerId) const{
    auto it = pointers.find(pointerId);
    if (it == pointers.end() || !it->second.down)
        return false;
    return it->second.captured;
}

void UISystem::resetButtonStates() {
    for (auto& pair : pointers){
        if (pair.second.control != NULL_ENTITY)
            clearControlSources(pair.second.control);
    }
    pointers.clear();

    auto buttons = scene->getComponentArray<ButtonComponent>();
    for (int i = 0; i < buttons->size(); i++) {
        ButtonComponent& button = buttons->getComponentFromIndex(i);
        button.hovered = false;
        if (!button.pressed) continue;

        Entity entity = buttons->getEntity(i);
        Signature signature = scene->getSignature(entity);
        button.pressed = false;
        if (signature.test(scene->getComponentId<UIComponent>())) {
            UIComponent& ui = scene->getComponent<UIComponent>(entity);
            applyButtonVisual(button, ui);
        }
    }
    auto panels = scene->getComponentArray<PanelComponent>();
    for (int i = 0; i < panels->size(); i++){
        panels->getComponentFromIndex(i).headerPointerDown = false;
        panels->getComponentFromIndex(i).edgePointerDown = PanelEdge::NONE;
    }
    auto scrollbars = scene->getComponentArray<ScrollbarComponent>();
    for (int i = 0; i < scrollbars->size(); i++)
        scrollbars->getComponentFromIndex(i).barPointerDown = false;

    // the on-screen controls: released, no input left behind
    auto sticks = scene->getComponentArray<OnScreenStickComponent>();
    for (int i = 0; i < sticks->size(); i++){
        const Entity entity = sticks->getEntity(i);
        if (sticks->getComponentFromIndex(i).pressed)
            stickRelease(entity, false);   // no callbacks into a scene being removed
        clearControlSources(entity);
    }
    auto osButtons = scene->getComponentArray<OnScreenButtonComponent>();
    for (int i = 0; i < osButtons->size(); i++){
        OnScreenButtonComponent& control = osButtons->getComponentFromIndex(i);
        control.pressed = false;
        control.pointers.clear();
        clearControlSources(osButtons->getEntity(i));
    }
    auto areas = scene->getComponentArray<TouchAreaComponent>();
    for (int i = 0; i < areas->size(); i++){
        TouchAreaComponent& area = areas->getComponentFromIndex(i);
        area.pressed = false;
        area.pointerId = INT_MIN;
        area.delta = Vector2(0, 0);
        clearControlSources(areas->getEntity(i));
    }
}

// ---- on-screen controls

bool UISystem::controlTakesPointers(Entity entity, Signature signature) const{
    // inactive or in a non-interactable Canvas Group: no (a held control is cancelled, D6)
    if (!scene->isEntityActiveInHierarchy(entity) || !isInteractable(entity))
        return false;
    const bool touchDevice = System::instance().isTouchDevice();
    if (signature.test(scene->getComponentId<OnScreenStickComponent>())){
        const OnScreenStickComponent& stick = scene->getComponent<OnScreenStickComponent>(entity);
        return stick.enabled && (stick.visibility == OnScreenVisibility::ALWAYS || touchDevice);
    }
    if (signature.test(scene->getComponentId<OnScreenButtonComponent>())){
        const OnScreenButtonComponent& button = scene->getComponent<OnScreenButtonComponent>(entity);
        return button.enabled && (button.visibility == OnScreenVisibility::ALWAYS || touchDevice);
    }
    if (signature.test(scene->getComponentId<TouchAreaComponent>())){
        const TouchAreaComponent& area = scene->getComponent<TouchAreaComponent>(entity);
        return area.enabled && (area.visibility == OnScreenVisibility::ALWAYS || touchDevice);
    }
    return false;
}

// the first control from the element up (the topmost element first, D14)
Entity UISystem::findControl(Entity from) const{
    for (Entity current = from; current != NULL_ENTITY && scene->isEntityCreated(current);){
        const Signature signature = scene->getSignature(current);
        if (controlTakesPointers(current, signature))
            return current;
        const Transform* transform = scene->findComponent<Transform>(current);
        current = transform ? transform->parent : NULL_ENTITY;
    }
    return NULL_ENTITY;
}

// the owner key of a control's input sources: a stable address of its own
uintptr_t UISystem::controlOwner(Entity entity){
    std::unique_ptr<uint8_t>& owner = controlOwners[entity];
    if (!owner)
        owner = std::make_unique<uint8_t>(0);
    return reinterpret_cast<uintptr_t>(owner.get());
}

void UISystem::clearControlSources(Entity entity){
    auto it = controlOwners.find(entity);
    if (it != controlOwners.end() && it->second)
        InputActions::clearSource(reinterpret_cast<uintptr_t>(it->second.get()));
}

void UISystem::controlDown(UIPointerState& state, Entity entity){
    const Signature signature = scene->getSignature(entity);
    const int pointerId = state.pointerId;

    if (signature.test(scene->getComponentId<OnScreenStickComponent>())){
        OnScreenStickComponent& stick = scene->getComponent<OnScreenStickComponent>(entity);
        if (stick.pressed)
            return;   // held by another pointer (D14)
        state.control = entity;
        stick.pressed = true;
        stick.pointerId = pointerId;

        // the rest positions, known once (a Floating base comes back there)
        if (!stick.restKnown){
            stick.backgroundRest = getElementRest(stick.background);
            stick.handleRest = getElementRest(stick.handle);
            stick.restKnown = true;
        }

        // the base's centre: under the finger (Floating, Dynamic), or where it is (Fixed)
        Transform* bgTransform = scene->findComponent<Transform>(stick.background);
        RectTransformComponent* bgLayout = scene->findComponent<RectTransformComponent>(stick.background);
        if (bgTransform && bgLayout){
            const Vector2 size(bgLayout->width * bgTransform->worldScale.x, bgLayout->height * bgTransform->worldScale.y);
            Vector2 center(bgTransform->worldPosition.x + size.x / 2.0f, bgTransform->worldPosition.y + size.y / 2.0f);
            if (stick.mode != OnScreenStickMode::FIXED){
                center = state.position;
                if (stick.clampToArea){
                    const Transform* areaTransform = scene->findComponent<Transform>(entity);
                    const RectTransformComponent* areaLayout = scene->findComponent<RectTransformComponent>(entity);
                    if (areaTransform && areaLayout){
                        const float left = areaTransform->worldPosition.x + size.x / 2.0f;
                        const float top = areaTransform->worldPosition.y + size.y / 2.0f;
                        const float right = areaTransform->worldPosition.x + areaLayout->width * areaTransform->worldScale.x - size.x / 2.0f;
                        const float bottom = areaTransform->worldPosition.y + areaLayout->height * areaTransform->worldScale.y - size.y / 2.0f;
                        center.x = (right >= left) ? std::max(left, std::min(right, center.x)) : (left + right) / 2.0f;
                        center.y = (bottom >= top) ? std::max(top, std::min(bottom, center.y)) : (top + bottom) / 2.0f;
                    }
                }
                // move the base by the difference to where it rests
                const Vector2 restCenter(bgTransform->worldPosition.x + size.x / 2.0f, bgTransform->worldPosition.y + size.y / 2.0f);
                const Vector2 current = getElementRest(stick.background);
                setElementOffset(stick.background, current, center - restCenter);
            }
            stick.center = center;
        }
        OnScreenStickComponent* again = scene->findComponent<OnScreenStickComponent>(entity);
        if (!again)
            return;
        again->onPress.call();
        again = scene->findComponent<OnScreenStickComponent>(entity);
        if (again)
            stickUpdate(entity, *again, state.position, true);
        return;
    }

    if (signature.test(scene->getComponentId<OnScreenButtonComponent>())){
        state.control = entity;
        buttonControlPress(entity, pointerId);
        return;
    }

    if (signature.test(scene->getComponentId<TouchAreaComponent>())){
        TouchAreaComponent& area = scene->getComponent<TouchAreaComponent>(entity);
        if (area.pressed)
            return;   // held by another pointer (D14)
        state.control = entity;
        area.pressed = true;
        area.pointerId = pointerId;
        area.delta = Vector2(0, 0);
    }
}

void UISystem::controlMove(UIPointerState& state){
    const Entity entity = state.control;
    if (!scene->isEntityCreated(entity))
        return;
    const Signature signature = scene->getSignature(entity);

    if (signature.test(scene->getComponentId<OnScreenStickComponent>())){
        OnScreenStickComponent& stick = scene->getComponent<OnScreenStickComponent>(entity);
        if (stick.pointerId == state.pointerId)
            stickUpdate(entity, stick, state.position, false);
        return;
    }

    if (signature.test(scene->getComponentId<OnScreenButtonComponent>())){
        const OnScreenButtonComponent& control = scene->getComponent<OnScreenButtonComponent>(entity);
        if (control.releaseWhenPointerLeaves && findControl(state.hit) != entity){
            const int pointerId = state.pointerId;
            state.control = NULL_ENTITY;
            buttonControlRelease(entity, pointerId);
        }
        return;
    }

    if (signature.test(scene->getComponentId<TouchAreaComponent>())){
        TouchAreaComponent& area = scene->getComponent<TouchAreaComponent>(entity);
        if (area.pointerId != state.pointerId)
            return;
        const Vector2 delta = state.delta;
        if (delta.x == 0.0f && delta.y == 0.0f)
            return;
        area.delta += delta;
        const uintptr_t owner = controlOwner(entity);
        const float sx = area.invertX ? -area.sensitivity : area.sensitivity;
        const float sy = area.invertY ? -area.sensitivity : area.sensitivity;
        // the mouse already moves the actions it is bound to (Mouse X/Y): no second count
        auto mouseBound = [&](const std::string& action){
            return !state.touch && !action.empty() && InputActions::isMouseMotionBound(action);
        };
        if (!area.horizontalAction.empty() && !mouseBound(area.horizontalAction))
            InputActions::addSourceDelta(owner, area.horizontalAction, delta.x * sx);
        if (!area.verticalAction.empty() && !mouseBound(area.verticalAction))
            InputActions::addSourceDelta(owner, area.verticalAction, -delta.y * sy);
    }
}

void UISystem::controlUp(UIPointerState& state){
    const Entity entity = state.control;
    const int pointerId = state.pointerId;
    state.control = NULL_ENTITY;
    if (!scene->isEntityCreated(entity))
        return;
    const Signature signature = scene->getSignature(entity);

    if (signature.test(scene->getComponentId<OnScreenStickComponent>())){
        if (scene->getComponent<OnScreenStickComponent>(entity).pointerId == pointerId)
            stickRelease(entity);
        return;
    }
    if (signature.test(scene->getComponentId<OnScreenButtonComponent>())){
        buttonControlRelease(entity, pointerId);
        return;
    }
    if (signature.test(scene->getComponentId<TouchAreaComponent>())){
        TouchAreaComponent& area = scene->getComponent<TouchAreaComponent>(entity);
        if (area.pointerId == pointerId)
            touchAreaRelease(entity);
    }
}

Vector2 UISystem::stickInput(const OnScreenStickComponent& stick, Vector2 input){
    if (stick.axes == OnScreenStickAxes::HORIZONTAL)
        input.y = 0.0f;
    else if (stick.axes == OnScreenStickAxes::VERTICAL)
        input.x = 0.0f;
    const float magnitude = input.length();
    if (magnitude <= stick.deadZone || magnitude == 0.0f)
        return Vector2(0, 0);
    if (magnitude > 1.0f)
        input = input / magnitude;
    return input;
}

void UISystem::stickUpdate(Entity entity, OnScreenStickComponent& stick, const Vector2& pointer, bool first){
    (void)first;
    Transform* bgTransform = scene->findComponent<Transform>(stick.background);
    RectTransformComponent* bgLayout = scene->findComponent<RectTransformComponent>(stick.background);
    float radius = 0.0f;
    if (bgTransform && bgLayout)
        radius = bgLayout->width * bgTransform->worldScale.x / 2.0f;
    if (radius <= 0.0f){
        // no base: the area's own rect
        const Transform* areaTransform = scene->findComponent<Transform>(entity);
        const RectTransformComponent* areaLayout = scene->findComponent<RectTransformComponent>(entity);
        if (areaTransform && areaLayout){
            radius = std::min(areaLayout->width * areaTransform->worldScale.x, areaLayout->height * areaTransform->worldScale.y) / 2.0f;
            if (stick.background == NULL_ENTITY)
                stick.center = Vector2(areaTransform->worldPosition.x + areaLayout->width * areaTransform->worldScale.x / 2.0f,
                                       areaTransform->worldPosition.y + areaLayout->height * areaTransform->worldScale.y / 2.0f);
        }
    }
    if (radius <= 0.0f)
        return;

    Vector2 raw = (pointer - stick.center) / radius;
    if (stick.axes == OnScreenStickAxes::HORIZONTAL)
        raw.y = 0.0f;
    else if (stick.axes == OnScreenStickAxes::VERTICAL)
        raw.x = 0.0f;

    // Dynamic: the base follows the finger past moveThreshold (DynamicJoystick.cs)
    const float rawMagnitude = raw.length();
    if (stick.mode == OnScreenStickMode::DYNAMIC && rawMagnitude > stick.moveThreshold && rawMagnitude > 0.0f){
        const Vector2 difference = (raw / rawMagnitude) * ((rawMagnitude - stick.moveThreshold) * radius);
        stick.center += difference;
        if (bgTransform){
            const Vector2 rest = getElementRest(stick.background);
            setElementOffset(stick.background, rest, difference);
        }
    }

    const Vector2 input = stickInput(stick, raw);

    // the knob: input * radius * handleRange from the base's centre
    if (stick.handle != NULL_ENTITY)
        setElementOffset(stick.handle, stick.handleRest, input * (radius * stick.handleRange) * 1.0f);

    // the value: snapped per Joystick Pack, y up
    const Vector2 inputUp(input.x, -input.y);
    const bool both = stick.axes == OnScreenStickAxes::BOTH;
    Vector2 value = inputUp;
    if (stick.snapX)
        value.x = snapAxis(inputUp.x, inputUp, true, both);
    if (stick.snapY)
        value.y = snapAxis(inputUp.y, inputUp, false, both);

    const uintptr_t owner = controlOwner(entity);
    if (!stick.horizontalAction.empty())
        InputActions::setSourceValue(owner, stick.horizontalAction, value.x);
    if (!stick.verticalAction.empty())
        InputActions::setSourceValue(owner, stick.verticalAction, value.y);

    if (value.x != stick.value.x || value.y != stick.value.y){
        stick.value = value;
        stick.onValueChanged.call(value.x, value.y);
    }
}

void UISystem::stickRelease(Entity entity, bool callbacks){
    OnScreenStickComponent* stick = scene->findComponent<OnScreenStickComponent>(entity);
    if (!stick)
        return;
    clearControlSources(entity);
    const bool changed = stick->value.x != 0.0f || stick->value.y != 0.0f;
    stick->value = Vector2(0, 0);
    stick->pressed = false;
    stick->pointerId = INT_MIN;
    if (stick->restKnown){
        if (stick->handle != NULL_ENTITY)
            setElementOffset(stick->handle, stick->handleRest, Vector2(0, 0));
        if (stick->mode != OnScreenStickMode::FIXED && stick->background != NULL_ENTITY)
            setElementOffset(stick->background, stick->backgroundRest, Vector2(0, 0));
    }
    if (!callbacks)
        return;
    if (changed){
        stick->onValueChanged.call(0.0f, 0.0f);
        stick = scene->findComponent<OnScreenStickComponent>(entity);
        if (!stick)
            return;
    }
    stick->onRelease.call();
}

void UISystem::buttonControlPress(Entity entity, int pointerId){
    OnScreenButtonComponent* control = scene->findComponent<OnScreenButtonComponent>(entity);
    if (!control || std::find(control->pointers.begin(), control->pointers.end(), pointerId) != control->pointers.end())
        return;
    control->pointers.push_back(pointerId);
    if (control->pressed)
        return;
    control->pressed = true;
    if (!control->action.empty())
        InputActions::setSourceValue(controlOwner(entity), control->action, control->value);
    control->onPress.call();
    refreshButtonPressed(entity);
}

void UISystem::buttonControlRelease(Entity entity, int pointerId){
    OnScreenButtonComponent* control = scene->findComponent<OnScreenButtonComponent>(entity);
    if (!control)
        return;
    control->pointers.erase(std::remove(control->pointers.begin(), control->pointers.end(), pointerId), control->pointers.end());
    if (!control->pointers.empty() || !control->pressed)
        return;
    control->pressed = false;
    clearControlSources(entity);
    control->onRelease.call();
    refreshButtonPressed(entity);
}

void UISystem::touchAreaRelease(Entity entity){
    TouchAreaComponent* area = scene->findComponent<TouchAreaComponent>(entity);
    if (!area)
        return;
    area->pressed = false;
    area->pointerId = INT_MIN;
    // the deltas of this frame stay (a quick flick counts); nothing else is left
}

// An element's own position: its Transform position, or an anchored element's positionOffset
Vector2 UISystem::getElementRest(Entity entity) const{
    const RectTransformComponent* layout = scene->findComponent<RectTransformComponent>(entity);
    const Transform* transform = scene->findComponent<Transform>(entity);
    if (!layout || !transform)
        return Vector2(0, 0);
    if (layout->usingAnchors)
        return layout->positionOffset;
    return Vector2(transform->position.x, transform->position.y);
}

// Places an element at rest + offset (canvas px, converted to its parent's scale)
void UISystem::setElementOffset(Entity entity, const Vector2& rest, const Vector2& offsetCanvas){
    RectTransformComponent* layout = scene->findComponent<RectTransformComponent>(entity);
    Transform* transform = scene->findComponent<Transform>(entity);
    if (!layout || !transform)
        return;
    Vector2 scale(1.0f, 1.0f);
    if (const Transform* parent = scene->findComponent<Transform>(transform->parent)){
        if (parent->worldScale.x != 0.0f) scale.x = parent->worldScale.x;
        if (parent->worldScale.y != 0.0f) scale.y = parent->worldScale.y;
    }
    const Vector2 target(rest.x + offsetCanvas.x / scale.x, rest.y + offsetCanvas.y / scale.y);
    if (layout->usingAnchors){
        if (layout->positionOffset.x != target.x || layout->positionOffset.y != target.y){
            layout->positionOffset = target;
            transform->needUpdate = true;
        }
    }else if (transform->position.x != target.x || transform->position.y != target.y){
        transform->position.x = target.x;
        transform->position.y = target.y;
        transform->needUpdate = true;
    }
}

// Once a frame: the Touch Areas' onDrag with the frame's movement, the visibility rules (a
// released Floating base hidden, controls for touch devices only)
void UISystem::updateControls(){
    if (!ScriptRuntime::hasStarted(scene))
        return;

    validatePointers();
    const bool touchDevice = System::instance().isTouchDevice();

    // a control and its children shown or hidden (only when that changes: a pass over the
    // Transforms); the visible flags it sets are runtime state of the playing scene
    auto setSubtreeVisible = [this](Entity root, bool visible){
        Transform* rootTransform = scene->findComponent<Transform>(root);
        if (!rootTransform || rootTransform->visible == visible)
            return;
        auto transforms = scene->getComponentArray<Transform>();
        for (int i = 0; i < transforms->size(); i++){
            const Entity entity = transforms->getEntity(i);
            bool inside = entity == root;
            for (const Transform* up = &transforms->getComponentFromIndex(i); !inside && up && up->parent != NULL_ENTITY;){
                inside = up->parent == root;
                up = scene->findComponent<Transform>(up->parent);
            }
            if (!inside)
                continue;
            Transform& transform = transforms->getComponentFromIndex(i);
            if (transform.visible != visible){
                transform.visible = visible;
                transform.needUpdate = true;
            }
        }
    };

    auto sticks = scene->getComponentArray<OnScreenStickComponent>();
    for (int i = 0; i < sticks->size(); i++){
        const Entity entity = sticks->getEntity(i);
        const OnScreenStickComponent& stick = sticks->getComponentFromIndex(i);
        const bool shown = stick.visibility == OnScreenVisibility::ALWAYS || touchDevice;
        if (stick.visibility == OnScreenVisibility::TOUCH_DEVICES_ONLY)
            setSubtreeVisible(entity, shown);
        if (shown && stick.background != NULL_ENTITY && stick.mode != OnScreenStickMode::FIXED && stick.hideWhenReleased)
            setSubtreeVisible(stick.background, stick.pressed);
    }

    auto buttons = scene->getComponentArray<OnScreenButtonComponent>();
    for (int i = 0; i < buttons->size(); i++){
        const OnScreenButtonComponent& control = buttons->getComponentFromIndex(i);
        if (control.visibility == OnScreenVisibility::TOUCH_DEVICES_ONLY)
            setSubtreeVisible(buttons->getEntity(i), touchDevice);
    }

    std::vector<std::pair<Entity, Vector2>> drags;
    auto areas = scene->getComponentArray<TouchAreaComponent>();
    for (int i = 0; i < areas->size(); i++){
        TouchAreaComponent& area = areas->getComponentFromIndex(i);
        if (area.visibility == OnScreenVisibility::TOUCH_DEVICES_ONLY)
            setSubtreeVisible(areas->getEntity(i), touchDevice);
        if (area.delta.x != 0.0f || area.delta.y != 0.0f)
            drags.emplace_back(areas->getEntity(i), area.delta);
        area.delta = Vector2(0, 0);
    }
    // callbacks last: they can change the arrays
    for (const auto& drag : drags){
        if (TouchAreaComponent* area = scene->findComponent<TouchAreaComponent>(drag.first))
            area->onDrag.call(drag.second.x, drag.second.y);
    }
}
