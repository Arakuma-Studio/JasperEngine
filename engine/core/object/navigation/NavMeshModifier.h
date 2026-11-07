// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NAV_MESH_MODIFIER_H
#define NAV_MESH_MODIFIER_H

#include "object/EntityHandle.h"
#include "component/NavMeshModifierComponent.h"

namespace jasperengine{

    // Unity's NavMeshModifier / NavMeshModifierVolume (docs/navigation.md): Lua
    // NavMeshModifier(scene, entity). With Use Volume and Apply At Runtime, enabled, area, center
    // and size change the navmesh without a rebake (S55 R11, R32); the rest are bake inputs.
    class JASPERENGINE_API NavMeshModifier: public EntityHandle{
    private:
        NavMeshModifierComponent& component() const;
        void sync() const;

    public:
        NavMeshModifier(Scene* scene, Entity entity);
        virtual ~NavMeshModifier();

        NavMeshModifier(const NavMeshModifier& rhs);
        NavMeshModifier& operator=(const NavMeshModifier& rhs);
        NavMeshModifier(NavMeshModifier&& rhs) noexcept;
        NavMeshModifier& operator=(NavMeshModifier&& rhs) noexcept;

        bool isValid() const;

        void setEnabled(bool enabled);
        bool isEnabled() const;
        void setArea(int area);
        int getArea() const;
        void setCenter(Vector3 center);
        Vector3 getCenter() const;
        void setSize(Vector3 size);
        Vector3 getSize() const;
        bool isUseVolume() const;
        bool isApplyAtRuntime() const;
        bool isOverrideArea() const;
        bool isApplyToChildren() const;
        // the polygons its runtime volume holds now (0: not applied)
        int getAppliedPolygons() const;
    };

}

#endif //NAV_MESH_MODIFIER_H
