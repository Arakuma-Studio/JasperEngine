#version 450

// S26 AC5 (MeshPose.MatchesGpuSkinning): a fragment stage of the debug variant of the skinned
// mesh shaders (world_position.vert): the vertex index as a 24-bit integer in RGB, alpha 1.

in vec3 v_position;
in float v_vertex;

out vec4 g_finalColor;

void main() {
    // both varyings read in every stage (no vertex output goes unread)
    if (v_vertex < 0.0 || any(isnan(v_position)) || any(isinf(v_position)))
        discard;
    float value = floor(v_vertex + 0.5);
    float high = floor(value / 65536.0);
    float mid = floor((value - high * 65536.0) / 256.0);
    float low = value - high * 65536.0 - mid * 256.0;
    g_finalColor = vec4(high / 255.0, mid / 255.0, low / 255.0, 1.0);
}
