// Shared vertex path of library mesh shaders (library/surface.vert includes only this).
// Supports every mesh property bit that applies to a plain mesh: skinning (GL texture and
// storage-buffer paths), morph targets, instancing (matrix, colour, texture rect, fade),
// the sprite texture rect, UV sets 1-2, vertex colour (vec3/vec4), normals, tangents and
// the lighting varyings, with the same invariant position as the depth pass.
//
// Which varyings it writes is decided by defines both stages see, so the fragment shader
// reads exactly what is written (an output the fragment shader does not read fails the
// cross-compile). The compiler defines them from the manifest's @require:
//
//   SURFACE_POSITION  @require position  out vec3 v_position   world position
//   SURFACE_NORMALS   @require normals   out vec3 v_normal     world normal, normalised
//   SURFACE_TANGENTS  @require tangents  out vec3 v_normal, v_tangent, v_bitangent (world)
//   SURFACE_UV1       @require uv1       out vec2 v_uv1
//   SURFACE_UV2       @require uv2       out vec2 v_uv2
//   SURFACE_COLOR     @require color     out vec4 v_color      vertex x instance colour
//
// A required input the mesh lacks gets a fallback: the normal is the model's +Y, a UV is
// 0 and the colour white. SURFACE_NO_VERTEX_COLOR (defined before including) keeps the
// vertex colour attribute out of v_color (the instance colour still applies).
//
// A shader's own .vert can move vertices before projection, in object space (after
// skinning, morphing and the instance matrix):
//
//   #define SURFACE_DISPLACE
//   #include "includes/surface_vs.glsl"
//   vec3 surfaceDisplace(vec3 objectPosition, vec3 objectNormal, vec4 color, vec2 uv){ ... }
//
// Its depth program (<name>_depth.vert, see includes/surface_depth_vs.glsl) must displace
// identically, or shadows and the depth pre-pass do not match.
//
// A displacement that turns the surface (a rotation, waves) also gives the moved normal,
// from the same inputs (the position before surfaceDisplace moved it); the lighting and
// v_normal use it. The depth programs have no normals to move, so they need no such hook:
//
//   #define SURFACE_DISPLACE_NORMAL
//   vec3 surfaceDisplaceNormal(vec3 objectPosition, vec3 objectNormal, vec4 color, vec2 uv){ ... }
//
// Lighting (includes/lights.glsl, which documents the units):
//   - USE_PUNCTUAL (pixel, mixed): the compiler adds SURFACE_POSITION and SURFACE_NORMALS,
//     so v_position and v_normal are written whatever @require says; the fragment stage
//     must read them (its lights need them anyway).
//   - USE_VERTEX_LIGHTS (vertex, mixed): out vec3 v_vertexLight, the vertex lights' Lambert
//     diffuse (vertexDiffuse), plus ambientLight when USE_PUNCTUAL is off (vertex mode: the
//     fragment stage has no lights block). Light to multiply by albedo, as is.
//   - USE_SHADOWS (pixel lights that receive shadows): out vec4 v_lightProjPos[], the
//     shadow-map coordinates lightShadow reads; the fragment stage must call lightShadow.

uniform u_vs_pbrParams {
    mat4 modelMatrix;
    mat4 normalMatrix;
    mat4 mvpMatrix;
} pbrParams;

// same expression as the depth programs, so a depth pre-pass and this pass agree bit for bit
invariant gl_Position;

#if defined(SURFACE_TANGENTS) && !defined(SURFACE_NORMALS)
    #define SURFACE_NORMALS
#endif

#ifdef USE_INSTANCE_FADE
    uniform u_vs_fade {
        vec4 fadeRange; //start.x, end.y
        vec4 fadeEye;   //camera in model space.xyz
    } fadeParams;
#endif

#if defined(HAS_TEXTURERECT) && (defined(HAS_UV_SET1) || defined(HAS_UV_SET2))
    uniform u_vs_spriteParams {
        vec4 textureRect;
    } spriteParams;
#endif

in vec3 a_position;

#ifdef HAS_NORMALS
    in vec3 a_normal;
#endif
#ifdef HAS_TANGENTS
    in vec4 a_tangent;
#endif
#if defined(HAS_UV_SET1) && !defined(HAS_TERRAIN)
    in vec2 a_texcoord1;
#endif
#ifdef HAS_UV_SET2
    in vec2 a_texcoord2;
#endif
#if defined(HAS_VERTEX_COLOR_VEC3) && !defined(SURFACE_NO_VERTEX_COLOR)
    in vec3 a_color;
#endif
#if defined(HAS_VERTEX_COLOR_VEC4) && !defined(SURFACE_NO_VERTEX_COLOR)
    in vec4 a_color;
#endif

#ifdef HAS_INSTANCING
    in vec4 i_matrix_col1;
    in vec4 i_matrix_col2;
    in vec4 i_matrix_col3;
    in vec4 i_matrix_col4;
    #ifdef SURFACE_COLOR
        in vec4 i_color;
    #endif
    #if defined(HAS_UV_SET1) || defined(HAS_UV_SET2)
        in vec4 i_textureRect;
    #endif
#endif

#ifdef SURFACE_POSITION
    out vec3 v_position;
#endif
#ifdef SURFACE_NORMALS
    out vec3 v_normal;
#endif
#ifdef SURFACE_TANGENTS
    out vec3 v_tangent;
    out vec3 v_bitangent;
#endif
// with MSAA an edge pixel is shaded at its center, which can lie outside a sprite
// or tile quad; centroid keeps the atlas UV inside the rect
#ifdef SURFACE_UV1
    #ifdef HAS_TEXTURERECT
        centroid out vec2 v_uv1;
    #else
        out vec2 v_uv1;
    #endif
#endif
#ifdef SURFACE_UV2
    #ifdef HAS_TEXTURERECT
        centroid out vec2 v_uv2;
    #else
        out vec2 v_uv2;
    #endif
#endif
#ifdef SURFACE_COLOR
    out vec4 v_color;
#endif

#ifdef USE_VERTEX_LIGHTS
    #ifndef LIGHTS_VS
        #define LIGHTS_VS
    #endif
    #include "includes/lights.glsl"
    out vec3 v_vertexLight;
#endif

#ifdef USE_SHADOWS
    uniform u_vs_shadows {
        mat4 lightVPMatrix[MAX_SHADOW_ATLAS_SLOTS];
        vec4 shadowParams[MAX_SHADOW_ATLAS_SLOTS]; // normalBias in .x
    };

    out vec4 v_lightProjPos[MAX_SHADOW_ATLAS_SLOTS];
#endif

#include "includes/skinning.glsl"
#include "includes/instance_normal.glsl"
#include "includes/morphtarget.glsl"

#ifdef SURFACE_DISPLACE
    // object-space position (after skinning, morph targets and the instance matrix), its
    // normal, the vertex colour and the first UV set; returns the moved position
    vec3 surfaceDisplace(vec3 objectPosition, vec3 objectNormal, vec4 color, vec2 uv);
#endif
#ifdef SURFACE_DISPLACE_NORMAL
    // the same inputs as surfaceDisplace; returns the moved object-space normal
    vec3 surfaceDisplaceNormal(vec3 objectPosition, vec3 objectNormal, vec4 color, vec2 uv);
#endif

vec4 surfaceVertexColor(){
    vec4 color = vec4(1.0);
    #if defined(HAS_VERTEX_COLOR_VEC3) && !defined(SURFACE_NO_VERTEX_COLOR)
        color = vec4(a_color, 1.0);
    #elif defined(HAS_VERTEX_COLOR_VEC4) && !defined(SURFACE_NO_VERTEX_COLOR)
        color = a_color;
    #endif
    #if defined(HAS_INSTANCING) && defined(SURFACE_COLOR)
        color *= i_color;
    #endif
    return color;
}

vec2 surfaceUV1(){
    vec2 uv = vec2(0.0);
    #if defined(HAS_UV_SET1) && !defined(HAS_TERRAIN)
        uv = a_texcoord1;
        #ifdef HAS_TEXTURERECT
            uv = uv * spriteParams.textureRect.zw + spriteParams.textureRect.xy;
        #endif
        #ifdef HAS_INSTANCING
            uv = uv * i_textureRect.zw + i_textureRect.xy;
        #endif
    #endif
    return uv;
}

vec2 surfaceUV2(){
    vec2 uv = vec2(0.0);
    #ifdef HAS_UV_SET2
        uv = a_texcoord2;
        #ifdef HAS_TEXTURERECT
            uv = uv * spriteParams.textureRect.zw + spriteParams.textureRect.xy;
        #endif
        #ifdef HAS_INSTANCING
            uv = uv * i_textureRect.zw + i_textureRect.xy;
        #endif
    #endif
    return uv;
}

void main() {
    mat4 boneTransform = getBoneTransform();

    vec3 position = getMorphPosition(a_position);
    position = getSkinPosition(position, boneTransform);

    // the model's +Y when the mesh has no normals
    vec3 objectNormal = vec3(0.0, 1.0, 0.0);
    #ifdef HAS_NORMALS
        objectNormal = normalize(getSkinNormal(getMorphNormal(a_normal), boneTransform));
    #endif

    #ifdef HAS_INSTANCING
        mat4 instanceMatrix = mat4(i_matrix_col1, i_matrix_col2, i_matrix_col3, i_matrix_col4);
        vec4 pos = instanceMatrix * vec4(position, 1.0);
        objectNormal = normalize(instanceNormal(mat3(instanceMatrix), objectNormal));
    #else
        vec4 pos = vec4(position, 1.0);
    #endif

    #ifdef USE_INSTANCE_FADE
        // Shrink toward the instance base, as mesh.vert and the depth pass do
        vec3 fadeOrigin = i_matrix_col4.xyz;
        float fadeVisible = 1.0;
        if (fadeParams.fadeRange.y > fadeParams.fadeRange.x){
            float fadeDistance = length(fadeParams.fadeEye.xz - fadeOrigin.xz);
            fadeVisible = clamp((fadeParams.fadeRange.y - fadeDistance) /
                (fadeParams.fadeRange.y - fadeParams.fadeRange.x), 0.0, 1.0);
        }
        pos.xyz = mix(fadeOrigin, pos.xyz, fadeVisible);
    #endif

    vec4 color = surfaceVertexColor();
    vec2 uv1 = surfaceUV1();

    #ifdef SURFACE_DISPLACE_NORMAL
        // from the undisplaced inputs, as the depth programs' surfaceDisplace sees them
        vec3 displacedNormal = normalize(surfaceDisplaceNormal(pos.xyz, objectNormal, color, uv1));
    #endif
    #ifdef SURFACE_DISPLACE
        pos.xyz = surfaceDisplace(pos.xyz, objectNormal, color, uv1);
    #endif
    #ifdef SURFACE_DISPLACE_NORMAL
        objectNormal = displacedNormal;
    #endif

    vec4 worldPos = pbrParams.modelMatrix * pos;
    vec3 worldNormal = normalize(vec3(pbrParams.normalMatrix * vec4(objectNormal, 0.0)));

    #ifdef SURFACE_POSITION
        v_position = worldPos.xyz / worldPos.w;
    #endif
    #ifdef SURFACE_NORMALS
        v_normal = worldNormal;
    #endif
    #ifdef SURFACE_TANGENTS
        #ifdef HAS_TANGENTS
            vec3 objectTangent = normalize(getSkinTangent(getMorphTangent(a_tangent.xyz), boneTransform));
            float tangentSign = a_tangent.w;
        #else
            // any direction across the normal when the mesh has no tangents
            vec3 objectTangent = abs(objectNormal.y) < 0.999 ? cross(vec3(0.0, 1.0, 0.0), objectNormal) : vec3(1.0, 0.0, 0.0);
            float tangentSign = 1.0;
        #endif
        #ifdef HAS_INSTANCING
            objectTangent = mat3(instanceMatrix) * objectTangent;
        #endif
        vec3 worldTangent = normalize(vec3(pbrParams.modelMatrix * vec4(objectTangent, 0.0)));
        // re-orthogonalised against the (possibly fallback) normal
        worldTangent = normalize(worldTangent - worldNormal * dot(worldNormal, worldTangent));
        v_tangent = worldTangent;
        v_bitangent = cross(worldNormal, worldTangent) * tangentSign;
    #endif
    #ifdef SURFACE_UV1
        v_uv1 = uv1;
    #endif
    #ifdef SURFACE_UV2
        v_uv2 = surfaceUV2();
    #endif
    #ifdef SURFACE_COLOR
        v_color = color;
    #endif

    #ifdef USE_VERTEX_LIGHTS
        v_vertexLight = vertexDiffuse(worldPos.xyz / worldPos.w, worldNormal);
        #ifndef USE_PUNCTUAL
            // vertex mode: the fragment stage has no lights block, the SH ambient (the scene
            // ambient and the lights over the cap) comes along
            v_vertexLight += ambientLight(worldNormal);
        #endif
    #endif

    #ifdef USE_SHADOWS
    for (int i = 0; i < MAX_SHADOW_ATLAS_SLOTS; ++i){
        v_lightProjPos[i] = lightVPMatrix[i] * (worldPos + vec4(worldNormal * shadowParams[i].x, 0.0));
    }
    #endif

    gl_Position = pbrParams.mvpMatrix * pos;
    #ifdef IS_VULKAN
        // GL [-1,1] to Vulkan [0,1] depth range (spirv-cross fixup_clipspace equivalent)
        gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
    #endif
}
