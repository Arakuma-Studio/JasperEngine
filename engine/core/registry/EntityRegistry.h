// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ENTITYREGISTRY_H
#define ENTITYREGISTRY_H

#include "Entity.h"
#include "EntityManager.h"
#include "ComponentManager.h"
#include "Signature.h"
#include "TagManager.h"
#include <memory>
#include <type_traits>
#include <utility>

#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/BoneComponent.h"
#include "component/SkyboxComponent.h"
#include "component/FogComponent.h"
#include "component/MirrorComponent.h"
#include "component/LatticeComponent.h"
#include "component/LatticeModifierComponent.h"
#include "component/LatticeFollowerComponent.h"
#include "component/LatticeTracksComponent.h"
#include "component/ReflectionProbeComponent.h"
#include "component/UIDocumentComponent.h"
#include "component/ImageComponent.h"
#include "component/RectTransformComponent.h"
#include "component/LayoutGroupComponent.h"
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
#include "component/PointsComponent.h"
#include "component/LineRendererComponent.h"
#include "component/TextComponent.h"
#include "component/UIComponent.h"
#include "component/ButtonComponent.h"
#include "component/PanelComponent.h"
#include "component/ScrollbarComponent.h"
#include "component/ProgressbarComponent.h"
#include "component/InputFieldComponent.h"
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
#include "component/AudioSourceComponent.h"
#include "component/TilemapComponent.h"
#include "component/Rigidbody2DComponent.h"
#include "component/Joint2DComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/Joint3DComponent.h"
#include "component/InstancedMeshComponent.h"
#include "component/PrefabComponent.h"

namespace jasperengine {

    enum class EntityPool { User, System };

    class JASPERENGINE_API EntityRegistry {
    private:
        EntityManager entityManager;
        ComponentManager componentManager;
        EntityPool defaultPool = EntityPool::User;

        void moveChildAux(Entity entity, bool increase, bool stopIfFound);
        void changeTransformChildren(Entity entity);

    protected:
        virtual void onComponentAdded(Entity entity, ComponentId componentId) { (void)entity; (void)componentId; }
        virtual void onComponentRemoved(Entity entity, ComponentId componentId) { (void)entity; (void)componentId; }
        virtual void onEntityDestroyed(Entity entity, Signature signature) { (void)entity; (void)signature; }
        virtual void onEntityLayerChanged(Entity entity) { (void)entity; }

    public:
        EntityRegistry();
        EntityRegistry(EntityPool defaultPool);
        virtual ~EntityRegistry();

        //Entity methods

        Entity createEntity();
        Entity createUserEntity();   // 1001..max
        Entity createSystemEntity(); // 1..1000
        bool recreateEntity(Entity entity); // for internal editor use only

        bool isEntityCreated(Entity entity) const;

        void destroyEntity(Entity entity);
        // Several entities: every component removal callback first, then each component
        // array compacted once (destroying many one by one shifts the arrays each time)
        void destroyEntities(const std::vector<Entity>& entities);

        void addEntityChild(Entity parent, Entity child, bool changeTransform);

        void moveChildToIndex(Entity entity, size_t index); // for internal/editor use
        // Reorders the component arrays kept in Transform order (meshes, UI, ...) that the
        // signature holds, after the Transform array itself was reordered. For internal/editor
        // use: moving a block of branches directly in the Transform array needs one call,
        // where moveChildToIndex re-sorts once per branch.
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

        Signature getSignature(Entity entity) const;

        void setEntityName(Entity entity, const std::string& name);
        std::string getEntityName(Entity entity) const;

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
        bool isEntityActive(Entity entity) const; // own flag (activeSelf)
        bool isEntityActiveInHierarchy(Entity entity) const; // own flag and every parent's
        bool hasInactiveEntities() const; // false: every entity is active in hierarchy

        // Tag (default "Untagged") and layer (0..31, default 0) of an entity
        void setEntityTag(Entity entity, const std::string& tag);
        std::string getEntityTag(Entity entity) const;
        void setEntityLayer(Entity entity, int layer);
        int getEntityLayer(Entity entity) const;
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

        void setDefaultEntityPool(EntityPool pool);
        EntityPool getDefaultEntityPool() const;

        void clear();

        // Component methods

        template<typename T>
        void registerComponent(){
            componentManager.registerComponent<T>();
        }

        template<typename T>
        void addComponent(Entity entity){
            componentManager.addComponent<T>(entity);
            auto signature = entityManager.getSignature(entity);
            signature.set(componentManager.getComponentId<T>(), true);
            entityManager.setSignature(entity, signature);

            onComponentAdded(entity, componentManager.getComponentId<T>());
        }

        template<typename T>
        void addComponent(Entity entity, const T& component){
            componentManager.addComponent<T>(entity, component);
            auto signature = entityManager.getSignature(entity);
            signature.set(componentManager.getComponentId<T>(), true);
            entityManager.setSignature(entity, signature);

            onComponentAdded(entity, componentManager.getComponentId<T>());
        }

        template<typename T>
        void addComponent(Entity entity, T&& component){
            using Component = std::remove_cv_t<std::remove_reference_t<T>>;
            componentManager.addComponent<Component>(entity, std::forward<T>(component));
            auto signature = entityManager.getSignature(entity);
            signature.set(componentManager.getComponentId<Component>(), true);
            entityManager.setSignature(entity, signature);

            onComponentAdded(entity, componentManager.getComponentId<Component>());
        }

        template<typename T>
        void removeComponent(Entity entity){
            onComponentRemoved(entity, componentManager.getComponentId<T>());

            if (componentManager.getComponentId<T>() == getComponentId<Transform>()) {
                changeTransformChildren(entity);
            }

            componentManager.removeComponent<T>(entity);
            auto signature = entityManager.getSignature(entity);
            signature.set(componentManager.getComponentId<T>(), false);
            entityManager.setSignature(entity, signature); 
        }

        template<typename T>
        T* findComponent(Entity entity) {
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
        T& getComponentFromIndex(size_t index) {
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
    };

} // namespace jasperengine

#endif // ENTITYREGISTRY_H
