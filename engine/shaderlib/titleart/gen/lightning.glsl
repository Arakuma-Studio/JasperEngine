// Lightning, ported from Pachislop src/generators/energy/lightning.ts (lineFrag, composeFrag) by
// SafEight, used with the author's permission: the bolts' cores and their compose.
//
// QuadsLightning: one QuadBatch quad per bolt segment (analysis/BoltBuilder.h boltQuad), drawn
// with MAX_PREMULTIPLIED so joints and crossings never exceed one segment (R21); v_quad.y runs
// -1..1 across it, Pachislop's aAcross -1.6..1.6, and v_color is the segment's intensity in all
// four channels. LightningCompose: TA_TEX0 the core, TA_TEX1 the tight glow, TA_TEX2 the wide
// one; p6 = (glow linear rgb, intensity), p7 = (glow strength, the colour's light scale (S83 D14's
// encoding; 0 = plain), 0, 0).

#ifndef TITLEART_GEN_LIGHTNING_GLSL
#define TITLEART_GEN_LIGHTNING_GLSL

#if TA_GEN_VERTICES == 1
// the core's profile across a segment: ~0 at its quad's long edges
vec4 gen_quadsLightning() {
    float a = v_quad.y * 1.6;
    return v_color * exp(-a * a * 2.2);
}
#endif

// The bolts' own glow: a tight and a wide Gaussian of the core in the glow colour, around a core
// whitened toward the centre
vec4 gen_lightningCompose() {
    vec2 uv = ta_uv();
    float c = texture(TA_TEX0, uv).r, g1 = texture(TA_TEX1, uv).r, g2 = texture(TA_TEX2, uv).r;
    vec3 col = ta.p[6].rgb;
    vec3 light = c * mix(col, vec3(1.0), 0.8) * 1.6 + (g1 * 5.0 + g2 * 4.0) * col * ta.p[7].x;
    vec4 L = gen_lightPremul(light * ta.p[6].w);
    // the encoded light keeps its coverage
    if (ta.p[7].y > 0.0) L.rgb *= ta.p[7].y;
    return L;
}

#endif // TITLEART_GEN_LIGHTNING_GLSL
