// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// FrameReadback's Metal reader (docs/specs/S30-shipping.md R20): a blit of the texture into a
// shared buffer on sokol's own command queue, so it runs after the frame that drew it. Header
// only (Objective-C++), included by the macOS and iOS platform sources that install it.

#import <Metal/Metal.h>

#include "render/FrameReadback.h"
#include "sokol_gfx.h"

#include <cstring>
#include <string>
#include <vector>

namespace jasperengine {

    inline bool readMetalTexture(TextureRender& texture, int width, int height, int bytesPerPixel,
                                 std::vector<uint8_t>& pixels, std::string* error) {
        id<MTLTexture> source = (__bridge id<MTLTexture>)const_cast<void*>(texture.getMetalHandler());
        id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)const_cast<void*>(sg_mtl_command_queue());
        if (!source || !queue) {
            if (error) *error = "no Metal texture or command queue to read";
            return false;
        }
        const NSUInteger bytesPerRow = static_cast<NSUInteger>(width) * bytesPerPixel;
        const NSUInteger dataSize = bytesPerRow * static_cast<NSUInteger>(height);
        id<MTLBuffer> buffer = [[source device] newBufferWithLength:dataSize options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
        [blit copyFromTexture:source
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(static_cast<NSUInteger>(width), static_cast<NSUInteger>(height), 1)
                     toBuffer:buffer
            destinationOffset:0
       destinationBytesPerRow:bytesPerRow
     destinationBytesPerImage:dataSize];
        [blit endEncoding];
        [commands commit];
        [commands waitUntilCompleted];
        if ([commands status] != MTLCommandBufferStatusCompleted) {
            if (error) *error = "the Metal readback blit failed";
            return false;
        }
        pixels.resize(dataSize);
        std::memcpy(pixels.data(), [buffer contents], dataSize);
        // BGRA8 drawable formats come back blue first: the PNG is RGBA
        if (bytesPerPixel == 4 && ([source pixelFormat] == MTLPixelFormatBGRA8Unorm || [source pixelFormat] == MTLPixelFormatBGRA8Unorm_sRGB)) {
            for (size_t i = 0; i + 3 < pixels.size(); i += 4) std::swap(pixels[i], pixels[i + 2]);
        }
        return true;
    }

    inline void installMetalReadback() {
        FrameReadback::setPlatformReader(&readMetalTexture);
    }

}
