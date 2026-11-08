// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef texturedata_h
#define texturedata_h

#include "render/Render.h"
#include "io/Data.h"
#include "texture/TextureFormat.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine {

    struct Ktx2Image;

    // what a cooked texture says about itself (the KTX2 JE.flags, spec S14 R32)
    inline constexpr uint32_t TEXTURE_FLAG_ALPHA = 1u << 0;            // some alpha < 255
    inline constexpr uint32_t TEXTURE_FLAG_NORMAL_RG = 1u << 1;        // a normal map stored as XY (decodeNormalMap)
    inline constexpr uint32_t TEXTURE_FLAG_COVERAGE = 1u << 2;         // mips keep the alpha-test coverage
    inline constexpr uint32_t TEXTURE_FLAG_DISPLAY_ENCODED = 1u << 3;  // float colour stored pow(1/2.2), like 8-bit colour
    inline constexpr uint32_t TEXTURE_FLAG_CPU_READABLE = 1u << 4;     // never block-compressed (Read/Write, CPU slots)
    inline constexpr uint32_t TEXTURE_FLAG_STREAMABLE = 1u << 5;       // the quality setting and the budget may drop its mips
    inline constexpr uint32_t TEXTURE_FLAG_CUBE = 1u << 6;

    class JASPERENGINE_API TextureData {
    private:

        int width;
        int height;

        int originalWidth;
        int originalHeight;

        unsigned int size; //in bytes
        ColorFormat color_format;
        int channels;
        void* data;

        bool transparent;

        bool dataOwned;

        // Rasterization scale applied to vector (.svg) sources only. Default 1.0.
        float svgScale;

        // The GPU levels (spec S14 D9): level 0 first, each holding every face contiguously, in
        // gpuFormat. levelStorage owns their bytes; with levels, `data` is level 0 when the format
        // is one the CPU readers know (RGBA8, R8, R16; a CPU-readable RGBA16F cook: a linear
        // RGBA32F copy in the same storage) and null otherwise.
        GpuTextureFormat gpuFormat;
        bool srgb;
        int faceCount;
        std::vector<TextureLevel> levels;
        std::shared_ptr<std::vector<uint8_t>> levelStorage;
        uint32_t flags;
        int droppedLevels;      // top mips the loader skipped (mip limit, budget)
        int fullLevelCount;     // levels of the cooked file
        // RGBA8 faces for CPU readers of compressed or float data (IBL from a cooked sky)
        std::shared_ptr<std::array<TextureData, 6>> cpuCopy;

        void addFloatCpuView();
        int getNearestPowerOfTwo(int size);
        // bytes per pixel of the data for an operation that walks it (crop, resize, flip...),
        // 0 with an error naming the operation when it can't be walked: no data, or a float
        // format whose channel count isn't the format's
        int getWalkablePixelSize(const char* operation) const;
        static bool looksLikeSvg(Data* filedata);
        bool loadSvgTexture(Data* filedata);
        // levels -> a plain malloc'd level 0, before an edit of the pixels
        void detachLevels();
        // a KTX2 in memory; decodeToPixels: compressed levels decoded for the CPU (level 0 only)
        bool loadKtx2Bytes(const unsigned char* bytes, unsigned int length, bool decodeToPixels, uint32_t firstLevel,
                           const TextureFormatSupport* support, bool lowMemoryFallback, std::string* error);

    public:

        TextureData();
        TextureData(int width, int height, unsigned int size, ColorFormat color_format, int channels, void* data);
        TextureData(const char* filename);
        TextureData(unsigned char* data, unsigned int dataLength);
        TextureData(const TextureData& v);
        TextureData& operator = ( const TextureData& v );
        bool operator == ( const TextureData& v ) const;
        bool operator != ( const TextureData& v ) const;
        virtual ~TextureData();
        void copy(const TextureData& v);
        
        bool loadTexture(Data* filedata);
        bool loadTextureFromFile(const char* filename);
        bool loadTextureFromMemory(unsigned char* data, unsigned int dataLength);
        // The bytes of the file `filename` names (read elsewhere: an asset reader thread), with
        // the same svgScale suffix and SVG handling as loadTextureFromFile.
        bool loadTextureFromFileData(const char* filename, unsigned char* data, unsigned int dataLength);

        static bool hasSvgExtension(const char* filename);

        // A cooked or source KTX2 file in memory, as the texture pool uploads it: levels from
        // firstLevel down, in the stored format when `support` (the device) samples it, else
        // CPU-decoded or (Basis) transcoded to the fallback TextureFallback picks (spec S14 R11).
        // Without `support` the stored format is kept (Basis: transcoded to RGBA8).
        bool loadKtx2(const unsigned char* bytes, unsigned int length, uint32_t firstLevel = 0,
                      const TextureFormatSupport* support = nullptr, bool lowMemoryFallback = false, std::string* error = nullptr);
        // the same from levels Ktx2::read already kept (the pool reads byte ranges itself); the
        // levels are taken from `image`
        bool loadKtx2Image(Ktx2Image& image, bool decodeToPixels, const TextureFormatSupport* support,
                           bool lowMemoryFallback, std::string& error);

        // Float decoders of extensions stb_image does not read (the editor registers ".exr");
        // `rgba` gets width x height linear RGBA floats
        using FloatDecoder = bool (*)(const unsigned char* bytes, size_t length, std::vector<float>& rgba, int& width, int& height, std::string& error);
        static void registerFloatDecoder(const std::string& extension, FloatDecoder decoder);
        static bool hasFloatDecoder(const std::string& extension);
    private:
        bool loadFloatTexture(FloatDecoder decoder, const unsigned char* bytes, size_t length);
    public:

        // SVG rasterization scale can be carried in a texture path/id as
        // "<path>?svgScale=<value>" so it survives serialization on a Texture reference.
        // parseSvgScalePath returns the clean filesystem path and, when outScale is given,
        // writes the parsed scale into it (left untouched when no suffix is present).
        static std::string parseSvgScalePath(const std::string& path, float* outScale = nullptr);
        // buildSvgScalePath appends the suffix when scale meaningfully differs from 1.0,
        // otherwise returns cleanPath unchanged.
        static std::string buildSvgScalePath(const std::string& cleanPath, float scale);

        static bool loadCubeMapFromSingleFile(const char* filename, std::array<TextureData, 6>& data);
        // loadCubeMapFromSingleFile from the file's bytes
        static bool loadCubeMapFromSingleFileData(const char* filename, unsigned char* bytes, unsigned int length, std::array<TextureData, 6>& data);
        // splits a horizontal (4:3) or vertical (3:4) cross into the six faces; releases `texture`
        static bool splitCubeMapCross(TextureData& texture, std::array<TextureData, 6>& data);

        void releaseImageData();
        
        bool hasAlpha();

        void resizePowerOfTwo();
        void resizeToSquare();
        void fitPowerOfTwo();

        void resize(int newWidth, int newHeight);
        void crop(int xOffset, int yOffset, int newWidth, int newHeight);
        void fitSize(int xOffset, int yOffset, int newWidth, int newHeight);

        void flipVertical();

        // Gives fully transparent pixels next to visible ones the color of their neighbors,
        // which linear filtering would otherwise blend in as a dark outline
        void fixAlphaBorder();

        // 8-bit formats only (RGBA, RED): any other logs an error and gives 0
        unsigned char getColorComponent(int x, int y, int color);
        
        void setDataOwned(bool dataOwned);
        bool getDataOwned() const;

        // Rasterization scale for vector (.svg) sources: 2.0 = twice the intrinsic
        // resolution. Set before loading. No effect on raster images. Default 1.0.
        void setSVGScale(float svgScale);
        float getSVGScale() const;

        int getWidth();
        int getHeight();
        int getOriginalWidth();
        int getOriginalHeight();
        unsigned int getSize();
        ColorFormat getColorFormat();
        int getChannels();
        void* getData();

        bool isTransparent();

        int getMinNearestPowerOfTwo();

        // ---- GPU levels (spec S14)

        // takes the bytes: levelBytes[i] is every face of mip i, `faces` faces of width x height at level 0
        void setLevels(GpuTextureFormat format, bool srgb, int faces, int width, int height, std::vector<std::vector<uint8_t>> levelBytes);
        bool hasLevels() const;
        const std::vector<TextureLevel>& getLevels() const;
        GpuTextureFormat getGpuFormat() const;
        bool isSrgb() const;
        int getFaceCount() const;
        bool isCompressed() const;
        // pixels to upload: levels, or the decoded image
        bool hasPixels() const;
        uint32_t getFlags() const;
        void setFlags(uint32_t flags);
        // the source's size, which Texture::getWidth reports whatever the GPU image is
        void setLogicalSize(int width, int height);
        int getDroppedLevels() const;
        void setDroppedLevels(int dropped, int fullLevelCount);
        int getFullLevelCount() const;
        // bytes of the levels (what the GPU holds of them)
        uint64_t getLevelBytes() const;

        void setCpuCopy(std::shared_ptr<std::array<TextureData, 6>> copy);
        const std::shared_ptr<std::array<TextureData, 6>>& getCpuCopy() const;
        // level 0 of a face as RGBA8 (float display-encoded, grey expanded); false for block formats
        bool toRgba8(std::vector<uint8_t>& out) const;

        // number of bytes per channel for a given color format: 1 for RGBA, RED and SRGB8A8,
        // 2 for RED16 and the half-float formats (RGBA16F, RG16F, R16F), 4 for RGBA32F and R32F.
        // The data of a texture is width * height * channels * getBytesPerChannel bytes
        static int getBytesPerChannel(ColorFormat format);
        // RGBA16F, RGBA32F, R32F, RG16F, R16F: data made in code (TextureData(width, height,
        // size, format, channels, data) with channels 4, 4, 1, 2, 1), never loaded from a file.
        // crop/resize/fitSize/flipVertical copy their texels whole; hasAlpha reads alpha < 1;
        // fixAlphaBorder and getColorComponent are 8-bit only
        static bool isFloatFormat(ColorFormat format);
        // Float data made in code (the constructor above with a float format, no levels): the
        // texture pool uploads it as it is, in its format. A decoded float file (.hdr, .exr) is
        // RGBA32F too, but its gpuFormat is the RGBA16F the importer's processing makes of it
        bool isCodeFloatData() const;
        // the channel count a float format's data has (0 for the others, whose data says it)
        static int getFloatFormatChannels(ColorFormat format);
        // "RGBA", "RED", "RED16", "SRGB8A8", "RGBA16F", "RGBA32F", "R32F", "RG16F", "R16F" (messages)
        static const char* getColorFormatName(ColorFormat format);

        // render callback clean function
        static void cleanupTexture(void* data);
    };
    
}


#endif /* texturedata_h */
