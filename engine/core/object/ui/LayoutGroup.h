// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LAYOUT_GROUP_H
#define LAYOUT_GROUP_H

#include "RectTransform.h"

namespace jasperengine{
    class JASPERENGINE_API LayoutGroup: public RectTransform{

    public:
        LayoutGroup(Scene* scene);
        LayoutGroup(Scene* scene, Entity entity);
        virtual ~LayoutGroup();
        
        void setType(LayoutGroupType type);
        LayoutGroupType getType() const;
        void setUseAllWrapSpace(bool useAllWrapSpace);
        bool isUseAllWrapSpace() const;

        void setWrapCellWidth(unsigned int width);
        unsigned int getWrapCellWidth() const;
        void setWrapCellHeight(unsigned int height);
        unsigned int getWrapCellHeight() const;

        unsigned int getContentWidth() const;
        unsigned int getContentHeight() const;

        void resize();

        void setBoxExpand(bool expand);
        void setBoxExpand(size_t id, bool expand);
        bool isBoxExpand(size_t id) const;
    };
}

#endif //LAYOUT_GROUP_H