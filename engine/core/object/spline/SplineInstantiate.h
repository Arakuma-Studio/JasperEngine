// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SPLINE_INSTANTIATE_H
#define SPLINE_INSTANTIATE_H

#include "object/EntityHandle.h"
#include "component/SplineInstantiateComponent.h"

#include <string>

namespace jasperengine{

    // A GameObject's Spline Instantiate (SplineInstantiateComponent): Lua self.entity:getSplineInstantiate(). Changing a value places the pieces again (SplineSystem).
    class JASPERENGINE_API SplineInstantiate: public EntityHandle{
    private:
        SplineInstantiateComponent& component() const;

    public:
        SplineInstantiate(Scene* scene, Entity entity);
        virtual ~SplineInstantiate();

        SplineInstantiate(const SplineInstantiate& rhs);
        SplineInstantiate& operator=(const SplineInstantiate& rhs);
        SplineInstantiate(SplineInstantiate&& rhs) noexcept;
        SplineInstantiate& operator=(SplineInstantiate&& rhs) noexcept;

        // the entity has the component
        bool isValid() const;

        void setEnabled(bool enabled);
        bool isEnabled() const;
        void setMode(SplineInstantiateMode value);
        SplineInstantiateMode getMode() const;
        void setSpacing(float value);
        float getSpacing() const;
        void setFit(SplineSpacingFit value);
        SplineSpacingFit getFit() const;
        void setPostPitch(float value);
        float getPostPitch() const;
        void setSpanMaxStretch(float value);
        float getSpanMaxStretch() const;
        void setFenceGrade(SplineGrade value);
        SplineGrade getFenceGrade() const;
        void setCrestPitch(float value);
        float getCrestPitch() const;
        void setLateralOffset(float value);
        float getLateralOffset() const;
        void setVerticalOffset(float value);
        float getVerticalOffset() const;
        void setSpacingJitter(float value);
        float getSpacingJitter() const;
        void setYawJitter(float value);
        float getYawJitter() const;
        void setLateralJitter(float value);
        float getLateralJitter() const;
        void setScaleRange(Vector2 value);
        Vector2 getScaleRange() const;
        void setDropout(float value);
        float getDropout() const;
        void setSeed(uint32_t value);
        uint32_t getSeed() const;
        void setEndTrim(float value);
        float getEndTrim() const;
        void setPlantMode(SplinePlantMode value);
        SplinePlantMode getPlantMode() const;
        void setMaxTilt(float value);
        float getMaxTilt() const;
        void setConform(SplineConform value);
        SplineConform getConform() const;
        void setSlopeBlend(float value);
        float getSlopeBlend() const;
        void setStationSpacing(float value);
        float getStationSpacing() const;

        // pool entries (models .gltf/.glb/.obj, prefabs .prefab), at most MAX_POOL_ENTRIES each
        bool addItem(const std::string& path, float weight);
        void clearItems();
        size_t getItemCount() const;

        // what the last build placed (SplineSystem::getInstantiateStats)
        unsigned int getInstanceCount() const;
        bool isPending() const;
        void rebuild();
    };

}

#endif //SPLINE_INSTANTIATE_H
