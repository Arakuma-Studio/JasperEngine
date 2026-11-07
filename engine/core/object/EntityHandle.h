// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ENTITYHANDLE_H
#define ENTITYHANDLE_H

#include "Scene.h"
#include "Entity.h"
#include <memory>
#include <type_traits>
#include <utility>

namespace jasperengine{
    // A subclass that declares a destructor or copy operations has no implicit move: it must
    // declare its moves too (= default when it adds no state), else a "move" of it is a copy,
    // which never owns the entity, and the moved-from wrapper takes the entity along.
    class JASPERENGINE_API EntityHandle{

    protected:
        Entity entity;
        Scene* scene;

        bool entityOwned;
        // The scene's lifetime (Scene::getLifetime) while the handle owns the entity
        std::weak_ptr<char> sceneLifetime;

        // Owns an entity that is still there: its scene was not deleted (a Lua handle can
        // outlive it) and did not destroy it (GameObject.destroy). Only then may the
        // handle's destructor touch the scene.
        bool ownsLiveEntity() const;

    public:
        EntityHandle(Scene* scene);
        EntityHandle(Scene* scene, Entity entity);
        virtual ~EntityHandle();

        EntityHandle(const EntityHandle& rhs);
        EntityHandle& operator=(const EntityHandle& rhs);

        EntityHandle(EntityHandle&& rhs) noexcept;
        EntityHandle& operator=(EntityHandle&& rhs) noexcept;

        Scene* getScene() const;
        Entity getEntity() const;

        // False for an empty handle or a destroyed GameObject (Unity's null): what
        // GameObject::instantiate returns when the copy's init destroyed it at once
        explicit operator bool() const;

        void setName(const std::string& name);
        std::string getName() const;

        // Unity GameObject.SetActive / activeSelf / activeInHierarchy
        void setActive(bool active);
        bool isActive() const;
        bool isActiveInHierarchy() const;

        // Unity GameObject.tag / CompareTag: a tag missing from the project's Tags and
        // Layers logs "Tag: X is not defined." (setTag leaves the tag unchanged)
        void setTag(const std::string& tag);
        std::string getTag() const;
        bool compareTag(const std::string& tag) const;

        // 0..31; out of range logs an error and leaves the layer unchanged
        void setLayer(int layer);
        int getLayer() const;

        bool isEntityOwned() const;
        void setEntityOwned(bool entityOwned);

        // Engine-internal: the GameObject moved to another scene (persistence, ObjectManager::
        // moveToScene), this handle follows it (an owning handle watches the new scene)
        void rebind(Scene* scene, Entity entity);

        template <typename T>
        void addComponent() {
            scene->addComponent<T>(entity);
        }

        template <typename T>
        void addComponent(const T& component) {
            scene->addComponent<T>(entity, component);
        }

        template <typename T>
        void addComponent(T&& component) {
            using Component = std::remove_cv_t<std::remove_reference_t<T>>;
            scene->addComponent<Component>(entity, std::forward<T>(component));
        }
    
        template <typename T>
        void removeComponent() {
            scene->removeComponent<T>(entity);
        }
    
        template<typename T>
    	T& getComponent() const {
    		return scene->getComponent<T>(entity);
    	}

        // A change made through the wrapper: the setters that set a needUpdate flag stamp
        // the component changed (Changed<T>, EntityRegistry::markChanged)
        template <typename T>
        void markChanged() const {
            scene->markChanged<T>(entity);
        }

    };
}

#endif //ENTITYHANDLE_H
