// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The component UI's selection (ui-components spec 6.4): Unity's EventSystem selection, one per
// scene, moved by the keyboard, gamepads and TV remotes with Unity's Navigation modes, Submit and
// Cancel, Select On Enable and a history that gives a lost selection back. Nothing is selected
// unless the game asks for it (Select On Enable, UI.setSelected): with no selection the game
// keeps every key and the whole gamepad.

#include "UISystem.h"
#include "component/ButtonComponent.h"
#include "component/CameraComponent.h"
#include "component/ImageComponent.h"
#include "component/InputFieldComponent.h"
#include "component/OnScreenControlComponents.h"
#include "component/PanelComponent.h"
#include "component/RectTransformComponent.h"
#include "component/Transform.h"
#include "component/UIComponent.h"

#include "Scene.h"
#include "UIEventData.h"
#include "script/ScriptMessages.h"
#include "script/ScriptRuntime.h"
#include "script/ScriptBase.h"
#include "script/LuaBinding.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace jasperengine;

namespace {

    constexpr size_t selectionHistoryLimit = 8;

    Vector2 navRectCenter(const Rect& rect){
        return Vector2(rect.getX() + rect.getWidth() * 0.5f, rect.getY() + rect.getHeight() * 0.5f);
    }

    void callCppSelectionMessage(ScriptBase& script, ScriptMessage m, AxisEventData& e){
        switch (m){
            case ScriptMessage::Select: script.onSelect(e); break;
            case ScriptMessage::Deselect: script.onDeselect(e); break;
            case ScriptMessage::Move: script.onMove(e); break;
            case ScriptMessage::Submit: script.onSubmit(e); break;
            case ScriptMessage::Cancel: script.onCancel(e); break;
            default: break;
        }
    }

}

void UISystem::setEditMode(bool editMode){
    // what a paused Play shows stays: only new selections and scales wait
    this->editMode = editMode;
}

Entity UISystem::getSelected() const{
    return selected;
}

bool UISystem::isSelectable(Entity entity) const{
    if (!isLiveUIEntity(entity))
        return false;
    const Signature signature = scene->getSignature(entity);
    if (!signature.test(scene->getComponentId<ButtonComponent>()))
        return false;
    // touch controls are never selected
    if (signature.test(scene->getComponentId<OnScreenButtonComponent>()))
        return false;
    if (!isUIActive(entity, signature))
        return false;
    return scene->getComponent<Transform>(entity).visible;
}

Rect UISystem::navRect(Entity entity) const{
    const Transform& transform = scene->getComponent<Transform>(entity);
    const RectTransformComponent* layout = scene->findComponent<RectTransformComponent>(entity);
    if (!layout)
        return Rect(0, 0, 0, 0);
    Rect rect(transform.worldPosition.x, transform.worldPosition.y, layout->width * transform.worldScale.x, layout->height * transform.worldScale.y);
    if (layout->panel != NULL_ENTITY && scene->findComponent<PanelComponent>(layout->panel))
        rect = const_cast<UISystem*>(this)->fitOnPanel(rect, layout->panel);
    // the part on the canvas: what a player can see and click
    const Vector2 canvas = getCanvasSize();
    const float left = std::max(rect.getX(), 0.0f);
    const float top = std::max(rect.getY(), 0.0f);
    const float right = std::min(rect.getX() + rect.getWidth(), canvas.x);
    const float bottom = std::min(rect.getY() + rect.getHeight(), canvas.y);
    if (right <= left || bottom <= top)
        return Rect(0, 0, 0, 0);
    return Rect(left, top, right - left, bottom - top);
}

bool UISystem::isNavigable(Entity entity) const{
    if (!isSelectable(entity))
        return false;
    if (scene->getComponent<ButtonComponent>(entity).navigationMode == NavigationMode::NONE)
        return false;
    if (!isInteractable(entity))
        return false;
    const Rect rect = navRect(entity);
    return rect.getWidth() > 0 && rect.getHeight() > 0;
}

std::vector<Entity> UISystem::navigationCandidates() const{
    std::vector<Entity> candidates;
    auto layouts = scene->getComponentArray<RectTransformComponent>();
    for (int i = 0; i < layouts->size(); i++){
        const Entity entity = layouts->getEntity(i);
        if (scene->getSignature(entity).test(scene->getComponentId<ButtonComponent>()) && isNavigable(entity))
            candidates.push_back(entity);
    }
    return candidates;
}

// Unity's Selectable.FindSelectable: from the edge point of the rect that way, the candidate
// with the best dot / squared distance; Wrap Around: the farthest the other way, in the same row
// or column first
Entity UISystem::findAutomatic(Entity from, MoveDirection dir, bool wrap) const{
    // the UI is y down under a UI camera, y up under a 2D camera
    bool yDown = true;
    const Entity cameraEntity = scene->getCamera();
    if (cameraEntity != NULL_ENTITY){
        if (const CameraComponent* camera = scene->findComponent<CameraComponent>(cameraEntity))
            yDown = camera->type == CameraType::CAMERA_UI;
    }
    Vector2 axis(0, 0);
    switch (dir){
        case MoveDirection::LEFT: axis = Vector2(-1, 0); break;
        case MoveDirection::RIGHT: axis = Vector2(1, 0); break;
        case MoveDirection::UP: axis = Vector2(0, yDown ? -1.0f : 1.0f); break;
        case MoveDirection::DOWN: axis = Vector2(0, yDown ? 1.0f : -1.0f); break;
        default: return NULL_ENTITY;
    }

    Rect fromRect = scene->isEntityCreated(from) && scene->findComponent<Transform>(from) ? navRect(from) : Rect(0, 0, 0, 0);
    if (fromRect.getWidth() <= 0 || fromRect.getHeight() <= 0){
        // a selection with no rect (off the canvas) searches from the canvas centre
        const Vector2 canvas = getCanvasSize();
        fromRect = Rect(canvas.x * 0.5f, canvas.y * 0.5f, 0, 0);
    }
    const Vector2 center = navRectCenter(fromRect);
    const Vector2 edge(center.x + axis.x * fromRect.getWidth() * 0.5f, center.y + axis.y * fromRect.getHeight() * 0.5f);

    const std::vector<Entity> candidates = navigationCandidates();
    Entity best = NULL_ENTITY;
    float bestScore = 0.0f;
    for (Entity candidate : candidates){
        if (candidate == from)
            continue;
        const Vector2 offset = navRectCenter(navRect(candidate)) - edge;
        const float along = offset.dotProduct(axis);
        if (along <= 0.0f)
            continue;
        const float score = along / offset.squaredLength();
        if (score > bestScore){
            bestScore = score;
            best = candidate;
        }
    }
    if (best != NULL_ENTITY || !wrap)
        return best;

    // the farthest the other way; overlapping on the other axis first (the same row or column)
    const bool horizontal = axis.x != 0.0f;
    Entity farthest = NULL_ENTITY;
    float farthestAlong = 0.0f;
    bool farthestOverlaps = false;
    for (Entity candidate : candidates){
        if (candidate == from)
            continue;
        const Rect rect = navRect(candidate);
        const float along = (navRectCenter(rect) - center).dotProduct(axis);
        if (along >= 0.0f)
            continue;
        const bool overlaps = horizontal
            ? (rect.getY() < fromRect.getY() + fromRect.getHeight() && fromRect.getY() < rect.getY() + rect.getHeight())
            : (rect.getX() < fromRect.getX() + fromRect.getWidth() && fromRect.getX() < rect.getX() + rect.getWidth());
        const bool better = (overlaps && !farthestOverlaps) || (overlaps == farthestOverlaps && -along > farthestAlong);
        if (farthest == NULL_ENTITY || better){
            farthest = candidate;
            farthestAlong = -along;
            farthestOverlaps = overlaps;
        }
    }
    return farthest;
}

Entity UISystem::findSelectableOnDirection(Entity from, MoveDirection dir) const{
    const ButtonComponent* button = scene->isEntityCreated(from) ? scene->findComponent<ButtonComponent>(from) : nullptr;
    const NavigationMode mode = button ? button->navigationMode : NavigationMode::AUTOMATIC;
    switch (mode){
        case NavigationMode::NONE:
            return NULL_ENTITY;
        case NavigationMode::EXPLICIT: {
            Entity target = NULL_ENTITY;
            switch (dir){
                case MoveDirection::UP: target = button->selectOnUp; break;
                case MoveDirection::DOWN: target = button->selectOnDown; break;
                case MoveDirection::LEFT: target = button->selectOnLeft; break;
                case MoveDirection::RIGHT: target = button->selectOnRight; break;
                default: break;
            }
            return (target != from && isNavigable(target)) ? target : NULL_ENTITY;
        }
        case NavigationMode::HORIZONTAL:
            if (dir != MoveDirection::LEFT && dir != MoveDirection::RIGHT)
                return NULL_ENTITY;
            return findAutomatic(from, dir, button->wrapAround);
        case NavigationMode::VERTICAL:
            if (dir != MoveDirection::UP && dir != MoveDirection::DOWN)
                return NULL_ENTITY;
            return findAutomatic(from, dir, button->wrapAround);
        case NavigationMode::AUTOMATIC:
        default:
            return findAutomatic(from, dir, button && button->wrapAround);
    }
}

Entity UISystem::findNextSelectable(Entity from, bool backwards) const{
    // every Button with a navigation mode, Explicit ones too, in Hierarchy order
    const std::vector<Entity> candidates = navigationCandidates();
    if (candidates.empty())
        return NULL_ENTITY;
    const auto it = std::find(candidates.begin(), candidates.end(), from);
    if (it == candidates.end())
        return backwards ? candidates.back() : candidates.front();
    const size_t index = (size_t)(it - candidates.begin());
    const size_t next = backwards ? (index + candidates.size() - 1) % candidates.size() : (index + 1) % candidates.size();
    return (candidates[next] != from) ? candidates[next] : NULL_ENTITY;
}

SelectionState UISystem::getButtonState(Entity entity) const{
    const ButtonComponent* button = scene->isEntityCreated(entity) ? scene->findComponent<ButtonComponent>(entity) : nullptr;
    const UIComponent* ui = button ? scene->findComponent<UIComponent>(entity) : nullptr;
    if (!button || !ui)
        return SelectionState::NORMAL;
    return buttonState(*button, *ui);
}

bool UISystem::sendSelectionMessage(Entity target, ScriptMessage m, MoveDirection dir, bool hierarchy, bool* used){
    if (used)
        *used = false;
    bool handled = false;
    for (Entity current = target; current != NULL_ENTITY && scene->isEntityCreated(current);){
        const Entity handler = hierarchy ? findPointerHandler(current, m) : current;
        if (handler == NULL_ENTITY)
            break;

        AxisEventData data;
        data.target = handler;
        data.selectedObject = selected;
        data.moveDir = dir;
        switch (dir){
            case MoveDirection::LEFT: data.moveVector = Vector2(-1, 0); break;
            case MoveDirection::RIGHT: data.moveVector = Vector2(1, 0); break;
            case MoveDirection::UP: data.moveVector = Vector2(0, -1); break;
            case MoveDirection::DOWN: data.moveVector = Vector2(0, 1); break;
            default: break;
        }
        data.scene = scene;
        data.sceneLifetime = scene->getLifetime();

        // every Lua script of the GameObject gets the same event object, read back after the calls
        lua_State* L = LuaBinding::getLuaState();
        int luaRef = LUA_NOREF;
        BaseEventData* luaBase = nullptr;
        const bool axis = m == ScriptMessage::Move;
        const int called = ScriptRuntime::sendMessage(scene, handler, m,
            [&](lua_State* Lpush) -> int {
                if (luaRef == LUA_NOREF){
                    if (axis){
                        if (!luabridge::push(Lpush, data))
                            lua_pushnil(Lpush);
                        auto pushed = luabridge::get<AxisEventData*>(Lpush, -1);
                        luaBase = pushed ? pushed.value() : nullptr;
                    }else{
                        BaseEventData base = data;
                        if (!luabridge::push(Lpush, base))
                            lua_pushnil(Lpush);
                        auto pushed = luabridge::get<BaseEventData*>(Lpush, -1);
                        luaBase = pushed ? pushed.value() : nullptr;
                    }
                    lua_pushvalue(Lpush, -1);
                    luaRef = luaL_ref(Lpush, LUA_REGISTRYINDEX);
                }else{
                    lua_rawgeti(Lpush, LUA_REGISTRYINDEX, luaRef);
                }
                return 1;
            },
            [&](ScriptBase& script){ callCppSelectionMessage(script, m, data); },
            DeliverRequireActive | DeliverRequireEnabled);
        if (luaBase)
            data.used = data.used || luaBase->used;
        if (L && luaRef != LUA_NOREF)
            luaL_unref(L, LUA_REGISTRYINDEX, luaRef);

        if (called > 0){
            handled = true;
            if (used)
                *used = data.used;
            break;
        }
        if (!hierarchy)
            break;
        const Transform* transform = scene->findComponent<Transform>(handler);
        current = transform ? transform->parent : NULL_ENTITY;
    }
    return handled;
}

void UISystem::changeSelection(Entity next, bool animate){
    const unsigned int request = ++selectionRequest;
    const Entity previous = selected;
    if (previous == next)
        return;

    selected = next;
    if (previous != NULL_ENTITY && scene->isEntityCreated(previous)){
        if (ButtonComponent* button = scene->findComponent<ButtonComponent>(previous)){
            button->selected = false;
            if (UIComponent* ui = scene->findComponent<UIComponent>(previous))
                applyButtonVisual(previous, *button, *ui, animate);
        }
        if (next != NULL_ENTITY){
            selectionHistory.erase(std::remove(selectionHistory.begin(), selectionHistory.end(), previous), selectionHistory.end());
            selectionHistory.push_back(previous);
            if (selectionHistory.size() > selectionHistoryLimit)
                selectionHistory.erase(selectionHistory.begin());
        }
        if (isLiveUIEntity(previous))
            sendSelectionMessage(previous, ScriptMessage::Deselect, MoveDirection::NONE, false, nullptr);
        // a handler that selected something else wins
        if (request != selectionRequest)
            return;
    }

    if (next != NULL_ENTITY){
        selectionHistory.erase(std::remove(selectionHistory.begin(), selectionHistory.end(), next), selectionHistory.end());
        if (ButtonComponent* button = scene->findComponent<ButtonComponent>(next)){
            button->selected = true;
            if (UIComponent* ui = scene->findComponent<UIComponent>(next))
                applyButtonVisual(next, *button, *ui, animate);
        }
        sendSelectionMessage(next, ScriptMessage::Select, MoveDirection::NONE, false, nullptr);
    }
}

bool UISystem::setSelected(Entity entity){
    if (entity == NULL_ENTITY){
        selectionHistory.clear();
        changeSelection(NULL_ENTITY, true);
        return true;
    }
    if (!isSelectable(entity))
        return false;
    changeSelection(entity, true);
    return true;
}

void UISystem::loseSelection(){
    const Entity lost = selected;
    if (lost != NULL_ENTITY && scene->isEntityCreated(lost)){
        if (ButtonComponent* button = scene->findComponent<ButtonComponent>(lost))
            button->selected = false;
    }
    selected = NULL_ENTITY;
    if (submitButton == lost)
        cancelSubmit();

    // the latest earlier selection that can still be reached (a submenu closed: its opener)
    while (!selectionHistory.empty()){
        const Entity candidate = selectionHistory.back();
        selectionHistory.pop_back();
        if (isNavigable(candidate)){
            changeSelection(candidate, false);
            return;
        }
    }
}

bool UISystem::hasLiveSelection(){
    if (selectionDestroyed){
        selectionDestroyed = false;
        loseSelection();
    }
    if (selected == NULL_ENTITY)
        return false;
    if (!isSelectable(selected))
        loseSelection();
    return selected != NULL_ENTITY;
}

bool UISystem::hasSelection() const{
    return selected != NULL_ENTITY && !selectionDestroyed && isSelectable(selected);
}

bool UISystem::isInputFieldTyping() const{
    auto inputfields = scene->getComponentArray<InputFieldComponent>();
    for (int i = 0; i < inputfields->size(); i++){
        const Entity entity = inputfields->getEntity(i);
        const Signature signature = scene->getSignature(entity);
        if (!signature.test(scene->getComponentId<UIComponent>()))
            continue;
        if (scene->getComponent<UIComponent>(entity).focused && isUIActive(entity, signature) && isInteractable(entity))
            return true;
    }
    return false;
}

void UISystem::updateSelection(){
    if (editMode)
        return;

    // Select On Enable: a Button that came on this update takes the selection, the first in
    // Hierarchy order when several do (HTML autofocus)
    Entity picked = NULL_ENTITY;
    auto layouts = scene->getComponentArray<RectTransformComponent>();
    for (int i = 0; i < layouts->size(); i++){
        const Entity entity = layouts->getEntity(i);
        const Signature signature = scene->getSignature(entity);
        if (!signature.test(scene->getComponentId<ButtonComponent>()))
            continue;
        ButtonComponent& button = scene->getComponent<ButtonComponent>(entity);
        const bool live = isSelectable(entity);
        if (live && !button.wasLive && button.selectOnEnable && picked == NULL_ENTITY)
            picked = entity;
        button.wasLive = live;
    }
    if (picked != NULL_ENTITY && picked != selected)
        changeSelection(picked, false);

    hasLiveSelection();
}

bool UISystem::eventOnMove(MoveDirection dir){
    if (!hasLiveSelection())
        return false;
    // what the handlers destroy goes when the dispatch is over
    StructuralDeferScope deferScope(*scene);
    const Entity from = selected;
    bool used = false;
    sendSelectionMessage(from, ScriptMessage::Move, dir, false, &used);
    if (!used && selected == from){
        const Entity next = findSelectableOnDirection(from, dir);
        if (next != NULL_ENTITY)
            changeSelection(next, true);
    }
    // consumed also where nothing is that way: the player does not walk at a menu's edge
    return true;
}

bool UISystem::eventOnTab(bool backwards){
    if (!hasLiveSelection())
        return false;
    StructuralDeferScope deferScope(*scene);
    const Entity next = findNextSelectable(selected, backwards);
    if (next != NULL_ENTITY)
        changeSelection(next, true);
    return true;
}

bool UISystem::eventOnSubmitDown(int source){
    if (!hasLiveSelection())
        return false;
    // one hold at a time: a second key or pad button is taken and ignored
    if (submitButton != NULL_ENTITY)
        return true;

    StructuralDeferScope deferScope(*scene);
    const Entity entity = selected;
    bool used = false;
    sendSelectionMessage(entity, ScriptMessage::Submit, MoveDirection::NONE, false, &used);
    if (used || !isLiveUIEntity(entity) || !isInteractable(entity))
        return true;
    ButtonComponent* button = scene->findComponent<ButtonComponent>(entity);
    if (!button)
        return true;

    // Unity's Button.OnSubmit: the click is on the press (shown Pressed until the release)
    submitButton = entity;
    submitSource = source;
    button->submitPressed = true;
    refreshButtonPressed(entity);
    if (!isLiveUIEntity(entity))
        return true;
    if ((button = scene->findComponent<ButtonComponent>(entity)) != nullptr)
        button->onClick.call();
    if (!isLiveUIEntity(entity))
        return true;
    // the legacy click: the element's centre, no PointerEventData
    if (UIComponent* ui = scene->findComponent<UIComponent>(entity)){
        const RectTransformComponent* layout = scene->findComponent<RectTransformComponent>(entity);
        const float x = layout ? layout->width * 0.5f : 0.0f;
        const float y = layout ? layout->height * 0.5f : 0.0f;
        ui->onClick.call(x, y);
    }
    return true;
}

bool UISystem::eventOnSubmitUp(int source){
    if (submitButton == NULL_ENTITY || submitSource != source)
        return false;
    cancelSubmit();
    return true;
}

void UISystem::cancelSubmit(){
    const Entity entity = submitButton;
    submitButton = NULL_ENTITY;
    submitSource = 0;
    if (entity == NULL_ENTITY || !scene->isEntityCreated(entity))
        return;
    if (ButtonComponent* button = scene->findComponent<ButtonComponent>(entity)){
        button->submitPressed = false;
        StructuralDeferScope deferScope(*scene);
        refreshButtonPressed(entity);
    }
}

bool UISystem::eventOnCancel(){
    if (!hasLiveSelection())
        return false;
    StructuralDeferScope deferScope(*scene);
    // up the hierarchy: a menu's script handles it once for its buttons
    return sendSelectionMessage(selected, ScriptMessage::Cancel, MoveDirection::NONE, true, nullptr);
}

void UISystem::pointerSelects(Entity hit){
    if (selected == NULL_ENTITY || editMode)
        return;

    // the Button pressed, or the one whose label (or other child) was
    Entity button = NULL_ENTITY;
    for (Entity current = hit; current != NULL_ENTITY && scene->isEntityCreated(current);){
        const Signature signature = scene->getSignature(current);
        // an InputField types and keeps the selection; the typing guard keeps its keys
        if (signature.test(scene->getComponentId<InputFieldComponent>()))
            return;
        if (signature.test(scene->getComponentId<ButtonComponent>())){
            // an on-screen control is a touch control: the selection stays
            if (signature.test(scene->getComponentId<OnScreenButtonComponent>()))
                return;
            button = current;
            break;
        }
        const Transform* transform = scene->findComponent<Transform>(current);
        current = transform ? transform->parent : NULL_ENTITY;
    }

    if (button != NULL_ENTITY && isSelectable(button) &&
        scene->getComponent<ButtonComponent>(button).navigationMode != NavigationMode::NONE){
        changeSelection(button, true);
    }else{
        // Unity's DeselectIfSelectionChanged: a press elsewhere lets go of the menu
        setSelected(NULL_ENTITY);
    }
}
