// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "terrain/TerrainMacroNormal.h"

#include "terrain/TerrainSurface.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine::terrain;

namespace {

    inline uint8_t encode(float value) {
        const float scaled = (value * 0.5f + 0.5f) * 255.0f + 0.5f;
        return static_cast<uint8_t>(scaled <= 0.0f ? 0.0f : (scaled >= 255.0f ? 255.0f : scaled));
    }

    inline float decode(uint8_t value) {
        return static_cast<float>(value) / 255.0f * 2.0f - 1.0f;
    }

}

void jasperengine::terrain::bakeMacroNormalRows(const MacroNormalSource& source, int width, int height, int rowBegin, int rowEnd, uint8_t* top) {
    if (!source.pixels || width <= 0 || height <= 0 || !top || source.terrainSize <= 0.0f) return;
    const float stepX = source.terrainSize / static_cast<float>(width);
    const float stepZ = source.terrainSize / static_cast<float>(height);
    const float half = source.terrainSize * 0.5f;
    const auto heightAt = [&](float localX, float localZ) {
        return source.maxHeight * sampleHeightLocal(source.pixels, source.width, source.height, source.channels,
                                                    source.bytesPerChannel, source.terrainSize, localX, localZ);
    };
    for (int y = rowBegin; y < rowEnd; y++) {
        const float localZ = (static_cast<float>(y) + 0.5f) * stepZ - half;
        for (int x = 0; x < width; x++) {
            const float localX = (static_cast<float>(x) + 0.5f) * stepX - half;
            // one texel each side, in metres
            const float dx = (heightAt(localX + stepX, localZ) - heightAt(localX - stepX, localZ)) / (2.0f * stepX);
            const float dz = (heightAt(localX, localZ + stepZ) - heightAt(localX, localZ - stepZ)) / (2.0f * stepZ);
            const float length = std::sqrt(dx * dx + dz * dz + 1.0f);
            const size_t at = (static_cast<size_t>(y) * width + x) * 2;
            top[at] = encode(-dx / length);
            top[at + 1] = encode(-dz / length);
        }
    }
}

void jasperengine::terrain::buildMacroNormalMips(std::vector<MacroNormalLevel>& levels) {
    if (levels.empty()) return;
    levels.resize(1);
    while (levels.back().width > 1 || levels.back().height > 1) {
        const MacroNormalLevel& above = levels.back();
        MacroNormalLevel level;
        level.width = std::max(above.width / 2, 1);
        level.height = std::max(above.height / 2, 1);
        level.rg.resize(static_cast<size_t>(level.width) * level.height * 2);
        for (int y = 0; y < level.height; y++) {
            const int y0 = std::min(y * 2, above.height - 1);
            const int y1 = std::min(y * 2 + 1, above.height - 1);
            for (int x = 0; x < level.width; x++) {
                const int x0 = std::min(x * 2, above.width - 1);
                const int x1 = std::min(x * 2 + 1, above.width - 1);
                float nx = 0.0f, ny = 0.0f, nz = 0.0f;
                for (int sy : {y0, y1}) {
                    for (int sx : {x0, x1}) {
                        const size_t at = (static_cast<size_t>(sy) * above.width + sx) * 2;
                        const float cx = decode(above.rg[at]);
                        const float cz = decode(above.rg[at + 1]);
                        nx += cx;
                        nz += cz;
                        ny += std::sqrt(std::max(0.0f, 1.0f - cx * cx - cz * cz));
                    }
                }
                const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
                const size_t at = (static_cast<size_t>(y) * level.width + x) * 2;
                level.rg[at] = encode(length > 0.0f ? nx / length : 0.0f);
                level.rg[at + 1] = encode(length > 0.0f ? nz / length : 0.0f);
            }
        }
        levels.push_back(std::move(level));
    }
}

std::vector<MacroNormalLevel> jasperengine::terrain::bakeMacroNormal(const MacroNormalSource& source) {
    std::vector<MacroNormalLevel> levels;
    if (!source.pixels || source.width <= 0 || source.height <= 0) return levels;
    MacroNormalLevel top;
    top.width = macroNormalSize(source.width);
    top.height = macroNormalSize(source.height);
    top.rg.resize(static_cast<size_t>(top.width) * top.height * 2);
    bakeMacroNormalRows(source, top.width, top.height, 0, top.height, top.rg.data());
    levels.push_back(std::move(top));
    buildMacroNormalMips(levels);
    return levels;
}
