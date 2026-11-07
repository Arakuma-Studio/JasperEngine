// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NAV_MESH_LINK_H
#define NAV_MESH_LINK_H

#include "object/EntityHandle.h"
#include "component/NavMeshLinkComponent.h"

namespace jasperengine{

    // Unity's NavMeshLink (docs/navigation.md): Lua NavMeshLink(scene, entity). activated works
    // without a rebake (S55 R16); the other fields are bake inputs: updateLink() moves the baked
    // connections to them (S53's link update), a rebake does the rest.
    class JASPERENGINE_API NavMeshLink: public EntityHandle{
    private:
        NavMeshLinkComponent& component() const;

    public:
        NavMeshLink(Scene* scene, Entity entity);
        virtual ~NavMeshLink();

        NavMeshLink(const NavMeshLink& rhs);
        NavMeshLink& operator=(const NavMeshLink& rhs);
        NavMeshLink(NavMeshLink&& rhs) noexcept;
        NavMeshLink& operator=(NavMeshLink&& rhs) noexcept;

        bool isValid() const;

        void setEnabled(bool enabled);
        bool isEnabled() const;
        void setAgentTypeID(int agentType);
        int getAgentTypeID() const;
        void setStartPoint(Vector3 point);
        Vector3 getStartPoint() const;
        void setEndPoint(Vector3 point);
        Vector3 getEndPoint() const;
        void setWidth(float width);
        float getWidth() const;
        void setCostModifier(float cost);
        float getCostModifier() const;
        void setBidirectional(bool bidirectional);
        bool isBidirectional() const;
        void setArea(int area);
        int getArea() const;
        void setAutoUpdate(bool autoUpdate);
        bool isAutoUpdate() const;
        void setActivated(bool activated);
        bool isActivated() const;
        // an agent is crossing one of its connections (between its link events)
        bool isOccupied() const;
        // its baked connections follow its current fields; false when a surface must be rebaked
        bool updateLink();
    };

}

#endif //NAV_MESH_LINK_H
