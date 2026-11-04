// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "RectTransform.h"
#include "component/ButtonComponent.h"
#include "component/InputFieldComponent.h"
#include "component/Transform.h"
#include "component/UIComponent.h"

#include "util/Color.h"
#include "subsystem/UISystem.h"

using namespace jasperengine;

RectTransform::RectTransform(Scene* scene): GameObject(scene){
    addComponent<RectTransformComponent>();
}

RectTransform::RectTransform(Scene* scene, Entity entity): GameObject(scene, entity){
}

RectTransform::~RectTransform(){
}

RectTransform::RectTransform(const RectTransform& rhs) = default;
RectTransform& RectTransform::operator=(const RectTransform& rhs) = default;
RectTransform::RectTransform(RectTransform&& rhs) noexcept = default;
RectTransform& RectTransform::operator=(RectTransform&& rhs) noexcept = default;

void RectTransform::setSize(unsigned int width, unsigned int height){
    RectTransformComponent& layout = getComponent<RectTransformComponent>();

    layout.width = width;
    layout.height = height;

    layout.needUpdateSizes = true;
    markChanged<RectTransformComponent>();
}

void RectTransform::setWidth(unsigned int width){
    RectTransformComponent& layout = getComponent<RectTransformComponent>();

    layout.width = width;

    layout.needUpdateSizes = true;
    markChanged<RectTransformComponent>();
}

void RectTransform::setHeight(unsigned int height){
    RectTransformComponent& layout = getComponent<RectTransformComponent>();

    layout.height = height;

    layout.needUpdateSizes = true;
    markChanged<RectTransformComponent>();
}

unsigned int RectTransform::getWidth() const{
    RectTransformComponent& layout = getComponent<RectTransformComponent>();

    return layout.width;
}

unsigned int RectTransform::getHeight() const{
    RectTransformComponent& layout = getComponent<RectTransformComponent>();

    return layout.height;
}

void RectTransform::setAnchorPoints(float left, float top, float right, float bottom){
    RectTransformComponent& layout = getComponent<RectTransformComponent>();

    layout.anchorPointLeft = left;
    layout.anchorPointTop = top;
    layout.anchorPointRight = right;
    layout.anchorPointBottom = bottom;

    layout.anchorPreset = AnchorPreset::NONE;
    layout.usingAnchors = true;
}

void RectTransform::setAnchorPointLeft(float left){
    RectTransformComponent& layout = getComponent<RectTransformComponent>();

    layout.anchorPointLeft = left;

    layout.anchorPreset = AnchorPreset::NONE;
    layout.usingAnchors = true;
}

void RectTransform::setAnchorPointTop(float top){
    RectTransformComponent& layout = getComponent<RectTransformComponent>();

    layout.anchorPointTop = top;

    layout.anchorPreset = AnchorPreset::NONE;
    layout.usingAnchors = true;
}

void RectTransform::setAnchorPointRight(float right){
    RectTransformComponent& layout = getComponent<RectTransformComponent>();

    layout.anchorPointRight = right;

    layout.anchorPreset = AnchorPreset::NONE;
    layout.usingAnchors = true;
}

void RectTransform::setAnchorPointBottom(float bottom){
    RectTransformComponent& layout = getComponent<RectTransformComponent>();

    layout.anchorPointBottom = bottom;

    layout.anchorPreset = AnchorPreset::NONE;
    layout.usingAnchors = true;
}

float RectTransform::getAnchorPointLeft() const{
    RectTransformComponent& layout = getComponent<RectTransformComponent>();

    return layout.anchorPointLeft;
}

float RectTransform::getAnchorPointTop() const{
    RectTransformComponent& layout = getComponent<RectTransformComponent>();

    return layout.anchorPointTop;
}

float RectTransform::getAnchorPointRight() const{
    RectTransformComponent& layout = getComponent<RectTransformComponent>();

    return layout.anchorPointRight;
}

float RectTransform::getAnchorPointBottom() const{
    RectTransformComponent& layout = getComponent<RectTransformComponent>();

    return layout.anchorPointBottom;
}

void RectTransform::setAnchorOffsets(int left, int top, int right, int bottom){
    RectTransformComponent& layout = getComponent<RectTransformComponent>();

    layout.anchorOffsetLeft = left;
    layout.anchorOffsetTop = top;
    layout.anchorOffsetRight = right;
    layout.anchorOffsetBottom = bottom;

    layout.anchorPreset = AnchorPreset::NONE;
    layout.usingAnchors = true;
}

void RectTransform::setAnchorOffsetLeft(int left){
    RectTransformComponent& layout = getComponent<RectTransformComponent>();

    layout.anchorOffsetLeft = left;

    layout.anchorPreset = AnchorPreset::NONE;
    layout.usingAnchors = true;
}

void RectTransform::setAnchorOffsetTop(int top){
    RectTransformComponent& layout = getComponent<RectTransformComponent>();

    layout.anchorOffsetTop = top;

    layout.anchorPreset = AnchorPreset::NONE;
    layout.usingAnchors = true;
}

void RectTransform::setAnchorOffsetRight(int right){
    RectTransformComponent& layout = getComponent<RectTransformComponent>();

    layout.anchorOffsetRight = right;

    layout.anchorPreset = AnchorPreset::NONE;
    layout.usingAnchors = true;
}

void RectTransform::setAnchorOffsetBottom(int bottom){
    RectTransformComponent& layout = getComponent<RectTransformComponent>();

    layout.anchorOffsetBottom = bottom;

    layout.anchorPreset = AnchorPreset::NONE;
    layout.usingAnchors = true;
}

int RectTransform::getAnchorOffsetLeft() const{
    RectTransformComponent& layout = getComponent<RectTransformComponent>();

    return layout.anchorOffsetLeft;
}

int RectTransform::getAnchorOffsetTop() const{
    RectTransformComponent& layout = getComponent<RectTransformComponent>();

    return layout.anchorOffsetTop;
}

int RectTransform::getAnchorOffsetRight() const{
    RectTransformComponent& layout = getComponent<RectTransformComponent>();

    return layout.anchorOffsetRight;
}

int RectTransform::getAnchorOffsetBottom() const{
    RectTransformComponent& layout = getComponent<RectTransformComponent>();

    return layout.anchorOffsetBottom;
}

void RectTransform::setPositionXOffset(float xOffset){
    RectTransformComponent& layout = getComponent<RectTransformComponent>();

    layout.positionOffset = Vector2(xOffset, layout.positionOffset.y);
}

float RectTransform::getPositionXOffset() const{
    RectTransformComponent& layout = getComponent<RectTransformComponent>();

    return layout.positionOffset.x;
}

void RectTransform::setPositionYOffset(float yOffset){
    RectTransformComponent& layout = getComponent<RectTransformComponent>();

    layout.positionOffset = Vector2(layout.positionOffset.x, yOffset);
}

float RectTransform::getPositionYOffset() const{
    RectTransformComponent& layout = getComponent<RectTransformComponent>();

    return layout.positionOffset.y;
}

void RectTransform::setPositionOffset(Vector2 positionOffset){
    RectTransformComponent& layout = getComponent<RectTransformComponent>();

    layout.positionOffset = positionOffset;
}

Vector2 RectTransform::getPositionOffset() const{
    RectTransformComponent& layout = getComponent<RectTransformComponent>();

    return layout.positionOffset;
}

void RectTransform::setPivot(Vector2 pivot){
    RectTransformComponent& layout = getComponent<RectTransformComponent>();
    Transform& transform = getComponent<Transform>();

    if (layout.pivot != pivot){
        layout.pivot = pivot;

        transform.needUpdate = true;
        markChanged<Transform>();
    }
}

void RectTransform::setPivot(float x, float y){
    setPivot(Vector2(x, y));
}

Vector2 RectTransform::getPivot() const{
    RectTransformComponent& layout = getComponent<RectTransformComponent>();

    return layout.pivot;
}

void RectTransform::setAnchorPreset(AnchorPreset anchorPreset){
    RectTransformComponent& layout = getComponent<RectTransformComponent>();

    layout.anchorPreset = anchorPreset;

    layout.usingAnchors = true;
}

AnchorPreset RectTransform::getAnchorPreset() const{
    RectTransformComponent& layout = getComponent<RectTransformComponent>();

    return layout.anchorPreset;
}

void RectTransform::setUsingAnchors(bool usingAnchors){
    RectTransformComponent& layout = getComponent<RectTransformComponent>();

    layout.usingAnchors = usingAnchors;
}

bool RectTransform::isUsingAnchors() const{
    RectTransformComponent& layout = getComponent<RectTransformComponent>();

    return layout.usingAnchors;
}

void RectTransform::setIgnoreScissor(bool ignoreScissor){
    RectTransformComponent& layout = getComponent<RectTransformComponent>();

    layout.ignoreScissor = ignoreScissor;
}

bool RectTransform::isIgnoreScissor() const{
    RectTransformComponent& layout = getComponent<RectTransformComponent>();

    return layout.ignoreScissor;
}

void RectTransform::setEnabled(bool enabled){
    if (UIComponent* ui = scene->findComponent<UIComponent>(entity)){
        ui->enabled = enabled;
    }else{
        Log::warn("Entity %u has no UIComponent to enable", entity);
    }
}

bool RectTransform::isEnabled() const{
    UIComponent* ui = scene->findComponent<UIComponent>(entity);
    return !ui || ui->enabled;
}

float RectTransform::getInheritedAlpha() const{
    return scene->getSystem<UISystem>()->getCanvasGroupState(entity).alpha;
}

bool RectTransform::getGroupsAllowInteraction() const{
    return scene->getSystem<UISystem>()->getCanvasGroupState(entity).interactable;
}

bool RectTransform::getGroupsBlockRaycasts() const{
    return scene->getSystem<UISystem>()->getCanvasGroupState(entity).blocksRaycasts;
}

bool RectTransform::isInteractable() const{
    if (!getGroupsAllowInteraction()){
        return false;
    }
    if (const ButtonComponent* button = scene->findComponent<ButtonComponent>(entity)){
        if (button->disabled) return false;
    }
    if (const InputFieldComponent* inputfield = scene->findComponent<InputFieldComponent>(entity)){
        if (inputfield->disabled) return false;
    }
    return true;
}
