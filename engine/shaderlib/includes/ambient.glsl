// The scene's gradient ambient (Lighting > Environment Lighting > Source Gradient, Unity's
// trilight; S94 F15): only in the lit variants of a Gradient scene (AMBIENT_GRADIENT, MESH 'Agr').
// Define AMBIENT_VS (in a .vert) or AMBIENT_FS (in a .frag) before including, in the stage that
// calls ambientGradient. includes/lights.glsl includes it for ambientLight; the built-in Lit for
// its scene ambient. Engine-internal block (RenderSystem ambient_gradient_t): linear colours times
// the intensity. Everything else (the SH ambient, frame.ambient, Lit's camera block, a variant
// without 'Agr') takes the gradient's average over every direction, which ambientLight swaps for
// the gradient.

#if defined(AMBIENT_GRADIENT) && !defined(AMBIENT_GRADIENT_DECLARED) && (defined(AMBIENT_VS) || defined(AMBIENT_FS))
#define AMBIENT_GRADIENT_DECLARED

#ifdef AMBIENT_VS
uniform u_vs_ambientGradient {
#else
uniform u_fs_ambientGradient {
#endif
    vec4 sky;       // rgb from above
    vec4 equator;   // rgb from the side
    vec4 ground;    // rgb from below
    vec4 average;   // rgb sky / 4 + equator / 2 + ground / 4: the SH ambient's share
} ambientGradientBlock;

// sky * max(y, 0) + equator * (1 - |y|) + ground * max(-y, 0) for a world normal: light to
// multiply by albedo, as ambientLight's
vec3 ambientGradient(vec3 worldNormal){
    float up = max(worldNormal.y, 0.0);
    float down = max(-worldNormal.y, 0.0);
    return ambientGradientBlock.sky.rgb * up + ambientGradientBlock.ground.rgb * down +
           ambientGradientBlock.equator.rgb * (1.0 - up - down);
}

#endif
