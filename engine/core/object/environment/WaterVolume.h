// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef WATER_VOLUME_H
#define WATER_VOLUME_H

#include "object/EntityHandle.h"
#include "component/WaterVolumeComponent.h"

namespace jasperengine{

    // A GameObject's Water Volume (WaterVolumeComponent): Lua self.entity:getWaterVolume() or WaterVolume(scene, entity).
    class JASPERENGINE_API WaterVolume: public EntityHandle{
    private:
        WaterVolumeComponent& component() const;

    public:
        WaterVolume(Scene* scene, Entity entity);
        virtual ~WaterVolume();

        WaterVolume(const WaterVolume& rhs);
        WaterVolume& operator=(const WaterVolume& rhs);
        WaterVolume(WaterVolume&& rhs) noexcept;
        WaterVolume& operator=(WaterVolume&& rhs) noexcept;

        // the entity has the component
        bool isValid() const;

        void setEnabled(bool enabled);
        bool isEnabled() const;
        void setSize(Vector3 size);
        Vector3 getSize() const;
        void setSurfaceOffset(float surfaceOffset);
        float getSurfaceOffset() const;
        void setInfinite(bool infinite);
        bool isInfinite() const;
        void setDensity(float density);
        float getDensity() const;
        void setFlow(Vector3 flow);
        Vector3 getFlow() const;
        void setLinearDrag(float linearDrag);
        float getLinearDrag() const;
        void setAngularDrag(float angularDrag);
        float getAngularDrag() const;
        void setWaves(WaterVolumeWaves waves);
        WaterVolumeWaves getWaves() const;
    };

}

#endif //WATER_VOLUME_H
