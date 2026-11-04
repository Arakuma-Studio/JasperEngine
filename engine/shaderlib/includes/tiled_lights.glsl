// Screen-tile light lists (Forward+, USE_TILED_LIGHTS, docs/specs/S57-forge-lighting.md R1): the
// fragment stage of includes/lights.glsl when the project's Light Mode draws Tiled. The pixel
// lights are the lights of this fragment's screen tile, read from the view's data texture
// (render/TiledLights.h has its layout), so pixelLightCount() and getLight(i, ...) keep their
// contract for every shader: only the indices' source changes. The vertex lights (MIXED,
// USE_VERTEX_LIGHTS) stay the object's own, in the vertex stage's block.
//
// The tiles start on even pixels from the grid's origin, so a 2x2 quad shares its tile: the
// loops over pixelLightCount() stay uniform per quad, as lightShadow's derivatives need.

#ifndef TILED_LIGHTS_GLSL
#define TILED_LIGHTS_GLSL

// the pixel lights' loops run to a tile's cap (TiledLights::MAX_LIGHTS_PER_TILE): a constant
// bound, as HLSL needs for a loop that samples shadow maps or masks
#undef LIGHT_CAPACITY
#define LIGHT_CAPACITY 32

// TiledLights::TEXTURE_WIDTH, LIGHT_TEXELS, TILE_TEXELS, LIGHT_RECORD_TEXELS
#define TILED_LIGHTS_WIDTH_SHIFT 9
#define TILED_LIGHTS_WIDTH_MASK 511
#define TILED_LIGHTS_LIST_START 1536
#define TILED_LIGHTS_TILE_TEXELS 9
#define TILED_LIGHTS_RECORD_TEXELS 5

// Engine-internal layout (RenderSystem::buildTiledObjectLights): read it through the functions
// of includes/lights.glsl
uniform u_fs_objectLights {
    vec4 counts;      // y vertex lights; z the object's layer; w 1 when a light's culling mask or
                      // shading may leave the object out (tiledLightExcluded reads the record)
    vec4 shR;         // L1 SH ambient: rgb_c(n) = dot(sh_c.xyz, n) + sh_c.w
    vec4 shG;
    vec4 shB;
    vec4 tileGrid;    // xy the grid's origin in gl_FragCoord pixels, zw a tile's size in pixels
    vec4 tileCount;   // x columns, y rows
} objectLights;

uniform texture2D u_tiledLights;
uniform sampler u_tiledLights_smp;

vec4 tiledLightsTexel(int texel){
    return texelFetch(sampler2D(u_tiledLights, u_tiledLights_smp),
                      ivec2(texel & TILED_LIGHTS_WIDTH_MASK, texel >> TILED_LIGHTS_WIDTH_SHIFT), 0);
}

// the header texel of this fragment's tile
int tiledLightsTile(){
    vec2 cell = floor((gl_FragCoord.xy - objectLights.tileGrid.xy) / objectLights.tileGrid.zw);
    ivec2 tile = clamp(ivec2(cell), ivec2(0, 0), ivec2(objectLights.tileCount.xy) - ivec2(1, 1));
    return TILED_LIGHTS_LIST_START + (tile.x + tile.y * int(objectLights.tileCount.x)) * TILED_LIGHTS_TILE_TEXELS;
}

int tiledLightCount(){
    return int(tiledLightsTexel(tiledLightsTile()).x + 0.5);
}

// the first texel of the record of the tile's index-th light
int tiledLightRecord(int index){
    vec4 four = tiledLightsTexel(tiledLightsTile() + 1 + (index >> 2));
    int lane = index & 3;
    float local = (lane == 0) ? four.x : ((lane == 1) ? four.y : ((lane == 2) ? four.z : four.w));
    return int(local + 0.5) * TILED_LIGHTS_RECORD_TEXELS;
}

// whether this object leaves the light out: its layer is not in the light's culling mask, or a
// light shaded per vertex in a MIXED variant (it is one of the vertex lights then)
bool tiledLightExcluded(int record){
    if (objectLights.counts.w < 0.5){
        return false;
    }
    vec4 mask = tiledLightsTexel(record + 4);
    int layer = int(objectLights.counts.z + 0.5);
    int word = int(((layer < 16) ? mask.x : mask.y) + 0.5);
    bool excluded = ((word >> (layer & 15)) & 1) == 0;
    #ifdef USE_VERTEX_LIGHTS
        excluded = excluded || mask.z > 0.5;
    #endif
    return excluded;
}

#endif // TILED_LIGHTS_GLSL
