#version 450

// @shader mesh
// @name "Vertex Blend"
// @category "Lit"
// @description "Blends a base layer and up to four texture layers by the vertex colour's R, G, B and A, each layer sharpened by its height (its albedo's alpha). Per layer: albedo, normal map, optional ORM map, tiling, height blend contrast, roughness and metallic. Lambert diffuse and GGX specular."
// @lighting pixel vertex mixed none
// @require rawcolor color
// @require uv1 feature=!TRIPLANAR
// @require position normals feature=TRIPLANAR
// @state blend=opaque
// @keywords LAYERS "Layers" TWO THREE FOUR FIVE
// @property baseColor color (1, 1, 1, 1) "Tint"
// @property normalStrength range(0, 2) 1 "Normal strength"
// @property triplanarSharpness range(1, 16) 4 "Triplanar blend sharpness" feature=TRIPLANAR
// @property baseTiling float 1 "Base tiling"
// @property baseContrast range(0.01, 1) 0.2 "Base height blend contrast"
// @property baseRoughness range(0, 1) 0.8 "Base roughness"
// @property baseMetallic range(0, 1) 0 "Base metallic"
// @property rTiling float 1 "R layer tiling"
// @property rContrast range(0.01, 1) 0.2 "R layer height blend contrast"
// @property rRoughness range(0, 1) 0.8 "R layer roughness"
// @property rMetallic range(0, 1) 0 "R layer metallic"
// @property gTiling float 1 "G layer tiling" feature=LAYERS_THREE,LAYERS_FOUR,LAYERS_FIVE
// @property gContrast range(0.01, 1) 0.2 "G layer height blend contrast" feature=LAYERS_THREE,LAYERS_FOUR,LAYERS_FIVE
// @property gRoughness range(0, 1) 0.8 "G layer roughness" feature=LAYERS_THREE,LAYERS_FOUR,LAYERS_FIVE
// @property gMetallic range(0, 1) 0 "G layer metallic" feature=LAYERS_THREE,LAYERS_FOUR,LAYERS_FIVE
// @property bTiling float 1 "B layer tiling" feature=LAYERS_FOUR,LAYERS_FIVE
// @property bContrast range(0.01, 1) 0.2 "B layer height blend contrast" feature=LAYERS_FOUR,LAYERS_FIVE
// @property bRoughness range(0, 1) 0.8 "B layer roughness" feature=LAYERS_FOUR,LAYERS_FIVE
// @property bMetallic range(0, 1) 0 "B layer metallic" feature=LAYERS_FOUR,LAYERS_FIVE
// @property aTiling float 1 "A layer tiling" feature=LAYERS_FIVE
// @property aContrast range(0.01, 1) 0.2 "A layer height blend contrast" feature=LAYERS_FIVE
// @property aRoughness range(0, 1) 0.8 "A layer roughness" feature=LAYERS_FIVE
// @property aMetallic range(0, 1) 0 "A layer metallic" feature=LAYERS_FIVE
// @texture u_baseAlbedo white "Base albedo (height in alpha)"
// @texture u_baseNormal normal "Base normal map"
// @texture u_baseOrm white "Base ORM (occlusion, roughness, metallic)" feature=ORM
// @texture u_rAlbedo white "R layer albedo (height in alpha)"
// @texture u_rNormal normal "R layer normal map"
// @texture u_rOrm white "R layer ORM" feature=ORM
// @texture u_gAlbedo white "G layer albedo (height in alpha)" feature=LAYERS_THREE,LAYERS_FOUR,LAYERS_FIVE
// @texture u_gNormal normal "G layer normal map" feature=LAYERS_THREE,LAYERS_FOUR,LAYERS_FIVE
// @texture u_gOrm white "G layer ORM" feature=ORM
// @texture u_bAlbedo white "B layer albedo (height in alpha)" feature=LAYERS_FOUR,LAYERS_FIVE
// @texture u_bNormal normal "B layer normal map" feature=LAYERS_FOUR,LAYERS_FIVE
// @texture u_bOrm white "B layer ORM" feature=ORM
// @texture u_aAlbedo white "A layer albedo (height in alpha)" feature=LAYERS_FIVE
// @texture u_aNormal normal "A layer normal map" feature=LAYERS_FIVE
// @feature ORM "ORM maps (up to four layers)"
// @feature TRIPLANAR "Triplanar projection"

// docs/specs/V1-vertex-colors.md R71. The vertex colour (v_rawColor, without the instance colour)
// weighs the layers: R the R layer, G the G layer, B the B layer, A the A layer, and the base
// w = max(0, 1 - (r + g + b + a)) over the channels of the layers there are (at five layers,
// all four). Each weight is then moved by its layer's height h (its albedo's alpha):
// w' = max(0, h + w - (max_j(h_j + w_j) - contrast)), and the weights renormalised, so a layer
// fills the gaps of the one below it (a higher contrast blends wider; no step: painting a
// channel up fades its layer in). Paint black for the base alone.
//
// LAYERS_TWO to LAYERS_FIVE keep that many layers (base, R, G, B, A in this order); the others
// compile out with their maps. Layers are sampled by uv1 times their tiling, or with TRIPLANAR
// projected along the three world axes (tiling per world unit) and blended by the normal.
// Normal maps are read by the pixel lights only (the tangent frame from the derivatives: no
// tangent attribute); ORM maps (R occlusion, G roughness, B metallic) multiply the layer's
// values. Five layers with ORM off fit the 16 fragment textures of GLES3 and WebGL2 with the
// lights: ORM reads up to four layers (at five the factors alone apply).
// Lit modes: Lambert diffuse and GGX specular (no Fresnel), shadows from pixel lights, the
// occlusion on the ambient. Without scene lights the variant is unlit.

#if defined(LAYERS_FIVE)
    #define VB_LAYERS 5
#elif defined(LAYERS_FOUR)
    #define VB_LAYERS 4
#elif defined(LAYERS_THREE)
    #define VB_LAYERS 3
#else
    #define VB_LAYERS 2
#endif
#if defined(ORM) && VB_LAYERS < 5
    #define VB_ORM
#endif

uniform u_fs_customParams {
    vec4 baseColor;
    float normalStrength;
    float baseTiling;
    float baseContrast;
    float baseRoughness;
    float baseMetallic;
    float rTiling;
    float rContrast;
    float rRoughness;
    float rMetallic;
    #if VB_LAYERS >= 3
        float gTiling;
        float gContrast;
        float gRoughness;
        float gMetallic;
    #endif
    #if VB_LAYERS >= 4
        float bTiling;
        float bContrast;
        float bRoughness;
        float bMetallic;
    #endif
    #if VB_LAYERS >= 5
        float aTiling;
        float aContrast;
        float aRoughness;
        float aMetallic;
    #endif
    #ifdef TRIPLANAR
        float triplanarSharpness;
    #endif
} customParams;

#include "includes/normal_map.glsl"

// one sampler for every layer map (the 16 of both stages stay for the lights)
uniform sampler u_layers_smp;
uniform texture2D u_baseAlbedo;
uniform texture2D u_rAlbedo;
#if VB_LAYERS >= 3
    uniform texture2D u_gAlbedo;
#endif
#if VB_LAYERS >= 4
    uniform texture2D u_bAlbedo;
#endif
#if VB_LAYERS >= 5
    uniform texture2D u_aAlbedo;
#endif
#ifdef VB_ORM
    uniform texture2D u_baseOrm;
    uniform texture2D u_rOrm;
    #if VB_LAYERS >= 3
        uniform texture2D u_gOrm;
    #endif
    #if VB_LAYERS >= 4
        uniform texture2D u_bOrm;
    #endif
#endif

#ifndef TRIPLANAR
    #include "includes/surface_uv.glsl"
    SURFACE_UV_IN vec2 v_uv1;
#endif
in vec4 v_color;    // the instance colour
in vec4 v_rawColor; // the layer weights

#if defined(USE_PUNCTUAL) || defined(TRIPLANAR)
    in vec3 v_position;
    in vec3 v_normal;
#endif

#ifdef USE_PUNCTUAL
    #define FRAME_FS
    #include "includes/frame.glsl"
    #define LIGHTS_FS
    #include "includes/lights.glsl"

    #define VB_NORMALS
    uniform texture2D u_baseNormal;
    uniform texture2D u_rNormal;
    #if VB_LAYERS >= 3
        uniform texture2D u_gNormal;
    #endif
    #if VB_LAYERS >= 4
        uniform texture2D u_bNormal;
    #endif
    #if VB_LAYERS >= 5
        uniform texture2D u_aNormal;
    #endif
#endif

#ifdef USE_VERTEX_LIGHTS
    in vec3 v_vertexLight;
#endif

out vec4 g_finalColor;

#include "includes/output.glsl"

// what a layer gives at the fragment: its albedo (as stored: sRGB, height in alpha), world normal
// and ORM
struct VertexBlendLayer {
    vec4 albedo;
    vec3 normal;
    vec3 orm;
};

#ifdef TRIPLANAR
    vec3 vbTriplanarWeights;
    vec3 vbAxisSign;
#endif
#ifdef VB_NORMALS
    // the surface's tangent frame (from the derivatives, taken in uniform control flow) and normal
    mat3 vbTangentFrame;
#endif
vec3 vbSurfaceNormal;

// the layer's texel of a map at the fragment: by uv1, or the three projections blended
vec4 vbSample(texture2D map, float tiling){
    #ifdef TRIPLANAR
        vec3 p = v_position * tiling;
        vec4 x = texture(sampler2D(map, u_layers_smp), vec2(p.z * vbAxisSign.x, p.y));
        vec4 y = texture(sampler2D(map, u_layers_smp), vec2(p.x * vbAxisSign.y, p.z));
        vec4 z = texture(sampler2D(map, u_layers_smp), vec2(-p.x * vbAxisSign.z, p.y));
        return x * vbTriplanarWeights.x + y * vbTriplanarWeights.y + z * vbTriplanarWeights.z;
    #else
        return texture(sampler2D(map, u_layers_smp), v_uv1 * tiling);
    #endif
}

#ifdef VB_NORMALS
// the layer's world normal from its normal map
vec3 vbNormal(texture2D map, float tiling){
    #ifdef TRIPLANAR
        // each projection's tangent normal swizzled onto its axis (whiteout blend)
        vec3 p = v_position * tiling;
        vec3 n = vbSurfaceNormal;
        vec3 tx = decodeNormalMap(texture(sampler2D(map, u_layers_smp), vec2(p.z * vbAxisSign.x, p.y)));
        vec3 ty = decodeNormalMap(texture(sampler2D(map, u_layers_smp), vec2(p.x * vbAxisSign.y, p.z)));
        vec3 tz = decodeNormalMap(texture(sampler2D(map, u_layers_smp), vec2(-p.x * vbAxisSign.z, p.y)));
        tx.xy *= customParams.normalStrength;
        ty.xy *= customParams.normalStrength;
        tz.xy *= customParams.normalStrength;
        tx = vec3(tx.xy * vec2(vbAxisSign.x, 1.0) + n.zy, abs(tx.z) * n.x);
        ty = vec3(ty.xy * vec2(vbAxisSign.y, 1.0) + n.xz, abs(ty.z) * n.y);
        tz = vec3(tz.xy * vec2(-vbAxisSign.z, 1.0) + n.xy, abs(tz.z) * n.z);
        return normalize(tx.zyx * vbTriplanarWeights.x + ty.xzy * vbTriplanarWeights.y + tz.xyz * vbTriplanarWeights.z);
    #else
        vec3 mapped = decodeNormalMap(texture(sampler2D(map, u_layers_smp), v_uv1 * tiling));
        mapped.xy *= customParams.normalStrength * sign(tiling);
        return normalize(vbTangentFrame * mapped);
    #endif
}
#endif

VertexBlendLayer vbLayer(texture2D albedo, float tiling){
    VertexBlendLayer layer;
    layer.albedo = vbSample(albedo, tiling);
    layer.normal = vbSurfaceNormal;
    layer.orm = vec3(1.0);
    return layer;
}

void main() {
    #if defined(USE_PUNCTUAL) || defined(TRIPLANAR)
        vbSurfaceNormal = normalize(v_normal);
    #else
        vbSurfaceNormal = vec3(0.0, 1.0, 0.0);
    #endif
    #ifdef TRIPLANAR
        vbTriplanarWeights = pow(abs(vbSurfaceNormal), vec3(customParams.triplanarSharpness));
        vbTriplanarWeights /= max(vbTriplanarWeights.x + vbTriplanarWeights.y + vbTriplanarWeights.z, 1e-5);
        vbAxisSign = vec3(vbSurfaceNormal.x < 0.0 ? -1.0 : 1.0, vbSurfaceNormal.y < 0.0 ? -1.0 : 1.0, vbSurfaceNormal.z < 0.0 ? -1.0 : 1.0);
    #endif
    #if defined(VB_NORMALS) && !defined(TRIPLANAR)
        {
            vec3 dpx = dFdx(v_position);
            vec3 dpy = dFdy(v_position);
            vec2 duvx = dFdx(v_uv1);
            vec2 duvy = dFdy(v_uv1);
            float det = duvx.x * duvy.y - duvy.x * duvx.y;
            vec3 tangent = abs(det) < 1e-12 ? vec3(1.0, 0.0, 0.0) : (duvy.y * dpx - duvx.y * dpy) / det;
            tangent = tangent - vbSurfaceNormal * dot(vbSurfaceNormal, tangent);
            float tangentLength = length(tangent);
            tangent = tangentLength < 1e-8 ? normalize(cross(vbSurfaceNormal, abs(vbSurfaceNormal.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)))
                                           : tangent / tangentLength;
            vbTangentFrame = mat3(tangent, cross(vbSurfaceNormal, tangent), vbSurfaceNormal);
        }
    #endif

    // the layers, base first
    VertexBlendLayer layers[VB_LAYERS];
    float tiling[VB_LAYERS];
    float contrast[VB_LAYERS];
    float roughness[VB_LAYERS];
    float metallic[VB_LAYERS];
    tiling[0] = customParams.baseTiling;
    contrast[0] = customParams.baseContrast;
    roughness[0] = customParams.baseRoughness;
    metallic[0] = customParams.baseMetallic;
    tiling[1] = customParams.rTiling;
    contrast[1] = customParams.rContrast;
    roughness[1] = customParams.rRoughness;
    metallic[1] = customParams.rMetallic;
    layers[0] = vbLayer(u_baseAlbedo, tiling[0]);
    layers[1] = vbLayer(u_rAlbedo, tiling[1]);
    #if VB_LAYERS >= 3
        tiling[2] = customParams.gTiling;
        contrast[2] = customParams.gContrast;
        roughness[2] = customParams.gRoughness;
        metallic[2] = customParams.gMetallic;
        layers[2] = vbLayer(u_gAlbedo, tiling[2]);
    #endif
    #if VB_LAYERS >= 4
        tiling[3] = customParams.bTiling;
        contrast[3] = customParams.bContrast;
        roughness[3] = customParams.bRoughness;
        metallic[3] = customParams.bMetallic;
        layers[3] = vbLayer(u_bAlbedo, tiling[3]);
    #endif
    #if VB_LAYERS >= 5
        tiling[4] = customParams.aTiling;
        contrast[4] = customParams.aContrast;
        roughness[4] = customParams.aRoughness;
        metallic[4] = customParams.aMetallic;
        layers[4] = vbLayer(u_aAlbedo, tiling[4]);
    #endif
    #ifdef VB_NORMALS
        layers[0].normal = vbNormal(u_baseNormal, tiling[0]);
        layers[1].normal = vbNormal(u_rNormal, tiling[1]);
        #if VB_LAYERS >= 3
            layers[2].normal = vbNormal(u_gNormal, tiling[2]);
        #endif
        #if VB_LAYERS >= 4
            layers[3].normal = vbNormal(u_bNormal, tiling[3]);
        #endif
        #if VB_LAYERS >= 5
            layers[4].normal = vbNormal(u_aNormal, tiling[4]);
        #endif
    #endif
    #ifdef VB_ORM
        layers[0].orm = vbSample(u_baseOrm, tiling[0]).rgb;
        layers[1].orm = vbSample(u_rOrm, tiling[1]).rgb;
        #if VB_LAYERS >= 3
            layers[2].orm = vbSample(u_gOrm, tiling[2]).rgb;
        #endif
        #if VB_LAYERS >= 4
            layers[3].orm = vbSample(u_bOrm, tiling[3]).rgb;
        #endif
    #endif

    // the weights: the channels' (the base's what they leave), moved by the heights, renormalised
    vec4 paint = clamp(v_rawColor, 0.0, 1.0);
    float weights[VB_LAYERS];
    float painted = paint.r;
    weights[1] = paint.r;
    #if VB_LAYERS >= 3
        weights[2] = paint.g;
        painted += paint.g;
    #endif
    #if VB_LAYERS >= 4
        weights[3] = paint.b;
        painted += paint.b;
    #endif
    #if VB_LAYERS >= 5
        weights[4] = paint.a;
        painted += paint.a;
    #endif
    weights[0] = max(0.0, 1.0 - painted);
    float highest = 0.0;
    for (int i = 0; i < VB_LAYERS; i++){
        highest = max(highest, layers[i].albedo.a + weights[i]);
    }
    float total = 0.0;
    for (int i = 0; i < VB_LAYERS; i++){
        weights[i] = max(0.0, layers[i].albedo.a + weights[i] - (highest - max(contrast[i], 0.01)));
        total += weights[i];
    }

    vec4 albedo = vec4(0.0);
    vec3 normal = vec3(0.0);
    vec3 surface = vec3(0.0); // occlusion, roughness, metallic
    for (int i = 0; i < VB_LAYERS; i++){
        float w = weights[i] / max(total, 1e-5);
        albedo += layers[i].albedo * w;
        normal += layers[i].normal * w;
        surface += vec3(layers[i].orm.r, roughness[i] * layers[i].orm.g, metallic[i] * layers[i].orm.b) * w;
    }
    vec3 base = sRGBToLinear(albedo.rgb) * customParams.baseColor.rgb * v_color.rgb;
    float alpha = customParams.baseColor.a * v_color.a;
    vec3 color = base;

    #if defined(USE_PUNCTUAL) || defined(USE_VERTEX_LIGHTS)
        float occlusion = surface.x;
        float metal = clamp(surface.z, 0.0, 1.0);
        vec3 diffuseColor = base * (1.0 - metal);
        vec3 diffuseLight = vec3(0.0);
        vec3 specularLight = vec3(0.0);
        #ifdef USE_PUNCTUAL
            vec3 n = normalize(normal);
            // a double-sided surface shades its back with the reversed normal
            if (!gl_FrontFacing){
                n = -n;
            }
            // towards the camera (an orthographic camera looks along one direction)
            vec3 v = (frame.cameraPosition.w > 0.5) ? -frame.cameraDirection.xyz : normalize(frame.cameraPosition.xyz - v_position);
            float rough = clamp(surface.y, 0.0, 1.0);
            diffuseLight += ambientLight(n) * occlusion;
            for (int i = 0; i < LIGHT_CAPACITY; i++){
                if (i >= pixelLightCount()) break;
                LightSample light = getLight(i, v_position);
                float shadow = 1.0;
                #ifdef USE_SHADOWS
                    shadow = lightShadow(i, v_position, n);
                #endif
                diffuseLight += lambert(light, n) * shadow;
                specularLight += ggxSpecular(light, n, v, rough, 1.0) * shadow;
            }
        #endif
        #ifdef USE_VERTEX_LIGHTS
            // the vertex lights' diffuse (and, in vertex mode, the ambient)
            diffuseLight += v_vertexLight;
        #endif
        color = diffuseColor * diffuseLight + mix(vec3(0.04), base, metal) * specularLight;
    #endif

    g_finalColor = finalizeColor(vec4(color, alpha));
}
