// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "terrain/TerrainDetailBuilder.h"

#include "Log.h"
#include "io/Data.h"
#include "render/SystemRender.h"
#include "texture/CookedTextures.h"
#include "texture/MipChain.h"
#include "texture/TextureData.h"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <map>

using namespace jasperengine;
using namespace jasperengine::terrain;

namespace {

    bool hasSurfaceMaps(const TerrainSurfaceLayer& layer) {
        return !layer.occlusionTexture.empty() || !layer.roughnessTexture.empty() ||
               !layer.metallicTexture.empty() || !layer.heightTexture.empty();
    }

    // A map is read from its conventional channel when it has color, and from its only channel when it
    // is grayscale, so packed and separate sources both work
    unsigned char sourceChannel(TextureData* source, size_t texel, int channel, unsigned char fallback) {
        if (!source || !source->getData()) {
            return fallback;
        }
        const int channels = source->getChannels();
        if (channel == 3 && channels < 4) {
            return fallback;
        }
        const unsigned char* pixels = static_cast<const unsigned char*>(source->getData());
        return pixels[texel * channels + ((channels < 3) ? 0 : channel)];
    }

    struct SliceSources {
        DetailSliceKind kind = DetailSliceKind::COLOR;
        TextureData* color = nullptr;
        TextureData* normal = nullptr;
        TextureData* occlusion = nullptr;
        TextureData* roughness = nullptr;
        TextureData* metallic = nullptr;
        TextureData* height = nullptr;
        // a PBR layer's colour slice: its alpha takes the height (R6)
        bool packHeight = false;
    };

    // A map shared by several layers is read once, and the array takes the largest of them
    struct SourceFiles {
        std::map<std::string, TextureData> files;
        int width = 0;
        int height = 0;

        TextureData* get(const Texture& texture) {
            const std::string key = detailSourceKey(texture);
            if (key.empty()) {
                return nullptr;
            }
            auto found = files.find(key);
            if (found == files.end()) {
                found = files.emplace(key, TextureData()).first;
                if (!found->second.loadTextureFromFile(key.c_str())) {
                    return nullptr;
                }
                // stb hands over the pixels, and nothing else will free them
                found->second.setDataOwned(true);
                width = std::max(width, found->second.getWidth());
                height = std::max(height, found->second.getHeight());
            }
            return found->second.getData() ? &found->second : nullptr;
        }
    };

    std::vector<SliceSources> gatherSources(const TerrainComponent& terrain, const DetailPlan& plan, SourceFiles& files) {
        std::vector<SliceSources> sources(plan.sliceKeys.size());
        const size_t layerCount = std::min(terrain.surfaceLayers.size(), static_cast<size_t>(TERRAIN_MAX_LAYERS));
        for (size_t i = 0; i < layerCount; i++) {
            const TerrainSurfaceLayer& layer = terrain.surfaceLayers[i];
            sources[i].color = files.get(layer.colorTexture);
            if (layer.pbr && !layer.heightTexture.empty()) {
                sources[i].height = files.get(layer.heightTexture);
                sources[i].packHeight = true;
            }
            if (plan.normalSlice[i] >= 0) {
                sources[plan.normalSlice[i]].kind = DetailSliceKind::NORMAL;
                sources[plan.normalSlice[i]].normal = files.get(layer.normalTexture);
            }
            if (plan.surfaceSlice[i] >= 0) {
                SliceSources& packed = sources[plan.surfaceSlice[i]];
                packed.kind = DetailSliceKind::SURFACE;
                packed.occlusion = files.get(layer.occlusionTexture);
                packed.roughness = files.get(layer.roughnessTexture);
                packed.metallic = files.get(layer.metallicTexture);
                packed.height = files.get(layer.heightTexture);
            }
        }
        return sources;
    }

    void resizeSources(SourceFiles& files, int width, int height) {
        for (auto& entry : files.files) {
            // A map that failed to load is kept so it is not retried, but it has nothing to resize
            if (!entry.second.getData()) {
                continue;
            }
            if (entry.second.getWidth() != width || entry.second.getHeight() != height) {
                entry.second.resize(width, height);
            }
        }
    }

    // An unassigned map falls back to the neutral value of its channel: white for color and surface, and
    // a flat normal that leaves the geometry alone
    void fillSliceTop(const SliceSources& src, size_t texels, std::vector<uint8_t>& out) {
        out.resize(texels * 4, 0xFF);
        for (size_t t = 0; t < texels; t++) {
            unsigned char* texel = &out[t * 4];
            if (src.kind == DetailSliceKind::NORMAL) {
                texel[0] = sourceChannel(src.normal, t, 0, 0x80);
                texel[1] = sourceChannel(src.normal, t, 1, 0x80);
                texel[2] = sourceChannel(src.normal, t, 2, 0xFF);
            } else if (src.kind == DetailSliceKind::SURFACE) {
                texel[0] = sourceChannel(src.occlusion, t, 0, 0xFF);
                texel[1] = sourceChannel(src.roughness, t, 1, 0xFF);
                texel[2] = sourceChannel(src.metallic, t, 2, 0xFF);
                texel[3] = sourceChannel(src.height, t, 0, 0xFF);
            } else {
                texel[0] = sourceChannel(src.color, t, 0, 0xFF);
                texel[1] = sourceChannel(src.color, t, 1, 0xFF);
                texel[2] = sourceChannel(src.color, t, 2, 0xFF);
                texel[3] = sourceChannel(src.color, t, 3, 0xFF);
            }
        }
    }

}

std::string jasperengine::terrain::detailSourceKey(const Texture& texture) {
    const std::string path = texture.getPath(0);
    if (path.empty()) {
        return std::string();
    }
    return TextureData::hasSvgExtension(path.c_str()) ? TextureData::buildSvgScalePath(path, texture.getSvgScale()) : path;
}

DetailPlan jasperengine::terrain::planDetailArray(const TerrainComponent& terrain) {
    DetailPlan plan;
    // The shader reaches all three layers of any bound blend map, so colors come in threes
    const size_t layerCount = std::min(terrain.surfaceLayers.size(), static_cast<size_t>(TERRAIN_MAX_LAYERS));
    plan.colorSlices = static_cast<int>(((layerCount + 2) / 3) * 3);
    plan.sliceKeys.resize(static_cast<size_t>(plan.colorSlices));
    plan.normalSlice.fill(-1);
    plan.surfaceSlice.fill(-1);
    for (size_t i = 0; i < layerCount; i++) {
        const TerrainSurfaceLayer& layer = terrain.surfaceLayers[i];
        plan.sliceKeys[i] = detailSourceKey(layer.colorTexture);
        // the height in its alpha is part of what this slice was built from
        if (layer.pbr && !layer.heightTexture.empty()) {
            plan.sliceKeys[i] += "|height|" + detailSourceKey(layer.heightTexture);
        }
        plan.anySource = plan.anySource || !plan.sliceKeys[i].empty();

        if (!layer.pbr) {
            continue;
        }
        if (!layer.normalTexture.empty()) {
            plan.normalSlice[i] = static_cast<int>(plan.sliceKeys.size());
            plan.sliceKeys.push_back("normal|" + detailSourceKey(layer.normalTexture));
            plan.anySource = true;
        }
        if (hasSurfaceMaps(layer)) {
            plan.surfaceSlice[i] = static_cast<int>(plan.sliceKeys.size());
            plan.sliceKeys.push_back("surface|" + detailSourceKey(layer.occlusionTexture) + "|" + detailSourceKey(layer.roughnessTexture) +
                                     "|" + detailSourceKey(layer.metallicTexture) + "|" + detailSourceKey(layer.heightTexture));
            plan.anySource = true;
        }
    }
    return plan;
}

DetailSliceKind jasperengine::terrain::detailSliceKind(const DetailPlan& plan, int slice) {
    if (slice < plan.colorSlices) return DetailSliceKind::COLOR;
    for (int layer = 0; layer < TERRAIN_MAX_LAYERS; layer++) {
        if (plan.normalSlice[layer] == slice) return DetailSliceKind::NORMAL;
    }
    return DetailSliceKind::SURFACE;
}

bool jasperengine::terrain::buildDetailPixels(const TerrainComponent& terrain, const DetailPlan& plan, const DetailBuildOptions& options, DetailPixels& out) {
    SourceFiles files;
    const std::vector<SliceSources> sliceSources = gatherSources(terrain, plan, files);
    int width = files.width;
    int height = files.height;
    if (width == 0) {
        return false;
    }
    const int cap = std::min(options.maxSize, MAX_TERRAIN_DETAIL_SIZE);
    if (width > cap || height > cap) {
        if (cap == MAX_TERRAIN_DETAIL_SIZE) {
            Log::warn("Terrain detail maps are capped at %d px, larger sources are downscaled", MAX_TERRAIN_DETAIL_SIZE);
        }
        width = std::min(width, cap);
        height = std::min(height, cap);
    }
    resizeSources(files, width, height);

    const size_t texels = static_cast<size_t>(width) * height;
    std::vector<std::vector<MipLevel>> sliceLevels(plan.sliceKeys.size());
    for (size_t s = 0; s < plan.sliceKeys.size(); s++) {
        const SliceSources& src = sliceSources[s];
        std::vector<uint8_t> top;
        fillSliceTop(src, texels, top);

        // the mips of each slice by its kind: colour in linear light, normals renormalized, packed
        // surface data per channel (MipChain, as every texture: spec S14 D6)
        MipChainOptions mipOptions;
        mipOptions.maxLevels = options.mipmapped ? 0 : 1;
        mipOptions.wrap = true;
        MipLevel base;
        base.width = width;
        base.height = height;
        if (src.kind == DetailSliceKind::NORMAL) {
            mipOptions.channels = 3;
            mipOptions.type = MipPixelType::F32;
            mipOptions.normalMap = true;
            base.pixels.resize(texels * 3 * sizeof(float));
            float* v = reinterpret_cast<float*>(base.pixels.data());
            for (size_t t = 0; t < texels * 3; t++) {
                v[t] = top[(t / 3) * 4 + t % 3] / 255.0f * 2.0f - 1.0f;
            }
        } else {
            const bool colour = src.kind == DetailSliceKind::COLOR;
            mipOptions.srgb = colour;
            mipOptions.alphaWeighted = colour;
            base.pixels = std::move(top);
        }
        sliceLevels[s].push_back(std::move(base));
        MipChain::build(sliceLevels[s], mipOptions);
        if (src.kind == DetailSliceKind::NORMAL) {
            for (MipLevel& level : sliceLevels[s]) {
                const size_t n = static_cast<size_t>(level.width) * level.height;
                const float* v = reinterpret_cast<const float*>(level.pixels.data());
                std::vector<uint8_t> rgba(n * 4, 0xFF);
                for (size_t t = 0; t < n; t++) {
                    for (int c = 0; c < 3; c++) {
                        rgba[t * 4 + c] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround((v[t * 3 + c] * 0.5f + 0.5f) * 255.0f)), 0, 255));
                    }
                }
                level.pixels = std::move(rgba);
            }
        }
        // R6: the colour bytes and their mips stay as a colour's; the height goes in the alpha, with
        // separately built mips of its own, which only Low reads
        if (src.kind == DetailSliceKind::COLOR && src.packHeight) {
            std::vector<MipLevel> heightLevels(1);
            heightLevels[0].width = width;
            heightLevels[0].height = height;
            heightLevels[0].pixels.resize(texels);
            for (size_t t = 0; t < texels; t++) {
                heightLevels[0].pixels[t] = sourceChannel(src.height, t, 0, 0xFF);
            }
            MipChainOptions heightOptions;
            heightOptions.maxLevels = options.mipmapped ? 0 : 1;
            heightOptions.wrap = true;
            heightOptions.channels = 1;
            MipChain::build(heightLevels, heightOptions);
            for (size_t level = 0; level < sliceLevels[s].size() && level < heightLevels.size(); level++) {
                const size_t n = static_cast<size_t>(sliceLevels[s][level].width) * sliceLevels[s][level].height;
                for (size_t t = 0; t < n; t++) {
                    sliceLevels[s][level].pixels[t * 4 + 3] = heightLevels[level].pixels[t];
                }
            }
        } else if (src.kind == DetailSliceKind::COLOR && static_cast<int>(s) < static_cast<int>(terrain.surfaceLayers.size()) && terrain.surfaceLayers[s].pbr) {
            // a PBR layer with no height map has none to bias its blend at Low: full height
            for (MipLevel& level : sliceLevels[s]) {
                const size_t n = static_cast<size_t>(level.width) * level.height;
                for (size_t t = 0; t < n; t++) {
                    level.pixels[t * 4 + 3] = 0xFF;
                }
            }
        }
    }

    out.width = width;
    out.height = height;
    out.slices = static_cast<int>(plan.sliceKeys.size());
    out.format = GpuTextureFormat::RGBA8;
    const size_t levelCount = sliceLevels.empty() ? 0 : sliceLevels[0].size();
    out.levels.assign(levelCount, {});
    out.levelWidths.resize(levelCount);
    out.levelHeights.resize(levelCount);
    for (size_t level = 0; level < levelCount; level++) {
        for (const std::vector<MipLevel>& slice : sliceLevels) {
            out.levels[level].insert(out.levels[level].end(), slice[level].pixels.begin(), slice[level].pixels.end());
        }
        out.levelWidths[level] = sliceLevels[0][level].width;
        out.levelHeights[level] = sliceLevels[0][level].height;
    }
    return true;
}

bool jasperengine::terrain::buildDetailSliceTop(const TerrainComponent& terrain, const DetailPlan& plan, int slice, int maxSize, std::vector<uint8_t>& rgba, int& width, int& height) {
    if (slice < 0 || slice >= static_cast<int>(plan.sliceKeys.size())) return false;
    SourceFiles files;
    const std::vector<SliceSources> sliceSources = gatherSources(terrain, plan, files);
    width = files.width;
    height = files.height;
    if (width == 0) return false;
    width = std::min(width, maxSize);
    height = std::min(height, maxSize);
    resizeSources(files, width, height);
    const size_t texels = static_cast<size_t>(width) * height;
    fillSliceTop(sliceSources[static_cast<size_t>(slice)], texels, rgba);
    const SliceSources& src = sliceSources[static_cast<size_t>(slice)];
    if (src.kind == DetailSliceKind::COLOR && (src.packHeight || (slice < static_cast<int>(terrain.surfaceLayers.size()) && terrain.surfaceLayers[static_cast<size_t>(slice)].pbr))) {
        for (size_t t = 0; t < texels; t++) {
            rgba[t * 4 + 3] = src.packHeight ? sourceChannel(src.height, t, 0, 0xFF) : 0xFF;
        }
    }
    return true;
}

bool jasperengine::terrain::concatenateCookedSlices(const std::vector<CookedSlice>& slices, int skipLevels, DetailPixels& out, std::string* error) {
    if (slices.empty()) {
        if (error) *error = "no slices";
        return false;
    }
    const CookedSlice& first = slices[0];
    for (size_t s = 1; s < slices.size(); s++) {
        if (slices[s].format != first.format || slices[s].width != first.width || slices[s].height != first.height ||
            slices[s].levels.size() != first.levels.size()) {
            if (error) *error = "the array's slices must share one format, one size and one level count";
            return false;
        }
    }
    const int levelCount = static_cast<int>(first.levels.size());
    const int skip = std::clamp(skipLevels, 0, std::max(levelCount - 1, 0));
    out.format = first.format;
    out.slices = static_cast<int>(slices.size());
    out.width = std::max(first.width >> skip, 1);
    out.height = std::max(first.height >> skip, 1);
    out.levels.assign(static_cast<size_t>(levelCount - skip), {});
    out.levelWidths.resize(static_cast<size_t>(levelCount - skip));
    out.levelHeights.resize(static_cast<size_t>(levelCount - skip));
    for (int level = skip; level < levelCount; level++) {
        std::vector<uint8_t>& combined = out.levels[static_cast<size_t>(level - skip)];
        for (const CookedSlice& slice : slices) {
            combined.insert(combined.end(), slice.levels[static_cast<size_t>(level)].begin(), slice.levels[static_cast<size_t>(level)].end());
        }
        out.levelWidths[static_cast<size_t>(level - skip)] = std::max(first.width >> level, 1);
        out.levelHeights[static_cast<size_t>(level - skip)] = std::max(first.height >> level, 1);
    }
    return true;
}

bool jasperengine::terrain::loadCookedDetailArray(const DetailPlan& plan, int maxSize, DetailPixels& out) {
    std::vector<CookedSlice> slices(plan.sliceKeys.size());
    const TextureFormatSupport support = SystemRender::getTextureFormatSupport();
    for (size_t s = 0; s < plan.sliceKeys.size(); s++) {
        // an absent colour (a layer with no map) has no cook: the array stays on the raw path
        CookedTextureEntry entry;
        if (plan.sliceKeys[s].empty() || !CookedTextures::find(cookedSliceIndexKey(plan.sliceKeys[s]), entry)) {
            return false;
        }
        Data file;
        if (file.open(entry.file.c_str()) != FileErrors::FILEDATA_OK) {
            return false;
        }
        const std::vector<uint8_t> bytes(file.getMemPtr(), file.getMemPtr() + file.length());
        // R16: runtime shrink skips the top levels of the chain; a device without the format decodes or
        // transcodes every slice alike (S14's fallback), so all still share one format
        uint32_t firstLevel = 0;
        while ((std::max(entry.width, entry.height) >> firstLevel) > maxSize && static_cast<int>(firstLevel) + 1 < entry.levels) firstLevel++;
        TextureData data;
        std::string error;
        if (!data.loadKtx2(bytes.data(), static_cast<unsigned int>(bytes.size()), firstLevel, &support, false, &error)) {
            Log::error("Terrain detail slice %s: %s", entry.file.c_str(), error.c_str());
            return false;
        }
        CookedSlice& slice = slices[s];
        slice.format = data.getGpuFormat();
        slice.width = data.getWidth();
        slice.height = data.getHeight();
        for (const TextureLevel& level : data.getLevels()) {
            const uint8_t* begin = static_cast<const uint8_t*>(level.data);
            slice.levels.emplace_back(begin, begin + level.size);
        }
    }
    std::string error;
    if (!concatenateCookedSlices(slices, 0, out, &error)) {
        Log::error("Terrain detail array: %s", error.c_str());
        return false;
    }
    return true;
}

std::string jasperengine::terrain::cookedSliceIndexKey(const std::string& sliceKey) {
    // a path of its own that CookedTextures::normalize leaves as it is: the key has '|' and whole paths
    uint64_t hash = 0xcbf29ce484222325ull;
    for (const unsigned char c : sliceKey) {
        hash = (hash ^ c) * 0x100000001b3ull;
    }
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
    return std::string("__terrain_slices/") + text + ".png";
}
