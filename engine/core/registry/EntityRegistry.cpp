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
    registerComponent<UIDocumentComponent>();
    registerComponent<FluidSimulationComponent>();
    registerComponent<LatticeComponent>();
    registerComponent<LatticeModifierComponent>();
    registerComponent<LatticeFollowerComponent>();
    registerComponent<LatticeTracksComponent>();
    registerComponent<PoseAnimatorComponent>();
    registerComponent<AnimationPlayerComponent>();
    registerComponent<VertexAnimatorComponent>();
    registerComponent<TrailRendererComponent>();
    registerComponent<SafeAreaComponent>();
}

EntityRegistry::EntityRegistry(EntityPool defaultPool) : EntityRegistry() {
    this->defaultPool = defaultPool;
}

EntityRegistry::~EntityRegistry(){
    Entity entity;
    while (entityManager.getFirstEntity(entity)) {
        // Destroy callbacks may remove other entities, so inspect the current first
        // entity each time without copying the entire registry.
        destroyEntity(entity);
    }
}

void EntityRegistry::sortComponentsByTransform(Signature entitySignature){
    // after a reorder of the Transform array (moveChildToIndex, the editor's block moves)
    hierarchyRev++;

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

bool EntityRegistry::changeTransformChildren(Entity entity){
    // Get the parent of the entity being removed before it's deleted
    Transform* removedTransform = findComponent<Transform>(entity);
    Entity newParent = NULL_ENTITY;
    if (removedTransform) {
        newParent = removedTransform->parent;
    }

    bool hadChildren = false;
    auto transformArray = getComponentArray<Transform>();
    for (size_t i = 0; i < transformArray->size(); i++) {
        Transform& childTransform = transformArray->getComponentFromIndex(i);
        if (childTransform.parent == entity) {
            childTransform.parent = newParent;
            childTransform.needUpdate = true;
            childTransform.needUpdateChildVisibility = true;
            hadChildren = true;
        }
    }
    return hadChildren;
}

bool EntityRegistry::isInHierarchy(Entity entity){
    if (!entityManager.isCreated(entity) || !entityManager.getSignature(entity).test(getComponentId<Transform>())){
        return false;
    }
    auto transforms = componentManager.getComponentArray<Transform>();
    const Transform* transform = transforms->findComponent(entity);
    if (!transform){
        return false;
    }
    if (transform->parent != NULL_ENTITY){
        return true;
    }
    // children follow their parent in the array (its branch); by entity, not index: destroying
    // entities one by one leaves holes, which an index read would rank in O(log n) (ComponentArray)
    bool hasChild = false;
    transforms->forEachAfter(entity, [&hasChild, entity](Entity, const Transform& next){
        hasChild = next.parent == entity;
        return false;
    });
    return hasChild;
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
    // undo of a delete: the entity's place in the hierarchy comes back
    hierarchyRev++;
    nameRev++;
    logHierarchyChange(destroyedLog, entity);
    return entityManager.recreateEntity(entity);
}

void EntityRegistry::destroyEntity(Entity entity) {
    if (isInHierarchy(entity)) {
        hierarchyRev++;
        logHierarchyChange(destroyedLog, entity);
    }
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
        if (isInHierarchy(entity)) {
            hierarchyRev++;
            logHierarchyChange(destroyedLog, entity);
        }
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

void EntityRegistry::compactComponents() {
    componentManager.compactAll();
}

Signature EntityRegistry::getSignature(Entity entity) const {
    return entityManager.getSignature(entity);
}

void EntityRegistry::setEntityName(Entity entity, const std::string& name) {
    // only a name under a parent is part of a path (a root's or a lone entity's never is)
    if (entityManager.isCreated(entity) && entityManager.getSignature(entity).test(getComponentId<Transform>()) &&
        componentManager.getComponent<Transform>(entity).parent != NULL_ENTITY && entityManager.getName(entity) != name) {
        hierarchyRev++;
        nameRev++;
        logHierarchyChange(renamedLog, entity);
    }
    entityManager.setName(entity, name);
}

std::string EntityRegistry::getEntityName(Entity entity) const {
    return entityManager.getName(entity);
}

const std::string& EntityRegistry::getEntityNameRef(Entity entity) const {
    return entityManager.getNameRef(entity);
}

Entity EntityRegistry::findEntity(const std::string& name) const {
    return entityManager.findByName(name);
}

Entity EntityRegistry::findEntity(const std::string& name, Entity parent) const {
    return entityManager.findByName(name, [this, parent](Entity entity){
        return isParentOf(parent, entity);
    });
}

void EntityRegistry::setEntityActive(Entity entity, bool active) {
    // the trails of a GameObject deactivated (it and its children) are cleared now, not at the
    // next TrailSystem update: reactivated in the same frame elsewhere it draws no streak
    if (!active && entityManager.isActive(entity)) {
        auto trails = componentManager.getComponentArray<TrailRendererComponent>();
        for (size_t i = 0; i < trails->size(); i++) {
            const Entity trailEntity = trails->getEntity(i);
            if (trailEntity == entity || isParentOf(entity, trailEntity)) {
                trails->getComponentFromIndex(i).clearPoints();
            }
        }
    }
    if (!isActivationObserved() || !isEntityCreated(entity)) {
        entityManager.setActive(entity, active);
        return;
    }
    const bool before = isEntityActiveInHierarchy(entity);
    entityManager.setActive(entity, active);
    const bool after = isEntityActiveInHierarchy(entity);
    if (before != after) {
        onActiveInHierarchyChanged(collectActivationBranch(entity), after);
    }
}

std::vector<Entity> EntityRegistry::collectActivationBranch(Entity root) {
    std::vector<Entity> branch;
    branch.push_back(root);
    if (!entityManager.getSignature(root).test(getComponentId<Transform>())) {
        return branch;
    }
    // the branch follows root in the Transform array; a descendant flips when it and every
    // parent below root are active themselves
    std::unordered_set<Entity> inBranch{root};
    std::unordered_set<Entity> flipping{root};
    componentManager.getComponentArray<Transform>()->forEachAfter(root, [&](Entity entity, const Transform& transform) {
        if (!inBranch.count(transform.parent)) {
            return false;
        }
        inBranch.insert(entity);
        if (flipping.count(transform.parent) && entityManager.isActive(entity)) {
            flipping.insert(entity);
            branch.push_back(entity);
        }
        return true;
    });
    return branch;
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
    return entityManager.findByTag(tag);
}

std::vector<Entity> EntityRegistry::findEntitiesByTag(const std::string& tag) const {
    std::vector<Entity> found;
    if (!TagManager::isTagDefined(tag)){
        Log::error("Tag: %s is not defined.", tag.c_str());
        return found;
    }
    return entityManager.findAllByTag(tag);
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

    int i = index - 1;
    while (i >= 0){
        // Walk adjacent bone chains directly, without hashing each parent.
        for (; i >= 0; i--){
            if (transforms->getEntity(i) != lastParent){
                break;
            }
            lastParent = transforms->getComponentFromIndex(i).parent;
            if (lastParent == parent){
                return true;
            }
            if (lastParent == NULL_ENTITY){
                return false;
            }
        }
        if (i < 0){
            break;
        }
        // Skip unrelated branches between a child and a more distant parent.
        if (!transforms->hasEntity(lastParent)){
            return false;
        }
        size_t parentIndex = transforms->getIndex(lastParent);
        // Keep the old backwards-only traversal, including its behavior for
        // malformed hierarchy order and cycles.
        if (parentIndex >= static_cast<size_t>(i)){
            return false;
        }
        i = static_cast<int>(parentIndex);
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

std::vector<Entity> EntityRegistry::getHierarchyOrder() const{
    std::vector<Entity> order;
    auto transforms = componentManager.getComponentArray<Transform>();
    order.reserve(transforms->size());
    transforms->forEach([&order](Entity entity, const Transform&) {
        order.push_back(entity);
        return true;
    });
    const ComponentId transformId = componentManager.getComponentId<Transform>();
    for (Entity entity : entityManager.getEntityList()) {
        if (!entityManager.getSignature(entity).test(transformId)) {
            order.push_back(entity);
        }
    }
    return order;
}

void EntityRegistry::setDefaultEntityPool(EntityPool pool) { 
    defaultPool = pool;
}

EntityPool EntityRegistry::getDefaultEntityPool() const {
    return defaultPool;
}

uint64_t EntityRegistry::hierarchyRevision() const {
    return hierarchyRev;
}

uint64_t EntityRegistry::nameRevision() const {
    return nameRev;
}

void EntityRegistry::hierarchyChanged() {
    hierarchyRev++;
}

void EntityRegistry::logHierarchyChange(std::vector<Entity>& log, Entity entity) {
    if (log.size() < 256) {
        log.push_back(entity);
    } else {
        hierarchyLogOverflow = true;
    }
}

void EntityRegistry::takeHierarchyLog(std::vector<Entity>& renamed, std::vector<Entity>& destroyed, bool& overflow) {
    renamed.swap(renamedLog);
    destroyed.swap(destroyedLog);
    overflow = hierarchyLogOverflow;
    renamedLog.clear();
    destroyedLog.clear();
    hierarchyLogOverflow = false;
}

void EntityRegistry::clear() {
    hierarchyRev++;
    nameRev++;
    hierarchyLogOverflow = true;
    std::vector<Entity> entities = getEntityList();
    for (Entity entity : entities) {
        // An owner's removal callbacks can destroy other entities in this snapshot.
        if (isEntityCreated(entity)) {
            destroyEntity(entity);
        }
    }
    // Reset user counter to just before the user range
    entityManager.setLastUserEntity(EntityManager::lastSystemEntity());
    compactComponents();
}

void EntityRegistry::addEntityChild(Entity parent, Entity child, bool changeTransform){
    // under an inactive (or active) parent the moved branch may change activeInHierarchy
    const bool observed = isActivationObserved() && isEntityCreated(child);
    const bool activeBefore = observed && isEntityActiveInHierarchy(child);
    setEntityChild(parent, child, changeTransform);
    if (observed && isEntityCreated(child)) {
        const bool activeAfter = isEntityActiveInHierarchy(child);
        if (activeAfter != activeBefore) {
            onActiveInHierarchyChanged(collectActivationBranch(child), activeAfter);
        }
    }
}

void EntityRegistry::setEntityChild(Entity parent, Entity child, bool changeTransform){
    Signature childSignature = entityManager.getSignature(child);
    if (childSignature.test(getComponentId<Transform>())){
        Transform& transformChild = componentManager.getComponent<Transform>(child);

        if (transformChild.parent == parent){
            return;
        }

        transformChild.needUpdate = true;
        hierarchyRev++;

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
                index--;
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
