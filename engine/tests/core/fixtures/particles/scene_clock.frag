#version 450

// ParticlesGpu.CustomBlockReadsTheSceneClock: a project's particle shader (fragment only, over the
// engine's particles.vert) that draws its custom block's engine-written scene clock: red the
// fraction of sceneTime, written as it is, alpha 1. It reads the particle's inputs as the
// library's particles.frag does, so its stages link the same way.

uniform u_fs_customParams {
    float sceneTime;
} customParams;

out vec4 g_finalColor;

#include "includes/particles_fs.glsl"

void main() {
    vec4 surface = particleSurfaceColor();
    g_finalColor = vec4(fract(customParams.sceneTime), 0.0, 0.0, clamp(surface.a + 1.0, 0.0, 1.0));
}
