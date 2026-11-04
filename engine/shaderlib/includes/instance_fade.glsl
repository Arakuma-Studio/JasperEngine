// The Instanced Mesh distance fade (USE_INSTANCE_FADE), shared by every vertex stage that draws
// an instance (mesh.vert, depth.vert, gbuffer.vert, includes/surface_vs.glsl and
// includes/surface_depth_vs.glsl) so the colour, shadow, pre-pass and SSR G-buffer shapes shrink
// in step. Included after the instance attributes (i_matrix_col4). Vertex stage only.
//
// An instance shrinks toward its base (its origin) by its horizontal distance from the camera in
// the mesh's local space: to nothing between fadeRange.x and .y (the far fade), and from nothing
// between fadeRange.z and .w (the near fade, a far level of detail's inner edge). An empty range
// (end <= start) leaves that side solid. The CPU drops instances where the fade has shrunk them
// to nothing (RenderSystem::updateInstancedMesh), with the same distances.
#ifndef INSTANCE_FADE_GLSL
#define INSTANCE_FADE_GLSL

uniform u_vs_fade {
    vec4 fadeRange; // far start, far end, near start, near end
    vec4 fadeEye;   // camera in model space.xyz
} fadeParams;

// how much of the instance at `origin` shows: 1 whole, 0 shrunk to its base
float instanceFadeVisible(vec3 origin){
    float fadeDistance = length(fadeParams.fadeEye.xz - origin.xz);
    float visible = 1.0;
    if (fadeParams.fadeRange.y > fadeParams.fadeRange.x){
        visible = clamp((fadeParams.fadeRange.y - fadeDistance) /
            (fadeParams.fadeRange.y - fadeParams.fadeRange.x), 0.0, 1.0);
    }
    if (fadeParams.fadeRange.w > fadeParams.fadeRange.z){
        visible *= clamp((fadeDistance - fadeParams.fadeRange.z) /
            (fadeParams.fadeRange.w - fadeParams.fadeRange.z), 0.0, 1.0);
    }
    return visible;
}

#endif
