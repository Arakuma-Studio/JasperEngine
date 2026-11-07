// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NAVIGATION_AREAS_H
#define NAVIGATION_AREAS_H

// Engine-internal: what NavigationSystem's runtime areas hold (docs/specs/S55-navigation-gameplay-api.md
// R11-R17, D1, D9; NavigationAreas.cpp). The system makes it on the first runtime modifier volume,
// script volume or deactivated link, so a scene without them holds nothing.

#include "math/Quaternion.h"
#include "math/Vector3.h"
#include "subsystem/NavigationSystem.h"

#include <map>
#include <set>
#include <vector>

namespace jasperengine {

    struct NavigationSystem::AreaState {
        // a NavMesh Modifier with Use Volume and Apply At Runtime, as last applied
        struct Modifier {
            Vector3 position, scale, center, size;
            Quaternion rotation;
            uint8_t area = 0;
            bool active = false;
            // its override on each surface's navmesh
            std::map<Entity, uint32_t> ids;
        };
        // a script volume (setAreaInVolume), world space
        struct Handle {
            Vector3 center, size;
            Quaternion rotation;
            uint8_t area = 0;
            int agentType = -1;
            std::map<Entity, uint32_t> ids;
        };
        // the navmesh the overrides of a surface were added to, and its deactivated links
        struct SurfaceMesh {
            const nav::NavMesh* mesh = nullptr;
            std::set<Entity> inactiveLinks;
        };

        std::map<Entity, Modifier> modifiers;
        std::map<uint32_t, Handle> handles;
        std::map<Entity, SurfaceMesh> surfaces;
        uint32_t nextHandle = 1;
        // the polygons whose flags or area changed since the last sync, per surface (sorted): the
        // crowds' replan (fixed tick step 1b)
        std::map<Entity, std::vector<uint32_t>> changed;
        std::vector<uint32_t> scratch;

        // the surface's navmesh was replaced: nothing of it holds the overrides any more
        void forgetSurface(Entity surface);
        bool empty() const { return modifiers.empty() && handles.empty() && surfaces.empty(); }
    };

}

#endif // NAVIGATION_AREAS_H
