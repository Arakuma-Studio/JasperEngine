// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "EntityRegistry.h"

#include <unordered_set>

using namespace jasperengine;

EntityRegistry::EntityRegistry() {
    registerComponent<MeshComponent>();
    registerComponent<ModelComponent>();
    registerComponent<BoneComponent>();
    registerComponent<SkyboxComponent>();
    registerComponent<FogComponent>();
    registerComponent<MirrorComponent>();
    registerComponent<LayoutGroupComponent>();
    registerComponent<RectTransformComponent>();
    registerComponent<SpriteComponent>();
    registerComponent<SpriteAnimationComponent>();
    registerComponent<Transform>();
    registerComponent<CameraComponent>();
    registerComponent<LightComponent>();
    registerComponent<Light2DComponent>();
    registerComponent<ShadowCaster2DComponent>();
    registerComponent<ActionComponent>();
    registerComponent<TweenComponent>();
    registerComponent<PositionTweenComponent>();
    registerComponent<RotationTweenComponent>();
    registerComponent<ScaleTweenComponent>();
    registerComponent<ColorTweenComponent>();
    registerComponent<AlphaTweenComponent>();
    registerComponent<ParticleSystemComponent>();
    registerComponent<PointsComponent>();
    registerComponent<LineRendererComponent>();
    registerComponent<TextComponent>();
    registerComponent<UIComponent>();
    registerComponent<ImageComponent>();
    registerComponent<ButtonComponent>();
    registerComponent<PanelComponent>();
    registerComponent<ScrollbarComponent>();
    registerComponent<ProgressbarComponent>();
    registerComponent<InputFieldComponent>();
    registerComponent<MeshPolygonComponent>();
    registerComponent<PolygonComponent>();
    registerComponent<AnimationComponent>();
    registerComponent<KeyframeTracksComponent>();
    registerComponent<MorphTracksComponent>();
    registerComponent<RotateTracksComponent>();
    registerComponent<TranslateTracksComponent>();
    registerComponent<ScaleTracksComponent>();
    registerComponent<ScriptComponent>();
    registerComponent<TerrainComponent>();
    registerComponent<AudioSourceComponent>();
    registerComponent<TilemapComponent>();
#ifdef JASPERENGINE_PHYSICS_2D
    registerComponent<Rigidbody2DComponent>();
    registerComponent<Joint2DComponent>();
#endif
#ifdef JASPERENGINE_PHYSICS_3D
    registerComponent<RigidbodyComponent>();
    registerComponent<Joint3DComponent>();
#endif
    registerComponent<InstancedMeshComponent>();
    registerComponent<PrefabComponent>();
    registerComponent<ReflectionProbeComponent>();
}

EntityRegistry::EntityRegistry(EntityPool defaultPool) : EntityRegistry() {
    this->defaultPool = defaultPool;
}

EntityRegistry::~EntityRegistry(){
    std::vector<Entity> entityList = entityManager.getEntityList();
    while(entityList.size() > 0){
        destroyEntity(entityList.front());
        // some entities can destroy other entities (ex: models)
        entityList = entityManager.getEntityList();
    }
}

void EntityRegistry::sortComponentsByTransform(Signature entitySignature){
    // Mesh component
    if (entitySignature.test(getComponentId<MeshComponent>())){
        auto meshes = componentManager.getComponentArray<MeshComponent>();
        meshes->sortByComponent<Transform>(componentManager.getComponentArray<Transform>());
    }

    // InstancedMesh component
    if (entitySignature.test(getComponentId<InstancedMeshComponent>())){
        auto instmeshes = componentManager.getComponentArray<InstancedMeshComponent>();
        instmeshes->sortByComponent<Transform>(componentManager.getComponentArray<Transform>());
    }

    // Model component
    if (entitySignature.test(getComponentId<ModelComponent>())){
        auto models = componentManager.getComponentArray<ModelComponent>();
        models->sortByComponent<Transform>(componentManager.getComponentArray<Transform>());
    }

    // Bone component
    if (entitySignature.test(getComponentId<BoneComponent>())){
        auto bones = componentManager.getComponentArray<BoneComponent>();
        bones->sortByComponent<Transform>(componentManager.getComponentArray<Transform>());
    }

    // Polygon component
    if (entitySignature.test(getComponentId<PolygonComponent>())){
        auto polygons = componentManager.getComponentArray<PolygonComponent>();
        polygons->sortByComponent<Transform>(componentManager.getComponentArray<Transform>());
    }

    // UI layout component
    if (entitySignature.test(getComponentId<RectTransformComponent>())){
        auto layout = componentManager.getComponentArray<RectTransformComponent>();
        layout->sortByComponent<Transform>(componentManager.getComponentArray<Transform>());
    }

    // UI component
    if (entitySignature.test(getComponentId<UIComponent>())){
        auto ui = componentManager.getComponentArray<UIComponent>();
        ui->sortByComponent<Transform>(componentManager.getComponentArray<Transform>());
    }

    // Points component
    if (entitySignature.test(getComponentId<PointsComponent>())){
        auto points = componentManager.getComponentArray<PointsComponent>();
        points->sortByComponent<Transform>(componentManager.getComponentArray<Transform>());
    }

    // Lines component
    if (entitySignature.test(getComponentId<LineRendererComponent>())){
        auto lines = componentManager.getComponentArray<LineRendererComponent>();
        lines->sortByComponent<Transform>(componentManager.getComponentArray<Transform>());
    }

    // Sound component
    if (entitySignature.test(getComponentId<AudioSourceComponent>())){
        auto audios = componentManager.getComponentArray<AudioSourceComponent>();
        audios->sortByComponent<Transform>(componentManager.getComponentArray<Transform>());
    }
}

void EntityRegistry::moveChildAux(Entity entity, bool increase, bool stopIfFound){
    Signature signature = entityManager.getSignature(entity);

    if (signature.test(getComponentId<Transform>())){
        auto transforms = componentManager.getComponentArray<Transform>();

        size_t entityIndex = transforms->getIndex(entity);
        //auto teste = transforms->getComponent(entity);
        Entity entityParent = transforms->getComponent(entity).parent;

        size_t nextIndex = entityIndex;
        if (increase){
            for (int i = (entityIndex+1); i < transforms->size(); i++){
                Transform& next = transforms->getComponentFromIndex(i);
                if (next.parent == entityParent){
                    nextIndex = i;
                    if (stopIfFound)
                        break;
                }
            }
            nextIndex = findBranchLastIndex(transforms->getEntity(nextIndex)) + 1;
        }else{
            for (int i = (entityIndex-1); i >= 0; i--){
                Transform& next = transforms->getComponentFromIndex(i);
                if (next.parent == entityParent){
                    nextIndex = i;
                    if (stopIfFound)
                        break;
                }
            }
        }

        moveChildToIndex(entity, nextIndex);
    }
}

void EntityRegistry::changeTransformChildren(Entity entity){
    // Get the parent of the entity being removed before it's deleted
    Transform* removedTransform = findComponent<Transform>(entity);
    Entity newParent = NULL_ENTITY;
    if (removedTransform) {
        newParent = removedTransform->parent;
    }

    // Find all entities with Transform components that have this entity as parent
    auto transformArray = getComponentArray<Transform>();
    if (transformArray) {
        for (size_t i = 0; i < transformArray->size(); i++) {
            Transform* childTransform = transformArray->findComponentFromIndex(i);
            if (childTransform && childTransform->parent == entity) {
                // Set the parent to the grandparent (parent of removed entity)
                childTransform->parent = newParent;
                // Mark for update since hierarchy changed
                childTransform->needUpdate = true;
                childTransform->needUpdateChildVisibility = true;
            }
        }
    }
}

Entity EntityRegistry::createEntity() {
    return (defaultPool == EntityPool::System)
        ? entityManager.createSystemEntity()
        : entityManager.createUserEntity();
}

Entity EntityRegistry::createUserEntity() {
    return entityManager.createUserEntity();
}

Entity EntityRegistry::createSystemEntity() {
    return entityManager.createSystemEntity();
}

bool EntityRegistry::isEntityCreated(Entity entity) const {
    return entityManager.isCreated(entity);
}

bool EntityRegistry::recreateEntity(Entity entity) {
    return entityManager.recreateEntity(entity);
}

void EntityRegistry::destroyEntity(Entity entity) {
    onEntityDestroyed(entity, entityManager.getSignature(entity));

    componentManager.entityDestroyed(entity);
    entityManager.destroy(entity);
}

void EntityRegistry::destroyEntities(const std::vector<Entity>& entities) {
    if (entities.size() == 1) {
        if (isEntityCreated(entities[0])) {
            destroyEntity(entities[0]);
        }
        return;
    }
    for (Entity entity : entities) {
        // a callback can destroy others of the list (a model its nodes)
        if (isEntityCreated(entity)) {
            onEntityDestroyed(entity, entityManager.getSignature(entity));
        }
    }
    std::unordered_set<Entity> doomed;
    doomed.reserve(entities.size());
    for (Entity entity : entities) {
        if (isEntityCreated(entity)) {
            doomed.insert(entity);
        }
    }
    if (doomed.empty()) {
        return;
    }
    componentManager.entitiesDestroyed(doomed);
    for (Entity entity : doomed) {
        entityManager.destroy(entity);
    }
}

Signature EntityRegistry::getSignature(Entity entity) const {
    return entityManager.getSignature(entity);
}

void EntityRegistry::setEntityName(Entity entity, const std::string& name) {
    entityManager.setName(entity, name);
}

std::string EntityRegistry::getEntityName(Entity entity) const {
    return entityManager.getName(entity);
}

Entity EntityRegistry::findEntity(const std::string& name) const {
    for (Entity entity : entityManager.getEntityList()){
        if (entityManager.getName(entity) == name){
            return entity;
        }
    }
    return NULL_ENTITY;
}

Entity EntityRegistry::findEntity(const std::string& name, Entity parent) const {
    for (Entity entity : entityManager.getEntityList()){
        if (entityManager.getName(entity) == name && isParentOf(parent, entity)){
            return entity;
        }
    }
    return NULL_ENTITY;
}

void EntityRegistry::setEntityActive(Entity entity, bool active) {
    entityManager.setActive(entity, active);
}

bool EntityRegistry::isEntityActive(Entity entity) const {
    return entityManager.isActive(entity);
}

bool EntityRegistry::isEntityActiveInHierarchy(Entity entity) const {
    if (!entityManager.hasInactive()){
        return true;
    }

    auto transforms = componentManager.getComponentArray<Transform>();
    // a parent cycle cannot happen through the registry, the bound only guards bad data
    for (int depth = 0; entity != NULL_ENTITY && depth < 1024; depth++){
        if (!entityManager.isActive(entity)){
            return false;
        }
        Transform* transform = transforms->findComponent(entity);
        entity = transform ? transform->parent : NULL_ENTITY;
    }
    return true;
}

bool EntityRegistry::hasInactiveEntities() const {
    return entityManager.hasInactive();
}

void EntityRegistry::setEntityTag(Entity entity, const std::string& tag) {
    entityManager.setTag(entity, tag);
}

std::string EntityRegistry::getEntityTag(Entity entity) const {
    return entityManager.getTag(entity);
}

void EntityRegistry::setEntityLayer(Entity entity, int layer) {
    const int previous = entityManager.getLayer(entity);
    entityManager.setLayer(entity, layer);
    if (entityManager.getLayer(entity) != previous) {
        onEntityLayerChanged(entity);
    }
}

int EntityRegistry::getEntityLayer(Entity entity) const {
    return entityManager.getLayer(entity);
}

Entity EntityRegistry::findEntityByTag(const std::string& tag) const {
    if (!TagManager::isTagDefined(tag)){
        Log::error("Tag: %s is not defined.", tag.c_str());
        return NULL_ENTITY;
    }
    for (Entity entity : entityManager.getEntityList()){
        if (entityManager.getTag(entity) == tag){
            return entity;
        }
    }
    return NULL_ENTITY;
}

std::vector<Entity> EntityRegistry::findEntitiesByTag(const std::string& tag) const {
    std::vector<Entity> found;
    if (!TagManager::isTagDefined(tag)){
        Log::error("Tag: %s is not defined.", tag.c_str());
        return found;
    }
    for (Entity entity : entityManager.getEntityList()){
        if (entityManager.getTag(entity) == tag){
            found.push_back(entity);
        }
    }
    return found;
}

Entity EntityRegistry::findOldestParent(Entity entity){
    auto transforms = componentManager.getComponentArray<Transform>();

    size_t index = transforms->getIndex(entity);
    Entity lastParent = transforms->getComponentFromIndex(index).parent;

    for (int i = index - 1; i >= 0; i--){
        entity = transforms->getEntity(i);
        Transform& transform = transforms->getComponentFromIndex(i);
        if (entity == lastParent){
            lastParent = transform.parent;

            if (lastParent == NULL_ENTITY){
                return entity;
            }
        }
    }

    return entity;
}

bool EntityRegistry::isParentOf(Entity parent, Entity child) const {
    auto transforms = componentManager.getComponentArray<Transform>();

    if (!getSignature(parent).test(getComponentId<Transform>()) || !getSignature(child).test(getComponentId<Transform>())) {
        return false;
    }

    size_t index = transforms->getIndex(child);
    Entity lastParent = transforms->getComponentFromIndex(index).parent;

    if (lastParent == parent){
        return true;
    }
    if (lastParent == NULL_ENTITY){
        return false;
    }

    for (int i = index - 1; i >= 0; i--){
        Entity entity = transforms->getEntity(i);
        Transform& transform = transforms->getComponentFromIndex(i);
        if (entity == lastParent){
            lastParent = transform.parent;

            if (lastParent == parent){
                return true;
            }
            if (lastParent == NULL_ENTITY){
                return false;
            }
        }
    }

    return false;
}

size_t EntityRegistry::findBranchLastIndex(Entity entity){
    auto transforms = componentManager.getComponentArray<Transform>();

    // will throw error if entity has not Transform
    size_t index = transforms->getIndex(entity);

    size_t currentIndex = index + 1;
    // The branch so far: a list while short (the common case), then a set, as a long
    // branch made the list scan quadratic
    std::vector<Entity> branchList{entity};
    std::unordered_set<Entity> branchSet;
    auto inBranch = [&](Entity candidate) {
        return branchSet.empty() ?
            std::find(branchList.begin(), branchList.end(), candidate) != branchList.end() :
            branchSet.count(candidate) != 0;
    };
    auto addToBranch = [&](Entity member) {
        if (branchSet.empty() && branchList.size() < 32) {
            branchList.push_back(member);
            return;
        }
        if (branchSet.empty()) {
            branchSet.insert(branchList.begin(), branchList.end());
        }
        branchSet.insert(member);
    };

    while (currentIndex < transforms->size()){
        const Transform& transform = transforms->getComponentFromIndex(currentIndex);
        if (!inBranch(transform.parent)) {
            break;
        }
        addToBranch(transforms->getEntity(currentIndex));
        currentIndex++;
    }

    currentIndex--;

    return currentIndex;
}

Entity EntityRegistry::getLastEntity() const{
    return entityManager.getLastUserEntity();
}

void EntityRegistry::setLastEntity(Entity entity){
    if (entity > entityManager.getLastUserEntity()){
        entityManager.setLastUserEntity(entity);
    }
}

std::vector<Entity> EntityRegistry::getEntityList() const{
    return entityManager.getEntityList();
}

void EntityRegistry::setDefaultEntityPool(EntityPool pool) { 
    defaultPool = pool;
}

EntityPool EntityRegistry::getDefaultEntityPool() const {
    return defaultPool;
}

void EntityRegistry::clear() {
    std::vector<Entity> entities = getEntityList();
    for (Entity entity : entities) {
        // An owner's removal callbacks can destroy other entities in this snapshot.
        if (isEntityCreated(entity)) {
            destroyEntity(entity);
        }
    }
    // Reset user counter to just before the user range
    entityManager.setLastUserEntity(EntityManager::lastSystemEntity());
}

void EntityRegistry::addEntityChild(Entity parent, Entity child, bool changeTransform){
    //----------DEBUG
    //printf("Add child - BEFORE\n");
    //auto transforms = componentManager.getComponentArray<Transform>();
    //for (int i = 0; i < transforms->size(); i++){
    //	auto transform = transforms->getComponentFromIndex(i);
    //	printf("Transform %i - Entity: %i - Parent: %i: %s\n", i, transforms->getEntity(i), transform.parent, getEntityName(transforms->getEntity(i)).c_str());
    //}
    //----------DEBUG

    Signature childSignature = entityManager.getSignature(child);
    if (childSignature.test(getComponentId<Transform>())){
        Transform& transformChild = componentManager.getComponent<Transform>(child);

        if (transformChild.parent == parent){
            return;
        }

        transformChild.needUpdate = true;

        if (parent == NULL_ENTITY){
            size_t newIndex = findBranchLastIndex(findOldestParent(child)) + 1;
            moveChildToIndex(child, newIndex);

            Transform& transformChild = componentManager.getComponent<Transform>(child);

            transformChild.parent = NULL_ENTITY;
            if (changeTransform){
                // set local position to be the same of world position
                transformChild.modelMatrix.decompose(transformChild.position, transformChild.scale, transformChild.rotation);
                // the matrix origin of a pivoted UI element is not its position
                if (RectTransformComponent* layout = findComponent<RectTransformComponent>(child)){
                    transformChild.position -= getUIPivotShift(*layout, transformChild.rotation, transformChild.scale);
                }
            }
        }else{
            Signature parentSignature = entityManager.getSignature(parent);
            if (parentSignature.test(getComponentId<Transform>())){
                size_t newIndex = findBranchLastIndex(parent) + 1;
                moveChildToIndex(child, newIndex);

                Transform& transformParent = componentManager.getComponent<Transform>(parent);
                Transform& transformChild = componentManager.getComponent<Transform>(child);

                if (transformChild.parent != parent) {
                    transformChild.parent = parent;

                    if (changeTransform){
                        Matrix4 localMatrix = transformParent.modelMatrix.inverse() * transformChild.modelMatrix;
                        localMatrix.decompose(transformChild.position, transformChild.scale, transformChild.rotation);
                        if (RectTransformComponent* layout = findComponent<RectTransformComponent>(child)){
                            transformChild.position -= getUIPivotShift(*layout, transformChild.rotation, transformChild.scale);
                        }
                    }

                }
            }
        }
    }

    //----------DEBUG
    //printf("Add child - AFTER\n");
    //for (int i = 0; i < transforms->size(); i++){
    //	auto transform = transforms->getComponentFromIndex(i);
    //	printf("Transform %i - Entity: %i - Parent: %i: %s\n", i, transforms->getEntity(i), transform.parent, getEntityName(transforms->getEntity(i)).c_str());
    //}
    //printf("\n");
    //----------DEBUG
}

void EntityRegistry::moveChildToIndex(Entity entity, size_t index){
    Signature signature = entityManager.getSignature(entity);

    if (signature.test(getComponentId<Transform>())){
        auto transforms = componentManager.getComponentArray<Transform>();
        size_t entityIndex = transforms->getIndex(entity);

        if (index != entityIndex){
            size_t lastChildIndex = findBranchLastIndex(entity);
            size_t length = lastChildIndex - entityIndex + 1;

            if (index > entityIndex){
                if (index > 0){ // Prevent underflow
                    index--;
                }
            }

            if (length == 1){
                transforms->moveEntityToIndex(entity, index);
            }else{
                if (index > entityIndex){
                    // Only adjust if moving past the entire range
                    if (index >= entityIndex + length) {
                        index = index - length + 1;
                    } else {
                        // Target is within our own range - nothing to do
                        return;
                    }
                }
                transforms->moveEntityRangeToIndex(entity, transforms->getEntity(lastChildIndex), index);
            }

            sortComponentsByTransform(entityManager.getSignature(entity));
        }
    }
}

void EntityRegistry::moveChildToTop(Entity entity){
    moveChildAux(entity, true, false);
}

void EntityRegistry::moveChildUp(Entity entity){
    moveChildAux(entity, true, true);
}

void EntityRegistry::moveChildDown(Entity entity){
    moveChildAux(entity, false, true);
}

void EntityRegistry::moveChildToBottom(Entity entity){
    moveChildAux(entity, false, false);
}
