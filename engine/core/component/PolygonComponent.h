// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef POLYGON_COMPONENT_H
#define POLYGON_COMPONENT_H

#include "Export.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include <vector>

namespace jasperengine{

    struct PolygonPoint{
        Vector3 position;
        Vector4 color;
    };

    struct JASPERENGINE_API PolygonComponent{
        std::vector<PolygonPoint> points;

        bool needUpdatePolygon = true;
    };

}

#endif //POLYGON_COMPONENT_H