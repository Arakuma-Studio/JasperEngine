// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ENTITYHANDLE_H
#define ENTITYHANDLE_H

#include "Scene.h"
#include "Entity.h"
#include <type_traits>
#include <utility>

namespace jasperengine{
    class JASPERENGINE_API EntityHandle{

    protected:
        Entity entity;
        Scene* scene;

        bool entityOwned;

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

        void setName(const std::string& name);
        std::string getName() const;

        // Unity GameObject.SetActive / activeSelf / activeInHierarchy
        void setActive(bool active);
        bool isActive() const;
        bool isActiveInHierarchy() const;

        void setTag(const std::string& tag);
        std::string getTag() const;
        bool compareTag(const std::string& tag) const;

        void setLayer(int layer);
        int getLayer() const;

        bool isEntityOwned() const;
        void setEntityOwned(bool entityOwned);

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

    };
}

#endif //ENTITYHANDLE_H
