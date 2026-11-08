#version 450
#include "includes/instance_normal.glsl"

// G-buffer geometry pass (vertex). Mirrors depth.vert's position pipeline
// (skinning / morph / terrain / instancing) and additionally carries the surface
// normal into view space for the fragment stage.
//
// The build defines DEPTH_SHADER (see ShaderBuilder) for the same reason depth.vert does:
// it suppresses the morph-normal machinery and, unless HAS_TERRAIN_PBR samples them, the
// terrain texture-coordinate varyings. Skinning and terrain normals are still applied;
// per-target morph-normal blending is not (fine for SSR).

uniform u_vs_gbufferParams {
    mat4 modelMatrix;
    mat4 viewProjectionMatrix;
    mat4 normalMatrix;       // view-space normal matrix: transpose(inverse(view*model))
} gbufferParams;

in vec3 a_position;
out vec2 v_projZW;
out vec3 v_normal;          // view space

#ifdef HAS_NORMALS
    in vec3 a_normal;
#endif

#include "includes/foliage_instance.glsl"
#include "includes/foliage_wind.glsl"
#ifdef USE_FOLIAGE_DITHER
    out float v_foliageFade;
#endif
#if defined(HAS_INSTANCING) && !defined(HAS_COMPACT_INSTANCE)
    in vec4 i_matrix_col1;
    in vec4 i_matrix_col2;
    in vec4 i_matrix_col3;
    in vec4 i_matrix_col4;
#endif

// the distance fade, as the colour and depth passes shrink the instance (a faded one must not
// reflect nor stop SSR's rays)
#ifdef USE_INSTANCE_FADE
    #include "includes/instance_fade.glsl"
#endif

#if defined(HAS_BASECOLOR_TEXTURE) || defined(HAS_METALLICROUGHNESS_TEXTURE) || defined(HAS_OCCLUSION_TEXTURE)
    #define GBUFFER_UV
#endif

#ifdef GBUFFER_UV
    // Terrain carries no texcoord vertex attribute (its UVs are generated from the
    // world position in-shader, like mesh.vert). Declaring a_texcoord1 for terrain
    // would leave that attribute slot unbound, making the pipeline's vertex layout
    // non-continuous and failing sokol validation, so only non-terrain declares it.
    #ifndef HAS_TERRAIN
        in vec2 a_texcoord1;
    #endif
    out vec2 v_uv1;
    // the second UV set, which the material's maps may be read on (never a terrain's)
    #ifdef HAS_UV_SET2
        in vec2 a_texcoord2;
        out vec2 v_uv2;
    #endif
#endif

// a lattice's vertex colour mask (includes/lattice.glsl)
#if defined(HAS_LATTICE) || defined(USE_FOLIAGE_WIND)
    #if defined(HAS_VERTEX_COLOR_VEC3)
        in vec3 a_color;
    #elif defined(HAS_VERTEX_COLOR_VEC4)
        in vec4 a_color;
    #endif
#endif

#include "includes/skinning.glsl"
#include "includes/morphtarget.glsl"
#ifdef HAS_TERRAIN
    #include "includes/terrain_vs.glsl"
#endif
#ifdef HAS_VOXEL
    #include "includes/voxel_vs.glsl"
#endif
#include "includes/lattice.glsl"
#include "includes/vat.glsl"

vec4 getPosition(mat4 boneTransform){
    vec3 pos = vatPosition(a_position);

    pos = getMorphPosition(pos);
    pos = getSkinPosition(pos, boneTransform);
    #ifdef HAS_TERRAIN
        pos = getTerrainPosition(pos, gbufferParams.modelMatrix);
    #endif

    return vec4(pos, 1.0);
}

vec3 getNormalObj(mat4 boneTransform, vec4 position){
    vec3 normal = vec3(0.0, 0.0, 1.0);
    #ifdef HAS_NORMALS
        normal = vatNormal(a_normal);
        normal = getSkinNormal(normal, boneTransform);
    #endif
    #ifdef HAS_TERRAIN
        // terrain generates its normal from the heightmap (needs HAS_NORMALS)
        normal = getTerrainNormal(normal, position.xyz);
    #endif
    return normalize(normal);
}

#if defined(HAS_VERTEX_COLOR_VEC3) || defined(HAS_VERTEX_COLOR_VEC4)
    #define FOLIAGE_VERTEX_RED a_color.r
#else
    #define FOLIAGE_VERTEX_RED 1.0
#endif

void main() {
    #ifdef HAS_COMPACT_INSTANCE
        compactInstanceSetup();
    #endif
    mat4 mvpMatrix = gbufferParams.viewProjectionMatrix * gbufferParams.modelMatrix;

    // the baked vertex animation first (HAS_VAT), as mesh.vert
    vatEvaluate();
    mat4 boneTransform = getBoneTransform();

    #ifdef HAS_LATTICE
        // as mesh.vert: the same position as the colour and depth passes, and the normal
        // through the same Jacobian
        vec3 latticePosition = getMorphPosition(vatPosition(a_position));
        vec3 latticeNormal = vec3(0.0, 0.0, 1.0);
        #ifdef HAS_NORMALS
            latticeNormal = vatNormal(a_normal);
        #endif
        vec3 latticeTangent = vec3(1.0, 0.0, 0.0);
        float latticeStretchValue = 1.0;
        #ifdef HAS_SKINNING
            latticeDeform(0, latticePosition, latticeNormal, latticeTangent, latticeStretchValue);
        #endif
        vec4 objPos = vec4(getSkinPosition(latticePosition, boneTransform), 1.0);
        latticeNormal = normalize(getSkinNormal(latticeNormal, boneTransform));
        #ifdef HAS_INSTANCING
            mat4 instanceMatrix = mat4(vec4(i_matrix_col1.xyz, 0.0), i_matrix_col2, i_matrix_col3, i_matrix_col4);
            vec4 pos = instanceMatrix * objPos;
            latticeNormal = instanceNormal(mat3(instanceMatrix), latticeNormal);
            #ifdef USE_INSTANCE_FADE
                pos.xyz = mix(i_matrix_col4.xyz, pos.xyz, instanceFadeVisible(i_matrix_col4.xyz));
            #endif
        #else
            vec4 pos = objPos;
        #endif
        latticeDeform(1, pos.xyz, latticeNormal, latticeTangent, latticeStretchValue);
        v_normal = normalize(mat3(gbufferParams.normalMatrix) * latticeNormal);
    #else
    vec4 objPos = getPosition(boneTransform);
    vec3 objNormal = getNormalObj(boneTransform, objPos);

    #ifdef HAS_INSTANCING
        mat4 instanceMatrix = mat4(vec4(i_matrix_col1.xyz, 0.0), i_matrix_col2, i_matrix_col3, i_matrix_col4);
        vec4 pos = instanceMatrix * objPos;
        #ifdef USE_INSTANCE_FADE
            pos.xyz = mix(i_matrix_col4.xyz, pos.xyz, instanceFadeVisible(i_matrix_col4.xyz));
        #endif
        // instance matrix sits between model and object; fold it in before the
        // (model+view) normal matrix
        v_normal = normalize(mat3(gbufferParams.normalMatrix) * instanceNormal(mat3(instanceMatrix), objNormal));
    #else
        vec4 pos = objPos;
        v_normal = normalize(mat3(gbufferParams.normalMatrix) * objNormal);
    #endif
    #endif

    #ifdef USE_FOLIAGE_WIND
        pos.xyz = foliageWindSway(pos.xyz, i_matrix_col4.xyz, a_position.y, FOLIAGE_VERTEX_RED);
    #endif
    #ifdef USE_FOLIAGE_DITHER
        v_foliageFade = foliageFadeFactor(i_matrix_col4.xyz);
    #endif

    #if defined(HAS_TERRAIN_PBR) && defined(HAS_NORMALS)
        setTerrainShadingAxes(gbufferParams.normalMatrix);
    #endif
    #ifdef HAS_VOXEL
        setVoxelVaryings(a_position, a_normal, gbufferParams.normalMatrix);
    #endif

    #ifdef HAS_TERRAIN_PBR
        // fills the surface varyings the painted layers are sampled with, and returns
        // the same base-tile UV the branch below computes by hand
        vec2 terrainBaseUV = getTerrainTiledTexture(objPos.xyz);
    #endif

    #ifdef GBUFFER_UV
        #ifdef HAS_UV_SET2
            v_uv2 = a_texcoord2;
        #endif
        #ifdef HAS_TERRAIN_PBR
            v_uv1 = terrainBaseUV;
        #elif defined(HAS_TERRAIN)
            // base-tile terrain UV, matching mesh.vert's getTerrainTiledTexture()
            v_uv1 = (objPos.xz + (terrain.size / 2.0)) / terrain.size * float(terrain.textureBaseTiles);
        #else
            v_uv1 = a_texcoord1;
        #endif
    #endif

    gl_Position = mvpMatrix * pos;

    v_projZW = gl_Position.zw;
    // No Y flip here: the G-buffer keeps the target's native orientation, and SSR
    // pairs it with the scene color through the flip flag in misc.z.
    #ifdef IS_VULKAN
        // GL [-1,1] to Vulkan [0,1] depth range (spirv-cross fixup_clipspace equivalent)
        gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
    #endif
}
