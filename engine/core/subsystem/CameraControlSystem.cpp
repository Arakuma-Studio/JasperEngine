// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CameraControlSystem.h"

#include "Engine.h"
#include "Log.h"
#include "Scene.h"
#include "camera/ShakerRuntime.h"
#include "component/CameraComponent.h"
#include "component/RectTransformComponent.h"
#include "component/Rigidbody2DComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/Transform.h"
#include "component/UIComponent.h"
#include "subsystem/UISystem.h"
#include "manager/SceneManager.h"
#include "pixelart/HitStop.h"
#include "pixelart/PixelSnap.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::camera;

namespace{

    // a pose with no change at all (S6: no matrix work, no Transform write)
    bool isZero(const Vector3& v){
        return v.x == 0.0f && v.y == 0.0f && v.z == 0.0f;
    }

    bool isOrthographic(const CameraComponent& camera){
        return camera.type == CameraType::CAMERA_ORTHO || camera.type == CameraType::CAMERA_UI;
    }

    float sanitizeScale(float scale){
        return std::isfinite(scale) && scale > 0.0f ? scale : 0.0f;
    }

    Quaternion yawOf(const Quaternion& rotation){
        const Vector3 forward = rotation * Vector3(0, 0, 1);
        if (forward.x * forward.x + forward.z * forward.z < 1e-12f) return Quaternion();
        const float half = std::atan2(forward.x, forward.z) * 0.5f;
        return Quaternion(std::cos(half), 0.0f, std::sin(half), 0.0f);
    }

}

CameraControlSystem::CameraControlSystem(Scene* scene): SubSystem(scene){
    signature.set(scene->getComponentId<CameraShakerComponent>());
    signature.set(scene->getComponentId<CameraFollowComponent>());
}

void CameraControlSystem::load(){
}

void CameraControlSystem::draw(){
}

void CameraControlSystem::destroy(){
}

void CameraControlSystem::setPaused(bool value){
    // the edit-mode preview never carries into play (nor play's shakes into edit mode)
    if (value != paused){
        stopPreview();
    }
    SubSystem::setPaused(value);
}

void CameraControlSystem::update(double dt){
    if (paused){
        // edit mode: only the Inspector's preview moves, on the editor's frame time
        updatePreview(std::min(Engine::getUnscaledDeltatime(), 0.1f));
        return;
    }
    step((float)dt, Engine::getUnscaledDeltatime());
}

void CameraControlSystem::step(float scaledDt, float unscaledDt){
    updateFollowers(scaledDt, unscaledDt);
    updateShakers(scaledDt, unscaledDt);
}

// ---------------------------------------------------------------- follow

float CameraControlSystem::smoothDamp(float current, float target, float& velocity, float smoothTime, float maxSpeed, float dt){
    if (smoothTime <= 0.0f){
        velocity = 0.0f;
        return target;
    }
    if (!(dt > 0.0f)) return current;
    // Unity's Mathf.SmoothDamp (Game Programming Gems 4, 1.10)
    smoothTime = std::max(0.0001f, smoothTime);
    const float omega = 2.0f / smoothTime;
    const float x = omega * dt;
    const float e = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
    float change = current - target;
    const float originalTo = target;
    const float maxChange = maxSpeed > 0.0f ? maxSpeed * smoothTime : std::numeric_limits<float>::infinity();
    change = std::max(-maxChange, std::min(maxChange, change));
    target = current - change;
    const float temp = (velocity + omega * change) * dt;
    velocity = (velocity - omega * temp) * e;
    float output = target + (change + temp) * e;
    // no overshoot
    if ((originalTo - current > 0.0f) == (output > originalTo)){
        output = originalTo;
        velocity = (output - originalTo) / dt;
    }
    return output;
}

CameraWorldPose CameraControlSystem::computeWorldNow(Entity entity) const{
    return computeWorldNow(scene, entity);
}

Scene* CameraControlSystem::targetSceneOf(const CameraFollowComponent& follow) const{
    return follow.targetPersistent ? SceneManager::getPersistentScene() : scene;
}

bool CameraControlSystem::targetAlive(const CameraFollowComponent& follow, Entity self) const{
    Scene* in = targetSceneOf(follow);
    return in && follow.target != NULL_ENTITY && !(in == scene && follow.target == self) &&
           in->isEntityCreated(follow.target) && in->findComponent<Transform>(follow.target);
}

void CameraControlSystem::remapTargets(Scene* to, const std::unordered_map<Entity, Entity>& moved){
    const bool persistent = to && to == SceneManager::getPersistentScene();
    auto followers = scene->getComponentArray<CameraFollowComponent>();
    for (size_t i = 0; i < followers->size(); i++){
        CameraFollowComponent& follow = followers->getComponentFromIndex(i);
        if (follow.targetPersistent) continue;
        auto it = moved.find(follow.target);
        if (it == moved.end()) continue;
        if (persistent){
            // DontDestroyOnLoad of the target: this scene's camera follows it into the persistent scene
            follow.target = it->second;
            follow.targetPersistent = true;
            follow.followedTarget = it->second;
        }else{
            Log::warn("Camera Follow on '%s': its target moved to another scene; only the persistent "
                      "(DontDestroyOnLoad) scene can be followed from here", scene->getEntityName(followers->getEntity(i)).c_str());
            follow.target = NULL_ENTITY;
        }
    }
}

CameraWorldPose CameraControlSystem::computeWorldNow(Scene* in, Entity entity){
    // the chain root first; a cycle (never valid) stops after 256 levels
    std::vector<const Transform*> chain;
    Entity current = entity;
    while (in && current != NULL_ENTITY && chain.size() < 256){
        const Transform* transform = in->findComponent<Transform>(current);
        if (!transform) break;
        chain.push_back(transform);
        current = transform->parent;
    }
    CameraWorldPose pose;
    pose.matrix = Matrix4();
    pose.rotation = Quaternion();
    for (auto it = chain.rbegin(); it != chain.rend(); ++it){
        const Transform& t = **it;
        const Matrix4 local = Matrix4::translateMatrix(t.position) * t.rotation.getRotationMatrix() * Matrix4::scaleMatrix(t.scale);
        pose.matrix = pose.matrix * local;
        pose.rotation = pose.rotation * t.rotation;
    }
    pose.position = pose.matrix * Vector3(0, 0, 0);
    return pose;
}

void CameraControlSystem::snap(CameraFollowComponent& follow){
    follow.placed = false;
    follow.velocity = Vector3(0, 0, 0);
}

FollowPlacement CameraControlSystem::computeFollow(Entity entity, CameraFollowComponent& follow, float dt, bool snapNow){
    FollowPlacement placement;
    Transform* transform = scene->findComponent<Transform>(entity);
    if (!transform || !targetAlive(follow, entity)){
        return placement;
    }

    const CameraWorldPose target = computeWorldNow(targetSceneOf(follow), follow.target);
    // the follower's parent now: the world placement goes back through it
    CameraWorldPose parent;
    if (transform->parent != NULL_ENTITY && scene->findComponent<Transform>(transform->parent)){
        parent = computeWorldNow(transform->parent);
    }
    const Matrix4 parentInverse = parent.matrix.inverse();
    const Vector3 currentWorld = parent.matrix * transform->position;

    Vector3 offset = follow.offset;
    if (follow.offsetSpace == FollowOffsetSpace::Target){
        offset = target.rotation * offset;
    }else if (follow.offsetSpace == FollowOffsetSpace::TargetYaw){
        offset = yawOf(target.rotation) * offset;
    }
    Vector3 desired = target.position + offset;

    CameraComponent* camera = scene->findComponent<CameraComponent>(entity);
    // the dead zone: in the view plane (world x / y) of a 2D camera, target moves inside the
    // rectangle centred on where the camera looks do not move it
    const bool deadZone = camera && isOrthographic(*camera) && (follow.deadZone.x > 0.0f || follow.deadZone.y > 0.0f);
    if (deadZone && !snapNow){
        const Vector3 focus = currentWorld - offset;
        auto zoneAxis = [](float focusValue, float targetValue, float width){
            const float half = std::max(0.0f, width) * 0.5f;
            const float d = targetValue - focusValue;
            if (d > half) return focusValue + d - half;
            if (d < -half) return focusValue + d + half;
            return focusValue;
        };
        desired.x = zoneAxis(focus.x, target.position.x, follow.deadZone.x) + offset.x;
        desired.y = zoneAxis(focus.y, target.position.y, follow.deadZone.y) + offset.y;
    }
    if (follow.useBounds){
        const Vector3 lo(std::min(follow.boundsMin.x, follow.boundsMax.x), std::min(follow.boundsMin.y, follow.boundsMax.y),
                         std::min(follow.boundsMin.z, follow.boundsMax.z));
        const Vector3 hi(std::max(follow.boundsMin.x, follow.boundsMax.x), std::max(follow.boundsMin.y, follow.boundsMax.y),
                         std::max(follow.boundsMin.z, follow.boundsMax.z));
        desired = Vector3(std::max(lo.x, std::min(hi.x, desired.x)), std::max(lo.y, std::min(hi.y, desired.y)),
                          std::max(lo.z, std::min(hi.z, desired.z)));
    }

    Vector3 world;
    if (snapNow){
        world = desired;
        follow.velocity = Vector3(0, 0, 0);
    }else{
        world = Vector3(smoothDamp(currentWorld.x, desired.x, follow.velocity.x, follow.damping.x, follow.maxSpeed, dt),
                        smoothDamp(currentWorld.y, desired.y, follow.velocity.y, follow.damping.y, follow.maxSpeed, dt),
                        smoothDamp(currentWorld.z, desired.z, follow.velocity.z, follow.damping.z, follow.maxSpeed, dt));
    }

    placement.valid = true;
    placement.desired = desired;
    placement.position = parentInverse * world;

    const Vector3 lookPoint = target.position + follow.lookAtOffset;
    if (follow.lookAt){
        if (camera && camera->useTarget){
            // CameraComponent::target is where updateCamera aims: in the parent's space, through
            // the camera's own rotation and scale when it has a parent (worldTarget = model * (target - position))
            placement.hasCameraTarget = true;
            if (transform->parent == NULL_ENTITY){
                placement.cameraTarget = lookPoint;
            }else{
                const Matrix4 rs = transform->rotation.getRotationMatrix() * Matrix4::scaleMatrix(transform->scale);
                placement.cameraTarget = placement.position + rs.inverse() * (parentInverse * lookPoint - placement.position);
            }
        }else{
            // a camera looks down its -Z, any other GameObject turns its +Z to the target (Unity's LookAt)
            const Vector3 direction = lookPoint - world;
            if (direction.squaredLength() > 1e-12f){
                const Quaternion worldRotation = Quaternion::lookRotation(camera ? -direction : direction, Vector3(0, 1, 0));
                const Quaternion local = parent.rotation.inverse() * worldRotation;
                placement.hasRotation = true;
                if (snapNow || follow.rotationDamping <= 0.0f || !(dt > 0.0f)){
                    placement.rotation = (snapNow || follow.rotationDamping <= 0.0f) ? local : transform->rotation;
                }else{
                    const float t = 1.0f - std::exp(-4.6f * dt / follow.rotationDamping);
                    placement.rotation = Quaternion::slerp(t, transform->rotation, local).normalized();
                }
            }
        }
    }else if (camera && camera->useTarget){
        // no aim: the camera keeps looking the way it does, its target moves with it
        placement.hasCameraTarget = true;
        placement.cameraTarget = camera->target + (placement.position - transform->position);
    }
    return placement;
}

void CameraControlSystem::followOne(Entity entity, CameraFollowComponent& follow, float dt){
    Transform* transform = scene->findComponent<Transform>(entity);
    if (!transform) return;

    if (!targetAlive(follow, entity)){
        // S9: a destroyed target stops the follower (it holds where it is)
        if (follow.target != NULL_ENTITY && !follow.warnedMissingTarget){
            follow.warnedMissingTarget = true;
            Log::warn("Camera Follow on '%s': its target is gone (destroyed or not in this scene); it holds where it is",
                      scene->getEntityName(entity).c_str());
        }
        return;
    }
    follow.warnedMissingTarget = false;

    CameraComponent* camera = scene->findComponent<CameraComponent>(entity);
    // what this follower wrote last frame is still there, or someone else moved it
    if (follow.placed){
        const bool rotationWritten = follow.lookAt && !(camera && camera->useTarget);
        if (transform->position != follow.writtenPosition || (rotationWritten && transform->rotation != follow.writtenRotation)){
            follow.foreignWriteDetected = true;
            if (!follow.warnedForeignWrite){
                follow.warnedForeignWrite = true;
                Log::warn("Camera Follow on '%s' owns its Transform: another script or component moved it and is overwritten "
                          "(disable the Camera Follow while something else moves it)", scene->getEntityName(entity).c_str());
            }
        }
    }

    const bool snapNow = !follow.placed || follow.followedTarget != follow.target;
    FollowPlacement placement = computeFollow(entity, follow, dt, snapNow);
    if (!placement.valid) return;

    transform->position = placement.position;
    if (placement.hasRotation){
        transform->rotation = placement.rotation;
    }
    transform->needUpdate = true;
    if (camera){
        if (placement.hasCameraTarget){
            camera->target = placement.cameraTarget;
        }
        camera->needUpdate = true;
    }

    follow.desired = placement.desired;
    follow.writtenPosition = transform->position;
    follow.writtenRotation = transform->rotation;
    follow.writtenTarget = camera ? camera->target : Vector3(0, 0, 0);
    follow.followedTarget = follow.target;
    follow.placed = true;
}

void CameraControlSystem::updateFollowers(float scaledDt, float unscaledDt){
    auto followers = scene->getComponentArray<CameraFollowComponent>();
    for (size_t i = 0; i < followers->size(); i++){
        CameraFollowComponent& follow = followers->getComponentFromIndex(i);
        const Entity entity = followers->getEntity(i);
        if (!follow.enabled || !scene->isEntityActiveInHierarchy(entity)) continue;
        // a hit-stop freezes a scaled follow, unless the camera is Hit Stop Exempt (S48)
        followOne(entity, follow, follow.unscaledTime ? unscaledDt : (float)pixelart::HitStop::deltaFor(scene, entity, scaledDt));
    }
}

// ---------------------------------------------------------------- shakers

ShakerRuntime& CameraControlSystem::ensureRuntime(CameraShakerComponent& shaker){
    if (!shaker.runtime){
        shaker.runtime = std::make_shared<ShakerRuntime>();
    }
    return *shaker.runtime;
}

uint64_t CameraControlSystem::play(Entity entity, CameraShakerComponent& shaker, const ShakePreset& preset,
                                   const std::string& name, const ShakeOptions& options, float scale){
    if (!(scale > 0.0f)) return 0;
    ShakerRuntime& runtime = ensureRuntime(shaker);

    ShakePreset clean = preset;
    sanitizeShakePreset(clean);
    ShakeInstance instance(clean, options.hasSeed ? options.seed : randomShakeSeed());
    instance.setFadeTimes(options.fadeIn, options.fadeOut);
    instance.strengthScale = sanitizeScale(options.strength) * scale;
    instance.roughnessScale = sanitizeScale(options.roughness) * scale;

    // the cap: the oldest one-shot goes (else the oldest), with a warning once
    if (runtime.instances.size() >= MAX_SHAKE_INSTANCES){
        auto victim = std::find_if(runtime.instances.begin(), runtime.instances.end(), [](const ShakerInstanceEntry& entry){
            return entry.instance.getPreset().type != ShakeType::Sustained;
        });
        if (victim == runtime.instances.end()) victim = runtime.instances.begin();
        runtime.instances.erase(victim);
        if (!runtime.warnedCap){
            runtime.warnedCap = true;
            Log::warn("Camera Shaker on '%s': more than %zu shakes at once, the oldest goes", scene->getEntityName(entity).c_str(),
                      MAX_SHAKE_INSTANCES);
        }
    }

    ShakerInstanceEntry entry;
    entry.id = ShakerRuntime::nextInstanceId();
    entry.presetName = name;
    entry.instance = instance;
    entry.unscaled = options.unscaled;
    runtime.instances.push_back(std::move(entry));
    return runtime.instances.back().id;
}

void CameraControlSystem::stopAll(CameraShakerComponent& shaker, float fade){
    if (!shaker.runtime) return;
    for (ShakerInstanceEntry& entry : shaker.runtime->instances){
        entry.instance.stop(fade);
    }
}

void CameraControlSystem::addTrauma(CameraShakerComponent& shaker, float amount){
    ensureRuntime(shaker).trauma.add(amount);
}

bool CameraControlSystem::is2D(Entity entity, const CameraShakerComponent& shaker) const{
    if (shaker.mode == CameraShakeMode::Mode2D) return true;
    if (shaker.mode == CameraShakeMode::Mode3D) return false;
    if (const CameraComponent* camera = scene->findComponent<CameraComponent>(entity)){
        return isOrthographic(*camera);
    }
    return scene->findComponent<UIComponent>(entity) || scene->findComponent<RectTransformComponent>(entity);
}

float CameraControlSystem::distanceScale(Entity entity, const CameraShakerComponent& shaker, const Vector3& point, float radius) const{
    if (!(radius > 0.0f)) return 0.0f;
    const Vector3 position = computeWorldNow(entity).position;
    Vector3 d = point - position;
    if (is2D(entity, shaker)) d.z = 0.0f;
    return std::max(0.0f, 1.0f - d.length() / radius);
}

void CameraControlSystem::restoreTransformBase(Entity entity, ShakerRuntime& runtime){
    if (!runtime.transformActive) return;
    runtime.transformActive = false;
    runtime.baseRestored = false;
    Transform* transform = scene->findComponent<Transform>(entity);
    if (!transform) return;
    // what the shake wrote goes back to the base; a value someone else wrote meanwhile stays
    if (runtime.anchored){
        if (RectTransformComponent* layout = scene->findComponent<RectTransformComponent>(entity)){
            if (layout->positionOffset == runtime.writtenAnchorOffset) layout->positionOffset = runtime.baseAnchorOffset;
        }
    }else if (transform->position == runtime.writtenPosition){
        transform->position = runtime.basePosition;
    }
    if (transform->rotation == runtime.writtenRotation){
        transform->rotation = runtime.baseRotation;
    }
    transform->needUpdate = true;
}

void CameraControlSystem::applyTransformOffset(Entity entity, ShakerRuntime& runtime, const Vector3& position, const Vector3& rotation){
    Transform* transform = scene->findComponent<Transform>(entity);
    if (!transform) return;
    RectTransformComponent* layout = scene->findComponent<RectTransformComponent>(entity);
    const bool anchored = layout && layout->usingAnchors;
    if (runtime.transformActive && runtime.anchored != anchored){
        restoreTransformBase(entity, runtime);
    }
    if (isZero(position) && isZero(rotation)){
        restoreTransformBase(entity, runtime);
        runtime.applied = false;
        return;
    }

    // PoseBlendBone's rule: the value still holds what was written last frame -> the base is the
    // one remembered; anything else was written by someone (a script, an animation) and is the base
    if (!runtime.transformActive || runtime.baseRestored){
        // first frame, or the base was written back before this frame's scripts: what is there
        // now is the base
        runtime.basePosition = transform->position;
        runtime.baseRotation = transform->rotation;
        if (anchored) runtime.baseAnchorOffset = layout->positionOffset;
        runtime.baseRestored = false;
    }else{
        if (anchored){
            if (layout->positionOffset != runtime.writtenAnchorOffset) runtime.baseAnchorOffset = layout->positionOffset;
        }else if (transform->position != runtime.writtenPosition){
            runtime.basePosition = transform->position;
        }
        if (transform->rotation != runtime.writtenRotation) runtime.baseRotation = transform->rotation;
    }

    if (anchored){
        // the layout places an anchored element from its offset every frame: the move goes to
        // the offset (in layout space), as flushPoseBlend does
        UISystem* ui = scene->getSystem<UISystem>().get();
        const Vector2 delta = ui->parentToLayoutSpace(entity, Vector2(position.x, position.y)) -
                              ui->parentToLayoutSpace(entity, Vector2(0, 0));
        layout->positionOffset = runtime.baseAnchorOffset + delta;
        runtime.writtenAnchorOffset = layout->positionOffset;
    }else{
        transform->position = runtime.basePosition + position;
        runtime.writtenPosition = transform->position;
    }
    transform->rotation = runtime.baseRotation * shakeEulerToQuaternion(rotation);
    runtime.writtenRotation = transform->rotation;
    transform->needUpdate = true;

    runtime.transformActive = true;
    runtime.anchored = anchored;
    runtime.applied = true;
}

void CameraControlSystem::applyOffset(Entity entity, const CameraShakerComponent& shaker, ShakerRuntime& runtime,
                                      const ShakeOffset& raw){
    CameraComponent* camera = scene->findComponent<CameraComponent>(entity);
    Vector3 position = raw.position;
    Vector3 rotation = raw.rotation;
    const bool flat = is2D(entity, shaker);
    // S48: what one art pixel is in this shaker's offset space
    const float pixel = flat ? pixelWorldSize(entity, camera) : 1.0f / (float)std::max(1, Engine::getPixelsPerUnit());
    if (flat){
        // x/y and roll; a preset unit is a tenth of the view height (Unity's 2D camera is 10 tall)
        float unit = 1.0f;
        if (camera){
            if (isOrthographic(*camera)) unit = std::fabs(camera->topClip - camera->bottomClip) / 10.0f;
        }else if (scene->findComponent<UIComponent>(entity) || scene->findComponent<RectTransformComponent>(entity)){
            // a UI element: tenths of the canvas height
            unit = (float)std::max(1, Engine::getCanvasHeight()) / 10.0f;
        }else{
            // a 2D object: tenths of what the scene's camera shows, as on a camera
            const CameraComponent* view = scene->findComponent<CameraComponent>(scene->getCamera());
            unit = view && isOrthographic(*view) ? std::fabs(view->topClip - view->bottomClip) / 10.0f
                                                 : (float)std::max(1, Engine::getCanvasHeight()) / 10.0f;
        }
        position = Vector3(position.x * unit + raw.pixels.x * pixel, position.y * unit + raw.pixels.y * pixel, 0.0f);
        rotation = Vector3(0.0f, 0.0f, rotation.z);
    }else{
        // a 3D shaker: a pixel-unit preset moves by world units of the pixel grid
        position += raw.pixels * pixel;
    }
    position = position * sanitizeScale(shaker.positionScale);
    rotation = rotation * sanitizeScale(shaker.rotationScale);
    if (shaker.pixelSnap && flat && pixel > 0.0f){
        // S48 Pixel Snap: whole art pixels (S41's rounding rule) and no roll (S42's no-rotation rule)
        position = Vector3(pixelart::snap(position.x / pixel) * pixel, pixelart::snap(position.y / pixel) * pixel, 0.0f);
        rotation = Vector3(0, 0, 0);
    }
    runtime.offsetPosition = position;
    runtime.offsetRotation = rotation;

    if (!camera){
        // a camera GameObject never gets here; a shaker without one shakes its Transform
        if (scene->findComponent<RigidbodyComponent>(entity) || scene->findComponent<Rigidbody2DComponent>(entity)){
            if (!runtime.warnedBody){
                runtime.warnedBody = true;
                Log::warn("Camera Shaker on '%s': a GameObject with a Rigidbody is not shaken (shake a camera, or a child "
                          "without a body)", scene->getEntityName(entity).c_str());
            }
            runtime.offsetPosition = Vector3(0, 0, 0);
            runtime.offsetRotation = Vector3(0, 0, 0);
            runtime.applied = false;
            return;
        }
        applyTransformOffset(entity, runtime, position, rotation);
        return;
    }

    if (isZero(position) && isZero(rotation)){
        if (camera->hasViewOffset){
            camera->hasViewOffset = false;
            camera->needUpdate = true;
        }
        runtime.applied = false;
        return;
    }
    camera->hasViewOffset = true;
    camera->viewOffsetPosition = position;
    camera->viewOffsetRotation = shakeEulerToQuaternion(rotation);
    // a 2D view rolls about its centre, a 3D one about the eye
    if (is2D(entity, shaker) && isOrthographic(*camera)){
        camera->viewOffsetPivot = Vector3((camera->leftClip + camera->rightClip) * 0.5f, (camera->bottomClip + camera->topClip) * 0.5f, 0.0f);
    }else{
        camera->viewOffsetPivot = Vector3(0, 0, 0);
    }
    camera->needUpdate = true;
    runtime.applied = true;
}

float CameraControlSystem::pixelWorldSize(Entity entity, const CameraComponent* camera) const{
    const CameraComponent* view = camera ? camera : scene->findComponent<CameraComponent>(scene->getCamera());
    if (!camera && (scene->findComponent<UIComponent>(entity) || scene->findComponent<RectTransformComponent>(entity))){
        // a UI element moves in canvas units
        return 1.0f;
    }
    // the S41 grid: a base pixel is 1 / pixels per unit world units
    const PixelPerfectViewRequest& grid = Engine::getPixelPerfectView();
    if (grid.active && grid.pixelsPerUnit > 0 && (!view || isOrthographic(*view))){
        return 1.0f / (float)grid.pixelsPerUnit;
    }
    // else the camera's screen pixels
    if (view && isOrthographic(*view)){
        const float pixels = Engine::getViewRect().getHeight();
        const float height = std::fabs(view->topClip - view->bottomClip);
        if (pixels > 0.0f && height > 0.0f) return height / pixels;
    }
    return 1.0f / (float)std::max(1, Engine::getPixelsPerUnit());
}

void CameraControlSystem::clearOffset(Entity entity, ShakerRuntime& runtime){
    if (CameraComponent* camera = scene->findComponent<CameraComponent>(entity)){
        if (camera->hasViewOffset){
            camera->hasViewOffset = false;
            camera->needUpdate = true;
        }
    }
    restoreTransformBase(entity, runtime);
    runtime.offsetPosition = Vector3(0, 0, 0);
    runtime.offsetRotation = Vector3(0, 0, 0);
    runtime.applied = false;
}

void CameraControlSystem::stepShaker(Entity entity, const CameraShakerComponent& shaker, ShakerRuntime& runtime,
                                     float scaledDt, float unscaledDt){
    // a hit-stop freezes the scaled shakes of this shaker, unless it is Hit Stop Exempt (S48)
    scaledDt = (float)pixelart::HitStop::deltaFor(scene, entity, scaledDt);
    ShakeOffset raw;
    bool anyUnscaled = shaker.unscaledTime;
    for (auto it = runtime.instances.begin(); it != runtime.instances.end();){
        const bool unscaled = it->unscaled < 0 ? shaker.unscaledTime : it->unscaled > 0;
        anyUnscaled = anyUnscaled || unscaled;
        raw += it->instance.update(unscaled ? unscaledDt : scaledDt);
        if (it->instance.isFinished()){
            it = runtime.instances.erase(it);
        }else{
            ++it;
        }
    }
    runtime.trauma.step(shaker.unscaledTime ? unscaledDt : scaledDt, shaker.traumaDecay, shaker.traumaFrequency);
    raw += runtime.trauma.getOffset(std::max(1.0f, std::min(4.0f, shaker.traumaPower)), shaker.traumaMaxOffset, shaker.traumaMaxAngle);
    if (shaker.tickRate > 0.0f && std::isfinite(shaker.tickRate)){
        // S48 Tick Rate: a new offset only on ticks of the shake's clock (unscaled when the
        // shaker or one of its shakes is: a hit-stop's shake ticks through the freeze)
        const float period = 1.0f / shaker.tickRate;
        if (!runtime.tickHeld || runtime.tickClock >= period){
            runtime.heldRaw = raw;
            if (runtime.tickHeld) runtime.tickClock = std::fmod(runtime.tickClock, period);
            runtime.tickHeld = true;
        }
        raw = runtime.heldRaw;
        const float dt = anyUnscaled ? unscaledDt : scaledDt;
        if (dt > 0.0f) runtime.tickClock += dt;
        if (runtime.instances.empty() && runtime.trauma.trauma <= 0.0f){
            // nothing left to shake: the next shake starts on a tick of its own
            runtime.tickHeld = false;
            runtime.tickClock = 0.0f;
        }
    }
    applyOffset(entity, shaker, runtime, raw);
}

void CameraControlSystem::updateShakers(float scaledDt, float unscaledDt){
    auto shakers = scene->getComponentArray<CameraShakerComponent>();
    for (size_t i = 0; i < shakers->size(); i++){
        CameraShakerComponent& shaker = shakers->getComponentFromIndex(i);
        const Entity entity = shakers->getEntity(i);
        const bool active = shaker.enabled && scene->isEntityActiveInHierarchy(entity);
        if (!active){
            // disabled: the offset goes once, the instances and the trauma hold
            if (shaker.runtime && shaker.runtime->wasActive){
                shaker.runtime->wasActive = false;
                clearOffset(entity, *shaker.runtime);
            }
            continue;
        }

        ShakerRuntime& runtime = ensureRuntime(shaker);
        if (!runtime.wasActive){
            runtime.wasActive = true;
            // Unity's OnEnable: the start preset plays on each enable, unless the one it started
            // still plays (it held while disabled): a disable and an enable never pile it up
            if (!shaker.startPreset.empty() && !runtime.findInstance(runtime.startInstanceId)){
                ShakePreset preset;
                if (CameraShake::resolveSource(ShakeSource(shaker.startPreset), preset)){
                    runtime.startInstanceId = play(entity, shaker, preset, shaker.startPreset, ShakeOptions());
                }
            }
        }
        if (runtime.isIdle()) continue;
        // play() above may have grown the runtime, never the component array
        stepShaker(entity, shaker, runtime, scaledDt, unscaledDt);
    }
}

void CameraControlSystem::resetAll(){
    stopPreview();
    auto shakers = scene->getComponentArray<CameraShakerComponent>();
    for (size_t i = 0; i < shakers->size(); i++){
        CameraShakerComponent& shaker = shakers->getComponentFromIndex(i);
        if (shaker.runtime){
            clearOffset(shakers->getEntity(i), *shaker.runtime);
            shaker.runtime.reset();
        }
    }
    auto followers = scene->getComponentArray<CameraFollowComponent>();
    for (size_t i = 0; i < followers->size(); i++){
        CameraFollowComponent& follow = followers->getComponentFromIndex(i);
        const CameraFollowComponent defaults;
        follow.velocity = defaults.velocity;
        follow.desired = defaults.desired;
        follow.followedTarget = NULL_ENTITY;
        follow.placed = false;
        follow.foreignWriteDetected = false;
        follow.warnedForeignWrite = false;
        follow.warnedMissingTarget = false;
    }
}

void CameraControlSystem::onComponentRemoved(Entity entity, ComponentId componentId){
    if (componentId == scene->getComponentId<CameraShakerComponent>()){
        CameraShakerComponent* shaker = scene->findComponent<CameraShakerComponent>(entity);
        if (shaker && shaker->runtime){
            clearOffset(entity, *shaker->runtime);
        }
    }else if (componentId == scene->getComponentId<CameraComponent>()){
        if (entity == previewEntity){
            stopPreview();
        }
        // the shaker goes on in transform mode from the GameObject's own pose (the view offset
        // goes with the camera)
        CameraShakerComponent* shaker = scene->findComponent<CameraShakerComponent>(entity);
        if (shaker && shaker->runtime){
            shaker->runtime->applied = false;
            shaker->runtime->transformActive = false;
            shaker->runtime->baseRestored = false;
            shaker->runtime->offsetPosition = Vector3(0, 0, 0);
            shaker->runtime->offsetRotation = Vector3(0, 0, 0);
        }
    }
}

void CameraControlSystem::onComponentAdded(Entity entity, ComponentId componentId){
    if (componentId != scene->getComponentId<CameraComponent>()) return;
    // a transform-mode shaker becomes a camera one: the Transform gets its base back, the
    // shake goes on through the view offset
    CameraShakerComponent* shaker = scene->findComponent<CameraShakerComponent>(entity);
    if (shaker && shaker->runtime){
        restoreTransformBase(entity, *shaker->runtime);
        shaker->runtime->applied = false;
        shaker->runtime->baseRestored = false;
    }
}

void CameraControlSystem::preUpdate(){
    if (paused) return;
    // transform mode: the base goes back before this frame's scripts and animations, so what
    // they do (position = position + v * dt included) works on the unshaken pose; update()
    // puts the offset back over whatever they left
    auto shakers = scene->getComponentArray<CameraShakerComponent>();
    for (size_t i = 0; i < shakers->size(); i++){
        CameraShakerComponent& shaker = shakers->getComponentFromIndex(i);
        if (!shaker.runtime || !shaker.runtime->transformActive) continue;
        ShakerRuntime& runtime = *shaker.runtime;
        const Entity entity = shakers->getEntity(i);
        Transform* transform = scene->findComponent<Transform>(entity);
        if (!transform) continue;
        if (runtime.anchored){
            if (RectTransformComponent* layout = scene->findComponent<RectTransformComponent>(entity)){
                if (layout->positionOffset == runtime.writtenAnchorOffset) layout->positionOffset = runtime.baseAnchorOffset;
            }
        }else if (transform->position == runtime.writtenPosition){
            transform->position = runtime.basePosition;
        }
        if (transform->rotation == runtime.writtenRotation){
            transform->rotation = runtime.baseRotation;
        }
        transform->needUpdate = true;
        runtime.baseRestored = true;
    }
}

// ---------------------------------------------------------------- edit-mode preview

bool CameraControlSystem::beginPreview(Entity entity){
    if (!scene->isEntityCreated(entity) || !scene->findComponent<CameraComponent>(entity)) return false;
    if (previewEntity != entity){
        stopPreview();
        previewEntity = entity;
    }
    // the GameObject's own settings when it has a shaker (the Inspector's edits show live), else the defaults
    const CameraShakerComponent* shaker = scene->findComponent<CameraShakerComponent>(entity);
    previewSettings = shaker ? *shaker : CameraShakerComponent();
    previewSettings.runtime.reset();
    if (!previewRuntime) previewRuntime = std::make_shared<ShakerRuntime>();
    return true;
}

bool CameraControlSystem::previewShake(Entity entity, const ShakePreset& preset, const std::string& name){
    if (!beginPreview(entity)) return false;
    ShakePreset clean = preset;
    sanitizeShakePreset(clean);
    ShakerInstanceEntry entry;
    entry.id = ShakerRuntime::nextInstanceId();
    entry.presetName = name;
    entry.instance = ShakeInstance(clean, randomShakeSeed());
    if (previewRuntime->instances.size() >= MAX_SHAKE_INSTANCES){
        previewRuntime->instances.erase(previewRuntime->instances.begin());
    }
    previewRuntime->instances.push_back(std::move(entry));
    return true;
}

bool CameraControlSystem::previewAddTrauma(Entity entity, float amount){
    if (!beginPreview(entity)) return false;
    previewRuntime->trauma.add(amount);
    return true;
}

void CameraControlSystem::previewStop(float fade){
    if (!previewRuntime) return;
    for (ShakerInstanceEntry& entry : previewRuntime->instances) entry.instance.stop(fade);
    previewRuntime->trauma.trauma = 0.0f;
}

void CameraControlSystem::stopPreview(){
    if (previewEntity != NULL_ENTITY && previewRuntime && scene->isEntityCreated(previewEntity)){
        clearOffset(previewEntity, *previewRuntime);
    }
    previewEntity = NULL_ENTITY;
    previewRuntime.reset();
}

bool CameraControlSystem::isPreviewing() const{
    return previewEntity != NULL_ENTITY && previewRuntime && !previewRuntime->isIdle();
}

Entity CameraControlSystem::getPreviewEntity() const{
    return previewEntity;
}

const ShakerRuntime* CameraControlSystem::getPreviewRuntime() const{
    return previewRuntime.get();
}

void CameraControlSystem::updatePreview(float dt){
    if (previewEntity == NULL_ENTITY || !previewRuntime) return;
    if (!scene->isEntityCreated(previewEntity) || !scene->findComponent<CameraComponent>(previewEntity)){
        previewEntity = NULL_ENTITY;
        previewRuntime.reset();
        return;
    }
    if (previewRuntime->isIdle()) return;
    // the Inspector's edits show in the preview as they are made
    if (const CameraShakerComponent* live = scene->findComponent<CameraShakerComponent>(previewEntity)){
        previewSettings = *live;
        previewSettings.runtime.reset();
    }
    // every preview shake runs on the editor's clock (edit mode has no time scale)
    for (ShakerInstanceEntry& entry : previewRuntime->instances) entry.unscaled = 1;
    previewSettings.unscaledTime = true;
    stepShaker(previewEntity, previewSettings, *previewRuntime, dt, dt);
}
