// Vertex stage of the library depth programs (shadow maps, depth pre-passes), the
// counterpart of includes/surface_vs.glsl: the same position, bit for bit, including a
// SURFACE_DISPLACE hook that must match the colour pass's:
//
//   #define SURFACE_DISPLACE
//   #include "includes/surface_depth_vs.glsl"
//   vec3 surfaceDisplace(vec3 objectPosition, vec3 objectNormal, vec4 color, vec2 uv){ ... }
//
// The inputs a manifest @require asks for come in as the colour pass's do (a missing one
// gets the same fallback, SURFACE_NO_VERTEX_COLOR works the same; the hook's colour is the
// raw vertex colour, a terrain's patches and UV and the sprite texture rect apply alike).
// With SURFACE_DEPTH_VARYINGS defined before including, it also writes v_uv1 (SURFACE_UV1)
// and v_color (SURFACE_COLOR, the vertex colour times the instance colour) for a fragment
// stage that alpha clips (includes/surface_depth_fs.glsl with SURFACE_DEPTH_DISCARD).
// SURFACE_SMOOTH_NORMALS gives the hook surfaceSmoothNormal as includes/surface_vs.glsl does,
// and surfaceFadeVisible (the instance distance fade's scale) is the same too. A Vertex
// Animation (HAS_VAT) and the lattices (HAS_LATTICE) move the vertex as there, before the hook.

// Inputs only the hooks read: without them they are not declared at all, since a GL
// driver drops an unread attribute and the bind then misses it
#if defined(SURFACE_DISPLACE) || defined(SURFACE_DEPTH_VARYINGS)
    #define SURFACE_DEPTH_INPUTS
#endif
#ifndef SURFACE_DISPLACE
    #undef HAS_NORMALS
#endif
#ifndef SURFACE_DEPTH_INPUTS
    #undef HAS_UV_SET1
    // (a lattice's vertex colour mask reads it)
    #ifndef HAS_LATTICE
        #undef HAS_VERTEX_COLOR_VEC3
        #undef HAS_VERTEX_COLOR_VEC4
    #endif
#endif
#ifndef HAS_UV_SET1
    #undef HAS_TEXTURERECT
#endif

// depthParams: per draw, or per instance in an automatically instanced batch
#include "includes/object_depth_params.glsl"

invariant gl_Position;

#ifdef USE_INSTANCE_FADE
    #include "includes/instance_fade.glsl"
#endif

#ifdef HAS_TEXTURERECT
    uniform u_vs_spriteParams {
        vec4 textureRect;
    } spriteParams;
#endif

in vec3 a_position;
out vec2 v_projZW;

#ifdef HAS_NORMALS
    in vec3 a_normal;
#endif
#if defined(SURFACE_SMOOTH_NORMALS) && defined(SURFACE_DISPLACE) && !defined(HAS_TERRAIN)
    #define SURFACE_SMOOTH_NORMAL_INPUT
    in vec3 a_smoothNormal;
#endif
#if defined(HAS_UV_SET1) && !defined(HAS_TERRAIN)
    in vec2 a_texcoord1;
#endif
#ifdef HAS_TERRAIN
    #define TERRAIN_NO_SURFACE_VARYINGS
    #include "includes/terrain_vs.glsl"
#endif
// as includes/surface_vs.glsl: under SURFACE_NO_VERTEX_COLOR only a hook that reads it
// (SURFACE_DISPLACE_COLOR) gets the attribute, or a lattice's vertex colour mask
#if (!defined(SURFACE_NO_VERTEX_COLOR) || defined(SURFACE_DISPLACE_COLOR)) && defined(SURFACE_DEPTH_INPUTS)
    #if defined(HAS_VERTEX_COLOR_VEC3)
        #define SURFACE_VERTEX_COLOR_INPUT
        in vec3 a_color;
    #elif defined(HAS_VERTEX_COLOR_VEC4)
        #define SURFACE_VERTEX_COLOR_INPUT
        in vec4 a_color;
    #endif
#elif defined(HAS_LATTICE)
    #if defined(HAS_VERTEX_COLOR_VEC3)
        in vec3 a_color;
    #elif defined(HAS_VERTEX_COLOR_VEC4)
        in vec4 a_color;
    #endif
#endif

#ifdef HAS_INSTANCING
    in vec4 i_matrix_col1;
    in vec4 i_matrix_col2;
    in vec4 i_matrix_col3;
    in vec4 i_matrix_col4;
    #if defined(SURFACE_COLOR) && defined(SURFACE_DEPTH_INPUTS)
        in vec4 i_color;
    #endif
    #ifdef HAS_UV_SET1
        in vec4 i_textureRect;
    #endif
#endif

#ifdef SURFACE_DEPTH_VARYINGS
    #ifdef SURFACE_UV1
        out vec2 v_uv1;
    #endif
    #ifdef SURFACE_COLOR
        out vec4 v_color;
    #endif
#endif

#include "includes/skinning.glsl"
#include "includes/instance_normal.glsl"
#include "includes/morphtarget.glsl"
#include "includes/lattice.glsl"
#include "includes/vat.glsl"

// as includes/surface_vs.glsl
#ifdef HAS_NORMALS
    #define SURFACE_REST_NORMAL a_normal
#else
    #define SURFACE_REST_NORMAL a_smoothNormal
#endif

// the object-space smoothed normal the hook may read (SURFACE_SMOOTH_NORMALS)
vec3 surfaceSmoothNormal;
// the instance distance fade's scale the hooks may read: 1 shown, 0 shrunk away (always 1
// without USE_INSTANCE_FADE)
float surfaceFadeVisible;

#ifdef SURFACE_DISPLACE
    vec3 surfaceDisplace(vec3 objectPosition, vec3 objectNormal, vec4 color, vec2 uv);
#endif

void main() {
    // the baked vertex animation first (HAS_VAT), as includes/surface_vs.glsl
    vatEvaluate();
    mat4 boneTransform = getBoneTransform();

    vec3 position = getMorphPosition(vatPosition(a_position));
    #ifdef HAS_LATTICE
        // as includes/surface_vs.glsl (no tangent here)
        float latticeStretchValue = 1.0;
        vec3 latticeNormal = vec3(0.0, 1.0, 0.0);
        #ifdef HAS_NORMALS
            latticeNormal = getMorphNormal(vatNormal(a_normal));
        #endif
        #ifdef SURFACE_SMOOTH_NORMAL_INPUT
            vec3 latticeSmoothNormal = vatSmoothNormal(a_smoothNormal, SURFACE_REST_NORMAL);
        #else
            vec3 latticeSmoothNormal = latticeNormal;
        #endif
        vec3 latticeTangent = vec3(1.0, 0.0, 0.0);
        #ifdef HAS_SKINNING
            latticeDeformSmooth(0, position, latticeNormal, latticeSmoothNormal, latticeTangent, latticeStretchValue);
        #endif
    #endif
    position = getSkinPosition(position, boneTransform);
    #ifdef HAS_TERRAIN
        position = getTerrainPosition(position, depthParams.modelMatrix);
    #endif

    vec3 objectNormal = vec3(0.0, 1.0, 0.0);
    #ifdef HAS_NORMALS
        #ifdef HAS_LATTICE
            objectNormal = getSkinNormal(latticeNormal, boneTransform);
        #else
            objectNormal = getSkinNormal(getMorphNormal(vatNormal(a_normal)), boneTransform);
        #endif
        #ifdef HAS_TERRAIN
            objectNormal = getTerrainNormal(objectNormal, position);
        #endif
        objectNormal = normalize(objectNormal);
    #endif

    #ifdef SURFACE_SMOOTH_NORMAL_INPUT
        #ifdef HAS_LATTICE
            vec3 smoothNormal = normalize(getSkinNormal(latticeSmoothNormal, boneTransform));
        #else
            vec3 smoothNormal = normalize(getSkinNormal(vatSmoothNormal(a_smoothNormal, SURFACE_REST_NORMAL), boneTransform));
        #endif
    #endif

    #ifdef HAS_INSTANCING
        mat4 instanceMatrix = mat4(vec4(i_matrix_col1.xyz, 0.0), i_matrix_col2, i_matrix_col3, i_matrix_col4);
        vec4 pos = instanceMatrix * vec4(position, 1.0);
        objectNormal = normalize(instanceNormal(mat3(instanceMatrix), objectNormal));
        #ifdef SURFACE_SMOOTH_NORMAL_INPUT
            smoothNormal = normalize(instanceNormal(mat3(instanceMatrix), smoothNormal));
        #endif
    #else
        vec4 pos = vec4(position, 1.0);
    #endif

    #ifdef SURFACE_SMOOTH_NORMAL_INPUT
        surfaceSmoothNormal = smoothNormal;
    #else
        surfaceSmoothNormal = objectNormal;
    #endif

    surfaceFadeVisible = 1.0;
    #ifdef USE_INSTANCE_FADE
        vec3 fadeOrigin = i_matrix_col4.xyz;
        float fadeVisible = instanceFadeVisible(fadeOrigin);
        pos.xyz = mix(fadeOrigin, pos.xyz, fadeVisible);
        surfaceFadeVisible = fadeVisible;
    #endif

    #ifdef HAS_LATTICE
        #ifdef SURFACE_SMOOTH_NORMAL_INPUT
            vec3 latticeHookNormal = smoothNormal;
        #else
            vec3 latticeHookNormal = objectNormal;
        #endif
        latticeDeformSmooth(1, pos.xyz, objectNormal, latticeHookNormal, latticeTangent, latticeStretchValue);
        #ifdef SURFACE_SMOOTH_NORMAL_INPUT
            surfaceSmoothNormal = latticeHookNormal;
        #else
            surfaceSmoothNormal = objectNormal;
        #endif
    #endif

    // the raw vertex colour (the hook's), and times the instance colour (v_color)
    vec4 rawColor = vec4(1.0);
    #if defined(SURFACE_VERTEX_COLOR_INPUT) && defined(HAS_VERTEX_COLOR_VEC3)
        rawColor = vec4(a_color, 1.0);
    #elif defined(SURFACE_VERTEX_COLOR_INPUT)
        rawColor = a_color;
    #endif
    #ifdef SURFACE_NO_VERTEX_COLOR
        vec4 color = vec4(1.0);
    #else
        vec4 color = rawColor;
    #endif
    #if defined(HAS_INSTANCING) && defined(SURFACE_COLOR) && defined(SURFACE_DEPTH_INPUTS)
        color *= i_color;
    #endif

    // as includes/surface_vs.glsl's uv1
    vec2 uv1 = vec2(0.0);
    #if defined(HAS_UV_SET1) && defined(HAS_TERRAIN)
        uv1 = getTerrainUV(position);
    #elif defined(HAS_UV_SET1)
        uv1 = a_texcoord1;
        #ifdef HAS_TEXTURERECT
            uv1 = uv1 * spriteParams.textureRect.zw + spriteParams.textureRect.xy;
        #endif
        #ifdef HAS_INSTANCING
            uv1 = uv1 * i_textureRect.zw + i_textureRect.xy;
        #endif
    #endif

    #ifdef SURFACE_DISPLACE
        pos.xyz = surfaceDisplace(pos.xyz, objectNormal, rawColor, uv1);
    #endif

    #ifdef SURFACE_DEPTH_VARYINGS
        #ifdef SURFACE_UV1
            v_uv1 = uv1;
        #endif
        #ifdef SURFACE_COLOR
            v_color = color;
        #endif
    #endif

    gl_Position = depthParams.mvpMatrix * pos;

    v_projZW = gl_Position.zw;
    #ifdef IS_VULKAN
        // GL [-1,1] to Vulkan [0,1] depth range (spirv-cross fixup_clipspace equivalent)
        gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
    #endif
}
