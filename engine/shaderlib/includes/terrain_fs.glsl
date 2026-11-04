uniform texture2D u_blendMap;
uniform texture2D u_blendMap1;
uniform texture2D u_blendMap2;
uniform texture2DArray u_terrainDetail;
uniform sampler u_blendMap_smp;
uniform sampler u_blendMap1_smp;
uniform sampler u_blendMap2_smp;
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
const int TERRAIN_BLENDMAPS = 3;

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
    } terrainLayers;

    #include "includes/stochastic.glsl"

    #ifdef HAS_NORMALS
        in vec3 v_terrainAxisX;
        in vec3 v_terrainAxisZ;
    #endif

    struct TerrainSurface{
        vec4 color;
        vec3 normal;    //in the space the pass shades in
        float roughness;
        float metallic;
        float occlusion;
    };
#endif

vec2 terrainFarUV;
float terrainFarAmount;
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
    // Which way each side plane faces, so a normal map can be mirrored back with its UV
    float terrainFaceX;
    float terrainFaceZ;
#endif

// A layer costs four fetches, so the projections are resolved once for all of them
void setupTerrainDetail(){
    terrainFarUV = v_terrainTextureDetailTiled * TERRAIN_TILE_BREAK_RATE;
    terrainFarAmount = smoothstep(TERRAIN_TILE_BREAK_NEAR, TERRAIN_TILE_BREAK_FAR, v_terrainEyeTiles) * TERRAIN_TILE_BREAK_MIX;
    terrainFlatDX = dFdx(v_terrainTextureDetailTiled);
    terrainFlatDY = dFdy(v_terrainTextureDetailTiled);

    #ifdef HAS_NORMALS
        vec3 normal = normalize(v_terrainNormal);
        vec3 weights = pow(abs(normal), vec3(TERRAIN_TRIPLANAR_SHARPNESS));
        weights /= max(weights.x + weights.y + weights.z, 0.0001);
        weights *= step(vec3(TERRAIN_TRIPLANAR_CUTOFF), weights);
        weights /= max(weights.x + weights.y + weights.z, 0.0001);
        // The flat projection takes weights.y, the side planes share the rest
        terrainSideAmount = weights.x + weights.z;
        terrainSideWeight = vec2(weights.x, weights.z) / max(terrainSideAmount, 0.0001);

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
        color = mix(
            sampleTerrainSlice(slice, v_terrainTextureDetailTiled * scale + offset, dx, dy),
            sampleTerrainSlice(slice, terrainFarUV * scale + offset, dx * TERRAIN_TILE_BREAK_RATE, dy * TERRAIN_TILE_BREAK_RATE),
            terrainFarAmount) * flatAmount;
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

vec4 getTerrainLayer(float layer){
    return getTerrainSlice(layer, vec2(1.0), vec2(0.0));
}

// Detail alpha carries layer height and biases the blend toward the taller layer, so it takes
// the contact zone. An opaque detail has no height and blends on its map weight alone.
vec4 getTerrainColor(vec4 color){
    setupTerrainDetail();

    vec3 blend[TERRAIN_BLENDMAPS];
    blend[0] = texture(sampler2D(u_blendMap, u_blendMap_smp), v_terrainTextureCoords).rgb;
    blend[1] = texture(sampler2D(u_blendMap1, u_blendMap1_smp), v_terrainTextureCoords).rgb;
    blend[2] = texture(sampler2D(u_blendMap2, u_blendMap2_smp), v_terrainTextureCoords).rgb;

    // The base holds whatever weight the layers leave unclaimed, and stands as solid as an
    // opaque detail
    float weightBase = 1.0;
    vec4 sum = vec4(0.0);
    float total = 0.0;

    for (int m = 0; m < TERRAIN_BLENDMAPS; m++){
        for (int c = 0; c < 3; c++){
            float mapWeight = blend[m][c];
            weightBase -= mapWeight;
            if (mapWeight > 0.0){
                vec4 layer = getTerrainLayer(float(m * 3 + c));
                float weight = mapWeight * pow(layer.a, TERRAIN_HEIGHT_CONTRAST);
                sum += layer * weight;
                total += weight;
            }
        }
    }

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
        color = luminance ? stochasticTextureArrayLuminance(u_terrainDetail, u_terrainDetail_smp, slice, ts.flatTiling)
                          : stochasticTextureArray(u_terrainDetail, u_terrainDetail_smp, slice, ts.flatTiling);
        color *= flatAmount;
    }

    #ifdef HAS_NORMALS
        if (terrainSideAmount > 0.0){
            vec4 side = vec4(0.0);
            if (terrainSideWeight.x > 0.0){
                side += (luminance ? stochasticTextureArrayLuminance(u_terrainDetail, u_terrainDetail_smp, slice, ts.sideX)
                                   : stochasticTextureArray(u_terrainDetail, u_terrainDetail_smp, slice, ts.sideX)) * terrainSideWeight.x;
            }
            if (terrainSideWeight.y > 0.0){
                side += (luminance ? stochasticTextureArrayLuminance(u_terrainDetail, u_terrainDetail_smp, slice, ts.sideZ)
                                   : stochasticTextureArray(u_terrainDetail, u_terrainDetail_smp, slice, ts.sideZ)) * terrainSideWeight.y;
            }
            color += side * terrainSideAmount;
        }
    #endif

    return color;
}

#ifdef HAS_NORMALS
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
        vec3 geom = normalize(v_terrainNormal);
        vec2 dx = terrainFlatDX * scale;
        vec2 dy = terrainFlatDY * scale;
        vec3 normal = vec3(0.0);

        if (terrainSideAmount < 1.0){
            vec3 flatNormal = mix(
                sampleTerrainTangentNormal(slice, v_terrainTextureDetailTiled * scale + offset, dx, dy, strength),
                sampleTerrainTangentNormal(slice, terrainFarUV * scale + offset, dx * TERRAIN_TILE_BREAK_RATE, dy * TERRAIN_TILE_BREAK_RATE, strength),
                terrainFarAmount);
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

    // getTerrainLayerNormal of a stochastic layer: each projection's taps are blended as
    // derivatives with the weights its color taps set, then whiteout-blended as above
    vec3 getTerrainLayerNormalStochastic(float slice, TerrainStochastic ts, float strength){
        vec3 geom = normalize(v_terrainNormal);
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
            vec3 geom = normalize(v_terrainNormal);

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

    vec3 blend[TERRAIN_BLENDMAPS];
    blend[0] = texture(sampler2D(u_blendMap, u_blendMap_smp), v_terrainTextureCoords).rgb;
    blend[1] = texture(sampler2D(u_blendMap1, u_blendMap1_smp), v_terrainTextureCoords).rgb;
    blend[2] = texture(sampler2D(u_blendMap2, u_blendMap2_smp), v_terrainTextureCoords).rgb;

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

    for (int m = 0; m < TERRAIN_BLENDMAPS; m++){
        for (int c = 0; c < 3; c++){
            int index = m * 3 + c;
            #ifdef TERRAIN_HEX_TILING
                // picked, not indexed: an array indexed by the loop's counters lives in local memory
                // where the loop is not unrolled (NVIDIA GL), and the hex variant measured faster so
                // (GPU campaign perf3; the same weights)
                vec3 mapWeights = m == 0 ? blend[0] : (m == 1 ? blend[1] : blend[2]);
                float mapWeight = c == 0 ? mapWeights.x : (c == 1 ? mapWeights.y : mapWeights.z);
            #else
                float mapWeight = blend[m][c];
            #endif
            weightBase -= mapWeight;
            if (mapWeight <= 0.0){
                continue;
            }

            vec2 scale = terrainLayers.uvTransform[index].xy;
            vec2 offset = terrainLayers.uvTransform[index].zw;

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

            float weight = mapWeight * pow(height, TERRAIN_HEIGHT_CONTRAST);
            sumColor += color * weight;
            sumNormal += normal * weight;
            sumRoughness += roughness * weight;
            sumMetallic += metallic * weight;
            sumOcclusion += occlusion * weight;
            total += weight;
        }
    }

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
