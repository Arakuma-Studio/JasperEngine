// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TERRAIN_LAYERS_H
#define TERRAIN_LAYERS_H

// T1 R92, T3 R3, R15: the one mapping from a surface layer to where its paint is stored, and the one
// C++ form of the shader's layer selection (includes/terrain_select.glsl). A blend map weights three
// layers in its RGB, and on a terrain of more than nine layers a fourth by its alpha, inverted, so
// that an opaque map means weight 0. Nothing else may do this arithmetic or assume nine layers.

namespace jasperengine::terrain {

    inline constexpr int TERRAIN_MAX_LAYERS = 12;
    inline constexpr int TERRAIN_LAYERS_IN_RGB = 9;

    struct LayerChannel {
        int map = 0;            // blendMaps[map]
        int channel = 0;        // its colour channel, 3 is its alpha
        bool inverted = false;  // the stored value is 1 - weight
    };

    inline LayerChannel terrainLayerChannel(int layer) {
        if (layer >= TERRAIN_LAYERS_IN_RGB) return LayerChannel{layer - TERRAIN_LAYERS_IN_RGB, 3, true};
        return LayerChannel{layer / 3, layer % 3, false};
    }

    // The blend maps that hold layers 0..layerCount-1
    inline int terrainBlendMapCount(int layerCount) {
        int count = 0;
        for (int layer = 0; layer < layerCount; layer++) {
            const int map = terrainLayerChannel(layer).map;
            if (map + 1 > count) count = map + 1;
        }
        // layers in the alphas read the maps of the RGB layers: all three
        return layerCount > TERRAIN_LAYERS_IN_RGB ? 3 : count;
    }

    struct LayerSelection {
        int count = 0;                          // layers kept
        int layer[TERRAIN_MAX_LAYERS] = {};     // strongest first
        float weight[TERRAIN_MAX_LAYERS] = {};  // after the cap
        float baseWeight = 0.0f;                // 1 - the sum of all the layers' weights, at least 0
    };

    // T3 R3. With keep kept and w the (keep + 1)-th strongest weight, each kept weight becomes
    // max(w_i - w, 0), then is scaled so the kept weights sum to the original sum of all layer
    // weights; a scaled sum of 0 (a tie of more than keep) uses them as read. So a layer's share is 0
    // at the moment it enters or leaves the cap. keep <= 0, or more than there are, keeps every layer
    // with a weight as read. Equal weights keep the lower layer first, as the shader's insertion does.
    inline LayerSelection selectLayers(const float* weights, int layerCount, int keep) {
        LayerSelection selection;
        float sorted[TERRAIN_MAX_LAYERS + 1];
        int order[TERRAIN_MAX_LAYERS + 1];
        int sortedCount = 0;
        float sum = 0.0f;
        for (int layer = 0; layer < layerCount && layer < TERRAIN_MAX_LAYERS; layer++) {
            const float weight = weights[layer];
            sum += weight;
            if (weight <= 0.0f) continue;
            int at = sortedCount;
            while (at > 0 && sorted[at - 1] < weight) {
                sorted[at] = sorted[at - 1];
                order[at] = order[at - 1];
                at--;
            }
            sorted[at] = weight;
            order[at] = layer;
            sortedCount++;
        }
        selection.baseWeight = sum < 1.0f ? 1.0f - sum : 0.0f;
        const int kept = (keep <= 0 || keep > sortedCount) ? sortedCount : keep;
        selection.count = kept;
        const float next = kept < sortedCount ? sorted[kept] : 0.0f;
        float capped = 0.0f;
        for (int i = 0; i < kept; i++) {
            selection.layer[i] = order[i];
            selection.weight[i] = sorted[i] > next ? sorted[i] - next : 0.0f;
            capped += selection.weight[i];
        }
        if (kept == sortedCount) {
            // nothing was left out: the weights as read
            for (int i = 0; i < kept; i++) selection.weight[i] = sorted[i];
        } else if (capped > 0.0f) {
            const float scale = sum / capped;
            for (int i = 0; i < kept; i++) selection.weight[i] *= scale;
        } else {
            for (int i = 0; i < kept; i++) selection.weight[i] = sorted[i];
        }
        return selection;
    }

}

#endif // TERRAIN_LAYERS_H
