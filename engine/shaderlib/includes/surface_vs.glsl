// Shared vertex path of library mesh shaders (library/surface.vert includes only this).
// Supports every mesh property bit that applies to a plain mesh: skinning (GL texture and
// storage-buffer paths), morph targets, instancing (matrix, colour, texture rect, fade),
// the sprite texture rect, UV sets 1-2, vertex colour (vec3/vec4), normals, tangents and
// the lighting varyings, with the same invariant position as the depth pass; and a
// Terrain (HAS_TERRAIN): its height-mapped, morphed patches, their normals, and as uv1 the
// terrain's tiled UV (Lit's v_uv1). Painted layers and blend maps are Lit's: on a terrain
// a library shader draws its own surface.
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
//   SURFACE_RAW_COLOR @require rawcolor  out vec4 v_rawColor   the vertex colour alone
//   SURFACE_SMOOTH_NORMALS @require smoothnormals  (no varying) surfaceSmoothNormal
//   SURFACE_NORMAL_MAP @require normalmap with the material's normal map under 2D lights:
//                      as SURFACE_TANGENTS (read by includes/lighting2d.glsl's getNormal2D)
//
// surfaceSmoothNormal (SURFACE_SMOOTH_NORMALS) is, for the displacement hooks below, the
// object-space normal averaged over every vertex sharing the vertex's position (the
// renderer generates it, a_smoothNormal), skinned and instanced like the normal: extruding
// along it closes the gaps a hard-edged mesh leaves at its corners (inverted-hull
// outlines). Morph targets do not move it (it is the base pose's). Only a hook reads it, so
// the attribute is declared only with SURFACE_DISPLACE or SURFACE_DISPLACE_NORMAL; without
// SURFACE_SMOOTH_NORMALS, and on a Terrain, it is the hook's objectNormal.
//
// surfaceFadeVisible is, for the hooks, how much of an instance its distance fade
// (USE_INSTANCE_FADE, InstancedMesh distance fade) keeps: 1 fully shown, falling to 0 where
// it has shrunk to its base; always 1 without the fade. objectPosition is already shrunk by
// it, so a hook that pushes vertices out by a fixed amount (an outline width) scales that
// amount by it, or a faded instance keeps a dot of that size.
//
// A required input the mesh lacks gets a fallback: the normal is the model's +Y, a UV is
// 0 and the colour white. SURFACE_NO_VERTEX_COLOR (defined before including) keeps the
// vertex colour attribute out of v_color (the instance colour still applies); the
// attribute is then read only for v_rawColor (SURFACE_RAW_COLOR) and, when the shader also
// defines SURFACE_DISPLACE_COLOR, for the displacement hook's colour (white otherwise).
//
// v_color is a tint (the instance colour multiplies it); v_rawColor is the vertex colour
// attribute alone, for data painted into it (a wind or AO mask) that an instance tint must
// not scale. Both can be required; with @require ... feature=<F> only while F is on.
//
// A shader's own .vert can move vertices before projection, in object space (after
// skinning, morphing and the instance matrix; color is the raw vertex colour, without the
// instance colour, as the depth programs see it; under SURFACE_NO_VERTEX_COLOR it is
// white unless SURFACE_DISPLACE_COLOR is defined too):
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
// Per-vertex work of a fork at the final world position and normal (vertex lighting read
// from global shader values, u_vs_globals: docs/mcp/guide/shaders.md "Global values"), with
// the vertex colour and UV the varyings get; it writes varyings of its own:
//
//   #define SURFACE_VERTEX
//   #include "includes/surface_vs.glsl"
//   out vec3 v_light;
//   void surfaceVertex(vec3 worldPosition, vec3 worldNormal, vec4 color, vec2 uv){ v_light = ...; }
//
// Lattices (HAS_LATTICE, includes/lattice.glsl): the vertex goes through the lattices bound
// to the mesh with its normal, smoothed normal and tangent, the before-skinning ones on the
// bind pose, the others after the instance matrix and the instance fade, so the hooks above
// see the deformed object-space position and normals. A shader that reads the volume ratio
// in its fragment stage (latticeStretch(), library/lattice_stretch) defines LATTICE_STRETCH
// before including, and in its .frag LATTICE_FS and LATTICE_STRETCH before including
// includes/lattice.glsl.
//
// Vertex Animation (HAS_VAT, includes/vat.glsl): a Vertex Animator's baked animation moves the
// rest position, normal, tangent and smoothed normal first, in place of the morph targets and
// the skinning the variant has not got, so lattices and the hooks above see the animated vertex.
//
// Lighting (includes/lights.glsl, which documents the units):
//   - USE_PUNCTUAL (pixel, mixed): the compiler adds SURFACE_POSITION and SURFACE_NORMALS,
//     so v_position and v_normal are written whatever @require says; the fragment stage
//     must read them (its lights need them anyway).
//   - USE_VERTEX_LIGHTS (vertex, mixed): out vec3 v_vertexLight, the vertex lights' Lambert
//     diffuse (vertexDiffuse), plus ambientLight when USE_PUNCTUAL is off (vertex mode: the
//     fragment stage has no lights block). Light to multiply by albedo, as is. In vertex
//     mode a shader that handles the ambient itself defines, before including, either
//     SURFACE_NO_VERTEX_AMBIENT (v_vertexLight leaves it out) or SURFACE_VERTEX_AMBIENT
//     (it comes apart, in out vec3 v_vertexAmbient, which the fragment stage must read).
//   - USE_SHADOWS (pixel lights that receive shadows): nothing here; the fragment stage
//     calls lightShadow, which computes the shadow coordinates from v_position.

uniform u_vs_pbrParams {
    mat4 modelMatrix;
    mat4 normalMatrix;
    mat4 mvpMatrix;
} pbrParams;

// same expression as the depth programs, so a depth pre-pass and this pass agree bit for bit
invariant gl_Position;

// @require normalmap (SURFACE_NORMAL_MAP): a variant with the material's normal map, tangents
// and uv1 (the engine gives it the 2D lights' normal map, RenderSystem) writes the tangent
// frame, which includes/lighting2d.glsl declares and reads in the fragment stage (the same
// test there)
#if defined(SURFACE_NORMAL_MAP) && defined(HAS_NORMAL_MAP) && defined(HAS_TANGENTS) && defined(HAS_UV_SET1)
    #define SURFACE_NORMAL_MAP_ACTIVE
    #ifndef SURFACE_TANGENTS
        #define SURFACE_TANGENTS
    #endif
#endif

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
// read by a displacement hook only (see surfaceSmoothNormal above)
#if defined(SURFACE_SMOOTH_NORMALS) && !defined(HAS_TERRAIN) && (defined(SURFACE_DISPLACE) || defined(SURFACE_DISPLACE_NORMAL))
    #define SURFACE_SMOOTH_NORMAL_INPUT
    in vec3 a_smoothNormal;
#endif
#if defined(HAS_UV_SET1) && !defined(HAS_TERRAIN)
    in vec2 a_texcoord1;
#endif
#ifdef HAS_TERRAIN
    // the patch position, height and morph (its own surface varyings left out)
    #define TERRAIN_NO_SURFACE_VARYINGS
    #include "includes/terrain_vs.glsl"
#endif
#ifdef HAS_UV_SET2
    in vec2 a_texcoord2;
#endif
// the vertex colour attribute is declared only where something reads it (a GL driver drops
// an unread attribute): the surface, or a lattice's vertex colour mask
#if !defined(SURFACE_NO_VERTEX_COLOR) || defined(SURFACE_RAW_COLOR) || defined(SURFACE_DISPLACE_COLOR)
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
// centroid under HAS_TEXTURERECT (includes/surface_uv.glsl, which fragment stages use too)
#include "includes/surface_uv.glsl"
#ifdef SURFACE_UV1
    SURFACE_UV_OUT vec2 v_uv1;
#endif
#ifdef SURFACE_UV2
    SURFACE_UV_OUT vec2 v_uv2;
#endif
#ifdef SURFACE_COLOR
    out vec4 v_color;
#endif
#ifdef SURFACE_RAW_COLOR
    out vec4 v_rawColor;
#endif

#ifdef USE_VERTEX_LIGHTS
    #ifndef LIGHTS_VS
        #define LIGHTS_VS
    #endif
    #include "includes/lights.glsl"
    out vec3 v_vertexLight;
    #if defined(SURFACE_VERTEX_AMBIENT) && !defined(USE_PUNCTUAL)
        out vec3 v_vertexAmbient;
    #endif
#endif

#include "includes/skinning.glsl"
#include "includes/instance_normal.glsl"
#include "includes/morphtarget.glsl"
#include "includes/lattice.glsl"
#include "includes/vat.glsl"
#ifdef SURFACE_TANGENTS
    // mirrorTangentFrame, tangentFrameMirror: a mirroring transform or texture rect
    #include "includes/tangent_frame.glsl"
#endif

// the rest normal vatSmoothNormal turns the smoothed normal from (the smoothed one itself on a
// mesh without normals)
#ifdef HAS_NORMALS
    #define SURFACE_REST_NORMAL a_normal
#else
    #define SURFACE_REST_NORMAL a_smoothNormal
#endif

// the object-space smoothed normal the hooks may read (SURFACE_SMOOTH_NORMALS)
vec3 surfaceSmoothNormal;
// the instance distance fade's scale the hooks may read: 1 shown, 0 shrunk away (always 1
// without USE_INSTANCE_FADE)
float surfaceFadeVisible;

#ifdef SURFACE_DISPLACE
    // object-space position (after skinning, morph targets and the instance matrix), its
    // normal, the raw vertex colour and the first UV set; returns the moved position
    vec3 surfaceDisplace(vec3 objectPosition, vec3 objectNormal, vec4 color, vec2 uv);
#endif
#ifdef SURFACE_DISPLACE_NORMAL
    // the same inputs as surfaceDisplace; returns the moved object-space normal
    vec3 surfaceDisplaceNormal(vec3 objectPosition, vec3 objectNormal, vec4 color, vec2 uv);
#endif
#ifdef SURFACE_VERTEX
    void surfaceVertex(vec3 worldPosition, vec3 worldNormal, vec4 color, vec2 uv);
#endif

// the vertex colour attribute, white without one
vec4 surfaceRawVertexColor(){
    vec4 color = vec4(1.0);
    #if defined(SURFACE_VERTEX_COLOR_INPUT) && defined(HAS_VERTEX_COLOR_VEC3)
        color = vec4(a_color, 1.0);
    #elif defined(SURFACE_VERTEX_COLOR_INPUT)
        color = a_color;
    #endif
    return color;
}

// v_color: the vertex colour (not with SURFACE_NO_VERTEX_COLOR) times the instance colour
vec4 surfaceVertexColor(vec4 rawColor){
    #ifdef SURFACE_NO_VERTEX_COLOR
        vec4 color = vec4(1.0);
    #else
        vec4 color = rawColor;
    #endif
    #if defined(HAS_INSTANCING) && defined(SURFACE_COLOR)
        color *= i_color;
    #endif
    return color;
}

vec2 surfaceUV1(vec3 objectPosition){
    vec2 uv = vec2(0.0);
    #if defined(HAS_UV_SET1) && defined(HAS_TERRAIN)
        uv = getTerrainUV(objectPosition);
    #elif defined(HAS_UV_SET1)
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
    // the baked vertex animation first (HAS_VAT): it takes the place of morph targets and skinning
    vatEvaluate();
    mat4 boneTransform = getBoneTransform();

    vec3 position = getMorphPosition(vatPosition(a_position));
    #ifdef HAS_LATTICE
        // the before-skinning lattices deform the bind pose: position, normals and tangent
        // together, then skinned as without a lattice
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
        #if defined(SURFACE_TANGENTS) && defined(HAS_TANGENTS)
            vec3 latticeTangent = getMorphTangent(vatTangent(a_tangent.xyz));
        #else
            vec3 latticeTangent = vec3(1.0, 0.0, 0.0);
        #endif
        #ifdef HAS_SKINNING
            latticeDeformSmooth(0, position, latticeNormal, latticeSmoothNormal, latticeTangent, latticeStretchValue);
        #endif
    #endif
    position = getSkinPosition(position, boneTransform);
    #ifdef HAS_TERRAIN
        position = getTerrainPosition(position, pbrParams.modelMatrix);
    #endif

    // the model's +Y when the mesh has no normals
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
        // skinned like the normal; morph targets leave it at the base pose
        #ifdef HAS_LATTICE
            vec3 smoothNormal = normalize(getSkinNormal(latticeSmoothNormal, boneTransform));
        #else
            vec3 smoothNormal = normalize(getSkinNormal(vatSmoothNormal(a_smoothNormal, SURFACE_REST_NORMAL), boneTransform));
        #endif
    #endif

    #if defined(SURFACE_TANGENTS) && defined(HAS_TANGENTS)
        #ifdef HAS_LATTICE
            vec3 objectTangent = normalize(getSkinTangent(latticeTangent, boneTransform));
        #else
            vec3 objectTangent = normalize(getSkinTangent(getMorphTangent(vatTangent(a_tangent.xyz)), boneTransform));
        #endif
    #endif

    #ifdef HAS_INSTANCING
        mat4 instanceMatrix = mat4(vec4(i_matrix_col1.xyz, 0.0), i_matrix_col2, i_matrix_col3, i_matrix_col4);
        vec4 pos = instanceMatrix * vec4(position, 1.0);
        objectNormal = normalize(instanceNormal(mat3(instanceMatrix), objectNormal));
        #ifdef SURFACE_SMOOTH_NORMAL_INPUT
            smoothNormal = normalize(instanceNormal(mat3(instanceMatrix), smoothNormal));
        #endif
        #if defined(SURFACE_TANGENTS) && defined(HAS_TANGENTS)
            objectTangent = mat3(instanceMatrix) * objectTangent;
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
        // Shrink toward the instance base, as mesh.vert and the depth pass do
        vec3 fadeOrigin = i_matrix_col4.xyz;
        float fadeVisible = 1.0;
        if (fadeParams.fadeRange.y > fadeParams.fadeRange.x){
            float fadeDistance = length(fadeParams.fadeEye.xz - fadeOrigin.xz);
            fadeVisible = clamp((fadeParams.fadeRange.y - fadeDistance) /
                (fadeParams.fadeRange.y - fadeParams.fadeRange.x), 0.0, 1.0);
        }
        pos.xyz = mix(fadeOrigin, pos.xyz, fadeVisible);
        surfaceFadeVisible = fadeVisible;
    #endif

    #ifdef HAS_LATTICE
        // the after-skinning lattices, before the hooks and the varyings
        #ifdef SURFACE_SMOOTH_NORMAL_INPUT
            vec3 latticeHookNormal = smoothNormal;
        #else
            vec3 latticeHookNormal = objectNormal;
        #endif
        #if defined(SURFACE_TANGENTS) && defined(HAS_TANGENTS)
            latticeDeformSmooth(1, pos.xyz, objectNormal, latticeHookNormal, objectTangent, latticeStretchValue);
        #else
            latticeDeformSmooth(1, pos.xyz, objectNormal, latticeHookNormal, latticeTangent, latticeStretchValue);
        #endif
        #ifdef SURFACE_SMOOTH_NORMAL_INPUT
            surfaceSmoothNormal = latticeHookNormal;
        #else
            surfaceSmoothNormal = objectNormal;
        #endif
        latticeWriteStretch(latticeStretchValue);
    #endif

    vec4 rawColor = surfaceRawVertexColor();
    vec4 color = surfaceVertexColor(rawColor);
    vec2 uv1 = surfaceUV1(position);

    #ifdef SURFACE_DISPLACE_NORMAL
        // from the undisplaced inputs, as the depth programs' surfaceDisplace sees them
        vec3 displacedNormal = normalize(surfaceDisplaceNormal(pos.xyz, objectNormal, rawColor, uv1));
    #endif
    #ifdef SURFACE_DISPLACE
        pos.xyz = surfaceDisplace(pos.xyz, objectNormal, rawColor, uv1);
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
            // skinned and instanced above (with the normal)
            float tangentSign = a_tangent.w;
        #else
            // any direction across the normal when the mesh has no tangents
            vec3 objectTangent = abs(objectNormal.y) < 0.999 ? cross(vec3(0.0, 1.0, 0.0), objectNormal) : vec3(1.0, 0.0, 0.0);
            float tangentSign = 1.0;
            #ifdef HAS_INSTANCING
                objectTangent = mat3(instanceMatrix) * objectTangent;
            #endif
        #endif
        vec3 worldTangent = normalize(vec3(pbrParams.modelMatrix * vec4(objectTangent, 0.0)));
        // re-orthogonalised against the (possibly fallback) normal
        worldTangent = normalize(worldTangent - worldNormal * dot(worldNormal, worldTangent));
        vec3 worldBitangent = cross(worldNormal, worldTangent) * tangentSign;
        // mirrored by a negative determinant and a mirrored texture rect
        // (includes/tangent_frame.glsl); the fallback tangent is not tied to u, so only by the
        // determinant
        #ifdef HAS_TANGENTS
            mirrorTangentFrame(worldTangent, worldBitangent);
        #else
            worldBitangent *= tangentFrameMirror();
        #endif
        v_tangent = worldTangent;
        v_bitangent = worldBitangent;
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
    #ifdef SURFACE_RAW_COLOR
        v_rawColor = rawColor;
    #endif

    #ifdef USE_VERTEX_LIGHTS
        v_vertexLight = vertexDiffuse(worldPos.xyz / worldPos.w, worldNormal);
        #ifndef USE_PUNCTUAL
            // vertex mode: the fragment stage has no lights block, the SH ambient (the scene
            // ambient and the lights over the cap) comes along
            #if defined(SURFACE_VERTEX_AMBIENT)
                v_vertexAmbient = ambientLight(worldNormal);
            #elif !defined(SURFACE_NO_VERTEX_AMBIENT)
                v_vertexLight += ambientLight(worldNormal);
            #endif
        #endif
    #endif

    #ifdef SURFACE_VERTEX
        surfaceVertex(worldPos.xyz / worldPos.w, worldNormal, color, uv1);
    #endif

    gl_Position = pbrParams.mvpMatrix * pos;
    #ifdef IS_VULKAN
        // GL [-1,1] to Vulkan [0,1] depth range (spirv-cross fixup_clipspace equivalent)
        gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
    #endif
}
