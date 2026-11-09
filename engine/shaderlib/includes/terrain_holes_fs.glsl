// T3 R18-R20, D7: the hole mask of a terrain, the fragment stage's half of terraincore::isHole
// (engine/core/terrain/TerrainHoles.h): the same texel, min(floor(u * W), W - 1) from the same
// terrain-local position, by texelFetch with no filter. Morphing moves the vertices, not the hole.
// The clamp at 0 is for a fragment extrapolated past the terrain's edge, which the query calls off
// the terrain: it reads the edge texel instead of an invalid address.

#ifndef TERRAIN_HOLES_FS_GLSL
#define TERRAIN_HOLES_FS_GLSL

uniform texture2D u_terrainHoles;
uniform sampler u_terrainHoles_smp;
in vec2 v_terrainHoleUV;

void terrainDiscardHole(){
    ivec2 size = textureSize(sampler2D(u_terrainHoles, u_terrainHoles_smp), 0);
    ivec2 texel = clamp(ivec2(floor(v_terrainHoleUV * vec2(size))), ivec2(0), size - ivec2(1));
    if (texelFetch(sampler2D(u_terrainHoles, u_terrainHoles_smp), texel, 0).r < 0.5) {
        discard;
    }
}

#endif
