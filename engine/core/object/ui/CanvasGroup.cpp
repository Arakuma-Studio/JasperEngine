// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CanvasGroup.h"
#include "component/Transform.h"

#include "component/RectTransformComponent.h"
#include "subsystem/UISystem.h"
#include "Log.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

namespace {

    // [0, 1]; NaN keeps the current value, with one warning per GameObject
    float acceptedAlpha(Scene* scene, Entity entity, CanvasGroupComponent& group, float alpha){
        if (std::isnan(alpha)){
            if (!group.warnedInvalidAlpha){
                group.warnedInvalidAlpha = true;
                Log::warn("CanvasGroup of '%s': alpha NaN rejected, kept %g", scene->getEntityName(entity).c_str(), group.alpha);
            }
            return group.alpha;
        }
        return std::clamp(alpha, 0.0f, 1.0f);
    }

}

CanvasGroup::CanvasGroup(Scene* scene): EntityHandle(scene){
    addComponent<Transform>();
    RectTransformComponent layout;
    // a container stretched over its parent
    layout.anchorPreset = AnchorPreset::FULL_LAYOUT;
    layout.usingAnchors = true;
    addComponent<RectTransformComponent>(layout);
    addComponent<CanvasGroupComponent>();
}

CanvasGroup::CanvasGroup(Scene* scene, Entity entity): EntityHandle(scene, entity){
    if (!scene->findComponent<CanvasGroupComponent>(entity)){
        scene->addComponent<CanvasGroupComponent>(entity, {});
    }
}

CanvasGroup::~CanvasGroup(){
}

CanvasGroup::CanvasGroup(const CanvasGroup& rhs) = default;
CanvasGroup& CanvasGroup::operator=(const CanvasGroup& rhs) = default;
CanvasGroup::CanvasGroup(CanvasGroup&& rhs) noexcept = default;
CanvasGroup& CanvasGroup::operator=(CanvasGroup&& rhs) noexcept = default;

void CanvasGroup::setEnabled(bool enabled){
    getComponent<CanvasGroupComponent>().enabled = enabled;
}

bool CanvasGroup::isEnabled() const{
    return getComponent<CanvasGroupComponent>().enabled;
}

void CanvasGroup::setAlpha(float alpha){
    CanvasGroupComponent& group = getComponent<CanvasGroupComponent>();
    group.alpha = acceptedAlpha(scene, entity, group, alpha);
}

float CanvasGroup::getAlpha() const{
    return getComponent<CanvasGroupComponent>().alpha;
}

void CanvasGroup::setInteractable(bool interactable){
    getComponent<CanvasGroupComponent>().interactable = interactable;
}

bool CanvasGroup::isInteractable() const{
    return getComponent<CanvasGroupComponent>().interactable;
}

void CanvasGroup::setBlocksRaycasts(bool blocksRaycasts){
    getComponent<CanvasGroupComponent>().blocksRaycasts = blocksRaycasts;
}

bool CanvasGroup::isBlocksRaycasts() const{
    return getComponent<CanvasGroupComponent>().blocksRaycasts;
}

void CanvasGroup::setIgnoreParentGroups(bool ignoreParentGroups){
    getComponent<CanvasGroupComponent>().ignoreParentGroups = ignoreParentGroups;
}

bool CanvasGroup::isIgnoreParentGroups() const{
    return getComponent<CanvasGroupComponent>().ignoreParentGroups;
}

void CanvasGroup::setRenderMode(CanvasGroupRenderMode renderMode){
    getComponent<CanvasGroupComponent>().renderMode = renderMode;
}

CanvasGroupRenderMode CanvasGroup::getRenderMode() const{
    return getComponent<CanvasGroupComponent>().renderMode;
}

AlphaTween CanvasGroup::fadeTo(float alpha, float duration){
    return fadeTo(alpha, duration, CanvasGroupFadeOptions());
}

AlphaTween CanvasGroup::fadeTo(float alpha, float duration, const CanvasGroupFadeOptions& options){
    CanvasGroupComponent& group = getComponent<CanvasGroupComponent>();
    const float startAlpha = group.alpha;
    const float endAlpha = acceptedAlpha(scene, entity, group, alpha);
    if (std::isnan(duration) || duration < 0.0f){
        duration = 0.0f;
    }

    // the scene owns the tween (UISystem): not this handle, not a Lua handle
    AlphaTween tween(scene);
    tween.setEntityOwned(false);
    tween.setTarget(entity);
    tween.setAction(startAlpha, endAlpha, duration, false);
    tween.setFunctionType(options.ease);
    tween.setUseUnscaledTime(options.unscaled);
    tween.setAlphaTarget(AlphaTweenTarget::CANVAS_GROUP);

    scene->getSystem<UISystem>()->startCanvasGroupFade(entity, tween.getEntity(), endAlpha, options.deactivate, options.onComplete);

    tween.start();
    return tween;
}

AlphaTween CanvasGroup::fadeIn(float duration){
    return fadeTo(1.0f, duration, CanvasGroupFadeOptions());
}

AlphaTween CanvasGroup::fadeIn(float duration, const CanvasGroupFadeOptions& options){
    return fadeTo(1.0f, duration, options);
}

AlphaTween CanvasGroup::fadeOut(float duration){
    return fadeTo(0.0f, duration, CanvasGroupFadeOptions());
}

AlphaTween CanvasGroup::fadeOut(float duration, const CanvasGroupFadeOptions& options){
    return fadeTo(0.0f, duration, options);
}

void CanvasGroup::stopFade(){
    stopFade(false);
}

void CanvasGroup::stopFade(bool jumpToEnd){
    scene->getSystem<UISystem>()->stopCanvasGroupFade(entity, jumpToEnd);
}

bool CanvasGroup::isFading() const{
    return scene->getSystem<UISystem>()->isCanvasGroupFading(entity);
}

GameObject CanvasGroup::getGameObject() const{
    return GameObject(scene, entity);
}
