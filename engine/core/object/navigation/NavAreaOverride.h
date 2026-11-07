// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NAV_AREA_OVERRIDE_H
#define NAV_AREA_OVERRIDE_H

#include "Export.h"
#include "math/Quaternion.h"
#include "math/Vector3.h"

#include <cstdint>
#include <memory>

namespace jasperengine{

    class NavigationSystem;
    class Scene;

    // A script volume (docs/navigation.md, S55 R12): what NavigationSystem:setAreaInVolume returns
    // in Lua. The polygons whose centroid lies in its box take its area until remove(); handles end
    // with the scene's navigation (Stop). A copy names the same volume.
    class JASPERENGINE_API NavAreaOverride{
    private:
        Scene* scene = nullptr;
        uint32_t handle = 0;
        // a handle kept past its scene (a Lua value) is no longer valid
        std::weak_ptr<char> sceneLifetime;

        NavigationSystem* system() const;

    public:
        NavAreaOverride() = default;
        NavAreaOverride(Scene* scene, uint32_t handle);

        // the volume still exists
        bool isValid() const;
        uint32_t getHandle() const { return handle; }

        void setArea(int area);
        int getArea() const;
        void setCenter(Vector3 center);
        Vector3 getCenter() const;
        void setSize(Vector3 size);
        Vector3 getSize() const;
        void setRotation(Quaternion rotation);
        Quaternion getRotation() const;
        int getAppliedPolygons() const;
        // ends it: its polygons get their baked areas back
        void remove();
    };

}

#endif //NAV_AREA_OVERRIDE_H
