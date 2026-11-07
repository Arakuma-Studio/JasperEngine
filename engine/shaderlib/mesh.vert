#version 450

// A fork may declare a u_vs_customParams block: its members are filled by name from the
// component's shader uniforms (Properties window, setShaderUniform). Two names are
// written by the engine instead: "time" (seconds since startup) and "resolution"
// (xy = render target size, zw = 1 / size). Do not mix int and float members: GL
// uploads the block typed after its first member.
//
// The fork drives the color pass only: to keep shadows (and SSAO, when SSR is off) in
// step with positions displaced here, fork the depth shader too (Depth Shader in Properties).
//
// A mesh deformed by lattices compiles with HAS_LATTICE: main() runs the vertex through
// latticeDeform (includes/lattice.glsl). A fork without that call draws the mesh undeformed;
// to follow the lattices it includes the file and calls it as main() does, before the model
// matrix (the depth fork too).
//
// A mesh played by a Vertex Animator compiles with HAS_VAT (and HAS_VAT_BONES in Bones mode),
// without skinning or morph targets: main() calls vatEvaluate() first and the rest position,
// normal and tangent go through vatPosition, vatNormal and vatTangent (includes/vat.glsl). A
// fork without them draws the mesh at rest; to play the animation it includes the file and
// calls them as main() does (the depth fork too).
//
//   uniform u_vs_customParams {
//       float time;
//       vec4 tint;
//   } customParams;

// the object's matrices: per draw (u_vs_pbrParams), or per instance in an automatically
// instanced batch (HAS_AUTO_INSTANCING); a fork keeping its own block is never batched
#include "includes/object_params.glsl"

// same expression as depth.vert, so a depth prepass and this pass agree bit for bit
invariant gl_Position;

#ifdef USE_INSTANCE_FADE
    #include "includes/instance_fade.glsl"
#endif

#if defined(HAS_TEXTURERECT) && (defined(HAS_UV_SET1) || defined(HAS_UV_SET2))
    uniform u_vs_spriteParams {
        vec4 textureRect;
    } spriteParams;
#endif

in vec3 a_position;

#if !defined(MATERIAL_UNLIT) || defined(USE_MIRROR) || defined(USE_LIGHT2D)
    out vec3 v_position;
#endif

#ifdef HAS_NORMALS
    in vec3 a_normal;
#endif

#ifdef HAS_TANGENTS
    in vec4 a_tangent;
#endif

#ifdef HAS_NORMALS
#ifdef HAS_TANGENTS
    out mat3 v_tbn;
#else
    out vec3 v_normal;
#endif
#endif

// with MSAA an edge pixel is shaded at its center, which can lie outside a sprite
// or tile quad; centroid keeps the atlas UV inside the rect
#ifdef HAS_UV_SET1
    #ifndef HAS_TERRAIN
        in vec2 a_texcoord1;
    #endif
    #ifdef HAS_TEXTURERECT
        centroid out vec2 v_uv1;
    #else
        out vec2 v_uv1;
    #endif
#endif

#ifdef HAS_UV_SET2
    in vec2 a_texcoord2;
    #ifdef HAS_TEXTURERECT
        centroid out vec2 v_uv2;
    #else
        out vec2 v_uv2;
    #endif
#endif

#ifdef HAS_VERTEX_COLOR_VEC3
    in vec3 a_color;
#endif

#ifdef HAS_VERTEX_COLOR_VEC4
    in vec4 a_color;
#endif
// instancing multiplies by the vec4 instance color, so a vec3 attribute is promoted
#if defined(HAS_VERTEX_COLOR_VEC3) && !defined(HAS_INSTANCING)
    out vec3 v_color;
#endif
#if defined(HAS_VERTEX_COLOR_VEC4) || defined(HAS_INSTANCING)
    out vec4 v_color;
#endif

#ifdef USE_VERTEX_LIGHTS
    // the object's vertex lights (includes/lights.glsl), Lambert per vertex
    #define LIGHTS_VS
    #include "includes/lights.glsl"

    out vec3 v_vertexLight;
#endif

#ifdef HAS_INSTANCING
    in vec4 i_matrix_col1;
    in vec4 i_matrix_col2;
    in vec4 i_matrix_col3;
    in vec4 i_matrix_col4;
    in vec4 i_color;
    #if defined(HAS_UV_SET1) || defined(HAS_UV_SET2)
        in vec4 i_textureRect;
    #endif
#endif

#define LOD_FADE_VS
#include "includes/lod_fade.glsl"
#include "includes/skinning.glsl"
#include "includes/instance_normal.glsl"
#include "includes/morphtarget.glsl"
#ifdef HAS_TERRAIN
    #include "includes/terrain_vs.glsl"
#endif
#ifdef HAS_VOXEL
    #include "includes/voxel_vs.glsl"
#endif
// the lattices deforming the mesh (HAS_LATTICE): latticeDeform, after a_color is declared
#include "includes/lattice.glsl"
// the baked vertex animation (HAS_VAT): vatEvaluate, vatPosition, vatNormal, vatTangent
#include "includes/vat.glsl"
// a mirroring transform or texture rect turns the tangent frame (HAS_TANGENTS): mirrorTangentFrame
#ifdef HAS_TANGENTS
    #include "includes/tangent_frame.glsl"
#endif

vec4 getPosition(mat4 boneTransform){
    vec3 pos = vatPosition(a_position);

    pos = getMorphPosition(pos);
    pos = getSkinPosition(pos, boneTransform);
    #ifdef HAS_TERRAIN
        pos = getTerrainPosition(pos, pbrParams.modelMatrix);
    #endif

    return vec4(pos, 1.0);
}

#ifdef HAS_NORMALS
vec3 getNormal(mat4 boneTransform, vec4 position){
    vec3 normal = vatNormal(a_normal);

    normal = getMorphNormal(normal);
    normal = getSkinNormal(normal, boneTransform);
    #ifdef HAS_TERRAIN
        normal = getTerrainNormal(normal, position.xyz);
    #endif

    return normalize(normal);
}
#endif

#ifdef HAS_TANGENTS
vec3 getTangent(mat4 boneTransform){
    vec3 tangent = vatTangent(a_tangent.xyz);

    tangent = getMorphTangent(tangent);
    tangent = getSkinTangent(tangent, boneTransform);

    return normalize(tangent);
}
#endif

void main() {
    #ifdef USE_LOD_FADE
        // an instance matrix is affine (its first column's w is 0) and so is a normal matrix's
        // direction part: those lanes carry the fade of a fading draw (includes/lod_fade.glsl)
        #ifdef HAS_INSTANCING
            v_lodFade = i_matrix_col1.w;
        #else
            v_lodFade = objectLodFade();
        #endif
    #endif
    // the baked vertex animation first (HAS_VAT): it takes the place of morph targets and skinning
    vatEvaluate();
    mat4 boneTransform = getBoneTransform();

    #ifdef HAS_LATTICE
        // position, normal and tangent go through each lattice together (one Jacobian):
        // morph targets, the before-skinning lattices, skinning, the instance matrix, then
        // (below, after the instance fade) the after-skinning ones
        vec3 latticePosition = getMorphPosition(vatPosition(a_position));
        vec3 latticeNormal = vec3(0.0, 1.0, 0.0);
        vec3 latticeTangent = vec3(1.0, 0.0, 0.0);
        #ifdef HAS_NORMALS
            latticeNormal = getMorphNormal(vatNormal(a_normal));
        #endif
        #ifdef HAS_TANGENTS
            latticeTangent = getMorphTangent(vatTangent(a_tangent.xyz));
        #endif
        float latticeStretchValue = 1.0;
        #ifdef HAS_SKINNING
            latticeDeform(0, latticePosition, latticeNormal, latticeTangent, latticeStretchValue);
        #endif
        // normalised where getNormal and getTangent normalise, so a lattice at rest draws
        // exactly what the variant without one does
        vec4 pos = vec4(getSkinPosition(latticePosition, boneTransform), 1.0);
        latticeNormal = normalize(getSkinNormal(latticeNormal, boneTransform));
        latticeTangent = normalize(getSkinTangent(latticeTangent, boneTransform));
        #ifdef HAS_INSTANCING
            mat4 instanceMatrix = mat4(vec4(i_matrix_col1.xyz, 0.0), i_matrix_col2, i_matrix_col3, i_matrix_col4);
            pos = instanceMatrix * pos;
            latticeNormal = instanceNormal(mat3(instanceMatrix), latticeNormal);
            latticeTangent = mat3(instanceMatrix) * latticeTangent;
        #endif
    #elif defined(HAS_INSTANCING)
        mat4 instanceMatrix = mat4(vec4(i_matrix_col1.xyz, 0.0), i_matrix_col2, i_matrix_col3, i_matrix_col4);
        vec4 pos = instanceMatrix * getPosition(boneTransform);
    #else
        vec4 pos = getPosition(boneTransform);
    #endif

    #ifdef USE_INSTANCE_FADE
        // Shrink toward the instance base, on XZ like the chunk selection so looking down does
        // not eat grass still inside the spawn square. An empty range keeps everything solid.
        vec3 fadeOrigin = i_matrix_col4.xyz;
        float fadeVisible = instanceFadeVisible(fadeOrigin);
        pos.xyz = mix(fadeOrigin, pos.xyz, fadeVisible);
    #endif

    #ifdef HAS_LATTICE
        latticeDeform(1, pos.xyz, latticeNormal, latticeTangent, latticeStretchValue);
        latticeWriteStretch(latticeStretchValue);
    #endif

    vec4 worldPos = pbrParams.modelMatrix * pos;

    #if !defined(MATERIAL_UNLIT) || defined(USE_MIRROR) || defined(USE_LIGHT2D)
        v_position = vec3(worldPos.xyz) / worldPos.w;
    #endif

    #ifdef HAS_NORMALS
        #ifdef HAS_LATTICE
            vec3 objectNormal = latticeNormal;
        #else
            vec3 objectNormal = getNormal(boneTransform, pos);
            #ifdef HAS_INSTANCING
                objectNormal = instanceNormal(mat3(instanceMatrix), objectNormal);
            #endif
        #endif
        vec3 worldNormal = normalize(vec3(pbrParams.normalMatrix * vec4(objectNormal, 0.0)));
    #ifdef HAS_TANGENTS
        #ifdef HAS_LATTICE
            vec3 tangent = latticeTangent;
        #else
            vec3 tangent = getTangent(boneTransform);
            #ifdef HAS_INSTANCING
                tangent = mat3(instanceMatrix) * tangent;
            #endif
        #endif
        vec3 tangentW = normalize(vec3(pbrParams.modelMatrix * vec4(tangent, 0.0)));
        vec3 bitangentW = cross(worldNormal, tangentW) * a_tangent.w;
        // a negative determinant or a mirrored texture rect (includes/tangent_frame.glsl)
        mirrorTangentFrame(tangentW, bitangentW);
        v_tbn = mat3(tangentW, bitangentW, worldNormal);
    #else // !HAS_TANGENTS
        v_normal = worldNormal;
    #endif
    #endif

    #if defined(HAS_TERRAIN_PBR) && defined(HAS_NORMALS)
        setTerrainShadingAxes(pbrParams.normalMatrix);
    #endif
    #ifdef HAS_VOXEL
        setVoxelVaryings(a_position, a_normal, pbrParams.normalMatrix);
    #endif

    #ifdef HAS_UV_SET1
        v_uv1 = vec2(0.0, 0.0);
    #endif
    #ifdef HAS_UV_SET2
        v_uv2 = vec2(0.0, 0.0);
    #endif

    #ifdef HAS_UV_SET1
        #ifndef HAS_TERRAIN
            v_uv1 = a_texcoord1;
            #ifdef HAS_TEXTURERECT
                v_uv1 = v_uv1 * spriteParams.textureRect.zw + spriteParams.textureRect.xy;
            #endif
            #ifdef HAS_INSTANCING
                v_uv1 = v_uv1 * i_textureRect.zw + i_textureRect.xy;
            #endif
        #else
            v_uv1 = getTerrainTiledTexture(pos.xyz);
        #endif
    #endif

    #ifdef HAS_UV_SET2
        v_uv2 = a_texcoord2;
        #ifdef HAS_TEXTURERECT
            v_uv2 = v_uv2 * spriteParams.textureRect.zw + spriteParams.textureRect.xy;
        #endif
        #ifdef HAS_INSTANCING
            v_uv2 = v_uv2 * i_textureRect.zw + i_textureRect.xy;
        #endif
    #endif

    #ifdef HAS_INSTANCING
        #ifdef HAS_VERTEX_COLOR_VEC3
            v_color = vec4(a_color, 1.0) * i_color;
        #elif defined(HAS_VERTEX_COLOR_VEC4)
            v_color = a_color * i_color;
        #else
            v_color = i_color;
        #endif
    #else
        #if defined(HAS_VERTEX_COLOR_VEC3) || defined(HAS_VERTEX_COLOR_VEC4)
            v_color = a_color;
        #endif
    #endif

    #ifdef USE_VERTEX_LIGHTS
        #ifdef HAS_NORMALS
            vec3 lightingNormal = worldNormal;
        #else
            vec3 lightingNormal = normalize(vec3(pbrParams.normalMatrix * vec4(0.0, 1.0, 0.0, 0.0)));
        #endif
        v_vertexLight = vertexDiffuse(worldPos.xyz / worldPos.w, lightingNormal);
        #ifndef USE_PUNCTUAL
            // VERTEX: the fragment stage has no lights block, so the SH ambient (the scene
            // ambient and the lights over the cap) comes along
            v_vertexLight += ambientLight(lightingNormal);
        #endif
    #endif

    gl_Position = pbrParams.mvpMatrix * pos;
    #ifdef HAS_VOXEL
        v_voxelPositionEye.w = gl_Position.w;
    #endif
    #ifdef IS_VULKAN
        // GL [-1,1] to Vulkan [0,1] depth range (spirv-cross fixup_clipspace equivalent)
        gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
    #endif
}
