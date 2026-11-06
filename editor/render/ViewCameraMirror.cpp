// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ViewCameraMirror.h"

#include "component/CameraComponent.h"
#include "component/Transform.h"

using namespace jasperengine;

Entity editor::ViewCameraMirror::ensure(Scene* target, Entity& slot, const char* name) {
    if (slot == NULL_ENTITY || !target->isEntityCreated(slot)) {
        slot = target->createSystemEntity();
        target->setEntityName(slot, name);
        target->addComponent<Transform>(slot, {});
        target->addComponent<CameraComponent>(slot, {});
    }
    return slot;
}

bool editor::ViewCameraMirror::copy(Scene* from, Entity fromCamera, Scene* to, Entity toCamera, const Matrix4& toTarget) {
    const CameraComponent* source = from->findComponent<CameraComponent>(fromCamera);
    const Transform* sourceTransform = from->findComponent<Transform>(fromCamera);
    CameraComponent* mirror = to->findComponent<CameraComponent>(toCamera);
    Transform* mirrorTransform = to->findComponent<Transform>(toCamera);
    if (!source || !sourceTransform || !mirror || !mirrorTransform) {
        return false;
    }
    mirror->type = source->type;
    mirror->leftClip = source->leftClip;
    mirror->rightClip = source->rightClip;
    mirror->bottomClip = source->bottomClip;
    mirror->topClip = source->topClip;
    mirror->yfov = source->yfov;
    mirror->aspect = source->aspect;
    mirror->nearClip = source->nearClip;
    mirror->farClip = source->farClip;
    mirror->useTarget = source->useTarget;
    mirror->autoResize = false;
    mirror->transparentSort = source->transparentSort;
    mirror->depthTest = source->depthTest;
    mirror->hasCustomViewMatrix = false;
    const Vector3 position = toTarget * sourceTransform->worldPosition;
    mirrorTransform->position = position;
    Vector3 dummyPosition;
    Vector3 dummyScale;
    Quaternion rotation;
    toTarget.decompose(dummyPosition, dummyScale, rotation);
    mirrorTransform->rotation = rotation * sourceTransform->worldRotation;
    mirror->target = toTarget * source->worldTarget;
    mirror->up = (toTarget * (sourceTransform->worldPosition + source->worldUp)) - position;
    mirror->needUpdate = true;
    mirrorTransform->needUpdate = true;
    return true;
}
