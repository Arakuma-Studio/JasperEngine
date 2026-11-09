// T4 (HAS_TERRAIN_WORLD, 'Twd'): a Terrain World's vertex reads. u_heightMap is the draw's page, or
// the overview: 16-bit samples in RG8 (high byte in red), read with texelFetch and a manual bilerp
// on every backend (D3), so two tiles give a shared border vertex the same height to the bit. The
// page's texel 0 sits at terrain.worldPage.xy, a texel every terrain.worldPage.z metres. Included by
// includes/terrain_vs.glsl after its uniform block.

float terrainWorldSample(ivec2 texel) {
    ivec2 last = ivec2(int(terrain.worldPageSize.x) - 1, int(terrain.worldPageSize.y) - 1);
    texel = clamp(texel, ivec2(0, 0), last);
    vec2 rg = texelFetch(sampler2D(u_heightMap, u_heightMap_smp), texel, 0).rg;
    return (floor(rg.r * 255.0 + 0.5) * 256.0 + floor(rg.g * 255.0 + 0.5)) / 65535.0 * terrain.maxHeight;
}

// the continuous texel coordinate of an entity-local position (exact on the lattice: a power-of-two
// spacing and dyadic positions)
vec2 terrainWorldTexel(vec2 xz) {
    return (xz - terrain.worldPage.xy) / terrain.worldPage.z;
}

float getHeight(vec3 position) {
    vec2 t = terrainWorldTexel(position.xz);
    vec2 base = floor(t);
    vec2 f = t - base;
    ivec2 c = ivec2(base);
    float h00 = terrainWorldSample(c);
    float h10 = terrainWorldSample(c + ivec2(1, 0));
    float h01 = terrainWorldSample(c + ivec2(0, 1));
    float h11 = terrainWorldSample(c + ivec2(1, 1));
    float low = h00 + (h10 - h00) * f.x;
    float high = h01 + (h11 - h01) * f.x;
    return low + (high - low) * f.y;
}

// D5: a skirt vertex (a_position.y 1, the outer ring of the grid) drops by the world's skirt depth
// where its edge lies on a tile border, and collapses onto the edge (a degenerate triangle) elsewhere
vec3 terrainWorldSkirt(vec3 pos, vec3 gridPos) {
    if (gridPos.y < 0.5) return pos;
    float tileSize = terrain.worldPageSize.w;
    bool edgeX = abs(gridPos.x) == 0.5 && fract(pos.x / tileSize) == 0.0;
    bool edgeZ = abs(gridPos.z) == 0.5 && fract(pos.z / tileSize) == 0.0;
    if (edgeX || edgeZ) pos.y -= terrain.worldPageSize.z;
    return pos;
}

// D3, AC9: central differences of the four nearest samples, deltaSamples away, held inside the page
// symmetrically: a vertex on a tile border reads its two neighbours alike on both tiles
vec3 terrainWorldNormal(vec3 position, float delta) {
    vec2 t = terrainWorldTexel(position.xz);
    ivec2 c = ivec2(floor(t + 0.5));
    int d = max(1, int(floor(delta / terrain.worldPage.z + 0.5)));
    int lastX = int(terrain.worldPageSize.x) - 1;
    int lastZ = int(terrain.worldPageSize.y) - 1;
    int dx = max(1, min(d, min(c.x, lastX - c.x)));
    int dz = max(1, min(d, min(c.y, lastZ - c.y)));
    float left = terrainWorldSample(c - ivec2(dx, 0));
    float right = terrainWorldSample(c + ivec2(dx, 0));
    float back = terrainWorldSample(c - ivec2(0, dz));
    float front = terrainWorldSample(c + ivec2(0, dz));
    float deltaX = float(dx) * terrain.worldPage.z;
    float deltaZ = float(dz) * terrain.worldPage.z;
    return normalize(vec3((left - right) * deltaZ, 2.0 * deltaX * deltaZ, (back - front) * deltaX));
}

// D14: the blend maps are the tile's, their samples shared at its borders
vec2 terrainWorldBlendUV(vec2 xz) {
    float quads = terrain.worldTile.z;
    return ((xz - terrain.worldTile.xy) / terrain.worldPageSize.w * quads + 0.5) / (quads + 1.0);
}

// T3 R19's u over the tile
vec2 terrainWorldHoleUV(vec2 xz) {
    return (xz - terrain.worldTile.xy) / terrain.worldPageSize.w;
}

// R9: an overview draw shows the macro colour where the base map goes, texel centres on its samples
bool terrainWorldOverview() {
    return terrain.worldPage.w > 0.5;
}

vec2 terrainWorldOverviewUV(vec2 xz) {
    return (terrainWorldTexel(xz) + 0.5) / terrain.worldPageSize.xy;
}
