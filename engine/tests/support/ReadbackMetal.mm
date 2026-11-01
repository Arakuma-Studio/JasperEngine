// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Readback.h on Metal (macOS): a blit of level 0 into a shared buffer, on sokol's own command
// queue. Command buffers of one queue run in order, so the blit sees everything sokol committed
// before it (every frame drawn so far), as glFinish + glGetTexImage do on GL.

#include "Readback.h"

#import <Metal/Metal.h>

#include <cstring>

using namespace jasperengine;

namespace {

    // level 0 of `texture`, width * height texels of bytesPerTexel bytes, rows top first (a
    // Metal texture's origin is its top-left corner, as a render target's first row)
    bool readMetalTexture(TextureRender& texture, int width, int height, size_t bytesPerTexel, std::vector<uint8_t>& bytes){
        bytes.clear();
        id<MTLTexture> source = (__bridge id<MTLTexture>)texture.getMetalHandler();
        id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)sg_mtl_command_queue();
        if (!source || !queue || width <= 0 || height <= 0)
            return false;
        if ((NSUInteger)width > source.width || (NSUInteger)height > source.height || source.sampleCount != 1)
            return false;

        const NSUInteger bytesPerRow = (NSUInteger)width * bytesPerTexel;
        const NSUInteger dataSize = bytesPerRow * (NSUInteger)height;
        id<MTLBuffer> buffer = [source.device newBufferWithLength:dataSize options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
        if (!buffer || !commands || !blit)
            return false;

        [blit copyFromTexture:source
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake((NSUInteger)width, (NSUInteger)height, 1)
                     toBuffer:buffer
            destinationOffset:0
       destinationBytesPerRow:bytesPerRow
     destinationBytesPerImage:dataSize];
        [blit endEncoding];
        [commands commit];
        [commands waitUntilCompleted];
        if (commands.status != MTLCommandBufferStatusCompleted)
            return false;

        bytes.resize(dataSize);
        std::memcpy(bytes.data(), buffer.contents, dataSize);
        return true;
    }

}

bool jasperengine::test::finishGpu(){
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)sg_mtl_command_queue();
    if (!queue)
        return false;
    // an empty command buffer completes after every one committed before it
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    [commands commit];
    [commands waitUntilCompleted];
    return commands.status == MTLCommandBufferStatusCompleted;
}

bool jasperengine::test::readTexture(TextureRender& texture, int width, int height, std::vector<uint8_t>& rgba){
    return readTexturePixels(texture, width, height, 4, rgba);
}

bool jasperengine::test::readTexturePixels(TextureRender& texture, int width, int height, int bytesPerPixel, std::vector<uint8_t>& pixels){
    pixels.clear();
    if (bytesPerPixel != 4 && bytesPerPixel != 1)
        return false;
    return readMetalTexture(texture, width, height, static_cast<size_t>(bytesPerPixel), pixels);
}

bool jasperengine::test::readTextureRaw(TextureRender& texture, int width, int height, ColorFormat format, std::vector<uint8_t>& bytes){
    bytes.clear();
    sg_pixel_format pixelFormat;
    size_t bytesPerTexel = 0;
    if (!SokolTexture::getDataPixelFormat(format, pixelFormat, bytesPerTexel))
        return false;
    // the copy keeps the texture's own format
    return readMetalTexture(texture, width, height, bytesPerTexel, bytes);
}

bool jasperengine::test::readTextureFloat(TextureRender& texture, int width, int height, std::vector<float>& rgba){
    rgba.clear();
    std::vector<uint8_t> bytes;
    const size_t count = static_cast<size_t>(width) * height * 4;
    // 16 bytes a texel (RGBA32F), copied as they are
    if (!readMetalTexture(texture, width, height, 16, bytes) || bytes.size() != count * sizeof(float))
        return false;
    rgba.resize(count);
    std::memcpy(rgba.data(), bytes.data(), bytes.size());
    return true;
}
