// The Hair shading model of the built-in Lit (PBR_HAIR, docs/specs/S57-forge-lighting.md R5):
// Kajiya and Kay's strand lighting (1989) with two highlights along tangents shifted towards the
// normal (Scheuermann, "Hair Rendering and Shading", GDC 2004), as The Forge's UT06 shades its
// hair (HairShortCutFillColor). The strand runs along hairT (includes/pbr_ext.glsl): the mesh's
// tangent turned by the material's rotation, or the flow map's direction.
//
//   diffuse   Scheuermann's wrapped Lambert, mix(0.25, 1, n.l), on the albedo
//   specular  the primary highlight (the light's colour, shifted towards the root) and the
//             secondary one (tinted by the albedo, shifted towards the tip), each
//             smoothstep(-1, 0, t.h) * sin(t, h)^exponent, normalised as Blinn-Phong is
//             (exponent + 2) / (8 PI), the light lights.glsl's blinnPhong gives.

#ifndef HAIR_GLSL
#define HAIR_GLSL

// the strand tangent moved along the normal by shift (positive: towards the normal's side)
vec3 hairShiftTangent(vec3 t, vec3 n, float shift){
    return normalize(t + shift * n);
}

// one Kajiya-Kay highlight of a strand along t, at the half vector h
float hairStrandSpecular(vec3 t, vec3 h, float exponent){
    float tdoth = dot(t, h);
    float sinTH = sqrt(max(1.0 - tdoth * tdoth, 0.0));
    float direction = smoothstep(-1.0, 0.0, tdoth);
    return direction * pow(sinTH, exponent) * (exponent + 2.0) * (0.125 * 0.31830988618);
}

#endif // HAIR_GLSL
