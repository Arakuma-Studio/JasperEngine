// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef texturedata_h
#define texturedata_h

#include "render/Render.h"
#include "io/Data.h"

#include <array>
#include <string>

namespace jasperengine {

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

        int getNearestPowerOfTwo(int size);
        // bytes per pixel of the data for an operation that walks it (crop, resize, flip...),
        // 0 with an error naming the operation when it can't be walked: no data, or a float
        // format whose channel count isn't the format's
        int getWalkablePixelSize(const char* operation) const;
        static bool looksLikeSvg(Data* filedata);
        bool loadSvgTexture(Data* filedata);

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

        static bool hasSvgExtension(const char* filename);

        // SVG rasterization scale can be carried in a texture path/id as
        // "<path>?svgScale=<value>" so it survives serialization on a Texture reference.
        // parseSvgScalePath returns the clean filesystem path and, when outScale is given,
        // writes the parsed scale into it (left untouched when no suffix is present).
        static std::string parseSvgScalePath(const std::string& path, float* outScale = nullptr);
        // buildSvgScalePath appends the suffix when scale meaningfully differs from 1.0,
        // otherwise returns cleanPath unchanged.
        static std::string buildSvgScalePath(const std::string& cleanPath, float scale);

        static bool loadCubeMapFromSingleFile(const char* filename, std::array<TextureData, 6>& data);

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

        // number of bytes per channel for a given color format: 1 for RGBA, RED and SRGB8A8,
        // 2 for RED16 and the half-float formats (RGBA16F, RG16F, R16F), 4 for RGBA32F and R32F.
        // The data of a texture is width * height * channels * getBytesPerChannel bytes
        static int getBytesPerChannel(ColorFormat format);
        // RGBA16F, RGBA32F, R32F, RG16F, R16F: data made in code (TextureData(width, height,
        // size, format, channels, data) with channels 4, 4, 1, 2, 1), never loaded from a file.
        // crop/resize/fitSize/flipVertical copy their texels whole; hasAlpha reads alpha != 1;
        // fixAlphaBorder and getColorComponent are 8-bit only
        static bool isFloatFormat(ColorFormat format);
        // the channel count a float format's data has (0 for the others, whose data says it)
        static int getFloatFormatChannels(ColorFormat format);
        // "RGBA", "RED", "RED16", "SRGB8A8", "RGBA16F", "RGBA32F", "R32F", "RG16F", "R16F" (messages)
        static const char* getColorFormatName(ColorFormat format);

        // render callback clean function
        static void cleanupTexture(void* data);
    };
    
}


#endif /* texturedata_h */
