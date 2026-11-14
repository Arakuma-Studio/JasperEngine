// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// What the pixels of a texture go through between the decoder and the GPU format (spec S14
// R1, R4): the alpha source, the channel of a single-channel texture, the green flip of a
// normal map, the size limits and the mip chain. The editor's cook runs it before encoding and
// the raw load path before uploading, with the same TextureProcessing, so an uncompressed cook
// and a raw load give the same bytes. The editor resolves a TextureProcessing from a texture's
// import settings; a game without settings for a file uses defaultFor() of its decoded pixels.

#ifndef TEXTUREPREPARE_H
#define TEXTUREPREPARE_H

#include "Export.h"
#include "texture/MipChain.h"
#include "texture/TextureFormat.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine {

    class TextureData;

    // Unity's Texture Type, in the importer's names (spec S14 R1)
    enum class TextureImportType : uint8_t { Default, Normal, Sprite, UI, Lightmap, SingleChannel, HDR };
    enum class TextureAlphaSource : uint8_t { Input, FromGray, None };
    enum class TextureChannel : uint8_t { R, G, B, A, Luma };
    // the project's power-of-two strategy (Engine::getTextureStrategy), applied at cook time too
    enum class TextureSizeStrategy : uint8_t { None, Fit, Resize };

    JASPERENGINE_API const char* textureImportTypeName(TextureImportType type);   // "default", "single_channel"
    JASPERENGINE_API bool textureImportTypeFromName(const std::string& name, TextureImportType& type);

    struct TextureProcessing {
        TextureImportType type = TextureImportType::Default;
        bool srgb = true;
        TextureAlphaSource alphaSource = TextureAlphaSource::Input;
        bool alphaIsTransparency = false;   // alpha borders bled (fixAlphaBorder) on every level
        int maxSize = 16384;
        MipFilter resizeFilter = MipFilter::Mitchell;
        bool mipmaps = true;
        MipFilter mipFilter = MipFilter::Mitchell;
        int mipLimit = 0;                   // the most levels kept, 0 = the full chain
        bool preserveCoverage = false;
        float alphaCutoff = 0.5f;
        // binary alpha (S42 R6): after the alpha source and before any resize, alpha becomes 255
        // at alphaCutoff and above and 0 below (RGBA8)
        bool alphaClip = false;
        bool flipGreen = false;
        TextureChannel channel = TextureChannel::R;
        // keep the decoded layout (R16, R8 or RGBA8, no type conversion): CPU-readable textures
        bool keepSourceChannels = false;
        TextureSizeStrategy strategy = TextureSizeStrategy::None;
        // mip-0 dimensions rescaled to multiples of this (block formats need 4, R6); 1 = none
        int sizeMultiple = 1;
        bool cube = false;                  // faces squared to the larger side

        bool operator==(const TextureProcessing& other) const = default;

        // what a game does with a file that has no import settings: today's look (colour RGBA in
        // sRGB, grey and 16-bit data linear), mips on
        static TextureProcessing defaultFor(const TextureData& decoded, bool fixAlphaBorder, bool cube);
    };

    // The levels before encoding: per face, level 0 first, in the layout `layout` says
    struct JASPERENGINE_API PreparedTexture {
        enum class Layout : uint8_t {
            RGBA8,      // colour (srgb says how it is encoded), alpha straight
            RG8,        // normal XY, 0.5 = 0
            R8,
            R16,
            RGBAF       // float RGBA (HDR), display-encoded (pow 1/2.2) like every colour texture
        };
        Layout layout = Layout::RGBA8;
        bool srgb = true;
        bool hasAlpha = false;      // any alpha < 255 after the alpha source
        int width = 0, height = 0;  // level 0 after the size rules
        int logicalWidth = 0, logicalHeight = 0;   // the source's size (Texture::getWidth)
        std::vector<std::vector<MipLevel>> faces;  // faces[f][level]

        int levelCount() const { return faces.empty() ? 0 : (int)faces[0].size(); }
        size_t texelBytes() const;
    };

    class JASPERENGINE_API TexturePrepare {
    public:
        // `faces` are decoded textures (1 or 6, same size and format); with layout RGBAF they must
        // be float (RGBA32F), see TextureData::loadTextureFromFileData's float decode
        static bool prepare(const std::vector<const TextureData*>& faces, const TextureProcessing& processing,
                            PreparedTexture& out, std::string& error);

        // The GPU format the layout uploads as uncompressed (RGBA8, RG8, R8, R16, RGBA16F)
        static GpuTextureFormat uncompressedFormat(PreparedTexture::Layout layout);
        // Every face of every level, faces contiguous per level, in `format` (an uncompressed
        // format of the layout: RGBA8/RGB565/RGBA4 for colour, RG8, R8, R16, RGBA16F/RGB9E5)
        static bool toUncompressed(const PreparedTexture& prepared, GpuTextureFormat format,
                                   std::vector<std::vector<uint8_t>>& levels, std::string& error);

        // the size rules alone: maxSize (aspect kept), the strategy, the multiple, squaring
        static void targetSize(int width, int height, const TextureProcessing& processing, int& outWidth, int& outHeight);
    };

    // float <-> half and RGB9E5 conversions (shared by the cook, the fallbacks and the tests)
    JASPERENGINE_API uint16_t floatToHalf(float value);
    JASPERENGINE_API float halfToFloat(uint16_t value);
    JASPERENGINE_API uint32_t packRGB9E5(float r, float g, float b);
    JASPERENGINE_API void unpackRGB9E5(uint32_t packed, float& r, float& g, float& b);

}

#endif // TEXTUREPREPARE_H
