// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef FOGZONE_H
#define FOGZONE_H

#include "object/GameObject.h"
#include "component/FogZoneComponent.h"

#include <string>

namespace jasperengine{

    // A GameObject holding a Fog Zone (see FogZoneComponent): its profile's overrides blend over
    // the scene's fog by where the camera is.
    class JASPERENGINE_API FogZone: public GameObject{

    public:
        FogZone(Scene* scene);
        FogZone(Scene* scene, Entity entity);
        virtual ~FogZone();

        FogZone(const FogZone& rhs);
        FogZone& operator=(const FogZone& rhs);
        FogZone(FogZone&& rhs) noexcept;
        FogZone& operator=(FogZone&& rhs) noexcept;

        void setShape(FogZoneShape shape);
        FogZoneShape getShape() const;

        void setSize(Vector3 size);
        void setSize(const float x, const float y, const float z);
        Vector3 getSize() const;

        void setBlendDistance(float blendDistance);
        float getBlendDistance() const;

        void setPriority(int priority);
        int getPriority() const;

        void setWeight(float weight);
        float getWeight() const;

        // a .fogprofile path (assets-relative), "builtin:<id>", a runtime key, or "" for none
        void setProfile(const std::string& profile);
        std::string getProfile() const;

        // the weight it blends with at the scene's camera now (0 without a readable profile)
        float getCurrentWeight() const;
    };
}

#endif //FOGZONE_H
