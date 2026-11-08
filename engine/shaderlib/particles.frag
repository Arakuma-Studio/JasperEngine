#version 450

// The built-in particle shader (a Renderer module without a shader): the particle texture times
// the particle colour, lit as the Renderer asks, plus its emission, fogged and encoded for the
// Renderer's blend (particlesFsParams.blend; the library shaders compile one variant per blend).
//
// A fork may declare a u_fs_customParams block: its members are filled by name from the
// Renderer module's shader properties. Three names are written by the engine instead: "time"
// (seconds since startup), "sceneTime" (the scene clock: scaled game seconds, held while
// paused) and "resolution" (xy = render target size, zw = 1 / size).

out vec4 g_finalColor;

#include "includes/particles_fs.glsl"
#include "includes/output.glsl"

void main() {
    vec4 color = particleSurfaceColor();
    color.a *= v_color.a;
    int blend = int(particlesFsParams.blend.x + 0.5);
    if (blend == 2){
        // additive: the fog fades it to black
        g_finalColor = finalizeBlendedColor(color, vec3(0.0));
    }else if (blend == 3){
        // multiply: white where it is transparent, the fog fades it to white
        g_finalColor = finalizeBlendedColor(vec4(mix(vec3(1.0), color.rgb, color.a), 1.0), vec3(1.0));
    }else{
        g_finalColor = finalizeColor(color);
        if (blend == 1)
            g_finalColor.rgb *= g_finalColor.a;
    }
}
