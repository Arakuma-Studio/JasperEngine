uniform texture2D u_heightMap;
uniform sampler u_heightMap_smp;

uniform u_vs_terrainParams {
    vec3 eyePos;
    float size;
    float maxHeight;
    float resolution;
    float textureBaseTiles; //int
    float textureDetailTiles; //int
#ifdef HAS_TERRAIN_WORLD
    // T4: the draw's page or overview (includes/terrain_world_vs.glsl)
    vec4 worldPage;     // x, z of texel 0 (entity-local), texel spacing, 1 for an overview draw
    vec4 worldPageSize; // texels along x and z, skirt depth, tile size
    vec4 worldTile;     // the tile's minimum corner (x, z), its blend quads, 0
#endif
} terrain;

// instancing part
in vec2 i_terrainnode_pos;
in float i_terrainnode_size;
in float i_terrainnode_range;
in float i_terrainnode_resolution; //int

// The color pass always carries the surface varyings; of the two DEPTH_SHADER passes only
// the G-buffer wants them, and only when painted layers supply the surface it writes. The
// library's shared vertex paths (includes/surface_vs.glsl, surface_depth_vs.glsl) define
// TERRAIN_NO_SURFACE_VARYINGS: a library shader reads its own varyings, not these.
#if (!defined(DEPTH_SHADER) || defined(HAS_TERRAIN_PBR)) && !defined(TERRAIN_NO_SURFACE_VARYINGS)
    #define TERRAIN_SURFACE_VARYINGS
#endif

#ifdef TERRAIN_SURFACE_VARYINGS
    out vec2 v_terrainTextureCoords;
    out vec2 v_terrainTextureDetailTiled;
    // surface height in the same units as the tiled coords, for the triplanar side planes
    out float v_terrainDetailHeight;
    // eye distance counted in detail tiles, so the tiling break follows the tile size
    out float v_terrainEyeTiles;
    #ifdef HAS_NORMALS
        out vec3 v_terrainNormal;
    #endif
#endif

#if defined(HAS_TERRAIN_PBR) && defined(HAS_NORMALS) && !defined(TERRAIN_NO_SURFACE_VARYINGS)
    // Layer normals are built on the terrain's own axes; these carry them into whichever
    // space the pass shades in, so a rotated terrain still lights correctly
    out vec3 v_terrainAxisX;
    out vec3 v_terrainAxisZ;

    void setTerrainShadingAxes(mat4 normalMatrix){
        v_terrainAxisX = normalize((normalMatrix * vec4(1.0, 0.0, 0.0, 0.0)).xyz);
        v_terrainAxisZ = normalize((normalMatrix * vec4(0.0, 0.0, 1.0, 0.0)).xyz);
    }
#endif

#ifdef TERRAIN_HOLES
    // T3 R20: the one UV the hole test reads, in every pass (includes/terrain_holes_fs.glsl)
    out vec2 v_terrainHoleUV;
#endif

// T3 R9: at Low the noise of Macro Variation is computed here: one varying, no fragment cost
#if defined(TERRAIN_MACRO) && defined(TERRAIN_QUALITY_LOW) && defined(TERRAIN_SURFACE_VARYINGS)
    #define TERRAIN_MACRO_VERTEX
    #include "includes/terrain_macro.glsl"
    uniform u_vs_terrainMacro {
        vec4 params; // x = 1 / scale, y = strength, z = slope fade
        vec4 tintA;
        vec4 tintB;
    } terrainMacroBlock;
    out vec3 v_terrainMacroTint;
#endif

float morphFactor;


vec2 morphVertex(vec2 gridPos, vec2 worldPos, float morph) {
    vec2 gridDim = vec2(i_terrainnode_resolution, i_terrainnode_resolution);

    // only the odd vertices morph, so gridPos (in the patch range [-0.5, 0.5]) is moved to
    // [0, 1] and rounded to the vertex index: scaling it loses the parity in float32
    vec2 index = floor((gridPos + 0.5) * gridDim.xy + 0.5);
    vec2 fracPart = fract(index * 0.5) * 2.0 / gridDim.xy;
    return worldPos - fracPart * i_terrainnode_size * morph;
}

#ifdef HAS_TERRAIN_WORLD
    #include "includes/terrain_world_vs.glsl"
#else
float getHeight(vec3 position) {
    return texture(sampler2D(u_heightMap, u_heightMap_smp), (position.xz + (terrain.size/2.0)) / terrain.size).r * terrain.maxHeight;
}
#endif

// must be called BEFORE getTerrainNormal because morphValue
vec3 getTerrainPosition(vec3 pos, mat4 modelMatrix){
    pos = i_terrainnode_size * pos;
    pos = pos + vec3(i_terrainnode_pos[0], 0.0, i_terrainnode_pos[1]);

    pos = vec3(pos.x, getHeight(pos), pos.z);

    float morphStart = 0.0;
    float morphEnd = 0.4;

    float dist = distance(terrain.eyePos, vec3(modelMatrix * vec4(pos, 1.0)));

    #ifdef TERRAIN_SURFACE_VARYINGS
        v_terrainEyeTiles = dist * float(terrain.textureDetailTiles) / terrain.size;
    #endif

    float nextlevel_thresh = ((i_terrainnode_range - dist) / i_terrainnode_size * i_terrainnode_resolution / float(terrain.resolution));
    morphFactor = 1.0 - smoothstep(morphStart, morphEnd, nextlevel_thresh);

    pos.xz = morphVertex(a_position.xz, pos.xz, morphFactor);

    pos = vec3(pos.x, getHeight(pos), pos.z);
    #ifdef HAS_TERRAIN_WORLD
        pos = terrainWorldSkirt(pos, a_position);
    #endif

    #ifdef TERRAIN_HOLES
        #ifdef HAS_TERRAIN_WORLD
            v_terrainHoleUV = terrainWorldHoleUV(pos.xz);
        #else
            v_terrainHoleUV = (pos.xz + (terrain.size / 2.0)) / terrain.size;
        #endif
    #endif
    #if defined(TERRAIN_MACRO_VERTEX) && !defined(HAS_NORMALS)
        v_terrainMacroTint = terrainMacroTint(pos.xz + vec2(terrain.size / 2.0), 1.0, terrainMacroBlock.params,
                                              terrainMacroBlock.tintA.rgb, terrainMacroBlock.tintB.rgb);
    #endif

    return pos;
}

vec3 getTerrainNormal(vec3 normal, vec3 position){
    #ifdef HAS_NORMALS
        float delta = (morphFactor + 1.0) * i_terrainnode_size / float(i_terrainnode_resolution);

    #ifdef HAS_TERRAIN_WORLD
        normal = terrainWorldNormal(position, delta);
    #else
        vec3 dA = delta * normalize(cross(normal.yzx, normal));
        vec3 dB = delta * normalize(cross(dA, normal));

        // Central differences: a one-sided slope sits half a step off the vertex, so the rim
        // of a cliff read as flat ground and the flat projection streaked down its wall
        vec3 pA0 = position - dA;
        vec3 pA1 = position + dA;
        vec3 pB0 = position - dB;
        vec3 pB1 = position + dB;

        pA0 += normal * getHeight(pA0);
        pA1 += normal * getHeight(pA1);
        pB0 += normal * getHeight(pB0);
        pB1 += normal * getHeight(pB1);

        normal = normalize(cross(pB1 - pB0, pA1 - pA0));
    #endif

        #ifdef TERRAIN_SURFACE_VARYINGS
            v_terrainNormal = normal;
        #endif
        #ifdef TERRAIN_MACRO_VERTEX
            v_terrainMacroTint = terrainMacroTint(position.xz + vec2(terrain.size / 2.0), abs(normal.y), terrainMacroBlock.params,
                                                  terrainMacroBlock.tintA.rgb, terrainMacroBlock.tintB.rgb);
        #endif
    #endif

    return normal;
}

#ifdef TERRAIN_SURFACE_VARYINGS
    vec2 getTerrainTiledTexture(vec3 position){
        v_terrainTextureCoords = (position.xz + (terrain.size/2.0)) / terrain.size;
        v_terrainTextureDetailTiled = v_terrainTextureCoords * float(terrain.textureDetailTiles);
        v_terrainDetailHeight = position.y / terrain.size * float(terrain.textureDetailTiles);
        #ifdef HAS_TERRAIN_WORLD
            // detail stays world-absolute (no seam at a border); the blend maps are the tile's
            vec2 baseUV = v_terrainTextureCoords * float(terrain.textureBaseTiles);
            if (terrainWorldOverview()) baseUV = terrainWorldOverviewUV(position.xz);
            v_terrainTextureCoords = terrainWorldBlendUV(position.xz);
            return baseUV;
        #else
            return v_terrainTextureCoords * float(terrain.textureBaseTiles);
        #endif
    }
#endif

// the terrain's UV set of a position from getTerrainPosition: its base map coordinates
// tiled textureBaseTiles times (what Lit's v_uv1 holds), without the surface varyings
vec2 getTerrainUV(vec3 position){
    #ifdef HAS_TERRAIN_WORLD
        if (terrainWorldOverview()) return terrainWorldOverviewUV(position.xz);
    #endif
    return (position.xz + (terrain.size / 2.0)) / terrain.size * float(terrain.textureBaseTiles);
}
