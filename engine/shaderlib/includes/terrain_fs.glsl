// The tiers (T3 R1): TERRAIN_QUALITY_MEDIUM and _LOW; no define is High, as before T3. A tier below High
// reads one projection per fragment (TERRAIN_ONE_PLANE) and caps the layers by TERRAIN_KEEP, as High does
// with Limit Layers Per Pixel (TERRAIN_LAYER_LIMIT).
#if defined(TERRAIN_QUALITY_MEDIUM)
    #define TERRAIN_TIER_READS
    #define TERRAIN_KEEP 3
#elif defined(TERRAIN_QUALITY_LOW)
    #define TERRAIN_TIER_READS
    #define TERRAIN_KEEP 2
#elif defined(TERRAIN_LAYER_LIMIT)
    #define TERRAIN_KEEP 4
#endif

// A terrain of more than 9 layers takes a block of 12 (T3 R15); up to 9 keeps the 9-entry block
#ifdef TERRAIN_EXTENDED_LAYERS
    #undef MAX_TERRAIN_LAYERS
    #define MAX_TERRAIN_LAYERS 12
#endif

uniform texture2D u_blendMap;
uniform sampler u_blendMap_smp;
// Medium and Low read only the blend maps that hold layers (T3 R7), and the others are not declared
#if !defined(TERRAIN_BLENDMAPS_1)
    uniform texture2D u_blendMap1;
    uniform sampler u_blendMap1_smp;
#endif
#if !defined(TERRAIN_BLENDMAPS_1) && !defined(TERRAIN_BLENDMAPS_2)
    uniform texture2D u_blendMap2;
    uniform sampler u_blendMap2_smp;
#endif
uniform texture2DArray u_terrainDetail;
uniform sampler u_terrainDetail_smp;

in vec2 v_terrainTextureCoords;
in vec2 v_terrainTextureDetailTiled;
in float v_terrainDetailHeight;
in float v_terrainEyeTiles;
#ifdef HAS_NORMALS
    in vec3 v_terrainNormal;
#endif

// A coarser second tiling rate fades in over this many detail tiles of distance
const float TERRAIN_TILE_BREAK_NEAR = 6.0;
const float TERRAIN_TILE_BREAK_FAR = 40.0;
const float TERRAIN_TILE_BREAK_RATE = 0.371;
const float TERRAIN_TILE_BREAK_MIX = 0.5;

// The three projections are weighted by abs(normal) raised to this power: the higher, the
// narrower the slope band where they overlap (each one stretches on the planes it does not
// face, and two halves of stretched projections read as ghosted streaks). 8 hands over around
// 45 degrees and leaves gentle slopes to the flat projection alone.
const float TERRAIN_TRIPLANAR_SHARPNESS = 8.0;
// A projection under this weight is dropped and the others take its share, so most pixels
// fetch a single projection
const float TERRAIN_TRIPLANAR_CUTOFF = 0.02;

// How hard layer height biases the blend. 0.0 leaves the plain blend map weights
const float TERRAIN_HEIGHT_CONTRAST = 6.0;

// Each blend map weights three layers of the detail array in its RGB
#if defined(TERRAIN_BLENDMAPS_1)
    const int TERRAIN_BLENDMAPS = 1;
#elif defined(TERRAIN_BLENDMAPS_2)
    const int TERRAIN_BLENDMAPS = 2;
#else
    const int TERRAIN_BLENDMAPS = 3;
#endif
// and on a terrain of more than 9 layers its alpha, inverted, a tenth to a twelfth
#ifdef TERRAIN_EXTENDED_LAYERS
    #define TERRAIN_BLEND_TYPE vec4
    #define TERRAIN_BLEND_SWIZZLE
#else
    #define TERRAIN_BLEND_TYPE vec3
    #define TERRAIN_BLEND_SWIZZLE .rgb
#endif

// One-plane band at Medium, a narrow slope range where the flat projection and the side plane cross
const float TERRAIN_ONE_PLANE_SLOPE = 0.7071;
const float TERRAIN_ONE_PLANE_BAND = 0.02;

#ifdef TERRAIN_HEIGHT_BLEND
    #include "includes/terrain_height_blend.glsl"
#endif
#ifdef TERRAIN_KEEP
    #include "includes/terrain_select.glsl"
#endif

#ifdef HAS_TERRAIN_PBR
    // One entry per layer. Every map a layer owns is a slice of the same detail array,
    // so a layer with no map of a kind stores -1 there and falls back to its factors.
    uniform u_fs_terrainLayers {
        vec4 colorFactor[MAX_TERRAIN_LAYERS]; //rgb tint (linear), a = 1.0 on a PBR layer
        vec4 uvTransform[MAX_TERRAIN_LAYERS]; //xy scale of the detail tiling, zw offset
        vec4 surface[MAX_TERRAIN_LAYERS];     //roughness, metallic, normal strength, occlusion strength
        vec4 slices[MAX_TERRAIN_LAYERS];      //color, normal and surface slice; w hex rotation (< 0 off, TERRAIN_HEX_TILING)
        // Stochastic Tiling (includes/stochastic.glsl) as the user set it: cell size (0 when
        // off), blend sharpness, contrast, rotation. A uniform per draw, so the branch on it
        // is coherent and no variant is needed.
        vec4 stochastic[MAX_TERRAIN_LAYERS];
        // T3 R33: the features' parameters end the block, so a variant without them has the block it
        // had, and the upload takes the reflected size (RenderSystem::applyTerrainLayersUniform)
        #ifdef TERRAIN_MACRO
            vec4 macroParams; // x = 1 / scale, y = strength, z = slope fade, w = 1 for the base map's mode
            vec4 macroTintA;  // rgb; w = the terrain's size, which takes its UV to metres
            vec4 macroTintB;
        #endif
        #ifdef TERRAIN_RESAMPLE
            vec4 resampleParams; // near and far in detail tiles, x and y; z = the far tiling's rate
        #endif
        #ifdef TERRAIN_CLIFF
            vec4 flatOnly[3]; // per layer: 1 = Flat Only, never a side plane (R11)
        #endif
        #ifdef TERRAIN_HEIGHT_BLEND
            vec4 heightBlendParams; // x = sharpness
        #endif
        #ifdef TERRAIN_MACRO_NORMAL
            vec4 macroNormalParams; // x = strength: 0 until the bake is ready
        #endif
        #ifdef TERRAIN_DEBUG_VIEW
            vec4 debugParams; // x = the view: 1 layer count, 2 fetch cost, 3 holes
        #endif
    } terrainLayers;

    #if defined(TERRAIN_MACRO) && !defined(TERRAIN_QUALITY_LOW)
        #include "includes/terrain_macro.glsl"
    #endif

    #include "includes/stochastic.glsl"

    #ifdef HAS_NORMALS
        in vec3 v_terrainAxisX;
        in vec3 v_terrainAxisZ;
    #endif

    #if defined(TERRAIN_MACRO) && defined(TERRAIN_QUALITY_LOW)
        // T3 R9: Low's noise is the vertex stage's, one varying and no fragment cost
        in vec3 v_terrainMacroTint;
    #endif

    struct TerrainSurface{
        vec4 color;
        vec3 normal;    //in the space the pass shades in
        float roughness;
        float metallic;
        float occlusion;
    };
#endif

// The Debug View's readback (TERRAIN_DEBUG_VIEW, editor only): every read of a terrain map counts here
#ifdef TERRAIN_DEBUG_VIEW
    float terrainDebugFetches = 0.0;
    float terrainDebugLayers = 0.0;
    #define TERRAIN_COUNT_FETCHES(n) terrainDebugFetches += (n)
    #define TERRAIN_COUNT_LAYER() terrainDebugLayers += 1.0
#else
    #define TERRAIN_COUNT_FETCHES(n)
    #define TERRAIN_COUNT_LAYER()
#endif

vec2 terrainFarUV;
float terrainFarAmount;
// the far tiling's rate: the fixed break, or Distance Resampling's
#ifdef TERRAIN_RESAMPLE
    float terrainFarRate;
    #define TERRAIN_FAR_RATE terrainFarRate
#else
    #define TERRAIN_FAR_RATE TERRAIN_TILE_BREAK_RATE
#endif
// Only the painted layers are fetched, and a branch around a fetch has no derivatives of
// its own, so the gradients are taken here where every layer still shares them
vec2 terrainFlatDX;
vec2 terrainFlatDY;
#ifdef HAS_NORMALS
    vec2 terrainSideXUV;
    vec2 terrainSideZUV;
    vec2 terrainSideXDX;
    vec2 terrainSideXDY;
    vec2 terrainSideZDX;
    vec2 terrainSideZDY;
    vec2 terrainSideWeight;
    float terrainSideAmount;
    // the fragment's before a layer's Flat Only took it away (R11)
    #ifdef TERRAIN_CLIFF
        float terrainSideAmountAll;
    #endif
    // Which way each side plane faces, so a normal map can be mirrored back with its UV
    float terrainFaceX;
    float terrainFaceZ;
#endif

// A layer costs four fetches, so the projections are resolved once for all of them
void setupTerrainDetail(){
    #ifdef TERRAIN_RESAMPLE
        // T3 R10: replaces the fixed break: near only below resampleNear, a crossfade between, and far
        // only past resampleFar, where the near read is skipped
        terrainFarRate = terrainLayers.resampleParams.z;
        terrainFarUV = v_terrainTextureDetailTiled * terrainFarRate;
        terrainFarAmount = smoothstep(terrainLayers.resampleParams.x, terrainLayers.resampleParams.y, v_terrainEyeTiles);
    #else
        terrainFarUV = v_terrainTextureDetailTiled * TERRAIN_TILE_BREAK_RATE;
        terrainFarAmount = smoothstep(TERRAIN_TILE_BREAK_NEAR, TERRAIN_TILE_BREAK_FAR, v_terrainEyeTiles) * TERRAIN_TILE_BREAK_MIX;
    #endif
    terrainFlatDX = dFdx(v_terrainTextureDetailTiled);
    terrainFlatDY = dFdy(v_terrainTextureDetailTiled);

    #ifdef HAS_NORMALS
        vec3 normal = normalize(v_terrainNormal);
        #ifdef TERRAIN_TIER_READS
            // T3 R11: one plane, flat where abs(n.y) >= 0.7071, else the larger of x and z. It reuses
            // the side UVs, gradients and whiteout normals, so it adds no fetch: it removes them
            vec3 facing = abs(normal);
            terrainSideWeight = facing.x > facing.z ? vec2(1.0, 0.0) : vec2(0.0, 1.0);
            #ifdef TERRAIN_QUALITY_LOW
                terrainSideAmount = facing.y >= TERRAIN_ONE_PLANE_SLOPE ? 0.0 : 1.0;
            #else
                terrainSideAmount = 1.0 - smoothstep(TERRAIN_ONE_PLANE_SLOPE - TERRAIN_ONE_PLANE_BAND, TERRAIN_ONE_PLANE_SLOPE + TERRAIN_ONE_PLANE_BAND, facing.y);
            #endif
        #else
            vec3 weights = pow(abs(normal), vec3(TERRAIN_TRIPLANAR_SHARPNESS));
            weights /= max(weights.x + weights.y + weights.z, 0.0001);
            weights *= step(vec3(TERRAIN_TRIPLANAR_CUTOFF), weights);
            weights /= max(weights.x + weights.y + weights.z, 0.0001);
            // The flat projection takes weights.y, the side planes share the rest
            terrainSideAmount = weights.x + weights.z;
            terrainSideWeight = vec2(weights.x, weights.z) / max(terrainSideAmount, 0.0001);
        #endif
        #ifdef TERRAIN_CLIFF
            terrainSideAmountAll = terrainSideAmount;
        #endif

        // The side U axis follows the face direction, or opposite faces come out mirrored
        terrainFaceX = (normal.x < 0.0) ? -1.0 : 1.0;
        terrainFaceZ = (normal.z < 0.0) ? 1.0 : -1.0;
        terrainSideXUV = vec2(v_terrainTextureDetailTiled.y * terrainFaceX, v_terrainDetailHeight);
        terrainSideZUV = vec2(v_terrainTextureDetailTiled.x * terrainFaceZ, v_terrainDetailHeight);

        float heightDX = dFdx(v_terrainDetailHeight);
        float heightDY = dFdy(v_terrainDetailHeight);
        terrainSideXDX = vec2(terrainFlatDX.y * terrainFaceX, heightDX);
        terrainSideXDY = vec2(terrainFlatDY.y * terrainFaceX, heightDY);
        terrainSideZDX = vec2(terrainFlatDX.x * terrainFaceZ, heightDX);
        terrainSideZDY = vec2(terrainFlatDY.x * terrainFaceZ, heightDY);
    #endif
}

vec4 sampleTerrainSlice(float slice, vec2 uv, vec2 dx, vec2 dy){
    TERRAIN_COUNT_FETCHES(1.0);
    return textureGrad(sampler2DArray(u_terrainDetail, u_terrainDetail_smp), vec3(uv, slice), dx, dy);
}

// A flat projection stretches over a cliff, so steep ground moves to the side planes.
// The layer transform rides on every projection, so all of its maps stay in step.
vec4 getTerrainSlice(float slice, vec2 scale, vec2 offset){
    vec2 dx = terrainFlatDX * scale;
    vec2 dy = terrainFlatDY * scale;

    // A projection with no weight is not fetched (textureGrad needs no derivatives of its own)
    vec4 color = vec4(0.0);
    float flatAmount = 1.0;
    #ifdef HAS_NORMALS
        flatAmount = 1.0 - terrainSideAmount;
    #endif

    if (flatAmount > 0.0){
        // The far read is skipped where its share is 0, which is exact: mix(a, b, 0.0) is a (T3 R4). And
        // the near read where its share is 0, which only Distance Resampling reaches.
        #ifdef TERRAIN_RESAMPLE
            if (terrainFarAmount >= 1.0){
                color = sampleTerrainSlice(slice, terrainFarUV * scale + offset, dx * TERRAIN_FAR_RATE, dy * TERRAIN_FAR_RATE) * flatAmount;
            }else
        #endif
        if (terrainFarAmount > 0.0){
            color = mix(
                sampleTerrainSlice(slice, v_terrainTextureDetailTiled * scale + offset, dx, dy),
                sampleTerrainSlice(slice, terrainFarUV * scale + offset, dx * TERRAIN_FAR_RATE, dy * TERRAIN_FAR_RATE),
                terrainFarAmount) * flatAmount;
        }else{
            color = sampleTerrainSlice(slice, v_terrainTextureDetailTiled * scale + offset, dx, dy) * flatAmount;
        }
    }

    #ifdef HAS_NORMALS
        if (terrainSideAmount > 0.0){
            vec4 side = vec4(0.0);
            if (terrainSideWeight.x > 0.0){
                side += sampleTerrainSlice(slice, terrainSideXUV * scale + offset, terrainSideXDX * scale, terrainSideXDY * scale) * terrainSideWeight.x;
            }
            if (terrainSideWeight.y > 0.0){
                side += sampleTerrainSlice(slice, terrainSideZUV * scale + offset, terrainSideZDX * scale, terrainSideZDY * scale) * terrainSideWeight.y;
            }
            color += side * terrainSideAmount;
        }
    #endif

    return color;
}

#ifdef TERRAIN_TIER_READS
    // getTerrainSlice under a tier's reads per map (T3 R1): at Low one read; at Medium two at most, the
    // far tiling on colour only (every map with Distance Resampling), and it yields to a side plane in
    // the one-plane band.
    vec4 getTerrainSliceTier(float slice, vec2 scale, vec2 offset, bool colourMap){
        vec2 dx = terrainFlatDX * scale;
        vec2 dy = terrainFlatDY * scale;
        vec4 color = vec4(0.0);
        float flatAmount = 1.0;
        float sideAmount = 0.0;
        #ifdef HAS_NORMALS
            sideAmount = terrainSideAmount;
            flatAmount = 1.0 - sideAmount;
        #endif

        if (flatAmount > 0.0){
            #ifdef TERRAIN_QUALITY_LOW
                color = sampleTerrainSlice(slice, v_terrainTextureDetailTiled * scale + offset, dx, dy) * flatAmount;
            #else
                #ifdef TERRAIN_RESAMPLE
                    bool readFar = sideAmount <= 0.0 && terrainFarAmount > 0.0;
                #else
                    bool readFar = colourMap && sideAmount <= 0.0 && terrainFarAmount > 0.0;
                #endif
                #ifdef TERRAIN_RESAMPLE
                    if (readFar && terrainFarAmount >= 1.0){
                        color = sampleTerrainSlice(slice, terrainFarUV * scale + offset, dx * TERRAIN_FAR_RATE, dy * TERRAIN_FAR_RATE) * flatAmount;
                    }else
                #endif
                if (readFar){
                    color = mix(
                        sampleTerrainSlice(slice, v_terrainTextureDetailTiled * scale + offset, dx, dy),
                        sampleTerrainSlice(slice, terrainFarUV * scale + offset, dx * TERRAIN_FAR_RATE, dy * TERRAIN_FAR_RATE),
                        terrainFarAmount) * flatAmount;
                }else{
                    color = sampleTerrainSlice(slice, v_terrainTextureDetailTiled * scale + offset, dx, dy) * flatAmount;
                }
            #endif
        }

        #ifdef HAS_NORMALS
            if (sideAmount > 0.0){
                vec4 side = terrainSideWeight.x > 0.0
                    ? sampleTerrainSlice(slice, terrainSideXUV * scale + offset, terrainSideXDX * scale, terrainSideXDY * scale)
                    : sampleTerrainSlice(slice, terrainSideZUV * scale + offset, terrainSideZDX * scale, terrainSideZDY * scale);
                color += side * sideAmount;
            }
        #endif
        return color;
    }
#endif

vec4 getTerrainLayer(float layer){
    #ifdef TERRAIN_TIER_READS
        return getTerrainSliceTier(layer, vec2(1.0), vec2(0.0), true);
    #else
        return getTerrainSlice(layer, vec2(1.0), vec2(0.0));
    #endif
}

// The blend maps a fragment reads, in place: an array passed out of a function is one more held through
// the layer loop
#if defined(TERRAIN_BLENDMAPS_1)
    #define TERRAIN_READ_BLEND_MAPS(blend) \
        blend[0] = texture(sampler2D(u_blendMap, u_blendMap_smp), v_terrainTextureCoords) TERRAIN_BLEND_SWIZZLE
#elif defined(TERRAIN_BLENDMAPS_2)
    #define TERRAIN_READ_BLEND_MAPS(blend) \
        blend[0] = texture(sampler2D(u_blendMap, u_blendMap_smp), v_terrainTextureCoords) TERRAIN_BLEND_SWIZZLE; \
        blend[1] = texture(sampler2D(u_blendMap1, u_blendMap1_smp), v_terrainTextureCoords) TERRAIN_BLEND_SWIZZLE
#else
    #define TERRAIN_READ_BLEND_MAPS(blend) \
        blend[0] = texture(sampler2D(u_blendMap, u_blendMap_smp), v_terrainTextureCoords) TERRAIN_BLEND_SWIZZLE; \
        blend[1] = texture(sampler2D(u_blendMap1, u_blendMap1_smp), v_terrainTextureCoords) TERRAIN_BLEND_SWIZZLE; \
        blend[2] = texture(sampler2D(u_blendMap2, u_blendMap2_smp), v_terrainTextureCoords) TERRAIN_BLEND_SWIZZLE
#endif

// The layers in a blend map's channels: RGB's m * 3 + c, and with more than 9 layers the alpha's 9 + m,
// inverted so that an opaque map means weight 0 (terrain::terrainLayerChannel)
#ifdef TERRAIN_EXTENDED_LAYERS
    const int TERRAIN_BLEND_CHANNELS = 4;
    float terrainChannelWeight(TERRAIN_BLEND_TYPE map, int c){
        return c == 3 ? 1.0 - map.a : map[c];
    }
    int terrainChannelLayer(int m, int c){
        return c == 3 ? 9 + m : m * 3 + c;
    }
#else
    const int TERRAIN_BLEND_CHANNELS = 3;
    float terrainChannelWeight(TERRAIN_BLEND_TYPE map, int c){
        return map[c];
    }
    int terrainChannelLayer(int m, int c){
        return m * 3 + c;
    }
#endif

#ifdef TERRAIN_KEEP
    // the picks of this fragment (includes/terrain_select.glsl)
    void terrainSelectLayers(TERRAIN_BLEND_TYPE blend[TERRAIN_BLENDMAPS]){
        float kept[TERRAIN_KEEP + 1];
        int order[TERRAIN_KEEP + 1];
        for (int i = 0; i <= TERRAIN_KEEP; i++){
            kept[i] = 0.0;
            order[i] = 0;
        }
        terrainPickSum = 0.0;
        for (int m = 0; m < TERRAIN_BLENDMAPS; m++){
            for (int c = 0; c < TERRAIN_BLEND_CHANNELS; c++){
                terrainPickInsert(terrainChannelWeight(blend[m], c), terrainChannelLayer(m, c), kept, order);
            }
        }
        terrainPickFinish(kept, order);
    }
#endif

// Detail alpha carries layer height and biases the blend toward the taller layer, so it takes
// the contact zone. An opaque detail has no height and blends on its map weight alone.
vec4 getTerrainColor(vec4 color){
    setupTerrainDetail();

    TERRAIN_BLEND_TYPE blend[TERRAIN_BLENDMAPS];
    TERRAIN_READ_BLEND_MAPS(blend);
    TERRAIN_COUNT_FETCHES(float(TERRAIN_BLENDMAPS));

    // The base holds whatever weight the layers leave unclaimed, and stands as solid as an
    // opaque detail
    float weightBase = 1.0;
    vec4 sum = vec4(0.0);
    float total = 0.0;

    #ifdef TERRAIN_KEEP
        terrainSelectLayers(blend);
        weightBase -= terrainPickSum;
        for (int pick = 0; pick < TERRAIN_KEEP; pick++){
            float mapWeight = terrainPickWeight[pick];
            if (mapWeight > 0.0){
                TERRAIN_COUNT_LAYER();
                vec4 layer = getTerrainLayer(float(terrainPickLayer[pick]));
                float weight = mapWeight * pow(layer.a, TERRAIN_HEIGHT_CONTRAST);
                sum += layer * weight;
                total += weight;
            }
        }
    #else
    for (int m = 0; m < TERRAIN_BLENDMAPS; m++){
        for (int c = 0; c < TERRAIN_BLEND_CHANNELS; c++){
            float mapWeight = terrainChannelWeight(blend[m], c);
            weightBase -= mapWeight;
            if (mapWeight > 0.0){
                TERRAIN_COUNT_LAYER();
                vec4 layer = getTerrainLayer(float(terrainChannelLayer(m, c)));
                float weight = mapWeight * pow(layer.a, TERRAIN_HEIGHT_CONTRAST);
                sum += layer * weight;
                total += weight;
            }
        }
    }
    #endif

    weightBase = max(weightBase, 0.0);
    sum += color * weightBase;
    total += weightBase;

    vec4 result = sum / max(total, 0.0001);

    // Height belongs to the blend, not the surface: the material keeps its own alpha
    result.a = color.a;

    return result;
}

#ifdef HAS_TERRAIN_PBR

// Stochastic Tiling of a layer: one lattice per projection, set up once for all of its maps
// (the weights come from its color taps and drive its surface and normal maps as well).
// It breaks the repetition itself, so the coarser far tiling is not mixed in.
struct TerrainStochastic{
    StochasticTiling flatTiling;
    #ifdef HAS_NORMALS
        StochasticTiling sideX;
        StochasticTiling sideZ;
    #endif
};

TerrainStochastic setupTerrainStochastic(vec2 scale, vec2 offset, vec4 settings, uint seed){ // lod-safe: the derivatives are taken before the layer loop (terrainFlatDX/DY, terrainSide*DX/DY)
    vec4 params = stochasticParams(settings.x, settings.y, settings.z, settings.w);
    TerrainStochastic ts;
    ts.flatTiling = stochasticSetup(v_terrainTextureDetailTiled * scale + offset, terrainFlatDX * scale, terrainFlatDY * scale, params, seed);
    #ifdef HAS_NORMALS
        // textureGrad needs no derivatives of its own, so flat ground skips the side planes
        if (terrainSideAmount > 0.0){
            ts.sideX = stochasticSetup(terrainSideXUV * scale + offset, terrainSideXDX * scale, terrainSideXDY * scale, params, seed);
            ts.sideZ = stochasticSetup(terrainSideZUV * scale + offset, terrainSideZDX * scale, terrainSideZDY * scale, params, seed);
        }
    #endif
    return ts;
}

// getTerrainSlice of a stochastic layer. The color slice passes luminance true: its taps set
// the weights of the layer's other maps.
vec4 getTerrainSliceStochastic(float slice, inout TerrainStochastic ts, bool luminance){
    vec4 color = vec4(0.0);
    float flatAmount = 1.0;
    #ifdef HAS_NORMALS
        flatAmount = 1.0 - terrainSideAmount;
    #endif

    if (flatAmount > 0.0){
        TERRAIN_COUNT_FETCHES(3.0);
        color = luminance ? stochasticTextureArrayLuminance(u_terrainDetail, u_terrainDetail_smp, slice, ts.flatTiling)
                          : stochasticTextureArray(u_terrainDetail, u_terrainDetail_smp, slice, ts.flatTiling);
        color *= flatAmount;
    }

    #ifdef HAS_NORMALS
        if (terrainSideAmount > 0.0){
            vec4 side = vec4(0.0);
            if (terrainSideWeight.x > 0.0){
                TERRAIN_COUNT_FETCHES(3.0);
                side += (luminance ? stochasticTextureArrayLuminance(u_terrainDetail, u_terrainDetail_smp, slice, ts.sideX)
                                   : stochasticTextureArray(u_terrainDetail, u_terrainDetail_smp, slice, ts.sideX)) * terrainSideWeight.x;
            }
            if (terrainSideWeight.y > 0.0){
                TERRAIN_COUNT_FETCHES(3.0);
                side += (luminance ? stochasticTextureArrayLuminance(u_terrainDetail, u_terrainDetail_smp, slice, ts.sideZ)
                                   : stochasticTextureArray(u_terrainDetail, u_terrainDetail_smp, slice, ts.sideZ)) * terrainSideWeight.y;
            }
            color += side * terrainSideAmount;
        }
    #endif

    return color;
}

#ifdef HAS_NORMALS
    // The whiteout's base (T3 R14): the macro normal replaces the vertex normal in it and as the shading
    // base, not in the projections' weights, which stay with the geometry
    #ifdef TERRAIN_MACRO_NORMAL
        vec3 terrainMacroGeom;
        #define TERRAIN_GEOM terrainMacroGeom
    #else
        #define TERRAIN_GEOM normalize(v_terrainNormal)
    #endif

    // Terrain object space into whichever space the pass shades in
    vec3 terrainShadingNormal(vec3 objectNormal){
        vec3 axisY = cross(v_terrainAxisZ, v_terrainAxisX);
        return objectNormal.x * v_terrainAxisX + objectNormal.y * axisY + objectNormal.z * v_terrainAxisZ;
    }

    vec3 sampleTerrainTangentNormal(float slice, vec2 uv, vec2 dx, vec2 dy, float strength){
        vec3 tangentNormal = sampleTerrainSlice(slice, uv, dx, dy).xyz * 2.0 - 1.0;
        tangentNormal.xy *= strength;
        return tangentNormal;
    }

    // Whiteout blending: a tangent normal is added to the geometric normal on its own
    // projection plane, never mixed as raw RGB across planes that face different ways.
    vec3 getTerrainLayerNormal(float slice, vec2 scale, vec2 offset, float strength){
        vec3 geom = TERRAIN_GEOM;
        vec2 dx = terrainFlatDX * scale;
        vec2 dy = terrainFlatDY * scale;
        vec3 normal = vec3(0.0);

        if (terrainSideAmount < 1.0){
            vec3 flatNormal;
            #ifdef TERRAIN_RESAMPLE
                if (terrainFarAmount >= 1.0){
                    flatNormal = sampleTerrainTangentNormal(slice, terrainFarUV * scale + offset, dx * TERRAIN_FAR_RATE, dy * TERRAIN_FAR_RATE, strength);
                }else
            #endif
            if (terrainFarAmount > 0.0){
                flatNormal = mix(
                    sampleTerrainTangentNormal(slice, v_terrainTextureDetailTiled * scale + offset, dx, dy, strength),
                    sampleTerrainTangentNormal(slice, terrainFarUV * scale + offset, dx * TERRAIN_FAR_RATE, dy * TERRAIN_FAR_RATE, strength),
                    terrainFarAmount);
            }else{
                flatNormal = sampleTerrainTangentNormal(slice, v_terrainTextureDetailTiled * scale + offset, dx, dy, strength);
            }
            normal = vec3(flatNormal.xy + geom.xz, abs(flatNormal.z) * geom.y).xzy * (1.0 - terrainSideAmount);
        }

        if (terrainSideAmount > 0.0){
            vec3 sideNormal = vec3(0.0);
            // Undo the mirrored U of each side plane, or opposite slopes tilt the wrong way
            if (terrainSideWeight.x > 0.0){
                vec3 sideX = sampleTerrainTangentNormal(slice, terrainSideXUV * scale + offset, terrainSideXDX * scale, terrainSideXDY * scale, strength);
                sideX.x *= terrainFaceX;
                sideNormal += vec3(sideX.xy + geom.zy, abs(sideX.z) * geom.x).zyx * terrainSideWeight.x;
            }
            if (terrainSideWeight.y > 0.0){
                vec3 sideZ = sampleTerrainTangentNormal(slice, terrainSideZUV * scale + offset, terrainSideZDX * scale, terrainSideZDY * scale, strength);
                sideZ.x *= terrainFaceZ;
                sideNormal += vec3(sideZ.xy + geom.xy, abs(sideZ.z) * geom.z).xyz * terrainSideWeight.y;
            }
            normal += sideNormal * terrainSideAmount;
        }

        return normalize(normal);
    }

    #ifdef TERRAIN_TIER_READS
        // getTerrainLayerNormal under a tier's reads per map: one plane, and at Medium the far tiling
        // only with Distance Resampling and away from the band, as getTerrainSliceTier reads
        vec3 getTerrainLayerNormalTier(float slice, vec2 scale, vec2 offset, float strength){
            vec3 geom = TERRAIN_GEOM;
            vec2 dx = terrainFlatDX * scale;
            vec2 dy = terrainFlatDY * scale;
            vec3 normal = vec3(0.0);

            if (terrainSideAmount < 1.0){
                vec3 flatNormal;
                #if defined(TERRAIN_RESAMPLE) && !defined(TERRAIN_QUALITY_LOW)
                    if (terrainSideAmount <= 0.0 && terrainFarAmount >= 1.0){
                        flatNormal = sampleTerrainTangentNormal(slice, terrainFarUV * scale + offset, dx * TERRAIN_FAR_RATE, dy * TERRAIN_FAR_RATE, strength);
                    }else if (terrainSideAmount <= 0.0 && terrainFarAmount > 0.0){
                        flatNormal = mix(
                            sampleTerrainTangentNormal(slice, v_terrainTextureDetailTiled * scale + offset, dx, dy, strength),
                            sampleTerrainTangentNormal(slice, terrainFarUV * scale + offset, dx * TERRAIN_FAR_RATE, dy * TERRAIN_FAR_RATE, strength),
                            terrainFarAmount);
                    }else
                #endif
                {
                    flatNormal = sampleTerrainTangentNormal(slice, v_terrainTextureDetailTiled * scale + offset, dx, dy, strength);
                }
                normal = vec3(flatNormal.xy + geom.xz, abs(flatNormal.z) * geom.y).xzy * (1.0 - terrainSideAmount);
            }

            if (terrainSideAmount > 0.0){
                vec3 sideNormal;
                if (terrainSideWeight.x > 0.0){
                    vec3 sideX = sampleTerrainTangentNormal(slice, terrainSideXUV * scale + offset, terrainSideXDX * scale, terrainSideXDY * scale, strength);
                    sideX.x *= terrainFaceX;
                    sideNormal = vec3(sideX.xy + geom.zy, abs(sideX.z) * geom.x).zyx;
                }else{
                    vec3 sideZ = sampleTerrainTangentNormal(slice, terrainSideZUV * scale + offset, terrainSideZDX * scale, terrainSideZDY * scale, strength);
                    sideZ.x *= terrainFaceZ;
                    sideNormal = vec3(sideZ.xy + geom.xy, abs(sideZ.z) * geom.z).xyz;
                }
                normal += sideNormal * terrainSideAmount;
            }
            return normalize(normal);
        }
    #endif

    // getTerrainLayerNormal of a stochastic layer: each projection's taps are blended as
    // derivatives with the weights its color taps set, then whiteout-blended as above
    vec3 getTerrainLayerNormalStochastic(float slice, TerrainStochastic ts, float strength){
        vec3 geom = TERRAIN_GEOM;
        vec3 normal = vec3(0.0);

        if (terrainSideAmount < 1.0){
            vec3 flatNormal = stochasticNormalArray(u_terrainDetail, u_terrainDetail_smp, slice, ts.flatTiling, strength, STOCHASTIC_NORMAL_DX);
            normal = vec3(flatNormal.xy + geom.xz, abs(flatNormal.z) * geom.y).xzy * (1.0 - terrainSideAmount);
        }

        if (terrainSideAmount > 0.0){
            vec3 sideNormal = vec3(0.0);
            if (terrainSideWeight.x > 0.0){
                vec3 sideX = stochasticNormalArray(u_terrainDetail, u_terrainDetail_smp, slice, ts.sideX, strength, STOCHASTIC_NORMAL_DX);
                sideX.x *= terrainFaceX;
                sideNormal += vec3(sideX.xy + geom.zy, abs(sideX.z) * geom.x).zyx * terrainSideWeight.x;
            }
            if (terrainSideWeight.y > 0.0){
                vec3 sideZ = stochasticNormalArray(u_terrainDetail, u_terrainDetail_smp, slice, ts.sideZ, strength, STOCHASTIC_NORMAL_DX);
                sideZ.x *= terrainFaceZ;
                sideNormal += vec3(sideZ.xy + geom.xy, abs(sideZ.z) * geom.z).xyz * terrainSideWeight.y;
            }
            normal += sideNormal * terrainSideAmount;
        }

        return normalize(normal);
    }
#endif

#ifdef TERRAIN_HEX_TILING
    // Hex tiling of a layer (TerrainSurfaceLayer::hexTiling): its flat projection sampled on
    // three rotated copies (includes/hex_tiling.glsl) instead of its near and far tiling; the
    // side planes stay as they are. Every tap is a textureGrad with the flat gradients turned by
    // its tile, so the branch on the blend map around it needs no derivatives.
    #include "includes/hex_tiling.glsl"

    // how hard the copies blend by luminance (the Lit material's default contrast)
    const float TERRAIN_HEX_CONTRAST = 0.6;

    // The taps of the first hex layer's tiling (its scale, offset and rotation), set up once a
    // fragment before the layers: a layer that tiles alike, and each of its maps, takes them
    // instead of the lattice, the hashes and the rotations of the same inputs (the same values:
    // hexSetup is a function of them alone). Taps made where each map needs them cost a set-up
    // per layer and map: 2.68 -> 1.90 ms on GL for the 4-layer fixture (GPU campaign perf3);
    // a cache filled inside the layer loop instead held its state through the loop's branches
    // and was slower than none.
    HexTaps terrainHexShared;
    // and its barycentrics' powers (hexWeightPowers), which every hex colour weighs its taps by
    vec3 terrainHexSharedPowers;
    vec4 terrainHexSharedTiling;
    float terrainHexSharedRotation;

    void setupTerrainHexShared(){
        // a hex rotation is >= 0, so without a hex layer nothing matches
        terrainHexSharedRotation = -1.0;
        int first = -1;
        for (int i = 0; i < TERRAIN_BLENDMAPS * 3; i++){
            bool hex = terrainLayers.slices[i].w >= 0.0;
            #ifdef TERRAIN_HEX_STOCHASTIC
                // a layer with both takes Stochastic Tiling (getTerrainSurface)
                hex = hex && !(terrainLayers.stochastic[i].x > 0.0);
            #endif
            if (first < 0 && hex){
                first = i;
            }
        }
        // uniform per draw, so the branch is coherent
        if (first >= 0){
            vec2 scale = terrainLayers.uvTransform[first].xy;
            vec2 offset = terrainLayers.uvTransform[first].zw;
            terrainHexSharedTiling = vec4(scale, offset);
            terrainHexSharedRotation = terrainLayers.slices[first].w;
            terrainHexShared = hexSetup(v_terrainTextureDetailTiled * scale + offset, terrainFlatDX * scale, terrainFlatDY * scale,
                                        terrainHexSharedRotation);
            terrainHexSharedPowers = hexWeightPowers(terrainHexShared.barycentrics);
        }
    }

    // whether a layer tiles as the shared taps' (else it sets up its own, for each of its maps)
    bool terrainHexIsShared(vec2 scale, vec2 offset, float rotation){
        return rotation == terrainHexSharedRotation && all(equal(vec4(scale, offset), terrainHexSharedTiling));
    }

    HexTaps terrainHexTaps(vec2 scale, vec2 offset, float rotation){
        return hexSetup(v_terrainTextureDetailTiled * scale + offset, terrainFlatDX * scale, terrainFlatDY * scale, rotation);
    }

    // One tap of a layer (scale: its tiling's). Its gradients are the layer's turned by the tap's
    // tile, made here as hexSetup makes taps.dx/dy (the same values) rather than read from the
    // taps: twelve floats fewer held through the layer loop took the hex variant from 164 to 128
    // registers on NVIDIA (GPU campaign perf3)
    vec4 terrainHexTap(float slice, HexTaps taps, int k, vec2 scale){
        vec2 dx = terrainFlatDX * scale;
        vec2 dy = terrainFlatDY * scale;
        if (k == 0) return sampleTerrainSlice(slice, taps.uv0, hexRotate(dx, taps.cs0), hexRotate(dy, taps.cs0));
        if (k == 1) return sampleTerrainSlice(slice, taps.uv1, hexRotate(dx, taps.cs1), hexRotate(dy, taps.cs1));
        return sampleTerrainSlice(slice, taps.uv2, hexRotate(dx, taps.cs2), hexRotate(dy, taps.cs2));
    }

    // the flat projection blended by weights, under the side planes as getTerrainSlice does
    vec4 getTerrainSliceHex(float slice, vec2 scale, vec2 offset, HexTaps taps, vec3 weights){
        vec4 color = terrainHexTap(slice, taps, 0, scale) * weights.x + terrainHexTap(slice, taps, 1, scale) * weights.y +
                     terrainHexTap(slice, taps, 2, scale) * weights.z;

        #ifdef HAS_NORMALS
            // a side plane with no weight is not fetched, as getTerrainSlice does: its tap times 0
            // and mix(color, side, 0) are exact, so the output is the same (GPU campaign perf2:
            // the hex terrain fetched both sides of every map everywhere)
            if (terrainSideAmount > 0.0){
                vec4 side = vec4(0.0);
                if (terrainSideWeight.x > 0.0)
                    side += sampleTerrainSlice(slice, terrainSideXUV * scale + offset, terrainSideXDX * scale, terrainSideXDY * scale) * terrainSideWeight.x;
                if (terrainSideWeight.y > 0.0)
                    side += sampleTerrainSlice(slice, terrainSideZUV * scale + offset, terrainSideZDX * scale, terrainSideZDY * scale) * terrainSideWeight.y;
                color = mix(color, side, terrainSideAmount);
            }
        #endif

        return color;
    }

    // the layer's colour, whose taps' luminance sets the weights its other maps take
    vec4 getTerrainColorHex(float slice, vec2 scale, vec2 offset, HexTaps taps, vec3 powers, out vec3 weights){
        vec4 c0 = terrainHexTap(slice, taps, 0, scale);
        vec4 c1 = terrainHexTap(slice, taps, 1, scale);
        vec4 c2 = terrainHexTap(slice, taps, 2, scale);
        // hexWeights, with the powers of the taps' barycentrics (hexWeightPowers)
        weights = hexWeightsOfPowers(taps.barycentrics, powers,
                                     vec3(hexLuminance(c0.rgb), hexLuminance(c1.rgb), hexLuminance(c2.rgb)), TERRAIN_HEX_CONTRAST);
        vec4 color = c0 * weights.x + c1 * weights.y + c2 * weights.z;

        #ifdef HAS_NORMALS
            // a side plane with no weight is not fetched, as getTerrainSlice does: its tap times 0
            // and mix(color, side, 0) are exact, so the output is the same (GPU campaign perf2:
            // the hex terrain fetched both sides of every map everywhere)
            if (terrainSideAmount > 0.0){
                vec4 side = vec4(0.0);
                if (terrainSideWeight.x > 0.0)
                    side += sampleTerrainSlice(slice, terrainSideXUV * scale + offset, terrainSideXDX * scale, terrainSideXDY * scale) * terrainSideWeight.x;
                if (terrainSideWeight.y > 0.0)
                    side += sampleTerrainSlice(slice, terrainSideZUV * scale + offset, terrainSideZDX * scale, terrainSideZDY * scale) * terrainSideWeight.y;
                color = mix(color, side, terrainSideAmount);
            }
        #endif

        return color;
    }

    #ifdef HAS_NORMALS
        // getTerrainLayerNormal with the flat normal from the hex taps, each turned back by its tile
        vec3 getTerrainLayerNormalHex(float slice, vec2 scale, vec2 offset, float strength, HexTaps taps, vec3 weights){
            vec3 geom = TERRAIN_GEOM;

            vec3 n0 = hexNormalBack(terrainHexTap(slice, taps, 0, scale).xyz * 2.0 - 1.0, taps.cs0);
            vec3 n1 = hexNormalBack(terrainHexTap(slice, taps, 1, scale).xyz * 2.0 - 1.0, taps.cs1);
            vec3 n2 = hexNormalBack(terrainHexTap(slice, taps, 2, scale).xyz * 2.0 - 1.0, taps.cs2);
            vec3 flatNormal = n0 * weights.x + n1 * weights.y + n2 * weights.z;
            flatNormal.xy *= strength;
            vec3 normal = vec3(flatNormal.xy + geom.xz, abs(flatNormal.z) * geom.y).xzy;

            // (the side planes only where they weigh, as above: the same normal)
            if (terrainSideAmount > 0.0){
                vec3 sideNormal = vec3(0.0);
                if (terrainSideWeight.x > 0.0){
                    vec3 sideX = sampleTerrainTangentNormal(slice, terrainSideXUV * scale + offset, terrainSideXDX * scale, terrainSideXDY * scale, strength);
                    sideX.x *= terrainFaceX;
                    sideNormal += vec3(sideX.xy + geom.zy, abs(sideX.z) * geom.x).zyx * terrainSideWeight.x;
                }
                if (terrainSideWeight.y > 0.0){
                    vec3 sideZ = sampleTerrainTangentNormal(slice, terrainSideZUV * scale + offset, terrainSideZDX * scale, terrainSideZDY * scale, strength);
                    sideZ.x *= terrainFaceZ;
                    sideNormal += vec3(sideZ.xy + geom.xy, abs(sideZ.z) * geom.z).xyz * terrainSideWeight.y;
                }
                normal = mix(normal, sideNormal, terrainSideAmount);
            }
            return normalize(normal);
        }
    #endif
#endif

// Everything the painted layers cover, evaluated once and shared by the color pass and
// the G-buffer. Whatever weight the layers leave unclaimed keeps the values handed in.
TerrainSurface getTerrainSurface(vec4 baseColor, vec3 baseNormal, float baseRoughness, float baseMetallic){
    setupTerrainDetail();

    TERRAIN_BLEND_TYPE blend[TERRAIN_BLENDMAPS];
    TERRAIN_READ_BLEND_MAPS(blend);
    TERRAIN_COUNT_FETCHES(float(TERRAIN_BLENDMAPS));

    #if defined(TERRAIN_MACRO_NORMAL) && defined(HAS_NORMALS)
        // T3 R14: 1 read of the slot the base material's normal map had. Strength 0 until the bake is ready
        vec2 macroXZ = texture(sampler2D(u_normalTexture, u_normal_smp), v_terrainTextureCoords).rg * 2.0 - 1.0;
        TERRAIN_COUNT_FETCHES(1.0);
        vec3 macroNormal = vec3(macroXZ.x, sqrt(max(1.0 - dot(macroXZ, macroXZ), 0.0)), macroXZ.y);
        terrainMacroGeom = normalize(mix(normalize(v_terrainNormal), macroNormal, terrainLayers.macroNormalParams.x));
        baseNormal = terrainShadingNormal(terrainMacroGeom);
    #endif

    #ifdef TERRAIN_HEX_TILING
        setupTerrainHexShared();
    #endif
    float weightBase = 1.0;
    vec4 sumColor = vec4(0.0);
    vec3 sumNormal = vec3(0.0);
    float sumRoughness = 0.0;
    float sumMetallic = 0.0;
    float sumOcclusion = 0.0;
    float total = 0.0;

    // T3 R9: the two values or the base map tint the layers, the base weight staying as it is
    #ifdef TERRAIN_KEEP
        terrainSelectLayers(blend);
        weightBase -= terrainPickSum;
        for (int pick = 0; pick < TERRAIN_KEEP; pick++){
            int index = terrainPickLayer[pick];
            float mapWeight = terrainPickWeight[pick];
    #else
    for (int m = 0; m < TERRAIN_BLENDMAPS; m++){
        for (int c = 0; c < TERRAIN_BLEND_CHANNELS; c++){
            int index = terrainChannelLayer(m, c);
            #ifdef TERRAIN_HEX_TILING
                // picked, not indexed: an array indexed by the loop's counters lives in local memory
                // where the loop is not unrolled (NVIDIA GL), and the hex variant measured faster so
                // (GPU campaign perf3; the same weights)
                TERRAIN_BLEND_TYPE mapWeights = m == 0 ? blend[0] : (m == 1 ? blend[1] : blend[2]);
                #ifdef TERRAIN_EXTENDED_LAYERS
                    float mapWeight = c == 0 ? mapWeights.x : (c == 1 ? mapWeights.y : (c == 2 ? mapWeights.z : 1.0 - mapWeights.w));
                #else
                    float mapWeight = c == 0 ? mapWeights.x : (c == 1 ? mapWeights.y : mapWeights.z);
                #endif
            #else
                float mapWeight = terrainChannelWeight(blend[m], c);
            #endif
            weightBase -= mapWeight;
    #endif
            if (mapWeight <= 0.0){
                continue;
            }
            TERRAIN_COUNT_LAYER();

            vec2 scale = terrainLayers.uvTransform[index].xy;
            vec2 offset = terrainLayers.uvTransform[index].zw;

            #ifdef TERRAIN_CLIFF
                // T3 R11: this layer's Flat Only never reads a side plane
                #ifdef HAS_NORMALS
                    terrainSideAmount = terrainLayers.flatOnly[index / 4][index % 4] > 0.5 ? 0.0 : terrainSideAmountAll;
                #endif
            #endif

            #ifdef TERRAIN_TIER_READS
                // Medium and Low: drawn plain, read by the tier's rule, and the same blend as below
                vec4 rawColor = getTerrainSliceTier(terrainLayers.slices[index].x, scale, offset, true);
                vec4 color = rawColor;
                float height = color.a;
                float roughness = baseRoughness;
                float metallic = baseMetallic;
                float occlusion = 1.0;
                vec3 normal = baseNormal;

                if (terrainLayers.colorFactor[index].a > 0.5){
                    color = vec4(sRGBToLinear(rawColor.rgb) * terrainLayers.colorFactor[index].rgb, baseColor.a);
                    roughness = terrainLayers.surface[index].x;
                    metallic = terrainLayers.surface[index].y;
                    #ifdef TERRAIN_QUALITY_LOW
                        // T3 R6: a PBR layer's colour slice carries its height in alpha: no surface read
                        height = rawColor.a;
                    #else
                        height = 1.0;
                        float surfaceSlice = terrainLayers.slices[index].z;
                        if (surfaceSlice >= 0.0){
                            vec4 orm = getTerrainSliceTier(surfaceSlice, scale, offset, false);
                            occlusion = mix(1.0, orm.r, terrainLayers.surface[index].w);
                            roughness *= orm.g;
                            metallic *= orm.b;
                            height = orm.a;
                        }
                    #endif

                    #ifdef HAS_NORMALS
                        float normalSlice = terrainLayers.slices[index].y;
                        #ifdef TERRAIN_QUALITY_LOW
                            // T3 R5: the strongest layer's normal, fading to none where the top two swap
                            if (pick == 0 && normalSlice >= 0.0){
                                float strongest = terrainPickWeight[0];
                                float strength = terrainLayers.surface[index].z * (strongest > 0.0 ? (strongest - terrainPickWeight[1]) / strongest : 0.0);
                                normal = terrainShadingNormal(getTerrainLayerNormalTier(normalSlice, scale, offset, strength));
                            }
                        #else
                            if (normalSlice >= 0.0){
                                normal = terrainShadingNormal(getTerrainLayerNormalTier(normalSlice, scale, offset, terrainLayers.surface[index].z));
                            }
                        #endif
                    #endif
                }
            #else

            #if defined(TERRAIN_HEX_TILING) && !defined(TERRAIN_HEX_STOCHASTIC)
                // no layer takes Stochastic Tiling (TERRAIN_HEX_STOCHASTIC otherwise), so the
                // hex variant leaves its path out: its lattices, kept for a layer's three maps,
                // took the variant to 255 registers and spills, half the occupancy of the
                // plain terrain's 128 (NVIDIA, GPU campaign perf3)
                const bool stochastic = false;
            #else
                bool stochastic = terrainLayers.stochastic[index].x > 0.0;
            #endif
            TerrainStochastic ts;
            vec4 color;
            // (S11's TERRAIN_HEX_TILING beside it: a layer with both takes Stochastic Tiling)
            #ifdef TERRAIN_HEX_TILING
                // a hex layer's taps and weights, shared by its maps; its colour is the hex
                // taps', so the flat projection it would overwrite is not sampled (GPU campaign
                // perf2: -2 %)
                bool hexLayer = !stochastic && terrainLayers.slices[index].w >= 0.0;
                vec3 hexWeights = vec3(1.0, 0.0, 0.0);
                // (each map takes the shared taps or sets up its own on its own path: a value
                // chosen between them would be a copy of the taps held through the layer)
                bool hexShared = hexLayer && terrainHexIsShared(scale, offset, terrainLayers.slices[index].w);
                if (hexShared){
                    color = getTerrainColorHex(terrainLayers.slices[index].x, scale, offset, terrainHexShared, terrainHexSharedPowers, hexWeights);
                }else if (hexLayer){
                    HexTaps hexTaps = terrainHexTaps(scale, offset, terrainLayers.slices[index].w);
                    color = getTerrainColorHex(terrainLayers.slices[index].x, scale, offset, hexTaps, hexWeightPowers(hexTaps.barycentrics), hexWeights);
                }else
            #endif
            if (stochastic){
                ts = setupTerrainStochastic(scale, offset, terrainLayers.stochastic[index], uint(index));
                color = getTerrainSliceStochastic(terrainLayers.slices[index].x, ts, true);
            }else{
                color = getTerrainSlice(terrainLayers.slices[index].x, scale, offset);
            }
            float height = color.a;
            float roughness = baseRoughness;
            float metallic = baseMetallic;
            float occlusion = 1.0;
            vec3 normal = baseNormal;

            if (terrainLayers.colorFactor[index].a > 0.5){
                // sRGB and tinted, with its alpha out of the blend. A color layer beside it
                // stays raw, as on the cheap path, so old layers keep their look.
                color = vec4(sRGBToLinear(color.rgb) * terrainLayers.colorFactor[index].rgb, baseColor.a);
                height = 1.0;
                roughness = terrainLayers.surface[index].x;
                metallic = terrainLayers.surface[index].y;

                float surfaceSlice = terrainLayers.slices[index].z;
                if (surfaceSlice >= 0.0){
                    #ifdef TERRAIN_HEX_TILING
                        // a hex layer's surface is its hex taps', so the flat one is not sampled
                        vec4 orm;
                        if (hexShared){
                            orm = getTerrainSliceHex(surfaceSlice, scale, offset, terrainHexShared, hexWeights);
                        }else if (hexLayer){
                            orm = getTerrainSliceHex(surfaceSlice, scale, offset, terrainHexTaps(scale, offset, terrainLayers.slices[index].w), hexWeights);
                        }else{
                            orm = stochastic ? getTerrainSliceStochastic(surfaceSlice, ts, false)
                                             : getTerrainSlice(surfaceSlice, scale, offset);
                        }
                    #else
                    vec4 orm = stochastic ? getTerrainSliceStochastic(surfaceSlice, ts, false)
                                          : getTerrainSlice(surfaceSlice, scale, offset);
                    #endif
                    occlusion = mix(1.0, orm.r, terrainLayers.surface[index].w);
                    roughness *= orm.g;
                    metallic *= orm.b;
                    height = orm.a;
                }

                #ifdef HAS_NORMALS
                    float normalSlice = terrainLayers.slices[index].y;
                    if (normalSlice >= 0.0){
                        float strength = terrainLayers.surface[index].z;
                        #ifdef TERRAIN_HEX_TILING
                            // and its normal (the flat projection's is not sampled either)
                            if (hexShared){
                                normal = terrainShadingNormal(getTerrainLayerNormalHex(normalSlice, scale, offset, strength, terrainHexShared, hexWeights));
                            }else if (hexLayer){
                                normal = terrainShadingNormal(getTerrainLayerNormalHex(normalSlice, scale, offset, strength,
                                                                                        terrainHexTaps(scale, offset, terrainLayers.slices[index].w), hexWeights));
                            }else{
                                normal = terrainShadingNormal(stochastic ? getTerrainLayerNormalStochastic(normalSlice, ts, strength)
                                                                         : getTerrainLayerNormal(normalSlice, scale, offset, strength));
                            }
                        #else
                        normal = terrainShadingNormal(stochastic ? getTerrainLayerNormalStochastic(normalSlice, ts, strength)
                                                                 : getTerrainLayerNormal(normalSlice, scale, offset, strength));
                        #endif
                    }
                #endif
            }
            #endif

            #ifdef TERRAIN_HEIGHT_BLEND
                float weight = terrainHeightBlend(mapWeight, height, terrainLayers.heightBlendParams.x);
            #else
                float weight = mapWeight * pow(height, TERRAIN_HEIGHT_CONTRAST);
            #endif
            sumColor += color * weight;
            sumNormal += normal * weight;
            sumRoughness += roughness * weight;
            sumMetallic += metallic * weight;
            sumOcclusion += occlusion * weight;
            total += weight;
    #ifdef TERRAIN_KEEP
        }
    #else
        }
    }
    #endif

    #ifdef TERRAIN_MACRO
        // Base Map (T3 R9): the base material's colour, already read, multiplies the layers with a neutral
        // at 0.5 grey. No new slot and no read.
        if (terrainLayers.macroParams.w > 0.5){
            sumColor.rgb *= mix(vec3(1.0), baseColor.rgb * 2.0, terrainLayers.macroParams.y);
        }else{
            #ifdef TERRAIN_QUALITY_LOW
                sumColor.rgb *= v_terrainMacroTint;
            #else
                vec2 macroLocalXZ = v_terrainTextureCoords * float(terrainLayers.macroTintA.w);
                #ifdef HAS_NORMALS
                    float macroUp = abs(normalize(v_terrainNormal).y);
                #else
                    const float macroUp = 1.0;
                #endif
                sumColor.rgb *= terrainMacroTint(macroLocalXZ, macroUp, terrainLayers.macroParams, terrainLayers.macroTintA.rgb, terrainLayers.macroTintB.rgb);
            #endif
        }
    #endif

    weightBase = max(weightBase, 0.0);
    sumColor += baseColor * weightBase;
    sumNormal += baseNormal * weightBase;
    sumRoughness += baseRoughness * weightBase;
    sumMetallic += baseMetallic * weightBase;
    sumOcclusion += weightBase;
    total += weightBase;

    TerrainSurface surface;
    // Painted over by layers with no height where they meet: the base stands, not black
    if (total <= 0.0){
        surface.color = baseColor;
        surface.normal = baseNormal;
        surface.roughness = baseRoughness;
        surface.metallic = baseMetallic;
        surface.occlusion = 1.0;
        return surface;
    }

    surface.color = sumColor / total;
    // Height belongs to the blend, not the surface: the material keeps its own alpha
    surface.color.a = baseColor.a;
    surface.roughness = clamp(sumRoughness / total, 0.0, 1.0);
    surface.metallic = clamp(sumMetallic / total, 0.0, 1.0);
    surface.occlusion = clamp(sumOcclusion / total, 0.0, 1.0);

    float normalLength = length(sumNormal);
    surface.normal = (normalLength > 0.0001) ? sumNormal / normalLength : baseNormal;

    return surface;
}

#ifdef TERRAIN_MATERIAL_INFO
    // The painted surface replaces what the material contributed, reflectance included
    MaterialInfo applyTerrainSurface(MaterialInfo info, TerrainSurface surface, float f0_ior){
        info.perceptualRoughness = surface.roughness;
        info.metallic = surface.metallic;

        vec3 f0 = vec3(f0_ior);
        info.albedoColor = mix(info.baseColor.rgb * (vec3(1.0) - f0), vec3(0.0), info.metallic);
        info.f0 = mix(f0, info.baseColor.rgb, info.metallic);

        return info;
    }
#endif

#endif
