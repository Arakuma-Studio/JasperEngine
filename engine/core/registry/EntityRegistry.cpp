// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "EntityRegistry.h"

#include <algorithm>
#include <cassert>
#include <unordered_set>

using namespace jasperengine;

std::vector<EntityRegistry*>& EntityRegistry::liveRegistries() {
    static std::vector<EntityRegistry*> registries;
    return registries;
}

std::mutex& EntityRegistry::liveRegistriesMutex() {
    static std::mutex mutex;
    return mutex;
}

EntityRegistry::EntityRegistry() {
    registerComponent<MeshComponent>(ComponentOrder::Hierarchy, "MeshComponent");
    registerComponent<ModelComponent>(ComponentOrder::Hierarchy, "ModelComponent");
    registerComponent<BoneComponent>(ComponentOrder::Hierarchy, "BoneComponent");
    registerComponent<SkyboxComponent>(ComponentOrder::Manual, "SkyboxComponent");
    registerComponent<FogComponent>(ComponentOrder::Insertion, "FogComponent");
    registerComponent<MirrorComponent>(ComponentOrder::Insertion, "MirrorComponent");
    registerComponent<LayoutGroupComponent>(ComponentOrder::Insertion, "LayoutGroupComponent");
    registerComponent<RectTransformComponent>(ComponentOrder::Hierarchy, "RectTransformComponent");
    registerComponent<SpriteComponent>(ComponentOrder::Insertion, "SpriteComponent");
    registerComponent<SpriteAnimationComponent>(ComponentOrder::Insertion, "SpriteAnimationComponent");
    registerComponent<Transform>(ComponentOrder::Insertion, "Transform");
    registerComponent<CameraComponent>(ComponentOrder::Insertion, "CameraComponent");
    registerComponent<LightComponent>(ComponentOrder::Insertion, "LightComponent");
    registerComponent<Light2DComponent>(ComponentOrder::Insertion, "Light2DComponent");
    registerComponent<ShadowCaster2DComponent>(ComponentOrder::Insertion, "ShadowCaster2DComponent");
    registerComponent<ActionComponent>(ComponentOrder::Insertion, "ActionComponent");
    registerComponent<TweenComponent>(ComponentOrder::Insertion, "TweenComponent");
    registerComponent<PositionTweenComponent>(ComponentOrder::Insertion, "PositionTweenComponent");
    registerComponent<RotationTweenComponent>(ComponentOrder::Insertion, "RotationTweenComponent");
    registerComponent<ScaleTweenComponent>(ComponentOrder::Insertion, "ScaleTweenComponent");
    registerComponent<ColorTweenComponent>(ComponentOrder::Insertion, "ColorTweenComponent");
    registerComponent<AlphaTweenComponent>(ComponentOrder::Insertion, "AlphaTweenComponent");
    registerComponent<ParticleSystemComponent>(ComponentOrder::Insertion, "ParticleSystemComponent");
    registerComponent<PointsComponent>(ComponentOrder::Hierarchy, "PointsComponent");
    registerComponent<LineRendererComponent>(ComponentOrder::Hierarchy, "LineRendererComponent");
    registerComponent<TextComponent>(ComponentOrder::Insertion, "TextComponent");
    registerComponent<UIComponent>(ComponentOrder::Hierarchy, "UIComponent");
    registerComponent<ImageComponent>(ComponentOrder::Insertion, "ImageComponent");
    registerComponent<ButtonComponent>(ComponentOrder::Insertion, "ButtonComponent");
    registerComponent<PanelComponent>(ComponentOrder::Insertion, "PanelComponent");
    registerComponent<ScrollbarComponent>(ComponentOrder::Insertion, "ScrollbarComponent");
    registerComponent<ProgressbarComponent>(ComponentOrder::Insertion, "ProgressbarComponent");
    registerComponent<InputFieldComponent>(ComponentOrder::Insertion, "InputFieldComponent");
    registerComponent<MeshPolygonComponent>(ComponentOrder::Insertion, "MeshPolygonComponent");
    registerComponent<PolygonComponent>(ComponentOrder::Hierarchy, "PolygonComponent");
    registerComponent<AnimationComponent>(ComponentOrder::Insertion, "AnimationComponent");
    registerComponent<KeyframeTracksComponent>(ComponentOrder::Insertion, "KeyframeTracksComponent");
    registerComponent<MorphTracksComponent>(ComponentOrder::Insertion, "MorphTracksComponent");
    registerComponent<RotateTracksComponent>(ComponentOrder::Insertion, "RotateTracksComponent");
    registerComponent<TranslateTracksComponent>(ComponentOrder::Insertion, "TranslateTracksComponent");
    registerComponent<ScaleTracksComponent>(ComponentOrder::Insertion, "ScaleTracksComponent");
    registerComponent<ScriptComponent>(ComponentOrder::Insertion, "ScriptComponent");
    registerComponent<TerrainComponent>(ComponentOrder::Insertion, "TerrainComponent");
    registerComponent<AudioSourceComponent>(ComponentOrder::Hierarchy, "AudioSourceComponent");
    registerComponent<TilemapComponent>(ComponentOrder::Insertion, "TilemapComponent");
#ifdef JASPERENGINE_PHYSICS_2D
    registerComponent<Rigidbody2DComponent>(ComponentOrder::Insertion, "Rigidbody2DComponent");
    registerComponent<Joint2DComponent>(ComponentOrder::Insertion, "Joint2DComponent");
#endif
#ifdef JASPERENGINE_PHYSICS_3D
    registerComponent<RigidbodyComponent>(ComponentOrder::Insertion, "RigidbodyComponent");
    registerComponent<Joint3DComponent>(ComponentOrder::Insertion, "Joint3DComponent");
#endif
    registerComponent<InstancedMeshComponent>(ComponentOrder::Hierarchy, "InstancedMeshComponent");
    registerComponent<PrefabComponent>(ComponentOrder::Insertion, "PrefabComponent");
    registerComponent<ReflectionProbeComponent>(ComponentOrder::Insertion, "ReflectionProbeComponent");
    registerComponent<UIDocumentComponent>(ComponentOrder::Insertion, "UIDocumentComponent");
    registerComponent<FluidSimulationComponent>(ComponentOrder::Insertion, "FluidSimulationComponent");
    registerComponent<LatticeComponent>(ComponentOrder::Insertion, "LatticeComponent");
    registerComponent<LatticeModifierComponent>(ComponentOrder::Insertion, "LatticeModifierComponent");
    registerComponent<LatticeFollowerComponent>(ComponentOrder::Insertion, "LatticeFollowerComponent");
    registerComponent<LatticeTracksComponent>(ComponentOrder::Insertion, "LatticeTracksComponent");
    registerComponent<LocalVolumetricFogComponent>(ComponentOrder::Insertion, "LocalVolumetricFogComponent");
    registerComponent<VolumetricCloudsComponent>(ComponentOrder::Insertion, "VolumetricCloudsComponent");
    registerComponent<PoseAnimatorComponent>(ComponentOrder::Insertion, "PoseAnimatorComponent");
    registerComponent<AnimationPlayerComponent>(ComponentOrder::Insertion, "AnimationPlayerComponent");

    // complete: markChangedComponent may look into it
    std::lock_guard<std::mutex> lock(liveRegistriesMutex());
    liveRegistries().push_back(this);
}

EntityRegistry::EntityRegistry(EntityPool defaultPool) : EntityRegistry() {
    this->defaultPool = defaultPool;
}

EntityRegistry::~EntityRegistry(){
    {
        std::lock_guard<std::mutex> lock(liveRegistriesMutex());
        auto& registries = liveRegistries();
        registries.erase(std::remove(registries.begin(), registries.end(), this), registries.end());
    }

    // A camera owns the framebuffer its component allocates. The renderer deletes it when the
    // camera's entity is destroyed, but a scene's (or a bare registry's) teardown reaches no
    // system: the entity loop below runs after the Scene part is gone, so they go here
    ComponentArray<CameraComponent>* cameras = componentManager.getComponentArray<CameraComponent>();
    for (size_t i = 0; i < cameras->size(); i++) {
        CameraComponent& camera = cameras->getComponentFromIndex(i);
        delete camera.framebuffer;
        camera.framebuffer = nullptr;
    }

    Entity entity;
    while (entityManager.getFirstEntity(entity)) {
        // Destroy callbacks may remove other entities, so inspect the current first
        // entity each time without copying the entire registry.
        destroyEntity(entity);
    }
}

void EntityRegistry::linkOrderSource(ComponentArrayBase* array){
    if (!componentManager.isRegistered<Transform>()){
        return;
    }
    ComponentArrayBase* transforms = componentManager.getComponentArray<Transform>();
    if (array == transforms){
        // the Transforms came after some Hierarchy types
        hierarchyTypes.forEachSet([&](size_t id){
            componentManager.getArray((ComponentId)id)->setOrderSource(transforms);
        });
    }else if (array->getOrder() == ComponentOrder::Hierarchy){
        array->setOrderSource(transforms);
    }
}

void EntityRegistry::sortComponentsByTransform(Signature entitySignature){
    if (structuralLocked("sortComponentsByTransform")) {
        return;
    }
    // after a reorder of the Transform array (moveChildToIndex, the editor's block moves)
    hierarchyRev++;
    markHierarchyOrderDirty(entitySignature);
}

void EntityRegistry::markHierarchyOrderDirty(Signature entitySignature){
    (entitySignature & hierarchyTypes).forEachSet([&](size_t id){
        componentManager.getArray((ComponentId)id)->markOrderDirty();
    });
}

void EntityRegistry::transformChanged(Entity entity){
    // where the entity sorts among its Hierarchy components' arrays changed (a hierarchy change
    // only when the Transform has a parent or children: addComponent / removeComponent count it)
    if (structuralLocked("transformChanged")) {
        return;
    }
    markHierarchyOrderDirty(entityManager.getSignature(entity));
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
    if (!removedTransform) {
        return false;
    }
    const Entity newParent = removedTransform->parent;

    // the children are in the entity's branch, right after it
    bool hadChildren = false;
    auto transformArray = getComponentArray<Transform>();
    const size_t first = transformArray->getIndex(entity) + 1;
    const size_t last = findBranchLastIndex(entity);
    for (size_t i = first; i <= last; i++) {
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
    const size_t index = transforms->getIndex(entity);
    if (transforms->getComponentFromIndex(index).parent != NULL_ENTITY){
        return true;
    }
    // children follow their parent in the array (its branch)
    return index + 1 < transforms->size() && transforms->getComponentFromIndex(index + 1).parent == entity;
}

Entity EntityRegistry::createEntity() {
    if (structuralLocked("createEntity")) {
        return NULL_ENTITY;
    }
    return (defaultPool == EntityPool::System)
        ? entityManager.createSystemEntity()
        : entityManager.createUserEntity();
}

Entity EntityRegistry::createUserEntity() {
    if (structuralLocked("createUserEntity")) {
        return NULL_ENTITY;
    }
    return entityManager.createUserEntity();
}

Entity EntityRegistry::createSystemEntity() {
    if (structuralLocked("createSystemEntity")) {
        return NULL_ENTITY;
    }
    return entityManager.createSystemEntity();
}

bool EntityRegistry::isEntityCreated(Entity entity) const {
    return entityManager.isCreated(entity);
}

bool EntityRegistry::recreateEntity(Entity entity) {
    if (structuralLocked("recreateEntity")) {
        return false;
    }
    // undo of a delete: the entity's place in the hierarchy comes back
    hierarchyRev++;
    nameRev++;
    logHierarchyChange(destroyedLog, entity);
    return entityManager.recreateEntity(entity);
}

bool EntityRegistry::structuralLocked(const char* operation) const {
    if (clock.isWaveLocked()) {
        Log::error("ECS: %s during a parallel system wave; record it in the system's command buffer", operation);
        assert(false && "ECS: structural change inside a parallel wave");
        return true;
    }
    return false;
}

bool EntityRegistry::deferStructural(Entity entity) const {
    return deferDepth > 0 && entityManager.isCreated(entity) && (deferAnyThread > 0 || std::this_thread::get_id() == deferThread);
}

void EntityRegistry::destroyEntity(Entity entity) {
    if (deferStructural(entity)) {
        deferred.destroy(makeRef(entity));
        return;
    }
    if (structuralLocked("destroyEntity")) {
        return;
    }
    if (isInHierarchy(entity)) {
        hierarchyRev++;
        logHierarchyChange(destroyedLog, entity);
    }

    const Signature signature = entityManager.getSignature(entity);
    // the arrays' slots load while the hooks run
    componentManager.prefetchEntity(entity, signature);
    onEntityDestroyed(entity, signature);
    if (editorData) {
        editorData->entityDestroyed(entity);
    }

    // the hooks may have changed what it has
    componentManager.entityDestroyed(entity, entityManager.isCreated(entity) ? entityManager.getSignature(entity) : signature);
    entityManager.destroy(entity);
}

void EntityRegistry::destroyEntities(const std::vector<Entity>& entities) {
    if (deferDepth > 0 && (deferAnyThread > 0 || std::this_thread::get_id() == deferThread)) {
        for (Entity entity : entities) {
            if (entityManager.isCreated(entity)) {
                deferred.destroy(makeRef(entity));
            }
        }
        return;
    }
    if (structuralLocked("destroyEntities")) {
        return;
    }
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
    std::vector<Entity> doomed;
    doomed.reserve(entities.size());
    std::unordered_set<Entity> seen;
    seen.reserve(entities.size());
    Signature types;
    for (Entity entity : entities) {
        if (isEntityCreated(entity) && seen.insert(entity).second) {
            doomed.push_back(entity);
            types |= entityManager.getSignature(entity);
        }
    }
    if (doomed.empty()) {
        return;
    }
    componentManager.entitiesDestroyed(doomed, types);
    for (Entity entity : doomed) {
        if (editorData) {
            editorData->entityDestroyed(entity);
        }
        entityManager.destroy(entity);
    }
}

void EntityRegistry::destroyHierarchy(Entity entity) {
    if (deferStructural(entity)) {
        deferred.destroyHierarchy(makeRef(entity));
        return;
    }
    if (isEntityCreated(entity) && !structuralLocked("destroyHierarchy")) {
        destroyHierarchyNow(entity);
    }
}

void EntityRegistry::destroyHierarchyNow(Entity entity) {
    std::vector<Entity> branch{entity};
    if (Transform* transform = findComponent<Transform>(entity)) {
        (void)transform;
        auto transforms = getComponentArray<Transform>();
        const size_t first = transforms->getIndex(entity);
        const size_t last = findBranchLastIndex(entity);
        for (size_t i = first + 1; i <= last; i++) {
            branch.push_back(transforms->getEntity(i));
        }
    }
    // children first, as GameObject.destroy does
    std::reverse(branch.begin(), branch.end());
    destroyEntities(branch);
}

EntityRef EntityRegistry::makeRef(Entity entity) const {
    return EntityRef{entity, entityManager.getSerial(entity)};
}

bool EntityRegistry::isAlive(const EntityRef& ref) const {
    return ref.id != NULL_ENTITY && ref.serial != 0 && entityManager.getSerial(ref.id) == ref.serial;
}

Tick EntityRegistry::changeTick() const {
    return clock.sample();
}

Tick EntityRegistry::advanceChangeTick() {
    return clock.advance();
}

ChangeClock& EntityRegistry::getChangeClock() {
    return clock;
}

bool EntityRegistry::markChanged(ComponentId componentId, Entity entity) {
    ComponentArrayBase* array = componentManager.getArray(componentId);
    return array && array->markChanged(entity);
}

std::vector<Entity> EntityRegistry::queryEntities(const std::vector<ComponentId>& include, const std::vector<ComponentId>& exclude,
                                                  const Tick* changedAfter, size_t limit) {
    std::vector<Entity> found;
    if (include.empty() || limit == 0) {
        return found;
    }
    Signature required;
    Signature excluded;
    std::vector<ComponentArrayBase*> arrays;
    for (ComponentId id : include) {
        ComponentArrayBase* array = componentManager.getArray(id);
        if (!array) {
            return found;
        }
        required.set(id);
        arrays.push_back(array);
    }
    for (ComponentId id : exclude) {
        excluded.set(id);
    }
    if (changedAfter) {
        // nothing newer anywhere: no row can pass
        bool newer = false;
        for (ComponentArrayBase* array : arrays) {
            newer = newer || array->getLastChangedTick() > *changedAfter;
        }
        if (!newer) {
            return found;
        }
    }

    ComponentArrayBase* driving = arrays.front();
    driving->settle();
    const size_t count = driving->size();
    for (size_t i = 0; i < count && found.size() < limit; i++) {
        const Entity entity = driving->getEntity(i);
        const Signature signature = entityManager.getSignature(entity);
        if (!signature.contains(required) || signature.intersects(excluded)) {
            continue;
        }
        if (changedAfter) {
            bool changed = false;
            for (ComponentArrayBase* array : arrays) {
                changed = changed || array->getChangedTickOf(entity) > *changedAfter;
            }
            if (!changed) {
                continue;
            }
        }
        found.push_back(entity);
    }
    return found;
}

void EntityRegistry::trimRemovedComponents(Tick tick) {
    componentManager.forEachArray([tick](ComponentId, ComponentArrayBase& array){
        array.trimRemoved(tick);
    });
}

void EntityRegistry::beginStructuralDefer(bool anyThread) {
    if (anyThread) {
        deferAnyThread++;
    }
    if (deferDepth == 0) {
        deferThread = std::this_thread::get_id();
    } else if (std::this_thread::get_id() != deferThread) {
        Log::error("StructuralDeferScope opened on a second thread; it applies to the first one only");
    }
    deferDepth++;
}

void EntityRegistry::endStructuralDefer(bool anyThread) {
    if (deferDepth <= 0) {
        Log::error("StructuralDeferScope closed more often than opened");
        return;
    }
    if (anyThread && deferAnyThread > 0) {
        deferAnyThread--;
    }
    if (--deferDepth > 0) {
        return;
    }
    // what the playback's hooks defer again plays next, until nothing is left
    while (!deferred.empty()) {
        EntityCommandBuffer pending(std::move(deferred));
        deferred = EntityCommandBuffer();
        pending.playback(*this);
    }
}

bool EntityRegistry::isStructuralDeferActive() const {
    return deferDepth > 0 && (deferAnyThread > 0 || std::this_thread::get_id() == deferThread);
}

EntityCommandBuffer& EntityRegistry::getDeferredCommands() {
    return deferred;
}

size_t EntityRegistry::getComponentTypeCount() const {
    return componentManager.getRegisteredCount();
}

const std::string& EntityRegistry::getComponentName(ComponentId componentId) const {
    return componentManager.getName(componentId);
}

bool EntityRegistry::findComponentId(const std::string& name, ComponentId& componentId) const {
    return componentManager.findId(name, componentId);
}

ComponentArrayBase* EntityRegistry::getComponentArray(ComponentId componentId) const {
    return componentManager.getArray(componentId);
}

size_t EntityRegistry::getMetadataBytes() const {
    return entityManager.bytes();
}

size_t EntityRegistry::getEntityCount() const {
    return entityManager.count();
}

size_t EntityRegistry::getSystemEntityCount() const {
    return entityManager.countSystem();
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
    entityManager.setActive(entity, active);
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

    // The branch is contiguous and parents come before children, so an element after it is
    // in the branch exactly when its parent lies in [index, currentIndex). The array is settled
    // (getIndex), so a component's slot is its index.
    size_t currentIndex = index + 1;
    const size_t size = transforms->size();
    while (currentIndex < size){
        const Transform& transform = transforms->getComponentFromIndex(currentIndex);
        const size_t parentIndex = transforms->orderKey(transform.parent);
        if (parentIndex < index || parentIndex >= currentIndex) {
            break;
        }
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
    if (editorData) {
        editorData->registryCleared();
    }
}

void EntityRegistry::addEntityChild(Entity parent, Entity child, bool changeTransform){
    if (structuralLocked("addEntityChild")) {
        return;
    }
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
    if (structuralLocked("moveChildToIndex")) {
        return;
    }
    Signature signature = entityManager.getSignature(entity);

    if (signature.test(getComponentId<Transform>())){
        auto transforms = componentManager.getComponentArray<Transform>();
        size_t entityIndex = transforms->getIndex(entity);

        if (index != entityIndex){
            size_t lastChildIndex = findBranchLastIndex(entity);
            size_t length = lastChildIndex - entityIndex + 1;

            // the types of the whole branch move along with it
            Signature branchTypes;
            for (size_t i = entityIndex; i <= lastChildIndex; i++){
                branchTypes |= entityManager.getSignature(transforms->getEntity(i));
            }

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

            sortComponentsByTransform(branchTypes);
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
