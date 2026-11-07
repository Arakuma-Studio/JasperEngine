// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TERRAIN_DETAIL_BUILDER_H
#define TERRAIN_DETAIL_BUILDER_H

// T3 D1, R6, R16: the CPU half of the terrain's detail array: which slices a terrain has, their pixels
// and mips, and their cooked form. One sampler and one format serve every slice. The render system takes
// the result to the GPU; the exporter takes the same slices to its encoders.

#include "Export.h"
#include "component/TerrainComponent.h"
#include "texture/TextureFormat.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::terrain {

    struct DetailPlan {
        // what each slice is built from, in array order: the colours first (rounded up to whole blend
        // maps), then the normal and packed surface slices of the layers that have them
        std::vector<std::string> sliceKeys;
        int colorSlices = 0;
        // where a layer's normal and packed surface slices landed, -1 when it has none
        std::array<int, MAX_TERRAIN_LAYERS> normalSlice;
        std::array<int, MAX_TERRAIN_LAYERS> surfaceSlice;
        bool anySource = false;
    };

    // The key of a layer's source file: the loader parses an SVG's scale out of its suffix
    JASPERENGINE_API std::string detailSourceKey(const Texture& texture);
    // A layer's colour slice with the height it carries in alpha (R6) has a key of its own
    JASPERENGINE_API DetailPlan planDetailArray(const TerrainComponent& terrain);

    struct DetailPixels {
        int width = 0, height = 0;
        int slices = 0;
        GpuTextureFormat format = GpuTextureFormat::RGBA8;
        // per level, the level of every slice one after the other
        std::vector<std::vector<uint8_t>> levels;
        std::vector<int> levelWidths;
        std::vector<int> levelHeights;
    };

    struct DetailBuildOptions {
        bool mipmapped = true;
        int maxSize = MAX_TERRAIN_DETAIL_SIZE;
    };

    // The array's raw form: RGBA8. A PBR layer's colour slice keeps its RGB and their mips as a colour's
    // and takes the height map in its alpha, with mips of their own (R6); High ignores that alpha. False
    // when no source could be read.
    JASPERENGINE_API bool buildDetailPixels(const TerrainComponent& terrain, const DetailPlan& plan, const DetailBuildOptions& options, DetailPixels& out);

    // One slice's top level, RGBA8, as the exporter hands it to an encoder: the same rule per kind.
    // false when the slice has no source at all
    JASPERENGINE_API bool buildDetailSliceTop(const TerrainComponent& terrain, const DetailPlan& plan, int slice, int maxSize, std::vector<uint8_t>& rgba, int& width, int& height);

    // What kind a slice is, which decides its mips and its cook: colour, normal or packed surface
    enum class DetailSliceKind : int { COLOR, NORMAL, SURFACE };
    JASPERENGINE_API DetailSliceKind detailSliceKind(const DetailPlan& plan, int slice);

    // R16: level sizes concatenated, all slices one format. Each entry is one slice's levels, top first
    struct CookedSlice {
        GpuTextureFormat format = GpuTextureFormat::RGBA8;
        int width = 0, height = 0;
        std::vector<std::vector<uint8_t>> levels;
    };
    // false, with the reason, when the slices do not share a format and a size
    JASPERENGINE_API bool concatenateCookedSlices(const std::vector<CookedSlice>& slices, int skipLevels, DetailPixels& out, std::string* error);

    // R16: the player's cooked array: every slice in the active family's index, loaded and concatenated,
    // skipping the top levels past maxSize. False when any slice is not cooked: the raw path builds it.
    // A device that lacks the format decodes every slice on the CPU, ending at RGBA8.
    JASPERENGINE_API bool loadCookedDetailArray(const DetailPlan& plan, int maxSize, DetailPixels& out);

    // The key a cooked slice goes by in je-cooked/<family>/index.json: the slice key, in a namespace of its
    // own, which shared maps dedupe by
    JASPERENGINE_API std::string cookedSliceIndexKey(const std::string& sliceKey);

}

#endif // TERRAIN_DETAIL_BUILDER_H
