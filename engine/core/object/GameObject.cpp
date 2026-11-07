// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "GameObject.h"
#include "component/Transform.h"
#include "object/environment/WaterVolume.h"
#include "object/environment/WaterExcluder.h"
#include "object/environment/WaterWake.h"
#include "object/physics/Buoyancy.h"
#include "object/physics/CharacterController.h"
#include "object/physics/CharacterController2D.h"
#include "object/spline/Spline.h"
#include "object/spline/SplineAnimate.h"
#include "object/spline/SplineExtrude.h"
#include "object/spline/SplineInstantiate.h"

#include "subsystem/RenderSystem.h"
#include "subsystem/PhysicsSystem.h"
#include "manager/ObjectManager.h"
#include "Engine.h"
#include "Log.h"

#include <algorithm>

using namespace jasperengine;

GameObject::GameObject(Scene* scene): EntityHandle(scene){
    addComponent<Transform>();
}

GameObject::GameObject(Scene* scene, Entity entity): EntityHandle(scene, entity){
}

GameObject::~GameObject(){
}

GameObject::GameObject(const GameObject& rhs) = default;
GameObject& GameObject::operator=(const GameObject& rhs) = default;
GameObject::GameObject(GameObject&& rhs) noexcept = default;
GameObject& GameObject::operator=(GameObject&& rhs) noexcept = default;

GameObject GameObject::instantiate(const PrefabAsset& prefab){
    Scene* scene = ObjectManager::getActiveScene();
    return GameObject(scene, ObjectManager::instantiate(prefab, scene));
}

GameObject GameObject::instantiate(const PrefabAsset& prefab, Scene* scene){
    scene = scene ? scene : ObjectManager::getActiveScene();
    return GameObject(scene, ObjectManager::instantiate(prefab, scene));
}

GameObject GameObject::instantiate(const PrefabAsset& prefab, const EntityHandle& parent, bool instantiateInWorldSpace){
    return GameObject(parent.getScene(), ObjectManager::instantiate(prefab, parent.getScene(), parent.getEntity(), instantiateInWorldSpace));
}

GameObject GameObject::instantiate(const PrefabAsset& prefab, const Vector3& position, const Quaternion& rotation){
    Scene* scene = ObjectManager::getActiveScene();
    return GameObject(scene, ObjectManager::instantiate(prefab, scene, position, rotation));
}

GameObject GameObject::instantiate(const PrefabAsset& prefab, const Vector3& position, const Quaternion& rotation, const EntityHandle& parent){
    return GameObject(parent.getScene(), ObjectManager::instantiate(prefab, parent.getScene(), position, rotation, parent.getEntity()));
}

GameObject GameObject::instantiate(const PrefabAsset& prefab, Scene* scene, const Vector3& position, const Quaternion& rotation){
    scene = scene ? scene : ObjectManager::getActiveScene();
    return GameObject(scene, ObjectManager::instantiate(prefab, scene, position, rotation));
}

GameObject GameObject::instantiate(const EntityHandle& original){
    return GameObject(original.getScene(), ObjectManager::instantiate(original.getScene(), original.getEntity()));
}

GameObject GameObject::instantiate(const EntityHandle& original, const EntityHandle& parent, bool instantiateInWorldSpace){
    return GameObject(parent.getScene(), ObjectManager::instantiate(original.getScene(), original.getEntity(), parent.getScene(), parent.getEntity(), instantiateInWorldSpace));
}

GameObject GameObject::instantiate(const EntityHandle& original, const Vector3& position, const Quaternion& rotation){
    return GameObject(original.getScene(), ObjectManager::instantiate(original.getScene(), original.getEntity(), original.getScene(), position, rotation));
}

GameObject GameObject::instantiate(const EntityHandle& original, const Vector3& position, const Quaternion& rotation, const EntityHandle& parent){
    return GameObject(parent.getScene(), ObjectManager::instantiate(original.getScene(), original.getEntity(), parent.getScene(), position, rotation, parent.getEntity()));
}

void GameObject::destroy(const EntityHandle& object, float delay){
    ObjectManager::destroy(object.getScene(), object.getEntity(), delay);
}

void GameObject::destroyImmediate(const EntityHandle& object){
    ObjectManager::destroyImmediate(object.getScene(), object.getEntity());
}

void GameObject::setPosition(Vector3 position){
    Transform& transform = getComponent<Transform>();

    if (transform.position != position){
        transform.position = position;

        transform.needUpdate = true;
        markChanged<Transform>();
    }
}

void GameObject::setPosition(const float x, const float y, const float z){
    setPosition(Vector3(x,y,z));
}

void GameObject::setPosition(const float x, const float y){
    setPosition(Vector3(x,y,0));
}

Vector3 GameObject::getPosition() const{
    Transform& transform = getComponent<Transform>();
    return transform.position;
}

Vector3 GameObject::getWorldPosition() const{
    Transform& transform = getComponent<Transform>();
    return transform.worldPosition;
}

void GameObject::setRotation(Quaternion rotation){
    Transform& transform = getComponent<Transform>();

    if (transform.rotation != rotation){
        transform.rotation = rotation;

        transform.needUpdate = true;
        markChanged<Transform>();
    }
}

void GameObject::setRotation(const float xAngle, const float yAngle, const float zAngle){
    setRotation(Quaternion(xAngle, yAngle, zAngle));
}

Quaternion GameObject::getRotation() const{
    Transform& transform = getComponent<Transform>();
    return transform.rotation;
}

Quaternion GameObject::getWorldRotation() const{
    Transform& transform = getComponent<Transform>();
    return transform.worldRotation;
}

void GameObject::setScale(const float factor){
    setScale(Vector3(factor,factor,factor));
}

void GameObject::setScale(Vector3 scale){
    Transform& transform = getComponent<Transform>();

    if (transform.scale != scale){
        transform.scale = scale;

        transform.needUpdate = true;
        markChanged<Transform>();
    }
}

Vector3 GameObject::getScale() const{
    Transform& transform = getComponent<Transform>();
    return transform.scale;
}

Vector3 GameObject::getWorldScale() const{
    Transform& transform = getComponent<Transform>();
    return transform.worldScale;
}

void GameObject::setVisible(bool visible){
    Transform& transform = getComponent<Transform>();
    // Hiding always reaches the children: a parent saved hidden (the editor stores each flag on
    // its own) keeps visible children until it is hidden again. Showing a visible parent stays a
    // no-op, so a child hidden on its own is not shown again by it. Only this explicit change
    // cascades: copies, scene moves and reparenting keep every child's own flag.
    if (transform.visible != visible || !visible){
        transform.visible = visible;
        transform.needUpdateChildVisibility = true;
        markChanged<Transform>();
    }
}

bool GameObject::isVisible() const{
    Transform& transform = getComponent<Transform>();
    return transform.visible;
}

void GameObject::setVisibleOnly(bool visible){
    Transform& transform = getComponent<Transform>();
    transform.visible = visible;
}

void GameObject::setBillboard(bool billboard, bool fake, bool cylindrical){
    Transform& transform = getComponent<Transform>();

    transform.billboard = billboard;
    transform.fakeBillboard = fake;
    transform.cylindricalBillboard = cylindrical;
}

void GameObject::setBillboard(bool billboard){
    Transform& transform = getComponent<Transform>();

    transform.billboard = billboard;
}

bool GameObject::isBillboard() const{
    Transform& transform = getComponent<Transform>();

    return transform.billboard;
}

void GameObject::setBillboardRotation(Quaternion rotation){
    Transform& transform = getComponent<Transform>();

    if (transform.billboardRotation != rotation){
        transform.billboardRotation = rotation;
        transform.needUpdate = true;
        markChanged<Transform>();
    }
}

void GameObject::setBillboardRotation(const float xAngle, const float yAngle, const float zAngle){
    setBillboardRotation(Quaternion(xAngle, yAngle, zAngle));
}

Quaternion GameObject::getBillboardRotation() const{
    Transform& transform = getComponent<Transform>();

    return transform.billboardRotation;
}

void GameObject::setFakeBillboard(bool fakeBillboard){
    Transform& transform = getComponent<Transform>();

    transform.fakeBillboard = fakeBillboard;
}

bool GameObject::isFakeBillboard() const{
    Transform& transform = getComponent<Transform>();

    return transform.fakeBillboard;
}

void GameObject::setCylindricalBillboard(bool cylindricalBillboard){
    Transform& transform = getComponent<Transform>();

    transform.cylindricalBillboard = cylindricalBillboard;
}

bool GameObject::isCylindricalBillboard() const{
    Transform& transform = getComponent<Transform>();

    return transform.cylindricalBillboard;
}

void GameObject::setLocalMatrix(Matrix4 localMatrix){
    Transform& transform = getComponent<Transform>();

    if (transform.localMatrix != localMatrix){
        transform.localMatrix = localMatrix;

        localMatrix.decompose(transform.position, transform.scale, transform.rotation);
        // the matrix origin of a pivoted UI element is not its position
        if (RectTransformComponent* layout = scene->findComponent<RectTransformComponent>(entity)){
            transform.position -= getUIPivotShift(*layout, transform.rotation, transform.scale);
        }

        transform.needUpdate = true;
        markChanged<Transform>();
    }
}

Matrix4 GameObject::getLocalMatrix() const{
    Transform& transform = getComponent<Transform>();
    return transform.localMatrix;
}

Matrix4 GameObject::getModelMatrix() const{
    Transform& transform = getComponent<Transform>();
    return transform.modelMatrix;
}

Matrix4 GameObject::getNormalMatrix() const{
    Transform& transform = getComponent<Transform>();
    return transform.normalMatrix;
}

void GameObject::addChild(GameObject* child){
    addChild(child->entity);
}

void GameObject::addChild(Entity child){
    scene->addEntityChild(this->entity, child, false);
}

void GameObject::removeChild(GameObject* child){
    removeChild(child->entity);
}

void GameObject::removeChild(Entity child){
    Transform* childTransform = scene->findComponent<Transform>(child);
    if (childTransform && childTransform->parent == this->entity){
        scene->addEntityChild(NULL_ENTITY, child, false);
    }
}

void GameObject::removeParent(){
    scene->addEntityChild(NULL_ENTITY, this->entity, false);
}

namespace {
    // the direct children of `entity` with a Transform, in Hierarchy order: its branch follows it
    // in the Transform array (parents before children), children are the ones it parents
    std::vector<Entity> directChildren(Scene* scene, Entity entity){
        std::vector<Entity> children;
        auto transforms = scene->getComponentArray<Transform>();
        if (!transforms->hasEntity(entity))
            return children;
        const size_t start = transforms->getIndex(entity);
        const size_t last = scene->findBranchLastIndex(entity);
        for (size_t i = start + 1; i <= last && i < transforms->size(); i++){
            if (transforms->getComponentFromIndex(i).parent == entity)
                children.push_back(transforms->getEntity(i));
        }
        return children;
    }

    // the world matrices of `entity` and its ancestors, root first, as of now (a value set
    // earlier in the frame is in the local fields only until the transform update)
    void refreshWorldMatrices(Scene* scene, Entity entity){
        std::vector<Entity> chain;
        for (Entity e = entity; e != NULL_ENTITY; ){
            Transform* transform = scene->findComponent<Transform>(e);
            if (!transform)
                break;
            chain.push_back(e);
            e = transform->parent;
        }
        RenderSystem* render = scene->getSystem<RenderSystem>().get();
        for (auto it = chain.rbegin(); it != chain.rend(); ++it){
            render->updateTransform(scene->getComponent<Transform>(*it), *it);
        }
    }
}

std::optional<GameObject> GameObject::getParent() const{
    const Transform* transform = scene->findComponent<Transform>(entity);
    if (!transform || transform->parent == NULL_ENTITY)
        return std::nullopt;
    return GameObject(scene, transform->parent);
}

void GameObject::setParent(const EntityHandle* parent, bool worldPositionStays){
    if (!scene->findComponent<Transform>(entity)){
        Log::error("setParent: %s has no Transform", scene->getEntityName(entity).c_str());
        return;
    }
    Entity parentEntity = NULL_ENTITY;
    if (parent && parent->getEntity() != NULL_ENTITY){
        if (parent->getScene() != scene){
            Log::error("setParent: the parent of %s must be in its scene", scene->getEntityName(entity).c_str());
            return;
        }
        parentEntity = parent->getEntity();
        if (parentEntity == entity || scene->isParentOf(entity, parentEntity)){
            Log::error("setParent: %s can't be a child of itself or of one of its children", scene->getEntityName(entity).c_str());
            return;
        }
        if (!scene->findComponent<Transform>(parentEntity)){
            Log::error("setParent: the parent of %s has no Transform", scene->getEntityName(entity).c_str());
            return;
        }
    }
    if (worldPositionStays){
        refreshWorldMatrices(scene, entity);
        if (parentEntity != NULL_ENTITY)
            refreshWorldMatrices(scene, parentEntity);
    }
    scene->addEntityChild(parentEntity, entity, worldPositionStays);
}

size_t GameObject::getChildCount() const{
    return directChildren(scene, entity).size();
}

std::optional<GameObject> GameObject::getChild(size_t index) const{
    const std::vector<Entity> children = directChildren(scene, entity);
    if (index >= children.size())
        return std::nullopt;
    return GameObject(scene, children[index]);
}

std::vector<GameObject> GameObject::getChildren() const{
    std::vector<GameObject> objects;
    for (Entity child : directChildren(scene, entity))
        objects.emplace_back(scene, child);
    return objects;
}

std::optional<GameObject> GameObject::findChild(const std::string& path) const{
    if (path.empty())
        return std::nullopt;
    Entity current = entity;
    size_t begin = 0;
    while (begin <= path.size()){
        const size_t end = std::min(path.find('/', begin), path.size());
        const std::string name = path.substr(begin, end - begin);
        if (name.empty())
            return std::nullopt;
        Entity found = NULL_ENTITY;
        for (Entity child : directChildren(scene, current)){
            if (scene->getEntityName(child) == name){
                found = child;
                break;
            }
        }
        if (found == NULL_ENTITY)
            return std::nullopt;
        current = found;
        begin = end + 1;
        if (end == path.size())
            break;
    }
    return GameObject(scene, current);
}

void GameObject::moveToTop(){
    scene->moveChildToTop(this->entity);
}

void GameObject::moveUp(){
    scene->moveChildUp(this->entity);
}

void GameObject::moveDown(){
    scene->moveChildDown(this->entity);
}

void GameObject::moveToBottom(){
    scene->moveChildToBottom(this->entity);
}

void GameObject::updateTransform(){
    Transform& transform = getComponent<Transform>();

    scene->getSystem<RenderSystem>()->updateTransform(transform, entity);
}

#ifdef JASPERENGINE_PHYSICS_2D
Rigidbody2D GameObject::getRigidbody2D(){
    scene->getSystem<PhysicsSystem>()->createRigidbody2D(entity);
    return Rigidbody2D(scene, entity);
}

void GameObject::removeRigidbody2D(){
    scene->getSystem<PhysicsSystem>()->removeRigidbody2D(entity);
}

CharacterController2D GameObject::getCharacterController2D(){
    if (!scene->findComponent<CharacterController2DComponent>(entity))
        scene->addComponent<CharacterController2DComponent>(entity, {});
    return CharacterController2D(scene, entity);
}

void GameObject::removeCharacterController2D(){
    if (scene->findComponent<CharacterController2DComponent>(entity))
        scene->removeComponent<CharacterController2DComponent>(entity);
}
#endif

#ifdef JASPERENGINE_PHYSICS_3D
Rigidbody GameObject::getRigidbody(){
    scene->getSystem<PhysicsSystem>()->createRigidbody(entity);
    return Rigidbody(scene, entity);
}

void GameObject::removeRigidbody(){
    scene->getSystem<PhysicsSystem>()->removeRigidbody(entity);
}

CharacterController GameObject::getCharacterController(){
    if (!scene->findComponent<CharacterControllerComponent>(entity))
        scene->addComponent<CharacterControllerComponent>(entity, {});
    return CharacterController(scene, entity);
}

void GameObject::removeCharacterController(){
    if (scene->findComponent<CharacterControllerComponent>(entity))
        scene->removeComponent<CharacterControllerComponent>(entity);
}

Buoyancy GameObject::getBuoyancy(){
    if (!scene->findComponent<BuoyancyComponent>(entity))
        scene->addComponent<BuoyancyComponent>(entity, {});
    return Buoyancy(scene, entity);
}
#endif

Ray GameObject::getRay(Vector3 direction){
    return Ray(getWorldPosition(), direction);
}

WaterVolume GameObject::getWaterVolume(){
    if (!scene->findComponent<WaterVolumeComponent>(entity))
        scene->addComponent<WaterVolumeComponent>(entity, {});
    return WaterVolume(scene, entity);
}

WaterExcluder GameObject::getWaterExcluder(){
    if (!scene->findComponent<WaterExcluderComponent>(entity))
        scene->addComponent<WaterExcluderComponent>(entity, {});
    return WaterExcluder(scene, entity);
}

WaterWake GameObject::getWaterWake(){
    if (!scene->findComponent<WaterWakeComponent>(entity))
        scene->addComponent<WaterWakeComponent>(entity, {});
    return WaterWake(scene, entity);
}

Spline GameObject::getSpline(){
    if (!scene->findComponent<SplineComponent>(entity))
        scene->addComponent<SplineComponent>(entity, {});
    return Spline(scene, entity);
}

SplineExtrude GameObject::getSplineExtrude(){
    if (!scene->findComponent<SplineComponent>(entity))
        scene->addComponent<SplineComponent>(entity, {});
    // the chunks' material and shadows are the GameObject's Mesh
    if (!scene->findComponent<MeshComponent>(entity))
        scene->addComponent<MeshComponent>(entity, {});
    if (!scene->findComponent<SplineExtrudeComponent>(entity))
        scene->addComponent<SplineExtrudeComponent>(entity, {});
    return SplineExtrude(scene, entity);
}

SplineInstantiate GameObject::getSplineInstantiate(){
    if (!scene->findComponent<SplineComponent>(entity))
        scene->addComponent<SplineComponent>(entity, {});
    if (!scene->findComponent<SplineInstantiateComponent>(entity))
        scene->addComponent<SplineInstantiateComponent>(entity, {});
    return SplineInstantiate(scene, entity);
}

SplineAnimate GameObject::getSplineAnimate(){
    if (!scene->findComponent<SplineAnimateComponent>(entity))
        scene->addComponent<SplineAnimateComponent>(entity, {});
    return SplineAnimate(scene, entity);
}
