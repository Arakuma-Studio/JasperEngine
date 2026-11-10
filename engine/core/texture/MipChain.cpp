// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "MipChain.h"

#include "stb_image_resize2.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace jasperengine;

// Compiled with -ffp-contract=off (engine/CMakeLists.txt), like stb_image_resize2: the levels
// are the same bytes on every CPU, which keeps the cook cache key free of the ISA.

namespace {
    stbir_filter mipStbirFilter(MipFilter filter){
        switch (filter){
            case MipFilter::Box: return STBIR_FILTER_BOX;
            case MipFilter::Triangle: return STBIR_FILTER_TRIANGLE;
            case MipFilter::CatmullRom: return STBIR_FILTER_CATMULLROM;
            case MipFilter::Mitchell: default: return STBIR_FILTER_MITCHELL;
        }
    }

    stbir_pixel_layout mipLayout(const MipChainOptions& options){
        switch (options.channels){
            case 1: return STBIR_1CHANNEL;
            case 2: return STBIR_2CHANNEL;
            case 3: return STBIR_RGB;
            default:
                // sRGB colour always names its alpha (converted linearly, not with the sRGB curve)
                if (options.alphaWeighted)
                    return STBIR_RGBA;
                return (options.srgb && options.type == MipPixelType::U8) ? STBIR_RGBA_NO_AW : STBIR_4CHANNEL;
        }
    }

    stbir_datatype mipDatatype(const MipChainOptions& options){
        switch (options.type){
            case MipPixelType::U16: return STBIR_TYPE_UINT16;
            case MipPixelType::F32: return STBIR_TYPE_FLOAT;
            case MipPixelType::U8: default: return options.srgb ? STBIR_TYPE_UINT8_SRGB : STBIR_TYPE_UINT8;
        }
    }

    void mipNormalizeLevel(MipLevel& level){
        float* v = reinterpret_cast<float*>(level.pixels.data());
        const size_t texels = (size_t)level.width * (size_t)level.height;
        for (size_t i = 0; i < texels; i++, v += 3){
            const float lengthSquared = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
            if (lengthSquared > 1e-12f){
                const float inverse = 1.0f / std::sqrt(lengthSquared);
                v[0] *= inverse;
                v[1] *= inverse;
                v[2] *= inverse;
            }else{
                v[0] = 0.0f;
                v[1] = 0.0f;
                v[2] = 1.0f;
            }
        }
    }

    // the alpha byte an alpha test at `cutoff` (alpha / 255 >= cutoff) starts passing at
    int mipCutoffByte(float cutoff){
        const float scaled = cutoff * 255.0f;
        int byte = (int)std::ceil(scaled - 1e-4f);
        return std::clamp(byte, 1, 255);
    }

    // Scales the level's alpha (zero stays zero) so as many texels pass the test as the base
    // level's coverage asks; when several scales are as close, the one nearest 1 wins
    void mipPreserveCoverage(MipLevel& level, float targetCoverage, float cutoff){
        const size_t texels = (size_t)level.width * (size_t)level.height;
        uint64_t histogram[256] = {};
        uint8_t* p = level.pixels.data();
        for (size_t i = 0; i < texels; i++)
            histogram[p[i * 4 + 3]]++;
        const int threshold = mipCutoffByte(cutoff);
        const double target = (double)targetCoverage * (double)texels;

        // passing(t): texels with alpha >= t
        uint64_t passing[257] = {};
        for (int a = 255; a >= 0; a--)
            passing[a] = passing[a + 1] + histogram[a];

        int best = threshold;
        double bestError = std::fabs((double)passing[threshold] - target);
        for (int t = 1; t <= 255; t++){
            const double error = std::fabs((double)passing[t] - target);
            if (error < bestError - 1e-9 || (std::fabs(error - bestError) <= 1e-9 && std::abs(t - threshold) < std::abs(best - threshold))){
                best = t;
                bestError = error;
            }
        }
        if (best == threshold)
            return;
        // texels with alpha >= best now reach the cutoff byte, alpha best - 1 stays below it
        const double scale = ((double)threshold - 0.5) / ((double)best - 0.5);
        uint8_t table[256];
        for (int a = 0; a < 256; a++)
            table[a] = (uint8_t)std::min(255.0, std::floor((double)a * scale + 0.5));
        for (size_t i = 0; i < texels; i++)
            p[i * 4 + 3] = table[p[i * 4 + 3]];
    }
}

size_t MipChain::texelBytes(const MipChainOptions& options){
    const size_t channel = options.type == MipPixelType::U8 ? 1 : (options.type == MipPixelType::U16 ? 2 : 4);
    return channel * (size_t)options.channels;
}

int MipChain::levelCount(int width, int height, const MipChainOptions& options){
    int levels = 1;
    while ((width > 1 || height > 1) && (options.maxLevels <= 0 || levels < options.maxLevels)){
        width = std::max(1, width / 2);
        height = std::max(1, height / 2);
        levels++;
    }
    return levels;
}

float MipChain::alphaCoverage(const MipLevel& level, float cutoff){
    const size_t texels = (size_t)level.width * (size_t)level.height;
    if (texels == 0)
        return 0.0f;
    const int threshold = mipCutoffByte(cutoff);
    size_t passing = 0;
    for (size_t i = 0; i < texels; i++)
        passing += level.pixels[i * 4 + 3] >= threshold ? 1 : 0;
    return (float)((double)passing / (double)texels);
}

void MipChain::build(std::vector<MipLevel>& levels, const MipChainOptions& options){
    if (levels.empty() || levels[0].width <= 0 || levels[0].height <= 0)
        return;
    levels.resize(1);
    const bool rgba8 = options.type == MipPixelType::U8 && options.channels == 4;
    const bool normals = options.normalMap && options.type == MipPixelType::F32 && options.channels == 3;
    const bool coverage = options.preserveCoverage && rgba8;
    const bool bleed = options.bleedAlpha && rgba8;

    if (normals)
        mipNormalizeLevel(levels[0]);
    const float baseCoverage = coverage ? alphaCoverage(levels[0], options.alphaCutoff) : 0.0f;

    const int count = levelCount(levels[0].width, levels[0].height, options);
    const size_t texel = texelBytes(options);
    const stbir_pixel_layout layout = mipLayout(options);
    const stbir_datatype datatype = mipDatatype(options);
    const stbir_edge edge = options.wrap ? STBIR_EDGE_WRAP : STBIR_EDGE_CLAMP;
    const stbir_filter filter = mipStbirFilter(options.filter);

    // each level is filtered from the previous one as computed, before its coverage scale
    // and bleed (which only change what is stored)
    std::vector<uint8_t> working = levels[0].pixels;
    int width = levels[0].width;
    int height = levels[0].height;
    for (int level = 1; level < count; level++){
        const int nextWidth = std::max(1, width / 2);
        const int nextHeight = std::max(1, height / 2);
        MipLevel next;
        next.width = nextWidth;
        next.height = nextHeight;
        next.pixels.resize((size_t)nextWidth * (size_t)nextHeight * texel);
        if (options.filter == MipFilter::Nearest)
            nearestResize(working.data(), width, height, next.pixels.data(), nextWidth, nextHeight, texel);
        else
            stbir_resize(working.data(), width, height, (int)(width * texel),
                         next.pixels.data(), nextWidth, nextHeight, (int)(nextWidth * texel),
                         layout, datatype, edge, filter);
        if (normals)
            mipNormalizeLevel(next);
        working = next.pixels;
        if (coverage)
            mipPreserveCoverage(next, baseCoverage, options.alphaCutoff);
        if (bleed)
            bleedAlphaBorder(next.pixels.data(), nextWidth, nextHeight);
        levels.push_back(std::move(next));
        width = nextWidth;
        height = nextHeight;
    }
    if (bleed)
        bleedAlphaBorder(levels[0].pixels.data(), levels[0].width, levels[0].height);
}

int jasperengine::nearestSourceIndex(int i, int sourceSize, int destinationSize){
    if (sourceSize <= 0 || destinationSize <= 0)
        return 0;
    // ceil((2i + 1) * s / 2d) - 1 == floor(((2i + 1) * s - 1) / 2d)
    const int64_t numerator = (2 * (int64_t)i + 1) * (int64_t)sourceSize - 1;
    const int64_t index = numerator / (2 * (int64_t)destinationSize);
    return (int)std::clamp<int64_t>(index, 0, sourceSize - 1);
}

void jasperengine::nearestResize(const uint8_t* source, int sourceWidth, int sourceHeight, uint8_t* destination,
                                 int destinationWidth, int destinationHeight, size_t texelBytes){
    if (!source || !destination || sourceWidth <= 0 || sourceHeight <= 0 || destinationWidth <= 0 || destinationHeight <= 0)
        return;
    std::vector<int> columns((size_t)destinationWidth);
    for (int x = 0; x < destinationWidth; x++)
        columns[(size_t)x] = nearestSourceIndex(x, sourceWidth, destinationWidth);
    for (int y = 0; y < destinationHeight; y++){
        const uint8_t* row = source + (size_t)nearestSourceIndex(y, sourceHeight, destinationHeight) * (size_t)sourceWidth * texelBytes;
        uint8_t* out = destination + (size_t)y * (size_t)destinationWidth * texelBytes;
        for (int x = 0; x < destinationWidth; x++)
            std::memcpy(out + (size_t)x * texelBytes, row + (size_t)columns[(size_t)x] * texelBytes, texelBytes);
    }
}

void jasperengine::bleedAlphaBorder(uint8_t* pixels, int width, int height, int rings){
    if (!pixels || width <= 0 || height <= 0)
        return;
    const int count = width * height;

    // 0 transparent, 1 coloured, 2 queued in the next ring
    std::vector<unsigned char> state(count);
    bool hasTransparent = false;
    bool hasColoured = false;
    for (int i = 0; i < count; i++){
        state[i] = (pixels[i * 4 + 3] > 0) ? 1 : 0;
        hasTransparent = hasTransparent || state[i] == 0;
        hasColoured = hasColoured || state[i] == 1;
    }
    if (!hasTransparent || !hasColoured)
        return;

    std::vector<int> ring;
    std::vector<int> current;

    auto queueNeighbors = [&](int x, int y){
        for (int ny = std::max(0, y - 1); ny <= std::min(height - 1, y + 1); ny++){
            for (int nx = std::max(0, x - 1); nx <= std::min(width - 1, x + 1); nx++){
                int j = ny * width + nx;
                if (state[j] == 0){
                    state[j] = 2;
                    ring.push_back(j);
                }
            }
        }
    };

    for (int y = 0; y < height; y++){
        for (int x = 0; x < width; x++){
            if (state[y * width + x] != 0)
                continue;
            bool touchesColor = false;
            for (int ny = std::max(0, y - 1); ny <= std::min(height - 1, y + 1) && !touchesColor; ny++){
                for (int nx = std::max(0, x - 1); nx <= std::min(width - 1, x + 1); nx++){
                    if (state[ny * width + nx] == 1){
                        touchesColor = true;
                        break;
                    }
                }
            }
            if (touchesColor){
                state[y * width + x] = 2;
                ring.push_back(y * width + x);
            }
        }
    }

    for (int pass = 0; pass < rings && !ring.empty(); pass++){
        current.swap(ring);
        ring.clear();

        for (int i : current){
            int x = i % width;
            int y = i / width;
            int color[3] = {0, 0, 0};
            int n = 0;
            for (int ny = std::max(0, y - 1); ny <= std::min(height - 1, y + 1); ny++){
                for (int nx = std::max(0, x - 1); nx <= std::min(width - 1, x + 1); nx++){
                    int j = ny * width + nx;
                    if (state[j] == 1){
                        for (int c = 0; c < 3; c++)
                            color[c] += pixels[j * 4 + c];
                        n++;
                    }
                }
            }
            for (int c = 0; c < 3; c++)
                pixels[i * 4 + c] = (unsigned char)(color[c] / n);
        }

        for (int i : current){
            state[i] = 1;
        }
        for (int i : current){
            queueNeighbors(i % width, i / width);
        }
    }
}
