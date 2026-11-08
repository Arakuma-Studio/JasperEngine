// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NAV_MESH_OBSTACLE_H
#define NAV_MESH_OBSTACLE_H

#include "object/EntityHandle.h"
#include "component/NavMeshObstacleComponent.h"

namespace jasperengine{

    // Unity's NavMeshObstacle (docs/navigation.md): Lua NavMeshObstacle(scene, entity). Unity's
    // scripting names over the component's fields (carving, carvingMoveThreshold,
    // carvingTimeToStationary; S76 R30, R33); changes apply at the next fixed tick.
    class JASPERENGINE_API NavMeshObstacle: public EntityHandle{
    private:
        NavMeshObstacleComponent& component() const;

    public:
        NavMeshObstacle(Scene* scene, Entity entity);
        virtual ~NavMeshObstacle();

        NavMeshObstacle(const NavMeshObstacle& rhs);
        NavMeshObstacle& operator=(const NavMeshObstacle& rhs);
        NavMeshObstacle(NavMeshObstacle&& rhs) noexcept;
        NavMeshObstacle& operator=(NavMeshObstacle&& rhs) noexcept;

        bool isValid() const;

        void setEnabled(bool enabled);
        bool isEnabled() const;
        void setShape(NavObstacleShape shape);
        NavObstacleShape getShape() const;
        void setCenter(Vector3 center);
        Vector3 getCenter() const;
        void setSize(Vector3 size);
        Vector3 getSize() const;
        void setRadius(float radius);
        float getRadius() const;
        void setHeight(float height);
        float getHeight() const;
        void setCarving(bool carving);
        bool isCarving() const;
        void setCarveOnlyStationary(bool only);
        bool isCarveOnlyStationary() const;
        void setCarvingMoveThreshold(float threshold);
        float getCarvingMoveThreshold() const;
        void setCarvingTimeToStationary(float seconds);
        float getCarvingTimeToStationary() const;
        // world, from its transform at the last fixed tick
        Vector3 getVelocity() const;
    };

}

#endif //NAV_MESH_OBSTACLE_H
