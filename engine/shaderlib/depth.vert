#version 450

#define DEPTH_SHADER

// A fork may declare a u_vs_customParams block: its members are filled by name from the
// mesh's shader uniforms, the same values the color fork reads. Two names are written by
// the engine instead: "time" (seconds since startup) and "resolution" (xy = shadow slot
// or depth target size, zw = 1 / size). Do not mix int and float members: GL uploads the
// block typed after its first member.
//
//   uniform u_vs_customParams {
//       float time;
//       float amplitude;
//   } customParams;

// mvpMatrix is lightVPMatrix * modelMatrix computed once, so a depth prepass matches
// mesh.vert bit for bit; a fork may still multiply the two itself
uniform u_vs_depthParams {
    mat4 modelMatrix;
    mat4 lightVPMatrix;
    mat4 mvpMatrix;
} depthParams;

invariant gl_Position;

#ifdef USE_INSTANCE_FADE
    uniform u_vs_fade {
        vec4 fadeRange; //start.x, end.y
        vec4 fadeEye;   //camera in model space.xyz
    } fadeParams;
#endif

in vec3 a_position;
out vec2 v_projZW;

#ifdef HAS_INSTANCING
    in vec4 i_matrix_col1;
    in vec4 i_matrix_col2;
    in vec4 i_matrix_col3;
    in vec4 i_matrix_col4;
    #if defined(HAS_TEXTURE)
        in vec4 i_textureRect;
    #endif
#endif

#if defined(HAS_TEXTURE)
    in vec2 a_texcoord1;
    out vec2 v_uv1;
    // the alpha clip samples the frame the colour pass draws: the sprite texture rect
    // (atlas frame) and the instance's, as mesh.vert's v_uv1
    #ifdef HAS_TEXTURERECT
        uniform u_vs_spriteParams {
            vec4 textureRect;
        } spriteParams;
    #endif
#endif

// a lattice's vertex colour mask (includes/lattice.glsl)
#ifdef HAS_LATTICE
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
#include "includes/lattice.glsl"
#include "includes/vat.glsl"

vec4 getPosition(mat4 boneTransform){
    vec3 pos = vatPosition(a_position);

    pos = getMorphPosition(pos);
    pos = getSkinPosition(pos, boneTransform);
    #ifdef HAS_TERRAIN
        pos = getTerrainPosition(pos, depthParams.modelMatrix);
    #endif

    return vec4(pos, 1.0);
}

void main() {
    // the baked vertex animation first (HAS_VAT), as mesh.vert
    vatEvaluate();
    mat4 boneTransform = getBoneTransform();

    #ifdef HAS_LATTICE
        // as mesh.vert, so both passes compute the same position (the normal and tangent
        // are not used here)
        vec3 latticePosition = getMorphPosition(vatPosition(a_position));
        vec3 latticeNormal = vec3(0.0, 1.0, 0.0);
        vec3 latticeTangent = vec3(1.0, 0.0, 0.0);
        float latticeStretchValue = 1.0;
        #ifdef HAS_SKINNING
            latticeDeform(0, latticePosition, latticeNormal, latticeTangent, latticeStretchValue);
        #endif
        vec4 pos = vec4(getSkinPosition(latticePosition, boneTransform), 1.0);
        #ifdef HAS_INSTANCING
            mat4 instanceMatrix = mat4(vec4(i_matrix_col1.xyz, 0.0), i_matrix_col2, i_matrix_col3, i_matrix_col4);
            pos = instanceMatrix * pos;
        #endif
    #elif defined(HAS_INSTANCING)
        mat4 instanceMatrix = mat4(vec4(i_matrix_col1.xyz, 0.0), i_matrix_col2, i_matrix_col3, i_matrix_col4);
        vec4 pos = instanceMatrix *  getPosition(boneTransform);
    #else
        vec4 pos = getPosition(boneTransform);
    #endif

    #if defined(HAS_TEXTURE)
        v_uv1 = a_texcoord1;
        #ifdef HAS_TEXTURERECT
            v_uv1 = v_uv1 * spriteParams.textureRect.zw + spriteParams.textureRect.xy;
        #endif
        #ifdef HAS_INSTANCING
            v_uv1 = v_uv1 * i_textureRect.zw + i_textureRect.xy;
        #endif
    #endif

    #ifdef USE_INSTANCE_FADE
        // Matches mesh.vert exactly, so a shrinking instance shrinks its shadow in step.
        vec3 fadeOrigin = i_matrix_col4.xyz;
        float fadeVisible = 1.0;
        if (fadeParams.fadeRange.y > fadeParams.fadeRange.x){
            float fadeDistance = length(fadeParams.fadeEye.xz - fadeOrigin.xz);
            fadeVisible = clamp((fadeParams.fadeRange.y - fadeDistance) /
                (fadeParams.fadeRange.y - fadeParams.fadeRange.x), 0.0, 1.0);
        }
        pos.xyz = mix(fadeOrigin, pos.xyz, fadeVisible);
    #endif

    #ifdef HAS_LATTICE
        latticeDeform(1, pos.xyz, latticeNormal, latticeTangent, latticeStretchValue);
    #endif

    gl_Position = depthParams.mvpMatrix * pos;

    v_projZW = gl_Position.zw;
    // No Y flip here: the target keeps its native orientation and the atlas lookup
    // (getShadowAtlasUV) converts the coordinate when sampling.
    #ifdef IS_VULKAN
        // GL [-1,1] to Vulkan [0,1] depth range (spirv-cross fixup_clipspace equivalent)
        gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
    #endif
}
