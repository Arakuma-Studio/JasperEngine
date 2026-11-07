// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TerrainDetailRules.h"

#include "util/Util.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>
#include <utility>

using namespace jasperengine;

namespace {

std::string number(float value) {
    std::ostringstream text;
    text << value;
    return text.str();
}

struct DetailPair {
    const char* minKey;
    const char* maxKey;
    float TerrainFoliageLayer::* min;
    float TerrainFoliageLayer::* max;
};

const DetailPair kPairs[] = {
    {"min_scale", "max_scale", &TerrainFoliageLayer::minScale, &TerrainFoliageLayer::maxScale},
    {"min_slope", "max_slope", &TerrainFoliageLayer::minSlope, &TerrainFoliageLayer::maxSlope},
    {"min_height", "max_height", &TerrainFoliageLayer::minHeight, &TerrainFoliageLayer::maxHeight},
    {"min_draw_distance", "draw_distance", &TerrainFoliageLayer::minDrawDistance, &TerrainFoliageLayer::drawDistance},
    {"min_height_ratio", "max_height_ratio", &TerrainFoliageLayer::minHeightRatio, &TerrainFoliageLayer::maxHeightRatio},
};

}

const std::vector<editor::terrain::DetailRange>& editor::terrain::detailRanges() {
    // density: FoliageSystem places at most 8192 instances per 8 m cell (128 per square unit)
    static const std::vector<DetailRange> ranges = {
        {"density", &TerrainFoliageLayer::density, 0.0f, 128.0f,
         "instances per square world unit where the density map is full; the engine draws at most 8192 per 8 m cell"},
        {"min_scale", &TerrainFoliageLayer::minScale, 0.01f, 20.0f, "a random scale per instance"},
        {"max_scale", &TerrainFoliageLayer::maxScale, 0.01f, 20.0f, "a random scale per instance"},
        {"rotation_jitter", &TerrainFoliageLayer::rotationJitter, 0.0f, 1.0f, "the share of a full turn of random yaw"},
        {"align_to_normal", &TerrainFoliageLayer::alignToNormal, 0.0f, 1.0f, "0 stands instances upright, 1 lays them along the ground"},
        {"min_slope", &TerrainFoliageLayer::minSlope, 0.0f, 90.0f, "ground slope in degrees"},
        {"max_slope", &TerrainFoliageLayer::maxSlope, 0.0f, 90.0f, "ground slope in degrees"},
        {"min_height", &TerrainFoliageLayer::minHeight, 0.0f, 1.0f, "ground height from the terrain base (0) to its max height (1)"},
        {"max_height", &TerrainFoliageLayer::maxHeight, 0.0f, 1.0f, "ground height from the terrain base (0) to its max height (1)"},
        {"draw_distance", &TerrainFoliageLayer::drawDistance, 1.0f, 500.0f, "world units from the camera"},
        {"min_draw_distance", &TerrainFoliageLayer::minDrawDistance, 0.0f, 500.0f,
         "world units from the camera where instances start to show (a nearer copy of the layer ends there)"},
        // T5 R65: the foliage fields, here so the tool, set_properties and the load clamp all hold them
        {"min_height_ratio", &TerrainFoliageLayer::minHeightRatio, 0.01f, 1.99f, "height over the width scale; 1 keeps the mesh's own proportions"},
        {"max_height_ratio", &TerrainFoliageLayer::maxHeightRatio, 0.01f, 1.99f, "height over the width scale; 1 keeps the mesh's own proportions"},
        {"noise_spread", &TerrainFoliageLayer::noiseSpread, 0.0f, 10.0f, "noise cells per world unit between the dry and healthy colours; 0 picks per instance"},
        {"wind_bend", &TerrainFoliageLayer::windBend, 0.0f, 1.0f, "how far the wind bends the layer's tips; 0 draws it still"},
        {"density_falloff_start", &TerrainFoliageLayer::densityFalloffStart, 0.0f, 1.0f, "the share of the draw distance where density starts to fall"},
        {"far_density", &TerrainFoliageLayer::farDensity, 0.0f, 1.0f, "the share of the density left at the draw distance"},
        {"density_scale", &TerrainFoliageLayer::densityScale, 0.0f, 1.0f, "the layer's own multiplier of the Terrain's Detail Density"},
    };
    return ranges;
}

bool editor::terrain::isDetailMeshPath(const std::string& path) {
    // every model the import pipeline loads (an FBX through its imported .glb, as the Terrain window assigns it)
    return Util::isModelFile(path);
}

std::string editor::terrain::checkDetailLayer(const TerrainFoliageLayer& layer) {
    for (const DetailRange& range : detailRanges()) {
        const float value = layer.*range.field;
        if (!std::isfinite(value) || value < range.min || value > range.max) {
            return std::string(range.key) + " " + number(value) + " is outside " + number(range.min) + "-" + number(range.max) +
                   " (" + range.meaning + ").";
        }
    }
    for (const DetailPair& pair : kPairs) {
        if (layer.*pair.min > layer.*pair.max) {
            return std::string(pair.minKey) + " " + number(layer.*pair.min) + " is above " + pair.maxKey + " " +
                   number(layer.*pair.max) + ".";
        }
    }
    const std::string& mesh = layer.meshPath;
    if (!mesh.empty() && !isDetailMeshPath(mesh)) {
        return "mesh \"" + mesh + "\" is not a model a detail layer draws: use an .obj, .gltf, .glb or .fbx file.";
    }
    return {};
}

std::vector<std::string> editor::terrain::clampDetailLayer(TerrainFoliageLayer& layer) {
    std::vector<std::string> changed;
    for (const DetailRange& range : detailRanges()) {
        float& value = layer.*range.field;
        const float before = value;
        const float fixed = std::isfinite(value) ? std::clamp(value, range.min, range.max) : range.min;
        if (fixed != before || !std::isfinite(before)) {
            value = fixed;
            changed.push_back(std::string(range.key) + " " + number(before) + " -> " + number(fixed));
        }
    }
    for (const DetailPair& pair : kPairs) {
        if (layer.*pair.min > layer.*pair.max) {
            std::swap(layer.*pair.min, layer.*pair.max);
            changed.push_back(std::string(pair.minKey) + " and " + pair.maxKey + " swapped");
        }
    }
    return changed;
}
