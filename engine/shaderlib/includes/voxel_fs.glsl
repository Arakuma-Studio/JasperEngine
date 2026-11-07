// A Voxel Volume's surface (T6 R42, R51): up to 4 slots per vertex, projected with no UVs, blended by
// the terrain's height formula, at a fixed cost per tier. The tier is a define, so no pixel pays for a
// branch on it (V11), and every read below is written out at its call site: the fetch budget is what
// the compiled shader holds, counted by VoxelShader.FetchBudgetPerTier.
//
//   High    4 layers, triplanar; top 2 colour, normal, surface; other 2 colour; far colour
//   Medium  3 layers, biplanar; colour, normal, surface; far colour
//   Low     2 layers, biplanar, near only; colour for both, normal for the strongest

uniform texture2DArray u_voxelDetail;
uniform sampler u_voxelDetail_smp;

uniform u_fs_voxelLayers {
    vec4 colorFactor[16];   // rgb tint (linear), a = 1.0 on a PBR slot
    vec4 uvTransform[16];   // xy per metre, zw offset, so an attached volume tiles exactly as its terrain
    vec4 surface[16];       // roughness, metallic, normal strength, occlusion strength
    vec4 slices[16];        // colour, normal and surface slice (-1 = none)
} voxelLayers;

in vec4 v_voxelPositionEye;
in vec3 v_voxelNormal;
in vec4 v_voxelWeights;
flat in vec4 v_voxelSlots;
flat in vec3 v_voxelAxisX;
flat in vec3 v_voxelAxisY;

// The terrain's own values, so a seam shows no change (R42)
const float VOXEL_TRIPLANAR_SHARPNESS = 8.0;
const float VOXEL_TRIPLANAR_CUTOFF = 0.02;
const float VOXEL_HEIGHT_CONTRAST = 6.0;
const float VOXEL_TILE_BREAK_NEAR = 6.0;
const float VOXEL_TILE_BREAK_FAR = 40.0;
const float VOXEL_TILE_BREAK_RATE = 0.371;
const float VOXEL_TILE_BREAK_MIX = 0.5;

#if !defined(VOXEL_TIER_HIGH) && !defined(VOXEL_TIER_MEDIUM) && !defined(VOXEL_TIER_LOW)
    // the G-buffer's variant has no Lit defines: a tier of its own until T3's Tqm and Tql choose one
    #define VOXEL_TIER_MEDIUM
#endif

struct VoxelSurface{
    vec4 color;
    vec3 normal;    // in the space the pass shades in
    float roughness;
    float metallic;
    float occlusion;
};

// One read of the array, always expanded in place: see the note above
#define VOXEL_READ(slice, uv, dx, dy) textureGrad(sampler2DArray(u_voxelDetail, u_voxelDetail_smp), vec3(uv, slice), dx, dy)

// A map through the projections of a layer, weighted. Zero-weight planes are not read.
#ifdef VOXEL_TIER_HIGH
    #define VOXEL_PROJECT(out_, slice, layer) \
        out_ = vec4(0.0); \
        if (planeWeight.y > 0.0) out_ += VOXEL_READ(slice, planeY * voxelLayers.uvTransform[layer].xy + voxelLayers.uvTransform[layer].zw, planeYdx * voxelLayers.uvTransform[layer].xy, planeYdy * voxelLayers.uvTransform[layer].xy) * planeWeight.y; \
        if (planeWeight.x > 0.0) out_ += VOXEL_READ(slice, planeX * voxelLayers.uvTransform[layer].xy + voxelLayers.uvTransform[layer].zw, planeXdx * voxelLayers.uvTransform[layer].xy, planeXdy * voxelLayers.uvTransform[layer].xy) * planeWeight.x; \
        if (planeWeight.z > 0.0) out_ += VOXEL_READ(slice, planeZ * voxelLayers.uvTransform[layer].xy + voxelLayers.uvTransform[layer].zw, planeZdx * voxelLayers.uvTransform[layer].xy, planeZdy * voxelLayers.uvTransform[layer].xy) * planeWeight.z;
#else
    // Biplanar: the dominant axis and the next
    #define VOXEL_PROJECT(out_, slice, layer) \
        out_ = VOXEL_READ(slice, planeA * voxelLayers.uvTransform[layer].xy + voxelLayers.uvTransform[layer].zw, planeAdx * voxelLayers.uvTransform[layer].xy, planeAdy * voxelLayers.uvTransform[layer].xy) * biplanarWeight.x; \
        if (biplanarWeight.y > 0.0) out_ += VOXEL_READ(slice, planeB * voxelLayers.uvTransform[layer].xy + voxelLayers.uvTransform[layer].zw, planeBdx * voxelLayers.uvTransform[layer].xy, planeBdy * voxelLayers.uvTransform[layer].xy) * biplanarWeight.y;
#endif

#define VOXEL_FAR(out_, slice, layer) \
    if (farAmount > 0.0) out_ = mix(out_, VOXEL_READ(slice, (planeY * voxelLayers.uvTransform[layer].xy + voxelLayers.uvTransform[layer].zw) * VOXEL_TILE_BREAK_RATE, planeYdx * voxelLayers.uvTransform[layer].xy * VOXEL_TILE_BREAK_RATE, planeYdy * voxelLayers.uvTransform[layer].xy * VOXEL_TILE_BREAK_RATE), farAmount);

// Whiteout blend of a tangent-space detail normal into the geometric normal, one plane at a time
vec3 voxelDetailNormal(vec4 sampled, vec3 geometric, float strength){
    vec3 detail = sampled.xyz * 2.0 - 1.0;
    detail.xy *= strength;
    return vec3(detail.x, detail.y, 0.0);
}

// Accumulates a layer's result, weighted by the vertex weight and T3's height formula
#define VOXEL_ACCUMULATE(layerWeight, color_, normalXY_, rough_, metal_, occl_, height_) \
    { \
        float weight = (layerWeight) * pow(max(height_, 0.0001), VOXEL_HEIGHT_CONTRAST); \
        sumColor += (color_) * weight; \
        sumDetail += (normalXY_) * weight; \
        sumRoughness += (rough_) * weight; \
        sumMetallic += (metal_) * weight; \
        sumOcclusion += (occl_) * weight; \
        total += weight; \
    }

// A colour layer (not PBR) stays raw and untinted, as the terrain keeps its historical layers
// (terrain_fs.glsl), so a slot following a Terrain Layer looks as that layer does; a PBR one is sRGB, tinted
vec4 voxelLayerColor(vec4 texel, vec4 factor){
    return factor.a > 0.5 ? vec4(sRGBToLinear(texel.rgb) * factor.rgb, 1.0) : vec4(texel.rgb, 1.0);
}

VoxelSurface getVoxelSurface(vec4 baseColor, vec3 baseNormal, float baseRoughness, float baseMetallic){
    vec3 p = v_voxelPositionEye.xyz;
    vec3 pdx = dFdx(p);
    vec3 pdy = dFdy(p);
    vec3 localNormal = normalize(v_voxelNormal);

    // sum 1 over the layers this tier reads: vertices are sorted by weight, so the first ones (V11)
    #if defined(VOXEL_TIER_HIGH)
        vec4 layerWeight = v_voxelWeights;
    #elif defined(VOXEL_TIER_MEDIUM)
        vec4 layerWeight = vec4(v_voxelWeights.xyz, 0.0);
    #else
        vec4 layerWeight = vec4(v_voxelWeights.xy, 0.0, 0.0);
    #endif
    layerWeight /= max(layerWeight.x + layerWeight.y + layerWeight.z + layerWeight.w, 0.0001);

    float faceX = localNormal.x < 0.0 ? -1.0 : 1.0;
    float faceZ = localNormal.z < 0.0 ? 1.0 : -1.0;
    vec2 planeY = p.xz;
    vec2 planeYdx = pdx.xz;
    vec2 planeYdy = pdy.xz;
    vec2 planeX = vec2(p.z * faceX, p.y);
    vec2 planeXdx = vec2(pdx.z * faceX, pdx.y);
    vec2 planeXdy = vec2(pdy.z * faceX, pdy.y);
    vec2 planeZ = vec2(p.x * faceZ, p.y);
    vec2 planeZdx = vec2(pdx.x * faceZ, pdx.y);
    vec2 planeZdy = vec2(pdy.x * faceZ, pdy.y);

    vec3 planeWeight = pow(abs(localNormal), vec3(VOXEL_TRIPLANAR_SHARPNESS));
    planeWeight /= max(planeWeight.x + planeWeight.y + planeWeight.z, 0.0001);
    planeWeight *= step(vec3(VOXEL_TRIPLANAR_CUTOFF), planeWeight);
    planeWeight /= max(planeWeight.x + planeWeight.y + planeWeight.z, 0.0001);

    #ifndef VOXEL_TIER_HIGH
        // the two largest of the three, the flat plane first on a tie
        vec2 planeA = planeY, planeAdx = planeYdx, planeAdy = planeYdy;
        vec2 planeB = planeX, planeBdx = planeXdx, planeBdy = planeXdy;
        vec2 biplanarWeight = vec2(planeWeight.y, planeWeight.x);
        if (planeWeight.z > planeWeight.y && planeWeight.z >= planeWeight.x){
            planeA = planeZ; planeAdx = planeZdx; planeAdy = planeZdy;
            biplanarWeight = vec2(planeWeight.z, max(planeWeight.x, planeWeight.y));
            if (planeWeight.y >= planeWeight.x){
                planeB = planeY; planeBdx = planeYdx; planeBdy = planeYdy;
            }
        }else if (planeWeight.x > planeWeight.y){
            planeA = planeX; planeAdx = planeXdx; planeAdy = planeXdy;
            biplanarWeight = vec2(planeWeight.x, max(planeWeight.y, planeWeight.z));
            if (planeWeight.y >= planeWeight.z){
                planeB = planeY; planeBdx = planeYdx; planeBdy = planeYdy;
            }else{
                planeB = planeZ; planeBdx = planeZdx; planeBdy = planeZdy;
            }
        }else if (planeWeight.z > planeWeight.x){
            planeB = planeZ; planeBdx = planeZdx; planeBdy = planeZdy;
            biplanarWeight = vec2(planeWeight.y, planeWeight.z);
        }
        biplanarWeight /= max(biplanarWeight.x + biplanarWeight.y, 0.0001);
    #endif

    #ifndef VOXEL_TIER_LOW
        float eyeTiles = v_voxelPositionEye.w * voxelLayers.uvTransform[int(v_voxelSlots.x)].x;
        float farAmount = smoothstep(VOXEL_TILE_BREAK_NEAR, VOXEL_TILE_BREAK_FAR, eyeTiles) * VOXEL_TILE_BREAK_MIX;
    #endif

    vec4 sumColor = vec4(0.0);
    vec2 sumDetail = vec2(0.0);
    float sumRoughness = 0.0;
    float sumMetallic = 0.0;
    float sumOcclusion = 0.0;
    float total = 0.0;

    // ---- the first layer: every tier reads its colour and its normal
    {
        int layer = int(v_voxelSlots.x);
        vec4 slices = voxelLayers.slices[layer];
        vec4 factor = voxelLayers.colorFactor[layer];
        vec4 surface = voxelLayers.surface[layer];
        vec4 color;
        VOXEL_PROJECT(color, slices.x, layer)
        #ifndef VOXEL_TIER_LOW
            VOXEL_FAR(color, slices.x, layer)
        #endif
        vec2 detail = vec2(0.0);
        if (slices.y >= 0.0){
            vec4 detailSample;
            VOXEL_PROJECT(detailSample, slices.y, layer)
            detail = voxelDetailNormal(detailSample, localNormal, surface.z).xy;
        }
        float roughness = surface.x;
        float metallic = surface.y;
        float occlusion = 1.0;
        float height = color.a;
        #ifndef VOXEL_TIER_LOW
            if (slices.z >= 0.0){
                vec4 packed_;
                VOXEL_PROJECT(packed_, slices.z, layer)
                roughness *= packed_.g;
                metallic *= packed_.b;
                occlusion = mix(1.0, packed_.r, surface.w);
                height = packed_.a;
            }
        #endif
        VOXEL_ACCUMULATE(layerWeight.x, voxelLayerColor(color, factor), detail, roughness, metallic, occlusion, height)
    }

    // ---- the second layer
    if (layerWeight.y > 0.0){
        int layer = int(v_voxelSlots.y);
        vec4 slices = voxelLayers.slices[layer];
        vec4 factor = voxelLayers.colorFactor[layer];
        vec4 surface = voxelLayers.surface[layer];
        vec4 color;
        VOXEL_PROJECT(color, slices.x, layer)
        #ifndef VOXEL_TIER_LOW
            VOXEL_FAR(color, slices.x, layer)
        #endif
        vec2 detail = vec2(0.0);
        float roughness = surface.x;
        float metallic = surface.y;
        float occlusion = 1.0;
        float height = color.a;
        #ifndef VOXEL_TIER_LOW
            if (slices.y >= 0.0){
                vec4 detailSample;
                VOXEL_PROJECT(detailSample, slices.y, layer)
                detail = voxelDetailNormal(detailSample, localNormal, surface.z).xy;
            }
            if (slices.z >= 0.0){
                vec4 packed_;
                VOXEL_PROJECT(packed_, slices.z, layer)
                roughness *= packed_.g;
                metallic *= packed_.b;
                occlusion = mix(1.0, packed_.r, surface.w);
                height = packed_.a;
            }
        #endif
        VOXEL_ACCUMULATE(layerWeight.y, voxelLayerColor(color, factor), detail, roughness, metallic, occlusion, height)
    }

    // ---- the third layer: Medium reads all its maps, High only colour
    #ifndef VOXEL_TIER_LOW
    if (layerWeight.z > 0.0){
        int layer = int(v_voxelSlots.z);
        vec4 slices = voxelLayers.slices[layer];
        vec4 factor = voxelLayers.colorFactor[layer];
        vec4 surface = voxelLayers.surface[layer];
        vec4 color;
        VOXEL_PROJECT(color, slices.x, layer)
        VOXEL_FAR(color, slices.x, layer)
        vec2 detail = vec2(0.0);
        float roughness = surface.x;
        float metallic = surface.y;
        float occlusion = 1.0;
        float height = color.a;
        #ifdef VOXEL_TIER_MEDIUM
            if (slices.y >= 0.0){
                vec4 detailSample;
                VOXEL_PROJECT(detailSample, slices.y, layer)
                detail = voxelDetailNormal(detailSample, localNormal, surface.z).xy;
            }
            if (slices.z >= 0.0){
                vec4 packed_;
                VOXEL_PROJECT(packed_, slices.z, layer)
                roughness *= packed_.g;
                metallic *= packed_.b;
                occlusion = mix(1.0, packed_.r, surface.w);
                height = packed_.a;
            }
        #endif
        VOXEL_ACCUMULATE(layerWeight.z, voxelLayerColor(color, factor), detail, roughness, metallic, occlusion, height)
    }
    #endif

    // ---- the fourth layer: High only, colour only
    #ifdef VOXEL_TIER_HIGH
    if (layerWeight.w > 0.0){
        int layer = int(v_voxelSlots.w);
        vec4 slices = voxelLayers.slices[layer];
        vec4 factor = voxelLayers.colorFactor[layer];
        vec4 surface = voxelLayers.surface[layer];
        vec4 color;
        VOXEL_PROJECT(color, slices.x, layer)
        VOXEL_FAR(color, slices.x, layer)
        VOXEL_ACCUMULATE(layerWeight.w, voxelLayerColor(color, factor), vec2(0.0), surface.x, surface.y, 1.0, color.a)
    }
    #endif

    VoxelSurface result;
    result.color = vec4(sumColor.rgb / max(total, 0.0001), 1.0) * baseColor;
    vec2 detail = sumDetail / max(total, 0.0001);
    // The detail normal in the volume's frame, about the geometric one, then into the pass's space
    vec3 axisZ = cross(v_voxelAxisX, v_voxelAxisY);
    vec3 tangentU = normalize(v_voxelAxisX - baseNormal * dot(v_voxelAxisX, baseNormal));
    vec3 tangentV = cross(baseNormal, tangentU);
    float flatness = abs(dot(baseNormal, v_voxelAxisX));
    if (flatness > 0.9){
        tangentU = normalize(axisZ - baseNormal * dot(axisZ, baseNormal));
        tangentV = cross(baseNormal, tangentU);
    }
    result.normal = normalize(baseNormal + tangentU * detail.x + tangentV * detail.y);
    result.roughness = sumRoughness / max(total, 0.0001);
    result.metallic = sumMetallic / max(total, 0.0001);
    result.occlusion = sumOcclusion / max(total, 0.0001);
    return result;
}

#ifdef VOXEL_MATERIAL_INFO
    MaterialInfo applyVoxelSurface(MaterialInfo info, VoxelSurface surface, float f0_ior){
        info.perceptualRoughness = surface.roughness;
        info.metallic = surface.metallic;
        vec3 f0 = vec3(f0_ior);
        info.albedoColor = mix(info.baseColor.rgb * (vec3(1.0) - f0), vec3(0.0), info.metallic);
        info.f0 = mix(f0, info.baseColor.rgb, info.metallic);
        return info;
    }
#endif
