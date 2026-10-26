// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef BUOYANCY_H
#define BUOYANCY_H

#include "object/EntityHandle.h"
#include "component/BuoyancyComponent.h"

namespace jasperengine{

    // A GameObject's Buoyancy (BuoyancyComponent): Lua self.entity:getBuoyancy() or Buoyancy(scene, entity).
    class JASPERENGINE_API Buoyancy: public EntityHandle{
    private:
        BuoyancyComponent& component() const;

    public:
        Buoyancy(Scene* scene, Entity entity);
        virtual ~Buoyancy();

        Buoyancy(const Buoyancy& rhs);
        Buoyancy& operator=(const Buoyancy& rhs);
        Buoyancy(Buoyancy&& rhs) noexcept;
        Buoyancy& operator=(Buoyancy&& rhs) noexcept;

        // the entity has the component
        bool isValid() const;

        void setEnabled(bool enabled);
        bool isEnabled() const;
        void setColumns(BuoyancyColumns columns);
        BuoyancyColumns getColumns() const;
        void setVolumeScale(float volumeScale);
        float getVolumeScale() const;
        void setDragScale(float dragScale);
        float getDragScale() const;
        // the columns' bottoms in local space (columns CUSTOM)
        void setCustomPoints(const std::vector<Vector3>& points);
        std::vector<Vector3> getCustomPoints() const;
    };

}

#endif //BUOYANCY_H
