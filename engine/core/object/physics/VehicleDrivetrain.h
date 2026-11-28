// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VEHICLE_DRIVETRAIN_H
#define VEHICLE_DRIVETRAIN_H

#include "object/EntityHandle.h"
#include "component/VehicleDrivetrainComponent.h"

namespace jasperengine{

    // The VehicleDrivetrain of a vehicle's body: Lua self.entity:getVehicleDrivetrain() or
    // VehicleDrivetrain(scene, entity). The inputs reach the vehicle at the next fixed step; rpm,
    // gear and speed are those of the last one.
    class JASPERENGINE_API VehicleDrivetrain: public EntityHandle{
    private:
        VehicleDrivetrainComponent& component() const;

    public:
        VehicleDrivetrain(Scene* scene, Entity entity);
        virtual ~VehicleDrivetrain();

        VehicleDrivetrain(const VehicleDrivetrain& rhs);
        VehicleDrivetrain& operator=(const VehicleDrivetrain& rhs);
        VehicleDrivetrain(VehicleDrivetrain&& rhs) noexcept;
        VehicleDrivetrain& operator=(VehicleDrivetrain&& rhs) noexcept;

        // the entity has the component
        bool isValid() const;

        void setEnabled(bool enabled);
        bool isEnabled() const;

        void setThrottle(float throttle);
        float getThrottle() const;
        void setSteer(float steer);
        float getSteer() const;
        void setBrake(float brake);
        float getBrake() const;
        void setHandBrake(float handBrake);
        float getHandBrake() const;
        // the manual transmission's gear at the next step (-1 reverse, 0 neutral, 1.. forward)
        void setGear(int gear);

        float getRpm() const;
        int getGear() const;
        float getSpeed() const;

        void setMaxTorque(float torque);
        float getMaxTorque() const;
        void setMinRPM(float rpm);
        float getMinRPM() const;
        void setMaxRPM(float rpm);
        float getMaxRPM() const;
        void setTransmission(VehicleTransmissionMode mode);
        VehicleTransmissionMode getTransmission() const;
        void setShiftUpRPM(float rpm);
        float getShiftUpRPM() const;
        void setShiftDownRPM(float rpm);
        float getShiftDownRPM() const;
        void setClutchStrength(float strength);
        float getClutchStrength() const;
        void setMaxPitchRollAngle(float angle);
        float getMaxPitchRollAngle() const;
    };

}

#endif //VEHICLE_DRIVETRAIN_H
