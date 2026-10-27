// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TextureData.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>
#include "stb_image.h"
#include "LunaSVGRuntime.h"
#include "Log.h"
#include "Texture.h"

using namespace jasperengine;

bool TextureData::hasSvgExtension(const char* filename) {
    if (!filename) {
        return false;
    }

    std::string path(filename);
    std::string extension;
    const size_t dotPos = path.find_last_of('.');
    if (dotPos != std::string::npos) {
        extension = path.substr(dotPos);
    }
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".svg";
}

static const std::string SVG_SCALE_MARKER = "?svgScale=";

std::string TextureData::parseSvgScalePath(const std::string& path, float* outScale) {
    const size_t pos = path.find(SVG_SCALE_MARKER);
    if (pos == std::string::npos) {
        return path;
    }

    if (outScale) {
        try {
            *outScale = std::stof(path.substr(pos + SVG_SCALE_MARKER.size()));
        } catch (...) {
            // Malformed suffix: leave the caller's scale untouched.
        }
    }
    return path.substr(0, pos);
}

std::string TextureData::buildSvgScalePath(const std::string& cleanPath, float scale) {
    // No suffix for the default scale (or invalid values): keep the path clean.
    if (!(scale > 0.0f) || std::fabs(scale - 1.0f) < 1e-4f) {
        return cleanPath;
    }

    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%g", scale);
    return cleanPath + SVG_SCALE_MARKER + buffer;
}

bool TextureData::looksLikeSvg(Data* filedata) {
    if (!filedata || !filedata->getMemPtr() || filedata->length() == 0) {
        return false;
    }

    const unsigned int inspectLength = std::min<unsigned int>(filedata->length(), 1024);
    unsigned char* mem = filedata->getMemPtr();

    // Skip an optional UTF-8 BOM so it doesn't hide the first real character.
    unsigned int start = 0;
    if (inspectLength >= 3 && mem[0] == 0xEF && mem[1] == 0xBB && mem[2] == 0xBF) {
        start = 3;
    }
    // Skip leading whitespace.
    while (start < inspectLength && std::isspace(mem[start])) {
        start++;
    }

    // A valid SVG/XML document always begins with '<' (e.g. "<?xml", "<!--",
    // "<!DOCTYPE", or "<svg"). Requiring this avoids misrouting a binary raster
    // image to the SVG decoder just because the bytes "<svg" appear in its metadata.
    if (start >= inspectLength || mem[start] != '<') {
        return false;
    }

    std::string header;
    header.reserve(inspectLength - start);
    for (unsigned int i = start; i < inspectLength; i++) {
        header.push_back(static_cast<char>(std::tolower(mem[i])));
    }

    return header.find("<svg") != std::string::npos;
}

TextureData::TextureData() {
    this->width = 0;
    this->height = 0;
    this->originalWidth = 0;
    this->originalHeight = 0;
    this->size = 0;
    this->color_format = ColorFormat::RGBA;
    this->channels = 0;
    this->data = NULL;

    this->transparent = false;

    this->dataOwned = false;

    this->svgScale = 1.0f;
}

TextureData::TextureData(int width, int height, unsigned int size, ColorFormat color_format, int channels, void* data){
    this->width = width;
    this->height = height;
    this->originalWidth = width;
    this->originalHeight = height;
    this->size = size;
    this->color_format = color_format;
    this->channels = channels;
    this->data = data;

    this->transparent = false;

    this->dataOwned = false;

    this->svgScale = 1.0f;
}

TextureData::TextureData(const char* filename) : TextureData(){
    loadTextureFromFile(filename);
}

TextureData::TextureData(unsigned char* data, unsigned int dataLength) : TextureData(){
    loadTextureFromMemory(data, dataLength);
}

TextureData::TextureData(const TextureData& v){
    this->copy(v);
}

TextureData& TextureData::operator = ( const TextureData& v ){
    this->copy(v);

    return *this;
}

bool TextureData::operator == ( const TextureData& v ) const{
    return (
        v.width == width &&
        v.height == height &&
        v.originalWidth == originalWidth &&
        v.originalHeight == originalHeight &&
        v.size == size &&
        v.color_format == color_format &&
        v.channels == channels &&
        v.data == data &&
        v.transparent == transparent &&
        v.dataOwned == dataOwned
    );
}

bool TextureData::operator != ( const TextureData& v ) const{
    return !(*this == v);
}

bool TextureData::loadTexture(Data* filedata) {
    filedata->seek(0);

    if (dataOwned && data)
        releaseImageData();

    if (looksLikeSvg(filedata)) {
        if (!loadSvgTexture(filedata)) {
            Log::error("Error loading SVG texture");
            return false;
        }
        return true;
    }
    
    //----- Start std_image read texture
    stbi_info_from_memory((stbi_uc const *)filedata->getMemPtr(), filedata->length(), &width, &height, &channels);

    //Renders (not GL) only support 8bpp and 32bpp
    int desired_channels = 1;
    color_format = ColorFormat::RED;

    if (channels != 1){
        desired_channels = 4;
        color_format = ColorFormat::RGBA;
    }

    // Single-channel 16-bit images (e.g. terrain heightmaps) are kept at full precision
    // as RED16. Multi-channel images are still downconverted to 8-bit RGBA.
    const bool is16Bit = desired_channels == 1 &&
                         stbi_is_16_bit_from_memory((stbi_uc const *)filedata->getMemPtr(), filedata->length());

    if (is16Bit){
        color_format = ColorFormat::RED16;
        data = stbi_load_16_from_memory((stbi_uc const *)filedata->getMemPtr(), filedata->length(), &width, &height, &channels, desired_channels);
    }else{
        data = stbi_load_from_memory((stbi_uc const *)filedata->getMemPtr(), filedata->length(), &width, &height, &channels, desired_channels);
    }

    if (!data){
        Log::error("Error loading texture: %s", stbi_failure_reason());
        return false;
    }

    channels = desired_channels;

    size = width * height * channels * (is16Bit ? 2 : 1); //in bytes
    //----- End std_image read texture

    originalWidth = width;
    originalHeight = height;

    transparent = hasAlpha();
    
    return true;
}

bool TextureData::loadSvgTexture(Data* filedata) {
    if (!filedata || !filedata->getMemPtr() || filedata->length() == 0) {
        return false;
    }

    // LunaSVG (the engine's SVG rasteriser, also behind HTML UI's <svg>): intrinsic size in
    // CSS px (96 dpi) x svgScale, rounded up and capped at 16384, straight alpha like every
    // other engine texture. A non-positive or non-finite scale means the intrinsic size.
    std::vector<uint8_t> pixels;
    int rasterWidth = 0;
    int rasterHeight = 0;
    if (!LunaSVGRuntime::rasterize(reinterpret_cast<const char*>(filedata->getMemPtr()), filedata->length(),
                                   svgScale, false, pixels, rasterWidth, rasterHeight)) {
        return false;
    }

    const size_t rasterSize = pixels.size();
    unsigned char* rasterData = static_cast<unsigned char*>(malloc(rasterSize));
    if (!rasterData) {
        return false;
    }
    memcpy(rasterData, pixels.data(), rasterSize);

    data = rasterData;
    width = rasterWidth;
    height = rasterHeight;
    originalWidth = rasterWidth;
    originalHeight = rasterHeight;
    channels = 4;
    color_format = ColorFormat::RGBA;
    size = static_cast<unsigned int>(rasterSize);
    transparent = hasAlpha();

    return true;
}

bool TextureData::loadTextureFromFile(const char* filename) {
    // A texture path may carry a rasterization scale as "<path>?svgScale=<value>".
    // Split it off here: load from the clean path, and let an embedded scale
    // override the current svgScale setting.
    float parsedScale = svgScale;
    std::string cleanPath = parseSvgScalePath(filename ? filename : "", &parsedScale);
    svgScale = parsedScale;

    Data filedata;

    int res = filedata.open(cleanPath.c_str());

    if (res==FileErrors::FILE_NOT_FOUND){
        Log::error("Texture file not found: %s", cleanPath.c_str());
        return false;
    }
    if (res==FileErrors::INVALID_PARAMETER){
        Log::error("Texture file path is invalid: %s", cleanPath.c_str());
        return false;
    }

    bool result = false;
    if (hasSvgExtension(cleanPath.c_str())) {
        if (dataOwned && data)
            releaseImageData();
        result = loadSvgTexture(&filedata);
    } else {
        result = loadTexture(&filedata);
    }

    if (!result){
        Log::error("Texture file not loaded: %s", cleanPath.c_str());
    }

    return result;
}

bool TextureData::loadTextureFromMemory(unsigned char* data, unsigned int dataLength){
    Data filedata;

    filedata.open(data, dataLength, false, false);

    bool result = loadTexture(&filedata);

    if (!result){
        Log::error("Texture from memory not loaded");
    }

    return result;
}

bool TextureData::loadCubeMapFromSingleFile(const char* filename, std::array<TextureData, 6>& data){
    TextureData texture;
    if (!texture.loadTextureFromFile(filename))
        return false;

    int tileW = 0;
    int tileH = 0;

    int right[2];
    int left[2];
    int top[2];
    int bottom[2];
    int front[2];
    int back[2];

    uint64_t ratio = texture.getWidth() * 100 / texture.getHeight();

    // Horizontal Cross
    if (ratio == 133){ // 4:3
        tileW = texture.getWidth() / 4;
        tileH = texture.getHeight() / 3;

        right[0] = 2; right[1] = 1;
        left[0] = 0; left[1] = 1;
        top[0] = 1; top[1] = 0;
        bottom[0] = 1; bottom[1] = 2;
        front[0] = 1; front[1] = 1;
        back[0] = 3; back[1] = 1;

    // Vertical Cross
    }else if (ratio == 75){ // 3:4
        tileW = texture.getWidth() / 3;
        tileH = texture.getHeight() / 4;

        right[0] = 2; right[1] = 1;
        left[0] = 0; left[1] = 1;
        top[0] = 1; top[1] = 0;
        bottom[0] = 1; bottom[1] = 2;
        front[0] = 1; front[1] = 1;
        back[0] = 1; back[1] = 3;
    }else{
        Log::error("Failed to load cubemap: format not supported");
        return false;
    }

    // Face indices follow the backend cubemap convention (sokol/OpenGL style):
    // 0=+X (Right), 1=-X (Left), 2=+Y (Top), 3=-Y (Bottom), 4=+Z, 5=-Z.
    // This loader uses OpenGL-style naming: Front = +Z (4), Back = -Z (5).

    // IMPORTANT:
    // Do not assign/copy 'texture' into each face and then call crop(), because TextureData::copy()
    // is shallow and would make all faces share the same underlying pixel pointer. The first crop()
    // frees that buffer, and subsequent crops crash.
    //
    // Instead, extract each face into its own buffer.

    auto extractFace = [&](int faceIndex, int xTile, int yTile){
        const int xOffset = xTile * tileW;
        const int yOffset = yTile * tileH;

        unsigned char* src = (unsigned char*)texture.getData();
        const int srcW = texture.getWidth();
        const int srcChannels = texture.getChannels();
        const ColorFormat srcFormat = texture.getColorFormat();

        // whole texels: RED16 has two bytes per channel
        const int srcPixelSize = srcChannels * TextureData::getBytesPerChannel(srcFormat);
        const int rowBytes = tileW * srcPixelSize;
        const int bufsize = tileW * tileH * srcPixelSize;
        unsigned char* faceData = (unsigned char*)malloc(bufsize * sizeof(unsigned char));
        if (!faceData)
            throw std::runtime_error("Out of memory while extracting cubemap face");

        for (int y = 0; y < tileH; y++){
            const size_t srcOffset = ((size_t)(yOffset + y) * (size_t)srcW + (size_t)xOffset) * (size_t)srcPixelSize;
            const size_t dstOffset = (size_t)y * (size_t)rowBytes;
            memcpy(faceData + dstOffset, src + srcOffset, (size_t)rowBytes);
        }

        data[faceIndex] = TextureData(tileW, tileH, (unsigned int)bufsize, srcFormat, srcChannels, faceData);
    };

    try {
        // Using JASPERENGINE convention:
        // Right (+X)
        extractFace(0, right[0], right[1]);
        // Left (-X)
        extractFace(1, left[0], left[1]);
        // Top (+Y)
        extractFace(2, top[0], top[1]);
        // Bottom (-Y)
        extractFace(3, bottom[0], bottom[1]);
        // Front (+Z)
        extractFace(4, front[0], front[1]);
        // Back (-Z)
        extractFace(5, back[0], back[1]);
    } catch (...) {
        texture.releaseImageData();
        throw;
    }

    // Release temporary source image data (it is not stored in the returned faces)
    texture.releaseImageData();

    return true;
}

void TextureData::copy ( const TextureData& v ){
    this->width = v.width;
    this->height = v.height;
    this->originalWidth = v.originalWidth;
    this->originalHeight = v.originalHeight;
    this->size = v.size;
    this->color_format = v.color_format;
    this->channels = v.channels;

    this->dataOwned = v.dataOwned;

    this->transparent = v.transparent;

    this->svgScale = v.svgScale;

    this->data = v.data;
}

TextureData::~TextureData() {
    if (dataOwned)
        releaseImageData();
}

void TextureData::releaseImageData(){
    if (data){
        stbi_image_free(data);
        data = NULL;
    }
}

int TextureData::getBytesPerChannel(ColorFormat format){
    switch (format){
        case ColorFormat::RED16:
        case ColorFormat::RGBA16F:
        case ColorFormat::RG16F:
        case ColorFormat::R16F:
            return 2;
        case ColorFormat::RGBA32F:
        case ColorFormat::R32F:
            return 4;
        default:
            return 1;
    }
}

bool TextureData::isFloatFormat(ColorFormat format){
    return getFloatFormatChannels(format) > 0;
}

int TextureData::getFloatFormatChannels(ColorFormat format){
    switch (format){
        case ColorFormat::RGBA16F:
        case ColorFormat::RGBA32F:
            return 4;
        case ColorFormat::RG16F:
            return 2;
        case ColorFormat::R32F:
        case ColorFormat::R16F:
            return 1;
        default:
            return 0;
    }
}

const char* TextureData::getColorFormatName(ColorFormat format){
    switch (format){
        case ColorFormat::RED:     return "RED";
        case ColorFormat::RGBA:    return "RGBA";
        case ColorFormat::RED16:   return "RED16";
        case ColorFormat::SRGB8A8: return "SRGB8A8";
        case ColorFormat::RGBA16F: return "RGBA16F";
        case ColorFormat::RGBA32F: return "RGBA32F";
        case ColorFormat::R32F:    return "R32F";
        case ColorFormat::RG16F:   return "RG16F";
        case ColorFormat::R16F:    return "R16F";
    }
    return "unknown";
}

int TextureData::getWalkablePixelSize(const char* operation) const{
    if (!data){
        Log::error("Texture data %s: no data", operation);
        return 0;
    }
    const int floatChannels = getFloatFormatChannels(color_format);
    if (floatChannels > 0 && channels != floatChannels){
        Log::error("Texture data %s: colour format %s has %i channels, the data says %i",
                   operation, getColorFormatName(color_format), floatChannels, channels);
        return 0;
    }
    return channels * getBytesPerChannel(color_format);
}

int TextureData::getNearestPowerOfTwo(int size){
    int i;
    for(i = 31; i >= 0; i--)
        if ((size - 1) & (1<<i))
            break;
    return (1<<(i + 1));
}

bool TextureData::hasAlpha(){
    if (!data)
        return false;
    if (channels == 4 && color_format == ColorFormat::RGBA16F){
        // half 1.0 is 0x3C00
        const uint16_t* texels = (const uint16_t*)data;
        for (size_t i = 0; i < (size_t)width * (size_t)height; i++){
            if (texels[i * 4 + 3] != 0x3C00)
                return true;
        }
        return false;
    }
    if (channels == 4 && color_format == ColorFormat::RGBA32F){
        const float* texels = (const float*)data;
        for (size_t i = 0; i < (size_t)width * (size_t)height; i++){
            if (texels[i * 4 + 3] != 1.0f)
                return true;
        }
        return false;
    }
    if (channels == 4 && getBytesPerChannel(color_format) == 1){
        for(int y = 0; y < height; y++){
            for(int x = 0; x < width; x++){
                int pixel = (y * (width * channels)) + (x * channels);
                if (((unsigned char*)data)[pixel + 3] != 255)
                    return true;
            }
        }
    }
    return false;
}

void TextureData::crop(int xOffset, int yOffset, int newWidth, int newHeight){

    // whole texels (RED16 and the float formats have several bytes per channel)
    const int pixelSize = getWalkablePixelSize("crop");
    if (pixelSize == 0)
        return;
    if (xOffset < 0 || yOffset < 0 || newWidth <= 0 || newHeight <= 0 ||
        xOffset + newWidth > width || yOffset + newHeight > height){
        Log::error("Texture data crop: %i x %i at (%i, %i) is outside the %i x %i image", newWidth, newHeight, xOffset, yOffset, width, height);
        return;
    }
    
    int rowsize = width * pixelSize;
    int newRowsize = newWidth * pixelSize;
    int bufsize = newWidth * newHeight * pixelSize;
    
    unsigned char* newData = (unsigned char*) malloc(bufsize*sizeof(unsigned char));
    
    int row_cnt;
    long off1 = 0;
    long off2 = 0;
    long off3 = xOffset*pixelSize;
    
    for (row_cnt=yOffset;row_cnt<yOffset+(newHeight);row_cnt++) {
        off1=row_cnt*rowsize;
        off2=(row_cnt-yOffset)*newRowsize;
        
        memcpy(newData+off2,(unsigned char*)data+off1+off3,newRowsize);
    }

    stbi_image_free(data);
    
    width = newWidth;
    height = newHeight;
    size = bufsize;
    data = newData;
}

void TextureData::resizePowerOfTwo(){
    resize(getNearestPowerOfTwo(width), getNearestPowerOfTwo(height));
}

void TextureData::resizeToSquare(){
    if (width != height){
        int side = std::max(width, height);
        resize(side, side);
    }
}

void TextureData::resize(int newWidth, int newHeight){

    if ((newWidth != width) || (newHeight != height)){

        // RED16 and the float formats carry several bytes per channel, so a pixel is not always
        // one byte per channel
        int pixelSize = getWalkablePixelSize("resize");
        if (pixelSize == 0 || newWidth <= 0 || newHeight <= 0)
            return;
        int bufsize = newWidth * newHeight * pixelSize;
        unsigned char* newData = (unsigned char*) malloc(bufsize*sizeof(unsigned char));

        double scaleWidth =  (double)newWidth / (double)width;
        double scaleHeight = (double)newHeight / (double)height;

        for(int cy = 0; cy < newHeight; cy++){
            for(int cx = 0; cx < newWidth; cx++){
                
                int pixel = (cy * (newWidth*pixelSize)) + (cx*pixelSize);
                int nearestMatch =  (((int)(cy / scaleHeight) * (width*pixelSize)) + ((int)(cx / scaleWidth) * pixelSize) );
            
                for (int b = 0; b < pixelSize; b++){
                    newData[pixel + b] =  ((unsigned char*)data)[nearestMatch + b];
                }
            
            }
        }

        stbi_image_free(data);
    
        width = newWidth;
        height = newHeight;
        size = bufsize;
        data = newData;
        
    }

}

void TextureData::fitPowerOfTwo(){
    fitSize(0, 0, getNearestPowerOfTwo(width), getNearestPowerOfTwo(height));
}

void TextureData::fitSize(int xOffset, int yOffset, int newWidth, int newHeight){
    
    if ((newWidth != width) || (newHeight != height)){

        // whole texels (RED16 and the float formats have several bytes per channel)
        const int pixelSize = getWalkablePixelSize("fitSize");
        if (pixelSize == 0)
            return;
        if (xOffset < 0 || yOffset < 0 || xOffset + width > newWidth || yOffset + height > newHeight){
            Log::error("Texture data fitSize: the %i x %i image at (%i, %i) doesn't fit %i x %i", width, height, xOffset, yOffset, newWidth, newHeight);
            return;
        }

        int bufsize = newWidth * newHeight * pixelSize;
        unsigned char* newData = (unsigned char*) malloc(bufsize*sizeof(unsigned char));
        
        for( unsigned int i = 0; i < bufsize; ++i )
        {
            newData[i] = 0;
        }
        
        for( unsigned int i = 0; i < height; ++i )
        {
            memcpy( (unsigned char*)newData + pixelSize * ( newWidth * ( yOffset + i ) + xOffset ), (unsigned char*)data + width * i * pixelSize, width * pixelSize );
        }
        
        stbi_image_free(data);
        
        width = newWidth;
        height = newHeight;
        size = bufsize;
        data = newData;
        
    }
    
}

void TextureData::fixAlphaBorder(){
    // rings of transparent pixels colored around visible ones, enough for a few mip levels
    const int borderSize = 4;

    if (!data || channels != 4 || getBytesPerChannel(color_format) != 1)
        return;

    unsigned char* pixels = (unsigned char*)data;
    const int count = width * height;

    // 0 transparent, 1 colored, 2 queued in the next ring
    std::vector<unsigned char> state(count);
    bool hasTransparent = false;
    for (int i = 0; i < count; i++){
        state[i] = (pixels[i * 4 + 3] > 0) ? 1 : 0;
        if (state[i] == 0)
            hasTransparent = true;
    }
    if (!hasTransparent)
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

    for (int pass = 0; pass < borderSize && !ring.empty(); pass++){
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

void TextureData::flipVertical(){

    // whole texels (RED16 and the float formats have several bytes per channel)
    const int pixelSize = getWalkablePixelSize("flipVertical");
    if (pixelSize == 0)
        return;
    
    int bufsize = width * pixelSize;
    
    unsigned char* tb1 = new unsigned char[bufsize];
    
    int row_cnt;
    long off1 = 0;
    long off2 = 0;
    
    for (row_cnt=0;row_cnt<height/2;row_cnt++) {
        off1=row_cnt*bufsize;
        off2=((height-1)-row_cnt)*bufsize;
        
        memcpy(tb1,(unsigned char*)data+off1,bufsize);
        memcpy((unsigned char*)data+off1,(unsigned char*)data+off2,bufsize);
        memcpy((unsigned char*)data+off2,tb1,bufsize);
    }
    
    delete [] tb1;
    
}

unsigned char TextureData::getColorComponent(int x, int y, int color){
    if (getBytesPerChannel(color_format) != 1){
        Log::error("Texture data getColorComponent: colour format %s is not 8-bit", getColorFormatName(color_format));
        return 0;
    }
    return ((unsigned char*)data)[((x + y*width)*channels)+color];
}

void TextureData::setDataOwned(bool dataOwned){
    this->dataOwned = dataOwned;
}

bool TextureData::getDataOwned() const{
    return this->dataOwned;
}

void TextureData::setSVGScale(float svgScale){
    this->svgScale = svgScale;
}

float TextureData::getSVGScale() const{
    return this->svgScale;
}

int TextureData::getWidth(){
    return width;
}

int TextureData::getHeight(){
    return height;
}

int TextureData::getOriginalWidth(){
    return originalWidth;
}

int TextureData::getOriginalHeight(){
    return originalHeight;
}

unsigned int TextureData::getSize(){
    return size;
}

ColorFormat TextureData::getColorFormat(){
    return color_format;
}

int TextureData::getChannels(){
    return channels;
}

void* TextureData::getData(){
    return data;
}

bool TextureData::isTransparent(){
    return transparent;
}

int TextureData::getMinNearestPowerOfTwo(){
    return getNearestPowerOfTwo(std::min(width, height));
}

void TextureData::cleanupTexture(void* data){
    TextureData* texData = (TextureData*)data;
    texData->releaseImageData();
}
