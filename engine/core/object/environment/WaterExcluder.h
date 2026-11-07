// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef WATER_EXCLUDER_H
#define WATER_EXCLUDER_H

#include "object/EntityHandle.h"
#include "component/WaterExcluderComponent.h"

#include <string>

namespace jasperengine{

    // A GameObject's Water Excluder (WaterExcluderComponent): Lua self.gameObject:getWaterExcluder()
    // or WaterExcluder(scene, entity).
    class JASPERENGINE_API WaterExcluder: public EntityHandle{
    private:
        WaterExcluderComponent& component() const;

    public:
        WaterExcluder(Scene* scene, Entity entity);
        virtual ~WaterExcluder();

        WaterExcluder(const WaterExcluder& rhs);
        WaterExcluder& operator=(const WaterExcluder& rhs);
        WaterExcluder(WaterExcluder&& rhs) noexcept;
        WaterExcluder& operator=(WaterExcluder&& rhs) noexcept;

        // the entity has the component
        bool isValid() const;

        void setEnabled(bool enabled);
        bool isEnabled() const;
        void setShape(WaterExcluderShape shape);
        WaterExcluderShape getShape() const;
        void setCenter(Vector3 center);
        Vector3 getCenter() const;
        void setSize(Vector3 size);
        Vector3 getSize() const;

        // whether a world point is inside its volume (enabled or not), at its current pose
        bool contains(Vector3 point) const;
        // its Convex Hull: "none" (another shape), "pending", "ready", "no_mesh", "flat" or "no_physics"
        std::string getHullStatus() const;
        // the hull's faces (0 without a hull)
        int getHullFaces() const;
    };

}

#endif //WATER_EXCLUDER_H
