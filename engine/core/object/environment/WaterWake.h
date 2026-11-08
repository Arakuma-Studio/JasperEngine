// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef WATER_WAKE_H
#define WATER_WAKE_H

#include "object/EntityHandle.h"
#include "component/WaterWakeComponent.h"

namespace jasperengine{

    // A GameObject's Water Wake (WaterWakeComponent): Lua self.gameObject:getWaterWake() or
    // WaterWake(scene, entity).
    class JASPERENGINE_API WaterWake: public EntityHandle{
    private:
        WaterWakeComponent& component() const;

    public:
        WaterWake(Scene* scene, Entity entity);
        virtual ~WaterWake();

        WaterWake(const WaterWake& rhs);
        WaterWake& operator=(const WaterWake& rhs);
        WaterWake(WaterWake&& rhs) noexcept;
        WaterWake& operator=(WaterWake&& rhs) noexcept;

        // the entity has the component
        bool isValid() const;

        void setEnabled(bool enabled);
        bool isEnabled() const;
        void setFoam(float foam);
        float getFoam() const;
        void setWaves(float waves);
        float getWaves() const;
        void setWidth(float width);
        float getWidth() const;
        void setLength(float length);
        float getLength() const;

        // on a water and moving at its last step (a running scene)
        bool isEmitting() const;
        // its smoothed speed over the water (m/s)
        float getSpeed() const;
    };

}

#endif //WATER_WAKE_H
