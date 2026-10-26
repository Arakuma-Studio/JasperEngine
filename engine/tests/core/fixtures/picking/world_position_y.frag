#version 450

// @shader mesh
// @name "World Position (Y, S26 test)"
// @require position

// S26 AC5 (MeshPose.MatchesGpuSkinning): the debug variant of the skinned mesh shaders. Its
// vertex stage (world_position.vert) skins and morphs as every library mesh shader; this one
// writes the world position's y as a 24-bit fixed point in RGB over [-8, 8], alpha 1.

in vec3 v_position;

out vec4 g_finalColor;

void main() {
    float value = clamp((v_position.y + 8.0) / 16.0, 0.0, 1.0) * 16777215.0;
    float high = floor(value / 65536.0);
    float mid = floor((value - high * 65536.0) / 256.0);
    float low = value - high * 65536.0 - mid * 256.0;
    g_finalColor = vec4(high / 255.0, mid / 255.0, low / 255.0, 1.0);
}
