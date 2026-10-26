#version 450

// @shader mesh
// @name "Lattice Stretch"
// @category "Effects"
// @description "Heat map of how much the lattices bound to the mesh squash or stretch it: white at rest, the squash colour where the volume shrinks, the stretch colour where it grows, full at range times (or 1 / range) the rest volume; folded (inside-out) cells show the squash colour. Tinted by the base map; unlit. A mesh without a lattice draws white."
// @require uv1 color
// @state blend=opaque
// @property squashColor color (0.1, 0.35, 1, 1) "Squashed colour"
// @property stretchColor color (1, 0.15, 0.1, 1) "Stretched colour"
// @property range range(1.05, 16) 2 "Volume ratio of the full colour"
// @property baseColor color (1, 1, 1, 1) "Tint"
// @texture u_baseTexture white "Base map"

// The ratio s is det(J), the lattices' Jacobian determinant (stacked lattices multiply):
// 1 at rest, 0.5 squashed to half the volume, 2 stretched to double. It is shown on a log
// scale, t = log2(s) / log2(range) clamped to [-1, 1], so halving and doubling the volume
// are equally strong: t < 0 mixes white towards squashColor, t > 0 towards stretchColor.
// A cell folded inside out (s <= 0) is full squashColor. The result multiplies the base
// map, the tint and the instance colour.

uniform u_fs_customParams {
    vec4 squashColor;
    vec4 stretchColor;
    vec4 baseColor;
    float range;
} customParams;

uniform texture2D u_baseTexture;
uniform sampler u_base_smp;

#include "includes/surface_uv.glsl"
SURFACE_UV_IN vec2 v_uv1;
in vec4 v_color; // the instance colour

#define LATTICE_FS
#define LATTICE_STRETCH
#include "includes/lattice.glsl"

out vec4 g_finalColor;

#include "includes/output.glsl"

void main() {
    float stretch = latticeStretch();
    float t = -1.0;
    if (stretch > 0.0){
        t = clamp(log2(stretch) / log2(max(customParams.range, 1.0001)), -1.0, 1.0);
    }
    vec4 heat = (t < 0.0) ? mix(vec4(1.0), customParams.squashColor, -t) : mix(vec4(1.0), customParams.stretchColor, t);

    vec4 color = heat * customParams.baseColor * sRGBToLinear(texture(sampler2D(u_baseTexture, u_base_smp), v_uv1)) * v_color;
    g_finalColor = finalizeColor(color);
}
