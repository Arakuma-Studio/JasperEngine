// T3 R3, D9: the strongest layers of a pixel, picked from the blend weights already read. The C++ form
// is terraincore::selectLayers (TerrainLayers.h), held to this one by TerrainMaterialsGpu.ShaderMatchesCpuSelect.
//
// With TERRAIN_KEEP kept and w the next strongest, each kept weight becomes max(w_i - w, 0), then is
// scaled so the kept weights sum to the sum of all the layers' weights: a layer's share is 0 at the
// moment it enters or leaves the cap, so no seam shows there. A tie of more than TERRAIN_KEEP has a
// scaled sum of 0, and keeps the weights as read. Loop-safe: the insertion stays in local variables of
// a fixed size, in the shape render-correctness' loops rule asks for.

int terrainPickLayer[TERRAIN_KEEP];
float terrainPickWeight[TERRAIN_KEEP];
// the sum of all the layers' weights, of which the base takes what is left
float terrainPickSum;

// one layer into the sorted lists; equal weights keep the lower layer first
void terrainPickInsert(float weight, int layer, inout float kept[TERRAIN_KEEP + 1], inout int order[TERRAIN_KEEP + 1]){
    terrainPickSum += weight;
    if (weight <= 0.0){
        return;
    }
    int at = TERRAIN_KEEP;
    if (weight <= kept[at]){
        return;
    }
    while (at > 0 && kept[at - 1] < weight){
        kept[at] = kept[at - 1];
        order[at] = order[at - 1];
        at--;
    }
    kept[at] = weight;
    order[at] = layer;
}

void terrainPickFinish(float kept[TERRAIN_KEEP + 1], int order[TERRAIN_KEEP + 1]){
    float next = kept[TERRAIN_KEEP];
    float capped = 0.0;
    for (int i = 0; i < TERRAIN_KEEP; i++){
        terrainPickLayer[i] = order[i];
        terrainPickWeight[i] = max(kept[i] - next, 0.0);
        capped += terrainPickWeight[i];
    }
    float scale = capped > 0.0 ? terrainPickSum / capped : 0.0;
    for (int i = 0; i < TERRAIN_KEEP; i++){
        terrainPickWeight[i] = capped > 0.0 ? terrainPickWeight[i] * scale : kept[i];
    }
}
