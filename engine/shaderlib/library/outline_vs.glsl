// The vertex side of library:outline and library:outline_stencil (their .vert include only
// this): the shared vertex path (includes/surface_vs.glsl, so skinning, morph targets,
// instancing and the sprite rect work as for any library shader) with the mesh pushed out
// along its smoothed normal (@require smoothnormals: a hard-edged mesh stays closed at its
// corners) in the displacement hook.
//
// WIDTH_MODE_SCREEN: a constant width in pixels whatever the distance. The projected
// vertex moves width pixels across the screen in the direction its normal points on screen
// (the derivative of clip.xy / clip.w along the normal, scaled to pixels by the target's
// resolution, so the aspect ratio and the perspective divide are accounted for), within the
// view plane: its depth does not change. The offset is turned back into object space
// (view -> world by the view rotation's transpose, world -> object by the normal matrix's
// transpose, the inverse of the model matrix) because the hook returns an object-space
// position. Orthographic cameras work the same (clip.w is 1).
// WIDTH_MODE_WORLD: width world units along the normal (in world space, so a scaled object
// keeps an even width). DISTANCE_SCALE grows it with the view depth, towards a constant
// on-screen size (distanceScale 1) beyond referenceDistance, at most maxWidth; an
// orthographic camera does not scale it (its picture does not shrink with distance).
// VERTEX_COLOR_WIDTH multiplies the width by the vertex colour's red (0 hides the outline
// there, a painted mask). Every width also scales by surfaceFadeVisible, so an instance the
// distance fade shrank to its base takes its outline with it (no outline-coloured dot).
//
// Hard edges: the push follows the smoothed normal, which keeps the hull closed at a hard
// corner but leaves the band beside it narrower than width wherever the silhouette runs along
// a face rather than the corner's diagonal: WORLD offsets each face by width times the cosine
// between face and smoothed normal (width / sqrt(3) on a cube), SCREEN moves the corner width
// pixels along its diagonal on screen, so an edge between two such corners sits width times
// that cosine away (width / sqrt(2) along a cube face seen square on). Smooth meshes, and
// silhouettes running through the corners, get the full width. A crack-free correction would
// need the miter length per welded vertex (not per face vertex, which reopens the gaps), so
// none is applied: raise the width, or bevel the mesh, for an even band on boxy meshes.
//
// The outline colour never takes the vertex or instance colour (SURFACE_NO_VERTEX_COLOR).
// No depth program: an outline is an extra pass, which is in no shadow or depth pass.

#if defined(WIDTH_MODE_SCREEN) || defined(DISTANCE_SCALE)
    #define FRAME_VS
    #include "includes/frame.glsl"
#endif

#define SURFACE_NO_VERTEX_COLOR
#ifdef VERTEX_COLOR_WIDTH
    #define SURFACE_DISPLACE_COLOR
#endif
#define SURFACE_DISPLACE
#include "includes/surface_vs.glsl"

uniform u_vs_customParams {
    #ifdef WIDTH_MODE_SCREEN
        float screenWidth;       // pixels
    #else
        float worldWidth;        // world units
    #endif
    #ifdef DISTANCE_SCALE
        float distanceScale;     // 0 world width, 1 constant on-screen size beyond referenceDistance
        float referenceDistance; // view depth at which the width is worldWidth
        float maxWidth;          // world units
    #endif
} outlineParams;

vec3 surfaceDisplace(vec3 objectPosition, vec3 objectNormal, vec4 color, vec2 uv){
    // the model matrix's inverse transpose: normals to world, and (transposed) world to object
    mat3 normalMatrix = mat3(pbrParams.normalMatrix);
    vec3 worldNormal = normalize(normalMatrix * surfaceSmoothNormal);

    #ifdef WIDTH_MODE_SCREEN
        float width = outlineParams.screenWidth;
    #else
        float width = outlineParams.worldWidth;
    #endif
    #ifdef VERTEX_COLOR_WIDTH
        width *= color.r;
    #endif

    #ifdef WIDTH_MODE_SCREEN
        vec4 clip = pbrParams.mvpMatrix * vec4(objectPosition, 1.0);
        vec4 clipNormal = frame.viewProjectionMatrix * vec4(worldNormal, 0.0);
        // where the normal moves the vertex on screen, in pixels (up to a positive factor)
        vec2 direction = (clipNormal.xy * clip.w - clip.xy * clipNormal.w) * frame.resolution.xy;
        float lengthSquared = dot(direction, direction);
        // a normal along the view ray (or a vertex behind the camera) has no screen direction
        if (lengthSquared < 1e-12 || clip.w <= 0.0){
            return objectPosition;
        }
        // width pixels in clip units at this depth (a pixel is 2 / resolution in NDC)
        vec2 clipOffset = direction * inversesqrt(lengthSquared) * (2.0 * width * clip.w) * frame.resolution.zw;
        // the view-plane offset that moves clip.xy by that much (clip.w and depth unchanged)
        vec3 viewOffset = vec3(clipOffset.x / frame.projectionMatrix[0][0], clipOffset.y / frame.projectionMatrix[1][1], 0.0);
        vec3 worldOffset = viewOffset * mat3(frame.viewMatrix);
    #else
        #ifdef DISTANCE_SCALE
            // view depth (clip.w) in perspective; orthographic pictures keep their size
            float depth = (pbrParams.mvpMatrix * vec4(objectPosition, 1.0)).w;
            float growth = mix(depth / max(outlineParams.referenceDistance, 1e-4), 1.0, frame.cameraPosition.w);
            width = min(width * mix(1.0, max(growth, 1.0), outlineParams.distanceScale), outlineParams.maxWidth);
        #endif
        vec3 worldOffset = worldNormal * width;
    #endif

    // a faded instance's hull shrinks with it (1 without instance fade, folded away)
    return objectPosition + (worldOffset * surfaceFadeVisible) * normalMatrix;
}
