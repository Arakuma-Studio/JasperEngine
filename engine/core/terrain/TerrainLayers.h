// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TERRAIN_LAYERS_H
#define TERRAIN_LAYERS_H

// T1 R92, T3 R15: the one mapping from a surface layer to where its paint is stored. A blend map
// weights three layers in its RGB; T3 extends this for layers 10-12 (inverted alpha), so nothing
// else may do this arithmetic or assume nine layers.

namespace jasperengine::terrain {

    struct LayerChannel {
        int map = 0;            // blendMaps[map]
        int channel = 0;        // its colour channel
        bool inverted = false;  // the stored value is 1 - weight
    };

    inline LayerChannel terrainLayerChannel(int layer) {
        return LayerChannel{layer / 3, layer % 3, false};
    }

    // The blend maps that hold layers 0..layerCount-1
    inline int terrainBlendMapCount(int layerCount) {
        int count = 0;
        for (int layer = 0; layer < layerCount; layer++) {
            const int map = terrainLayerChannel(layer).map;
            if (map + 1 > count) count = map + 1;
        }
        return count;
    }

}

#endif // TERRAIN_LAYERS_H
