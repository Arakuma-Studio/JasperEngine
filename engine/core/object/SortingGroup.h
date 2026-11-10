// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The Sorting Group of a GameObject (docs/specs/S52-sprite-instancing.md R5, R37): Lua
// `SortingGroup(scene, entity)` and C++ scripts. It ADDS a Sorting Group when the GameObject has
// none. Main thread only (ECS).

#ifndef SORTING_GROUP_H
#define SORTING_GROUP_H

#include "object/EntityHandle.h"
#include "component/SortingGroupComponent.h"

#include <string>

namespace jasperengine{

    class JASPERENGINE_API SortingGroup: public EntityHandle{

    public:
        SortingGroup(Scene* scene, Entity entity);
        virtual ~SortingGroup();

        SortingGroup(const SortingGroup& rhs);
        SortingGroup& operator=(const SortingGroup& rhs);

        void setEnabled(bool enabled);
        bool isEnabled() const;
        // a SortingLayers id; an unknown one logs an error and changes nothing
        void setSortingLayerId(int id);
        int getSortingLayerId() const;
        // a layer name; an unknown one logs an error and changes nothing
        void setSortingLayerName(const std::string& name);
        std::string getSortingLayerName() const;
        // -32768 to 32767
        void setSortingOrder(int order);
        int getSortingOrder() const;
    };

}

#endif //SORTING_GROUP_H
