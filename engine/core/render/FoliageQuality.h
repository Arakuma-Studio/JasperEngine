// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef FOLIAGE_QUALITY_H
#define FOLIAGE_QUALITY_H

namespace jasperengine{

    // Foliage Quality per platform group (T5 R21). What each scales and caps is
    // terraincore::foliageTierLimits; distance is Foliage Distance's (Engine::setFoliageDistanceScale).
    enum class FoliageQuality{
        LOW = 0,
        MEDIUM = 1,
        HIGH = 2
    };

}

#endif //FOLIAGE_QUALITY_H
