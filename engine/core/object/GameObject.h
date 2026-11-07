// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef GAMEOBJECT_H
#define GAMEOBJECT_H

#include "EntityHandle.h"
// the classes its getters return (EntityRegistry.h has the reason)
#ifndef JASPERENGINE_EXPLICIT_COMPONENT_INCLUDES
#ifdef JASPERENGINE_PHYSICS_2D
#include "Rigidbody2D.h"
#include "CharacterController2D.h"
#endif
#ifdef JASPERENGINE_PHYSICS_3D
#include "Rigidbody.h"
#include "CharacterController.h"
#include "Buoyancy.h"
#endif
#include "WaterVolume.h"
#include "WaterExcluder.h"
#include "WaterWake.h"
#include "object/spline/Spline.h"
#include "object/spline/SplineExtrude.h"
#include "object/spline/SplineInstantiate.h"
#include "object/spline/SplineAnimate.h"
#endif
#include "math/Ray.h"
#include "manager/PrefabAsset.h"

#include <optional>
#include <string>
#include <vector>

namespace jasperengine{

    class Rigidbody2D;
    class CharacterController2D;
    class Rigidbody;
    class CharacterController;
    class Buoyancy;
    class WaterVolume;
    class WaterExcluder;
    class WaterWake;
    class Spline;
    class SplineExtrude;
    class SplineInstantiate;
    class SplineAnimate;

    class JASPERENGINE_API GameObject: public EntityHandle{
    public:
        GameObject(Scene* scene);
        GameObject(Scene* scene, Entity entity);
        virtual ~GameObject();

        GameObject(const GameObject& rhs);
        GameObject& operator=(const GameObject& rhs);
        GameObject(GameObject&& rhs) noexcept;
        GameObject& operator=(GameObject&& rhs) noexcept;

        // Unity's Object.Instantiate. From a prefab: in the active scene (Engine::getScene)
        // or `scene`; from a GameObject: a copy with its children, components and scripts
        // (references among them go to the copies), in the original's scene. The root is
        // named "<name>(Clone)". Below `parent` it keeps its local placement, or its world
        // one with instantiateInWorldSpace; `position` and `rotation` are world values.
        // The scripts of the new GameObjects run (init) before it returns. A GameObject
        // whose entity is NULL_ENTITY on failure (logged).
        static GameObject instantiate(const PrefabAsset& prefab);
        static GameObject instantiate(const PrefabAsset& prefab, Scene* scene);
        static GameObject instantiate(const PrefabAsset& prefab, const EntityHandle& parent, bool instantiateInWorldSpace = false);
        static GameObject instantiate(const PrefabAsset& prefab, const Vector3& position, const Quaternion& rotation);
        static GameObject instantiate(const PrefabAsset& prefab, const Vector3& position, const Quaternion& rotation, const EntityHandle& parent);
        static GameObject instantiate(const PrefabAsset& prefab, Scene* scene, const Vector3& position, const Quaternion& rotation);
        static GameObject instantiate(const EntityHandle& original);
        static GameObject instantiate(const EntityHandle& original, const EntityHandle& parent, bool instantiateInWorldSpace = false);
        static GameObject instantiate(const EntityHandle& original, const Vector3& position, const Quaternion& rotation);
        static GameObject instantiate(const EntityHandle& original, const Vector3& position, const Quaternion& rotation, const EntityHandle& parent);

        // Unity's Object.Destroy: the GameObject and its children go after this frame's
        // update, or `delay` seconds later; their scripts stop first
        static void destroy(const EntityHandle& object, float delay = 0.0f);
        // Unity's Object.DestroyImmediate
        static void destroyImmediate(const EntityHandle& object);

        void addChild(GameObject* child);
        void addChild(Entity child);

        void removeChild(GameObject* child);
        void removeChild(Entity child);

        void removeParent();

        // Unity's Transform hierarchy. The parent: none for a root or a Transform-less object
        std::optional<GameObject> getParent() const;
        // Unity's SetParent; nullptr moves it to the root. worldPositionStays keeps its world
        // placement (the local one changes), else its local values. A parent in another scene,
        // itself, one of its descendants or one without a Transform is refused (logged)
        void setParent(const EntityHandle* parent, bool worldPositionStays = true);
        // the direct children with a Transform, in Hierarchy order
        size_t getChildCount() const;
        // 0-based; none out of range
        std::optional<GameObject> getChild(size_t index) const;
        std::vector<GameObject> getChildren() const;
        // Unity's Transform.Find: a child by name, or a path of names below this one ("Arm/Hand")
        std::optional<GameObject> findChild(const std::string& path) const;

        void moveToTop();
        void moveUp();
        void moveDown();
        void moveToBottom();

        void setPosition(Vector3 position);
        void setPosition(const float x, const float y, const float z);
        void setPosition(const float x, const float y);
        Vector3 getPosition() const;
        Vector3 getWorldPosition() const;

        void setRotation(Quaternion rotation);
        void setRotation(const float xAngle, const float yAngle, const float zAngle);
        Quaternion getRotation() const;
        Quaternion getWorldRotation() const;

        void setScale(const float factor);
        void setScale(Vector3 scale);
        Vector3 getScale() const;
        Vector3 getWorldScale() const;

        void setVisible(bool visible);
        bool isVisible() const;
        void setVisibleOnly(bool visible);

        void setBillboard(bool billboard, bool fake, bool cylindrical);

        void setBillboard(bool billboard);
        bool isBillboard() const;

        void setBillboardRotation(Quaternion rotation);
        void setBillboardRotation(const float xAngle, const float yAngle, const float zAngle);
        Quaternion getBillboardRotation() const;

        void setFakeBillboard(bool fakeBillboard);
        bool isFakeBillboard() const;

        void setCylindricalBillboard(bool cylindricalBillboard);
        bool isCylindricalBillboard() const;

        void setLocalMatrix(Matrix4 localMatrix);
        Matrix4 getLocalMatrix() const;

        Matrix4 getModelMatrix() const;
        Matrix4 getNormalMatrix() const;

        void updateTransform();

#ifdef JASPERENGINE_PHYSICS_2D
        Rigidbody2D getRigidbody2D();
        void removeRigidbody2D();
        // the GameObject's 2D CharacterController, added with its defaults when it has none
        CharacterController2D getCharacterController2D();
        void removeCharacterController2D();
#endif
#ifdef JASPERENGINE_PHYSICS_3D
        Rigidbody getRigidbody();
        void removeRigidbody();
        // the GameObject's CharacterController, added with its defaults when it has none
        CharacterController getCharacterController();
        void removeCharacterController();
        // the GameObject's Buoyancy, added with its defaults when it has none
        Buoyancy getBuoyancy();
#endif
        // the GameObject's Water Volume, added with its defaults when it has none
        WaterVolume getWaterVolume();
        // the GameObject's Water Excluder and Water Wake, each added with its defaults when it has none
        WaterExcluder getWaterExcluder();
        WaterWake getWaterWake();
        // the GameObject's Spline, Spline Extrude (with a Mesh), Spline Instantiate and Spline
        // Animate, each added with its defaults when it has none
        Spline getSpline();
        SplineExtrude getSplineExtrude();
        SplineInstantiate getSplineInstantiate();
        SplineAnimate getSplineAnimate();

        Ray getRay(Vector3 direction);
    };

}

#endif //GAMEOBJECT_H
