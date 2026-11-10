// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ENTITYREGISTRY_H
#define ENTITYREGISTRY_H

#include "Entity.h"
#include "EntityManager.h"
#include "ComponentManager.h"
#include "Signature.h"
#include "ChangeTracking.h"
#include "CommandBuffer.h"
#include "EntityRef.h"
#include "View.h"
#include "TagManager.h"
#include <memory>
#include <cstdint>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

// Every component for code outside the engine (a game's scripts have always had them through Scene.h).
// The engine's and the editor's own sources include the ones they use (tools/check_fanout.py): an
// edit of a component rebuilds those, not everything that reaches this header.
#ifndef JASPERENGINE_EXPLICIT_COMPONENT_INCLUDES
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/BoneComponent.h"
#include "component/SkyboxComponent.h"
#include "component/FogComponent.h"
#include "component/LocalVolumetricFogComponent.h"
#include "component/VolumetricCloudsComponent.h"
#include "component/MirrorComponent.h"
#include "component/LatticeComponent.h"
#include "component/LatticeModifierComponent.h"
#include "component/LatticeFollowerComponent.h"
#include "component/LatticeTracksComponent.h"
#include "component/ReflectionProbeComponent.h"
#include "component/UIDocumentComponent.h"
#include "component/FluidSimulationComponent.h"
#include "component/VideoPlayerComponent.h"
#include "component/PoseAnimatorComponent.h"
#include "component/AnimatorComponent.h"
#include "component/AnimationPlayerComponent.h"
#include "component/VertexAnimatorComponent.h"
#include "component/TextEffectComponent.h"
#include "component/ImageComponent.h"
#include "component/RectTransformComponent.h"
#include "component/LayoutGroupComponent.h"
#include "component/CanvasGroupComponent.h"
#include "component/SafeAreaComponent.h"
#include "component/CapsuleShadowCasterComponent.h"
#include "component/AudioSnapshotComponent.h"
#include "component/PixelPerfectCameraComponent.h"
#include "component/SortingGroupComponent.h"
#include "component/SpriteBatchComponent.h"
#include "component/HitStopExemptComponent.h"   // S48
#include "component/GridComponent.h"
#include "component/TilemapLayerComponent.h"
#include "component/TilemapCollider2DComponent.h"
#include "component/TitleArtRendererComponent.h"
#include "component/SpriteComponent.h"
#include "component/SpriteAnimationComponent.h"
#include "component/Transform.h"
#include "component/LightComponent.h"
#include "component/Light2DComponent.h"
#include "component/ShadowCaster2DComponent.h"
#include "component/ActionComponent.h"
#include "component/TweenComponent.h"
#include "component/PositionTweenComponent.h"
#include "component/RotationTweenComponent.h"
#include "component/ScaleTweenComponent.h"
#include "component/ColorTweenComponent.h"
#include "component/AlphaTweenComponent.h"
#include "component/ParticleSystemComponent.h"
#include "component/ForceFieldComponent.h"
#include "component/PointsComponent.h"
#include "component/LineRendererComponent.h"
#include "component/TrailRendererComponent.h"
#include "component/TextComponent.h"
#include "component/UIComponent.h"
#include "component/ButtonComponent.h"
#include "component/PanelComponent.h"
#include "component/ScrollbarComponent.h"
#include "component/ProgressbarComponent.h"
#include "component/InputFieldComponent.h"
#include "component/OnScreenControlComponents.h"
#include "component/MeshPolygonComponent.h"
#include "component/PolygonComponent.h"
#include "component/AnimationComponent.h"
#include "component/KeyframeTracksComponent.h"
#include "component/MorphTracksComponent.h"
#include "component/RotateTracksComponent.h"
#include "component/TranslateTracksComponent.h"
#include "component/ScaleTracksComponent.h"
#include "component/ScriptComponent.h"
#include "component/TerrainComponent.h"
#include "component/TerrainWorldComponent.h"
#include "component/AudioSourceComponent.h"
#include "component/TilemapComponent.h"
#include "component/Rigidbody2DComponent.h"
#include "component/Joint2DComponent.h"
#include "component/CharacterController2DComponent.h"
#include "component/CharacterControllerComponent.h"
#include "component/WaterExcluderComponent.h"
#include "component/WaterVolumeComponent.h"
#include "component/WaterWakeComponent.h"
#include "component/BuoyancyComponent.h"
#include "component/NavMeshSurfaceComponent.h"
#include "component/NavMeshAgentComponent.h"
#include "component/NavMeshObstacleComponent.h"
#include "component/NavMeshLinkComponent.h"
#include "component/NavMeshModifierComponent.h"
#include "component/NavGridComponent.h"
#include "component/SplineComponent.h"
#include "component/SplineExtrudeComponent.h"
#include "component/VoxelVolumeComponent.h"
#include "component/SplineInstantiateComponent.h"
#include "component/SplineAnimateComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/Joint3DComponent.h"
#include "component/RagdollComponent.h"
#include "component/InstancedMeshComponent.h"
#include "component/PrefabComponent.h"
#include "component/CameraShakerComponent.h"
#include "component/CameraFollowComponent.h"
#endif

namespace jasperengine {

    enum class EntityPool { User, System };

    class Scene;
    struct Transform;

    // Data the editor keeps next to a registry: what a scene file held that the editor could
    // not read, written back on save (editor/StreamPreserved.h). The engine never reads it; it
    // only reports destroyed entities and removed components, so no entry outlives the entity
    // or component it belongs to.
    class JASPERENGINE_API EntityRegistryEditorData {
    public:
        virtual ~EntityRegistryEditorData() = default;
        virtual void entityDestroyed(Entity entity) = 0;
        virtual void componentRemoved(Entity entity, ComponentId componentId) = 0;
        virtual void registryCleared() = 0;
    };

    class JASPERENGINE_API EntityRegistry {
    private:
        EntityManager entityManager;
        ComponentManager componentManager;
        EntityPool defaultPool = EntityPool::User;
        std::shared_ptr<EntityRegistryEditorData> editorData;

        ChangeClock clock;

        // the Hierarchy-ordered types, re-sorted by the Transforms
        Signature hierarchyTypes;

        // StructuralDeferScope: depth, the thread that opened it and what it deferred
        int deferDepth = 0;
        std::thread::id deferThread;
        // callbacks of other threads that the opening thread waits for, serialized by their
        // own lock (the Jolt contact listener) are deferred too
        int deferAnyThread = 0;
        EntityCommandBuffer deferred;

        // every registry alive (markChangedComponent)
        static std::vector<EntityRegistry*>& liveRegistries();
        static std::mutex& liveRegistriesMutex();
        // see hierarchyRevision() / nameRevision()
        uint64_t hierarchyRev = 0;
        uint64_t nameRev = 0;
        // see takeHierarchyLog()
        std::vector<Entity> renamedLog;
        std::vector<Entity> destroyedLog;
        bool hierarchyLogOverflow = false;
        void logHierarchyChange(std::vector<Entity>& log, Entity entity);

        void moveChildAux(Entity entity, bool increase, bool stopIfFound);
        // true when `entity` had children (now its parent's)
        bool changeTransformChildren(Entity entity);
        // the entity has a Transform with a parent or with children
        bool isInHierarchy(Entity entity);
        void linkOrderSource(ComponentArrayBase* array);
        // a Transform added to or removed from an entity moves its Hierarchy components'
        // place in their order
        void transformChanged(Entity entity);
        // the entity's Hierarchy-ordered arrays sort again on their next read
        void markHierarchyOrderDirty(Signature entitySignature);
        bool deferStructural(Entity entity) const;
        // true (logged, asserts in debug) while a parallel wave runs: systems record
        // structural changes into their command buffer instead
        bool structuralLocked(const char* operation) const;
        // `root` and its descendants whose activeInHierarchy is the same as root's (each one's
        // own flag and every parent's up to root are active), in pre-order
        std::vector<Entity> collectActivationBranch(Entity root);
        // addEntityChild without the activation check
        void setEntityChild(Entity parent, Entity child, bool changeTransform);

    protected:
        virtual void onComponentAdded(Entity entity, ComponentId componentId) { (void)entity; (void)componentId; }
        virtual void onComponentRemoved(Entity entity, ComponentId componentId) { (void)entity; (void)componentId; }
        virtual void onEntityDestroyed(Entity entity, Signature signature) { (void)entity; (void)signature; }
        virtual void onEntityLayerChanged(Entity entity) { (void)entity; }
        // destroyHierarchy once it is not deferred: the entity's Transform branch, children
        // first (Scene: ObjectManager, which stops the scripts too)
        virtual void destroyHierarchyNow(Entity entity);
        // setEntityActive / addEntityChild: `changed` are the entities whose activeInHierarchy
        // flipped (the moved or (de)activated one and the descendants that follow it), parents
        // before children, all now `nowActive`. Only asked for when isActivationObserved().
        virtual bool isActivationObserved() const { return false; }
        virtual void onActiveInHierarchyChanged(const std::vector<Entity>& changed, bool nowActive) { (void)changed; (void)nowActive; }

    public:
        EntityRegistry();
        EntityRegistry(EntityPool defaultPool);
        virtual ~EntityRegistry();
        // not copyable (EntityManager owns its signature pages; MSVC instantiates the copies
        // of an exported class)
        EntityRegistry(const EntityRegistry&) = delete;
        EntityRegistry& operator=(const EntityRegistry&) = delete;

        // The Scene this registry is, if it is one
        virtual Scene* asScene() { return nullptr; }

        //Entity methods

        Entity createEntity();
        Entity createUserEntity();   // 1001..max
        Entity createSystemEntity(); // 1..1000
        bool recreateEntity(Entity entity); // for internal editor use only

        bool isEntityCreated(Entity entity) const;
        // made by createSystemEntity (the engine's and the editor's own: the Default Camera, the
        // Scene view's helpers), not a GameObject a file recreated with an id in the system range
        bool isPooledSystemEntity(Entity entity) const;

        // O(1) whatever the number of entities (a component removed from the middle of its
        // array leaves a hole, closed once at the next index observation or compactComponents(),
        // see ComponentArray), so destroying all of a scene's, one by one or by its teardown, is O(n)
        void destroyEntity(Entity entity);
        // Several entities: every component removal callback first, then the components
        void destroyEntities(const std::vector<Entity>& entities);
        // GameObject.destroy semantics: the entity and its Transform children (a Scene
        // stops their scripts first)
        void destroyHierarchy(Entity entity);
        // Closes the holes destroyed entities left in every component array (moving the
        // components after them), as the next index observation of each array would. The scene
        // does it at the start of its update and draw and the engine after the frame's deferred
        // destroys; the editor between commands and MCP calls and before a thread reads the scene.
        void compactComponents();

        void addEntityChild(Entity parent, Entity child, bool changeTransform);

        void moveChildToIndex(Entity entity, size_t index); // for internal/editor use
        // Marks the component arrays kept in Transform order (meshes, UI, ...) that the
        // signature holds for a re-sort, after the Transform array itself was reordered:
        // each re-sorts once, at its next index-based read. For internal/editor use: moving
        // a block of branches directly in the Transform array needs one call.
        void sortComponentsByTransform(Signature entitySignature);
        // The T components in the order their entities have in `entities` (the others last):
        // for components without a Transform ordered by a list, e.g. the skyboxes by the
        // editor's scene entity list, whose first one is the scene's sky
        template<typename T>
        void sortComponentsByEntityOrder(const std::vector<Entity>& entities){
            componentManager.getComponentArray<T>()->sortByEntityOrder(entities);
        }

        void moveChildToTop(Entity entity);
        void moveChildUp(Entity entity);
        void moveChildDown(Entity entity);
        void moveChildToBottom(Entity entity);

        // inline: the metadata getters are on the hot lookup paths
        JE_ECS_INLINE Signature getSignature(Entity entity) const { return entityManager.getSignature(entity); }

        void setEntityName(Entity entity, const std::string& name);
        std::string getEntityName(Entity entity) const;
        // the same without a copy (valid until the entity is renamed or destroyed; "" when unknown)
        const std::string& getEntityNameRef(Entity entity) const;

        // Looks up an existing entity by its name (as shown in the editor Structure panel).
        // Returns the first match, or NULL_ENTITY when no entity has that name.
        Entity findEntity(const std::string& name) const;
        // Same lookup, but restricted to descendants of 'parent'. Use this to resolve a
        // named child without depending on globally unique names.
        Entity findEntity(const std::string& name, Entity parent) const;

        // GameObject active state (Unity SetActive / activeSelf / activeInHierarchy). An
        // inactive entity and all its Transform descendants are not rendered, simulated,
        // heard or updated by scripts; their own flags are kept for reactivation.
        void setEntityActive(Entity entity, bool active);
        bool isEntityActive(Entity entity) const { return entityManager.isActive(entity); } // own flag (activeSelf)
        bool isEntityActiveInHierarchy(Entity entity) const; // own flag and every parent's
        bool hasInactiveEntities() const; // false: every entity is active in hierarchy

        // Tag (default "Untagged") and layer (0..31, default 0) of an entity
        void setEntityTag(Entity entity, const std::string& tag);
        std::string getEntityTag(Entity entity) const;
        void setEntityLayer(Entity entity, int layer);
        JE_ECS_INLINE int getEntityLayer(Entity entity) const { return entityManager.getLayer(entity); }

        // The editor's Scene view state of an entity (EDITOR_HIDDEN, EDITOR_PICK_LOCKED in
        // picking/PickTypes.h): runtime only, never saved or cloned, not in Lua
        void setEntityEditorFlags(Entity entity, uint8_t flags) { entityManager.setEditorFlags(entity, flags); }
        JE_ECS_INLINE uint8_t getEntityEditorFlags(Entity entity) const { return entityManager.getEditorFlags(entity); }
        bool hasEntityEditorFlags() const { return entityManager.hasEditorFlags(); }
        // First entity with the tag, NULL_ENTITY when none. Like Unity's FindWithTag, a tag
        // the project does not define (TagManager) logs "Tag: X is not defined." and finds nothing.
        Entity findEntityByTag(const std::string& tag) const;
        std::vector<Entity> findEntitiesByTag(const std::string& tag) const;

        Entity findOldestParent(Entity entity);
        bool isParentOf(Entity parent, Entity child) const;
        size_t findBranchLastIndex(Entity entity);

        Entity getLastEntity() const;
        // Raises the floor createUserEntity counts up from; recreateEntity is not bound by it.
        // A scene sets it past the ids it recreates, so what it creates next stays clear of them.
        void setLastEntity(Entity entity); // for internal editor use only
        std::vector<Entity> getEntityList() const;
        // fn(Entity) for every entity in ascending id order, without building a list
        template<typename F>
        void forEachEntity(F&& fn) const {
            entityManager.forEachEntity(std::forward<F>(fn));
        }
        size_t getEntityCount() const;
        size_t getSystemEntityCount() const;
        // Every entity in hierarchy pre-order: the Transform hierarchy as the Hierarchy panel
        // shows it (parents before children, siblings in order, roots in order), then the
        // entities without a Transform by ascending id
        std::vector<Entity> getHierarchyOrder() const;

        void setDefaultEntityPool(EntityPool pool);
        EntityPool getDefaultEntityPool() const;

        // Increases when the Transform hierarchy or a name changes in a way that can move a
        // path-bound target (animation clips spec 2.2, 9.R9): setEntityName of an entity under a
        // parent (a root's or a lone entity's name is in no path), addEntityChild,
        // child moves and sorts, a Transform with a parent added or removed, destroying or
        // recreating an entity that has a parent or children, clear(). Creating a parentless
        // entity leaves it unchanged. Animation Players compare it each frame and rebind.
        uint64_t hierarchyRevision() const;
        // Increases with the renames among those (an entity under a parent renamed), recreateEntity
        // and clear(): a player whose bound entities kept their parents and places compares
        // names only when it changed
        uint64_t nameRevision() const;
        // For code that changes Transform::parent directly instead of through addEntityChild
        void hierarchyChanged();
        // The entities renamed under a parent and the ones destroyed (or recreated) with a parent
        // or children since the last call, for the Animation Players' validation (ActionSystem
        // takes it once per update); `overflow` when more happened than the log keeps (or a
        // clear()): then everything counts as changed
        void takeHierarchyLog(std::vector<Entity>& renamed, std::vector<Entity>& destroyed, bool& overflow);

        void clear();

        // Handles for deferred and asynchronous code (EntityRef)

        EntityRef makeRef(Entity entity) const;
        bool isAlive(const EntityRef& ref) const;

        // Change tracking: the registry's change tick (see ChangeClock). changeTick() samples
        // it: whatever changes after the call is stamped with a newer tick.
        Tick changeTick() const;
        Tick advanceChangeTick();
        ChangeClock& getChangeClock();

        // A change to the entity's T that went through a T& (direct writes are not tracked)
        template<typename T>
        void markChanged(Entity entity){
            componentManager.getComponentArray<T>()->markChanged(entity);
        }
        // by id (editor and MCP code that knows the type at run time); false when the entity
        // has no such component
        bool markChanged(ComponentId componentId, Entity entity);

        // The entities with every `include` type and none of `exclude`, in the order of the
        // first include type's array (parents before children for Transform and Hierarchy
        // types); with `changedAfter`, only those where an included type changed after that
        // tick. At most `limit`. Lua findEntitiesWith / findChangedEntities, MCP query_entities.
        std::vector<Entity> queryEntities(const std::vector<ComponentId>& include, const std::vector<ComponentId>& exclude,
                                          const Tick* changedAfter = nullptr, size_t limit = SIZE_MAX);

        // A change made through a pointer to a component of any live registry (the Lua
        // setters of component fields, which have no entity): finds the registry and entity
        // owning `component` and stamps it changed. False when no registry holds it.
        template<typename T>
        static bool markChangedComponent(const T* component){
            std::lock_guard<std::mutex> lock(liveRegistriesMutex());
            for (EntityRegistry* registry : liveRegistries()){
                if (!registry->isComponentRegistered<T>()){
                    continue;
                }
                const Entity entity = registry->componentManager.getComponentArray<T>()->findOwner(component);
                if (entity != NULL_ENTITY){
                    registry->componentManager.getComponentArray<T>()->markChanged(entity);
                    return true;
                }
            }
            return false;
        }

        // Removed<T> entries stamped at or before `tick` go (Scene, at the end of a frame)
        void trimRemovedComponents(Tick tick);

        // Typed query: view<Read<A>, Write<B>, Optional<C>, Without<D>>() (see View.h)
        template<typename... Q>
        View<Q...> view() const {
            return View<Q...>(componentManager);
        }

        // Structural changes during iteration (StructuralDeferScope): while a scope is open on
        // the thread that opened it, destroyEntity, destroyEntities, destroyHierarchy and
        // removeComponent go to a command buffer played back when the outermost scope closes.
        // Adding components stays immediate (an add never moves a component).
        void beginStructuralDefer(bool anyThread = false);
        void endStructuralDefer(bool anyThread = false);
        bool isStructuralDeferActive() const;
        EntityCommandBuffer& getDeferredCommands();

        // Component types: count, names given at registration, lookup by name and by id

        size_t getComponentTypeCount() const;
        const std::string& getComponentName(ComponentId componentId) const;
        bool findComponentId(const std::string& name, ComponentId& componentId) const;
        ComponentArrayBase* getComponentArray(ComponentId componentId) const;
        // the bytes of the entity metadata (names' heap buffers left out)
        size_t getMetadataBytes() const;

        // For editor use: the editor's side table of this registry (nullptr until it sets one)
        EntityRegistryEditorData* getEditorData() const { return editorData.get(); }
        void setEditorData(std::shared_ptr<EntityRegistryEditorData> data) { editorData = std::move(data); }

        // Component methods

        // `order`: how the type's array keeps its order (ComponentOrder). Past 256 types the
        // registration is refused (logged) and the type stays unregistered.
        template<typename T>
        void registerComponent(ComponentOrder order = ComponentOrder::Insertion){
            registerComponent<T>(order, nullptr);
        }

        // ...with the name scripts and tools look the type up by
        template<typename T>
        bool registerComponent(ComponentOrder order, const char* name){
            if (!componentManager.registerComponent<T>(order, name)){
                return false;
            }
            ComponentArray<T>* array = componentManager.getComponentArray<T>();
            array->setClock(&clock);
            if (order == ComponentOrder::Hierarchy){
                hierarchyTypes.set(componentManager.getComponentId<T>());
            }
            linkOrderSource(array);
            return true;
        }

        template<typename T>
        void addComponent(Entity entity){
            if (structuralLocked("addComponent")){
                return;
            }
            componentManager.addComponent<T>(entity);
            componentAdded<T>(entity);
        }

        template<typename T>
        void addComponent(Entity entity, const T& component){
            if (structuralLocked("addComponent")){
                return;
            }
            if constexpr (std::is_same_v<T, Transform>) {
                // a Transform that arrives with its parent set (decode, clone) joins a hierarchy
                if (component.parent != NULL_ENTITY) hierarchyRev++;
            }
            componentManager.addComponent<T>(entity, component);
            componentAdded<T>(entity);
        }

        template<typename T>
        void addComponent(Entity entity, T&& component){
            if (structuralLocked("addComponent")){
                return;
            }
            using Component = std::remove_cv_t<std::remove_reference_t<T>>;
            if constexpr (std::is_same_v<Component, Transform>) {
                if (component.parent != NULL_ENTITY) hierarchyRev++;
            }
            componentManager.addComponent<Component>(entity, std::forward<T>(component));
            componentAdded<Component>(entity);
        }

        template<typename T>
        void removeComponent(Entity entity){
            if (deferStructural(entity)){
                deferred.remove<T>(makeRef(entity));
                return;
            }
            if (structuralLocked("removeComponent")){
                return;
            }
            if (!componentManager.findComponent<T>(entity)){
                // the removal hooks read the component: none to remove is an error, not a crash
                Log::error("Removing non-existent component: entity %u", entity);
                return;
            }
            // the record (signature) and the component load together, while the hooks run
            entityManager.prefetch(entity);
            componentManager.getComponentArray<T>()->prefetchEntity(entity);

            onComponentRemoved(entity, componentManager.getComponentId<T>());
            if (editorData) {
                editorData->componentRemoved(entity, componentManager.getComponentId<T>());
            }

            if constexpr (std::is_same_v<T, Transform>) {
                T* removed = componentManager.findComponent<T>(entity);
                const bool hadParent = removed && removed->parent != NULL_ENTITY;
                // children are reparented to its parent: a hierarchy change when it had either
                if (changeTransformChildren(entity) || hadParent) {
                    hierarchyRev++;
                }
            }

            componentManager.removeComponent<T>(entity);
            entityManager.setSignatureBit(entity, componentManager.getComponentId<T>(), false);

            if constexpr (std::is_same_v<T, Transform>) {
                transformChanged(entity);
            }
        }

        template<typename T>
        JE_ECS_INLINE T* findComponent(Entity entity) {
            return componentManager.findComponent<T>(entity);
        }

        template<typename T>
        T& getComponent(Entity entity) const {
            return componentManager.getComponent<T>(entity);
        }

        template<typename T>
        T* findComponentFromIndex(size_t index) {
            return componentManager.findComponentFromIndex<T>(index);
        }

        template<typename T>
        JE_ECS_INLINE T& getComponentFromIndex(size_t index) {
            return componentManager.getComponentFromIndex<T>(index);
        }

        // whether the registry has the component type (the component getters throw otherwise)
        template<typename T>
        bool isComponentRegistered() const{
            return componentManager.isRegistered<T>();
        }

        template<typename T>
        ComponentId getComponentId() const{
            return componentManager.getComponentId<T>();
        }

        template<typename T>
        ComponentArray<T>* getComponentArray() const{
            return componentManager.getComponentArray<T>();
        }

    private:
        template<typename T>
        void componentAdded(Entity entity){
            const ComponentId id = componentManager.getComponentId<T>();
            if (entity == NULL_ENTITY){
                return; // refused (logged by the array)
            }
            auto signature = entityManager.getSignature(entity);
            signature.set(id, true);
            entityManager.setSignature(entity, signature);

            if constexpr (std::is_same_v<T, Transform>) {
                transformChanged(entity);
            }

            onComponentAdded(entity, id);
        }
    };

    // Defers the registry's structural changes while it is open (see beginStructuralDefer);
    // scopes nest, the outermost one plays the changes back when it closes
    class StructuralDeferScope {
    private:
        EntityRegistry& registry;
        bool anyThread;

    public:
        // anyThread: also for the callbacks other threads run while this one waits for them,
        // which their own lock serializes (a physics step's contact listener)
        explicit StructuralDeferScope(EntityRegistry& registry, bool anyThread = false): registry(registry), anyThread(anyThread) {
            registry.beginStructuralDefer(anyThread);
        }

        ~StructuralDeferScope(){
            registry.endStructuralDefer(anyThread);
        }

        StructuralDeferScope(const StructuralDeferScope&) = delete;
        StructuralDeferScope& operator=(const StructuralDeferScope&) = delete;
    };

} // namespace jasperengine

#endif // ENTITYREGISTRY_H
