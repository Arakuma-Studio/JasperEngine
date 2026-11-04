// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LODFADE_H
#define LODFADE_H

// Detail level selection and the dithered cross-fade between levels (spec S54 R5-R8,
// docs/forge-meshopt-analysis/lod-crossfade.md §4). Pure functions over plain values, so the
// renderer (RenderSystem) and the tests run the same code.
//
// A fading mesh draws both levels, opaque and depth-writing, through one screen-space 8x8 Bayer
// rank (includes/lod_fade.glsl): the level faded from keeps the pixels ranked below k, the level
// faded to the others (k in 64ths, 1..63), so every pixel is drawn by exactly one level.

#include "Export.h"
#include "Engine.h"

#include <cstdint>

namespace jasperengine{

    // a coarser level stays until its error passes the budget by this share, so a mesh at a
    // threshold does not swap (and fade) back and forth with every small camera move
    constexpr float LOD_HYSTERESIS = 0.1f;
    // smaller than this (pixels tall) a level change snaps: the pop does not show, the draw costs
    constexpr float LOD_FADE_MIN_PIXELS = 8.0f;
    // the fade's resolution: the pixels of an 8x8 tile
    constexpr int LOD_FADE_STEPS = 64;
    // the "level" of a mesh culled for its size (Cull Below Screen Height)
    constexpr uint8_t LOD_CULLED = MAX_MESH_LODS;

    // How a mesh changes level (MeshComponent::lodFadeMode, Unity's LOD Group Fade Mode)
    enum class LodFadeMode{
        NONE,       // switch at once
        CROSS_FADE  // dithered cross-fade
    };

    // What a view draws of a mesh: one level (k == 0, from == to), or from and to, from keeping
    // k/64 of the pixels. LOD_CULLED on either side draws nothing there
    struct LodDraw{
        uint8_t from = 0;
        uint8_t to = 0;
        int8_t k = 0;

        bool fading() const{ return k != 0; }
        // the level the passes that draw one level use (SSAO, scene depth, G-buffer): the one
        // keeping most of the pixels
        uint8_t dominant() const{ return (k == 0) ? to : (k >= LOD_FADE_STEPS / 2 ? from : to); }
        bool culled() const{ return k == 0 && to == LOD_CULLED; }
        // the signed fade a draw of each side carries (contract C3: positive fades in, negative
        // fades out, 0 none): from keeps the ranks below k, to the ranks from k on
        float fromFade() const{ return k == 0 ? 0.0f : -(float)k / LOD_FADE_STEPS; }
        float toFade() const{ return k == 0 ? 0.0f : (float)(LOD_FADE_STEPS - k) / LOD_FADE_STEPS; }
    };

    // The fade of one mesh in one view (time mode), runtime only: about 16 bytes, no allocation
    struct MeshLodFade{
        uint8_t level = 0;          // the level drawn alone, or faded from
        uint8_t target = 0;         // == level when not fading
        float progress = 1.0f;      // 0..1 from level to target
        float lastAllowed = 0.0f;   // the error budget at the last update (camera cut test)
        uint64_t frame = 0;         // the view frame it was last updated in (0: never)
        uint32_t revision = 0;      // MeshComponent::lodRevision it was updated for
        // the plan of that frame, which every pass of the frame draws
        LodDraw plan;
    };

    // What decides a mesh's level in a view this frame
    struct LodFadeInput{
        const float* errors = nullptr; // per level, world units (lodError * scale); [0] is 0
        unsigned int numLods = 1;
        float allowed = 0.0f;          // the world error the view hides at the mesh
        float screenHeight = 0.0f;     // projected height, pixels
        float viewHeight = 1.0f;       // the view's height, pixels
        float cullBelow = 0.0f;        // Cull Below Screen Height (share of the view, 0: never)
        int forced = -1;               // a forced level (Mesh.forceLod), -1: automatic
        bool fadeAllowed = false;      // cross-fade on (scene, mesh), its fade variant ready
        bool animated = true;          // Animate Cross-fading (time), else distance (width)
        float transitionWidth = 0.1f;  // Fade Transition Width (distance mode)
        float duration = 0.5f;         // seconds of a time fade
        float deltaTime = 0.0f;        // the fade clock's seconds since the last frame
        uint64_t frame = 1;            // the view's frame (from 1)
    };

    // the deepest level whose error fits the budget
    JASPERENGINE_API unsigned int lodSelect(const float* errors, unsigned int numLods, float allowed);
    // the same, but a coarser current level stays until its error passes the budget by
    // LOD_HYSTERESIS, and a culled mesh stays culled until it grows past the cull height by it
    JASPERENGINE_API unsigned int lodSelectStable(const LodFadeInput& input, unsigned int current);
    // the level change of a time fade (Animate Cross-fading): starts, runs, reverses or snaps
    // (not drawn last frame, camera cut, under LOD_FADE_MIN_PIXELS, fade not allowed)
    JASPERENGINE_API LodDraw lodFadeUpdate(MeshLodFade& state, const LodFadeInput& input);
    // the stateless distance fade (Fade Transition Width; always for instanced meshes): a level
    // and the next one fade over the last share `width` of the level's band of the budget
    JASPERENGINE_API LodDraw lodDistanceFade(const LodFadeInput& input);

    // the detail levels a view drew in a frame (spec S54 R12; the Stats overlay, get_render_stats):
    // meshes per level and culled for their size, meshes fading and the draws they added
    struct LodFrameStats{
        uint32_t levels[MAX_MESH_LODS] = {};
        uint32_t culledBySize = 0;
        uint32_t fading = 0;
        uint32_t fadeDraws = 0;
    };

    // the colour of a level in the editor (the Scene view's LOD Level Draw Mode, the Model Import
    // inspector's LOD bar): LOD 0 green, 1 blue, 2 amber, 3 red; rgb 0..1, linear
    inline void lodLevelColor(unsigned int level, float rgb[3]){
        static const float colors[MAX_MESH_LODS][3] = {{0.20f, 0.70f, 0.25f}, {0.20f, 0.45f, 0.95f}, {0.95f, 0.65f, 0.10f}, {0.90f, 0.20f, 0.15f}};
        const float* c = colors[level < MAX_MESH_LODS ? level : MAX_MESH_LODS - 1];
        rgb[0] = c[0];
        rgb[1] = c[1];
        rgb[2] = c[2];
    }

    // the projected height (pixels) under which a level of this error (mesh units) is drawn: the
    // threshold's pixels over the error at the mesh's scale, times its height in world units
    // (worldHeight), over the bias (the mesh's times the project's)
    inline float lodSwitchHeight(float error, float worldScale, float worldHeight, float threshold, float bias){
        const float projected = error * worldScale * (bias > 0.001f ? bias : 0.001f);
        return projected > 0.0f ? threshold * worldHeight / projected : 0.0f;
    }

    // the rank 0..63 of a pixel in the 8x8 Bayer matrix (includes/lod_fade.glsl lodFadeRank)
    inline int lodFadeRank(int x, int y){
        const int px = x & 7, py = y & 7;
        const int v = px ^ py;
        return ((v & 1) << 5) | ((py & 1) << 4) | ((v & 2) << 2) | ((py & 2) << 1) | ((v & 4) >> 1) | ((py & 4) >> 2);
    }
    // whether a draw of that fade keeps a pixel of that rank (includes/lod_fade.glsl lodFadeKeeps)
    inline bool lodFadeKeeps(float fade, int rank){
        const int k = (int)((fade < 0.0f ? -fade : fade) * LOD_FADE_STEPS + 0.5f);
        return (fade < 0.0f) ? (rank < k) : (rank >= LOD_FADE_STEPS - k);
    }
}

#endif //LODFADE_H
