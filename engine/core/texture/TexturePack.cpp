// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TexturePack.h"

#include <algorithm>
#include <cstdlib>
#include <vector>

using namespace jasperengine;

namespace {

    using Pick = TexturePack::Pick;
    using Channel = TexturePack::Channel;

    // one source's channel at a texel, as 8 bits: a gray image is its value in R, G and B; a
    // gray-alpha one has its alpha in A; a missing alpha is opaque
    inline unsigned char texel(const TexturePack::Source& source, int x, int y, int channel){
        const int components = source.components;
        int index = channel;
        if (components == 1){
            if (channel == 3) return 255;
            index = 0;
        }else if (components == 2){
            index = channel == 3 ? 1 : 0;
        }else if (components == 3 && channel == 3){
            return 255;
        }
        const size_t offset = ((size_t)y * (size_t)source.width + (size_t)x) * (size_t)components + (size_t)index;
        // 16 bits, little endian: the high byte
        return source.bytesPerComponent == 2 ? source.pixels[offset * 2 + 1] : source.pixels[offset];
    }

    // one channel of a source resampled to width x height: bilinear, texel centres aligned
    void resample(const TexturePack::Source& source, int channel, int width, int height, std::vector<unsigned char>& out){
        out.resize((size_t)width * (size_t)height);
        if (source.width == width && source.height == height){
            for (int y = 0; y < height; y++){
                for (int x = 0; x < width; x++)
                    out[(size_t)y * width + x] = texel(source, x, y, channel);
            }
            return;
        }
        const float scaleX = (float)source.width / (float)width;
        const float scaleY = (float)source.height / (float)height;
        for (int y = 0; y < height; y++){
            const float sy = std::clamp(((float)y + 0.5f) * scaleY - 0.5f, 0.0f, (float)(source.height - 1));
            const int y0 = (int)sy;
            const int y1 = std::min(y0 + 1, source.height - 1);
            const float fy = sy - (float)y0;
            for (int x = 0; x < width; x++){
                const float sx = std::clamp(((float)x + 0.5f) * scaleX - 0.5f, 0.0f, (float)(source.width - 1));
                const int x0 = (int)sx;
                const int x1 = std::min(x0 + 1, source.width - 1);
                const float fx = sx - (float)x0;
                const float top = texel(source, x0, y0, channel) * (1.0f - fx) + texel(source, x1, y0, channel) * fx;
                const float bottom = texel(source, x0, y1, channel) * (1.0f - fx) + texel(source, x1, y1, channel) * fx;
                out[(size_t)y * width + x] = (unsigned char)std::clamp(top * (1.0f - fy) + bottom * fy + 0.5f, 0.0f, 255.0f);
            }
        }
    }

}

const TexturePack::Layout TexturePack::CLEARCOAT = {"cc", {{0, Channel::R}, {1, Channel::G}, {-1, Channel::R}, {-1, Channel::R}}};
const TexturePack::Layout TexturePack::SHEEN = {"sh", {{0, Channel::R}, {0, Channel::G}, {0, Channel::B}, {1, Channel::A}}};
const TexturePack::Layout TexturePack::SPECULAR = {"sp", {{1, Channel::R}, {1, Channel::G}, {1, Channel::B}, {0, Channel::A}}};
const TexturePack::Layout TexturePack::IRIDESCENCE = {"ir", {{0, Channel::R}, {1, Channel::G}, {-1, Channel::R}, {-1, Channel::R}}};
const TexturePack::Layout TexturePack::TRANSMISSION = {"tr", {{0, Channel::R}, {1, Channel::G}, {-1, Channel::R}, {-1, Channel::R}}};

bool TexturePack::Source::valid() const{
    return pixels && width > 0 && height > 0 && components >= 1 && components <= 4 &&
           (bytesPerComponent == 1 || bytesPerComponent == 2);
}

TextureData TexturePack::pack(const Source* first, const Source* second, const Layout& layout){
    const Source* sources[2] = {(first && first->valid()) ? first : nullptr, (second && second->valid()) ? second : nullptr};

    int width = 1;
    int height = 1;
    for (const Source* source : sources){
        if (source){
            width = std::max(width, source->width);
            height = std::max(height, source->height);
        }
    }

    const size_t texels = (size_t)width * (size_t)height;
    unsigned char* pixels = static_cast<unsigned char*>(std::malloc(texels * 4));
    if (!pixels)
        return TextureData();

    std::vector<unsigned char> channelData;
    for (int c = 0; c < 4; c++){
        const Pick& pick = layout.channels[c];
        const Source* source = (pick.source == 0 || pick.source == 1) ? sources[pick.source] : nullptr;
        if (!source){
            for (size_t t = 0; t < texels; t++)
                pixels[t * 4 + c] = 255;
            continue;
        }
        resample(*source, (int)pick.channel, width, height, channelData);
        for (size_t t = 0; t < texels; t++)
            pixels[t * 4 + c] = channelData[t];
    }

    // not owned: copies of a TextureData share its pixels, the one the pool keeps owns them
    // (TextureDataPool::loadGenerated)
    return TextureData(width, height, (unsigned int)(texels * 4), ColorFormat::RGBA, 4, pixels);
}

std::string TexturePack::makeId(const std::string& firstKey, const std::string& secondKey, const Layout& layout){
    return std::string("pack|") + layout.name + "|" + firstKey + "|" + secondKey;
}
