// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TerrainMaps.h"
#include "component/TerrainComponent.h"

#include "Project.h"
#include "mcp/McpRefs.h"
#include "pool/TextureDataPool.h"
#include "texture/TextureData.h"
#include "util/FileUtils.h"
#include "util/PngWriter.h"
#include "terrain/RawMapStore.h"
#include "terrain/TerrainOps.h"

#include "stb_image_write.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

namespace fs = std::filesystem;

namespace jasperengine::editor::mcp {

namespace {

constexpr int kHeightmapChannels = 1;
constexpr ColorFormat kHeightmapFormat = ColorFormat::RED16;

// T1 R19: the internal fill_noise op on a store over the new map's own bytes. flat, middle and
// fractal_noise keep their bytes; random_noise hashes per texel, so a seed means one map everywhere.
std::vector<unsigned char> makeHeightPixels(const TerrainHeightmapRequest& request) {
    const int resolution = std::max(2, request.resolution);
    const terrain::MapId heightMap{terrain::MapKind::Height, 0};

    terrain::RawMapStore store(static_cast<float>(resolution), 1.0f);
    store.addMap(heightMap, resolution, resolution, 1, 16);

    terrain::Op op;
    op.type = terrain::OpType::FillNoise;
    op.fill = request.mode == "random_noise" ? terrain::FillMode::RandomNoise
            : (request.mode == "fractal_noise" ? terrain::FillMode::FractalNoise : terrain::FillMode::Flat);
    op.baseHeight = request.baseHeight;
    op.amplitude = request.amplitude;
    op.frequency = request.frequency;
    op.octaves = std::max(1, std::min(8, request.octaves));
    op.seed = request.seed;

    terrain::OpOptions options;
    options.inPlace = true;
    terrain::applyOps(store, std::span<const terrain::Op>(&op, 1), options);

    const unsigned char* bytes = store.mapBytes(heightMap);
    return std::vector<unsigned char>(bytes, bytes + store.mapByteCount(heightMap));
}

std::string makeTerrainHeightmapPath(Project* project, uint32_t sceneId, Entity entity, uint32_t seed) {
    fs::path baseDir = project->getTerrainMapsDir();
    std::error_code ec;
    FileUtils::createDirectories(baseDir, ec);

    for (uint32_t i = 0; i < 10000; ++i) {
        fs::path candidate = baseDir / ("terrain_" + std::to_string(sceneId) + "_" +
                                        std::to_string(entity) + "_height_" +
                                        std::to_string(seed) + "_" + std::to_string(i) + ".png");
        if (!fs::exists(candidate, ec)) {
            fs::path rel = fs::relative(candidate, project->getAssetsPath(), ec);
            return ec ? candidate.generic_string() : rel.generic_string();
        }
    }

    fs::path fallback = baseDir / ("terrain_" + std::to_string(sceneId) + "_" +
                                   std::to_string(entity) + "_height.png");
    fs::path rel = fs::relative(fallback, project->getAssetsPath(), ec);
    return ec ? fallback.generic_string() : rel.generic_string();
}

bool setFileBackedHeightTexture(Project* project, Texture& texture, const std::string& relativePath,
                                int resolution, const std::vector<unsigned char>& pixels) {
    const int bytesPerChannel = TextureData::getBytesPerChannel(kHeightmapFormat);
    const size_t expectedSize = static_cast<size_t>(resolution) * static_cast<size_t>(resolution) *
                                static_cast<size_t>(kHeightmapChannels) * static_cast<size_t>(bytesPerChannel);
    if (!project || project->getProjectPath().empty() || relativePath.empty() ||
        resolution <= 0 || pixels.size() < expectedSize) {
        return false;
    }

    fs::path outputPath = project->resolveAssetPath(relativePath);

    // encoded in memory, then written beneath the project (the tool's write scope: no link on the
    // way followed out of it)
    std::error_code ec;
    FileUtils::createDirectories(outputPath.parent_path(), ec);
    std::vector<unsigned char> png;
    if (ec || !encodePng16(resolution, resolution, 1, pixels.data(), pixels.size(), png) ||
        !FileUtils::writeFileReplacing(outputPath, std::string(png.begin(), png.end()))) {
        return false;
    }

    texture.setPath(relativePath);
    texture.setReleaseDataAfterLoad(false);

    unsigned char* raw = static_cast<unsigned char*>(std::malloc(expectedSize));
    if (!raw) {
        return false;
    }
    std::memcpy(raw, pixels.data(), expectedSize);

    TextureData seed(resolution, resolution, static_cast<unsigned int>(expectedSize),
                     kHeightmapFormat, kHeightmapChannels, raw);
    seed.setDataOwned(false);

    std::array<TextureData, 6> arr;
    arr[0] = seed;
    auto poolEntry = TextureDataPool::get(relativePath, arr);
    const bool seedAccepted = poolEntry && poolEntry->at(0).getData() == raw;
    if (!seedAccepted) {
        std::free(raw);
    }

    TextureLoadResult result = texture.load();
    if (result.state != ResourceLoadState::Finished || !result.data || !texture.getData().getData()) {
        return false;
    }
    if (texture.getData().getChannels() != kHeightmapChannels ||
        texture.getData().getColorFormat() != kHeightmapFormat) {
        return false;
    }
    if (seedAccepted) {
        texture.getData().setDataOwned(true);
    }
    return true;
}

fs::path uniqueChildPath(const fs::path& directory, const std::string& stem, const std::string& extension) {
    fs::path candidate = directory / (stem + extension);
    for (int i = 2; fs::exists(candidate) && i < 10000; ++i) {
        candidate = directory / (stem + "-" + std::to_string(i) + extension);
    }
    return candidate;
}

unsigned char blendChannel(float value) {
    return static_cast<unsigned char>(std::lround(std::max(0.0f, std::min(1.0f, value)) * 255.0f));
}

} // namespace

TerrainHeightmapResult TerrainMaps::createHeightmap(Project* project,
                                                            uint32_t sceneId,
                                                            Entity entity,
                                                            const TerrainHeightmapRequest& request) {
    TerrainHeightmapResult result;
    if (!project) {
        result.error = "No project is available.";
        return result;
    }
    // the generated maps' folder: inside the project once links are resolved
    if (const std::string escape = mcp::McpRefs::writeEscape(project, project->getTerrainMapsDir()); !escape.empty()) {
        result.error = escape;
        return result;
    }

    const int resolution = std::max(2, request.resolution);
    std::vector<unsigned char> pixels = makeHeightPixels(request);
    result.relativePath = makeTerrainHeightmapPath(project, sceneId, entity, request.seed);

    if (!setFileBackedHeightTexture(project, result.texture, result.relativePath, resolution, pixels)) {
        result.error = "Failed to write generated terrain heightmap.";
        return result;
    }

    result.success = true;
    return result;
}

namespace {

// A constant RGBA PNG for the project's terrain maps folder, named terrain_<scene>_<entity>_<kind>, encoded in
// memory: written here (write), or by the caller's undoable command (encoded)
TerrainBlendmapResult encodeConstantMap(Project* project, uint32_t sceneId, Entity entity, const std::string& kind, int requested,
                                        const unsigned char rgba[4], const char* what) {
    TerrainBlendmapResult result;
    if (!project) {
        result.error = "No project is available.";
        return result;
    }
    // the generated maps' folder: inside the project once links are resolved
    if (const std::string escape = mcp::McpRefs::writeEscape(project, project->getTerrainMapsDir()); !escape.empty()) {
        result.error = escape;
        return result;
    }

    const int resolution = std::max(2, requested);
    std::vector<unsigned char> pixels(static_cast<size_t>(resolution) * static_cast<size_t>(resolution) * 4);
    for (size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i] = rgba[0];
        pixels[i + 1] = rgba[1];
        pixels[i + 2] = rgba[2];
        pixels[i + 3] = rgba[3];
    }

    const fs::path outputDir = project->getTerrainMapsDir();
    result.fullPath = uniqueChildPath(
        outputDir,
        "terrain_" + std::to_string(sceneId) + "_" + std::to_string(entity) + "_" + kind,
        ".png");
    auto append = [](void* context, void* data, int size) {
        static_cast<std::string*>(context)->append(static_cast<const char*>(data), static_cast<size_t>(size));
    };
    if (!stbi_write_png_to_func(append, &result.encoded, resolution, resolution, 4, pixels.data(), resolution * 4)) {
        result.error = std::string("Failed to encode generated terrain ") + what + ".";
        return result;
    }
    std::error_code ec;
    fs::path rel = fs::relative(result.fullPath, project->getAssetsPath(), ec);
    result.relativePath = (ec ? result.fullPath : rel).generic_string();
    result.success = true;
    return result;
}

TerrainBlendmapResult writeConstantMap(Project* project, uint32_t sceneId, Entity entity, const std::string& kind, int requested,
                                       const unsigned char rgba[4], const char* what) {
    TerrainBlendmapResult result = encodeConstantMap(project, sceneId, entity, kind, requested, rgba, what);
    if (!result.success) return result;
    std::error_code ec;
    FileUtils::createDirectories(project->getTerrainMapsDir(), ec);
    if (ec) {
        result.success = false;
        result.error = "Failed to create terrain maps directory: " + ec.message();
        return result;
    }
    // written beneath the project (the tool's write scope)
    if (!FileUtils::writeFileReplacing(result.fullPath, result.encoded)) {
        result.success = false;
        result.error = std::string("Failed to write generated terrain ") + what + ".";
    }
    return result;
}

} // namespace

TerrainBlendmapResult TerrainMaps::createBlendmap(Project* project,
                                                          uint32_t sceneId,
                                                          Entity entity,
                                                          const TerrainBlendmapRequest& request) {
    const unsigned char rgba[4] = {blendChannel(request.red), blendChannel(request.green), blendChannel(request.blue), 255};
    return writeConstantMap(project, sceneId, entity, "blend", request.resolution, rgba, "blendmap");
}

TerrainBlendmapResult TerrainMaps::encodeDensityMap(Project* project,
                                                    uint32_t sceneId,
                                                    Entity entity,
                                                    int layer,
                                                    const TerrainDensityMapRequest& request) {
    // MeshSystem::sampleFoliageDensity reads the first channel
    const unsigned char grey = blendChannel(request.fill);
    const unsigned char rgba[4] = {grey, grey, grey, 255};
    return encodeConstantMap(project, sceneId, entity, "density" + std::to_string(layer), request.resolution, rgba, "density map");
}

} // namespace jasperengine::editor::mcp
