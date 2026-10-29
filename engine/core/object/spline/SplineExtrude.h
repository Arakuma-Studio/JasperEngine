// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SPLINE_EXTRUDE_H
#define SPLINE_EXTRUDE_H

#include "object/EntityHandle.h"
#include "component/SplineExtrudeComponent.h"

#include <string>

namespace jasperengine{

    // A GameObject's Spline Extrude (SplineExtrudeComponent): Lua self.entity:getSplineExtrude(). Changing a value regenerates the mesh (SplineSystem, within its budget).
    class JASPERENGINE_API SplineExtrude: public EntityHandle{
    private:
        SplineExtrudeComponent& component() const;

    public:
        SplineExtrude(Scene* scene, Entity entity);
        virtual ~SplineExtrude();

        SplineExtrude(const SplineExtrude& rhs);
        SplineExtrude& operator=(const SplineExtrude& rhs);
        SplineExtrude(SplineExtrude&& rhs) noexcept;
        SplineExtrude& operator=(SplineExtrude&& rhs) noexcept;

        // the entity has the component
        bool isValid() const;

        void setEnabled(bool enabled);
        bool isEnabled() const;
        void setShape(SplineExtrudeShape value);
        SplineExtrudeShape getShape() const;
        void setWidthSource(SplineWidthSource value);
        SplineWidthSource getWidthSource() const;
        void setWidth(float value);
        float getWidth() const;
        void setEdgeDrop(float value);
        float getEdgeDrop() const;
        void setEdgeInset(float value);
        float getEdgeInset() const;
        void setCrownRise(float value);
        float getCrownRise() const;
        void setShoulderWidth(float value);
        float getShoulderWidth() const;
        void setShoulderDrop(float value);
        float getShoulderDrop() const;
        void setThickness(float value);
        float getThickness() const;
        void setHeight(float value);
        float getHeight() const;
        void setGrade(SplineGrade value);
        SplineGrade getGrade() const;
        void setStepHeight(float value);
        float getStepHeight() const;
        void setTubeSides(unsigned int value);
        unsigned int getTubeSides() const;
        void setMaxHalfWidth(float value);
        float getMaxHalfWidth() const;
        void setWaterEdgeDrop(float value);
        float getWaterEdgeDrop() const;
        void setFlow(bool value);
        bool isFlow() const;
        void setFlowSpeed(float value);
        float getFlowSpeed() const;
        void setTilesPerMetreU(float value);
        float getTilesPerMetreU() const;
        void setTilesPerMetreV(float value);
        float getTilesPerMetreV() const;
        void setLevelCourseV(bool value);
        bool isLevelCourseV() const;
        void setCorners(SplineCornerStyle value);
        SplineCornerStyle getCorners() const;
        void setEndCaps(bool value);
        bool isEndCaps() const;
        void setEndTaper(float value);
        float getEndTaper() const;
        void setLateralOffset(float value);
        float getLateralOffset() const;
        void setVerticalOffset(float value);
        float getVerticalOffset() const;
        void setConform(SplineConform value);
        SplineConform getConform() const;
        void setSlopeBlend(float value);
        float getSlopeBlend() const;
        void setStationSpacing(float value);
        float getStationSpacing() const;
        void setCollision(SplineCollision value);
        SplineCollision getCollision() const;

        // what the last build made (SplineSystem::getExtrudeStats)
        unsigned int getVertexCount() const;
        unsigned int getChunkCount() const;
        // a rebuild is scheduled or running
        bool isPending() const;
        // rebuilds now and waits (the spline and this component)
        void rebuild();
    };

}

#endif //SPLINE_EXTRUDE_H
