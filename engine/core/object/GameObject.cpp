// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "GameObject.h"

#include "subsystem/RenderSystem.h"
#include "subsystem/PhysicsSystem.h"
#include "manager/ObjectManager.h"
#include "Engine.h"

using namespace jasperengine;

GameObject::GameObject(Scene* scene): EntityHandle(scene){
    addComponent<Transform>();
}

GameObject::GameObject(Scene* scene, Entity entity): EntityHandle(scene, entity){
}

GameObject::~GameObject(){
}

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
    if (transform.visible != visible){
        transform.visible = visible;
        transform.needUpdateChildVisibility = true;
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
#endif

#ifdef JASPERENGINE_PHYSICS_3D
Rigidbody GameObject::getRigidbody(){
    scene->getSystem<PhysicsSystem>()->createRigidbody(entity);
    return Rigidbody(scene, entity);
}

void GameObject::removeRigidbody(){
    scene->getSystem<PhysicsSystem>()->removeRigidbody(entity);
}
#endif

Ray GameObject::getRay(Vector3 direction){
    return Ray(getWorldPosition(), direction);
}
