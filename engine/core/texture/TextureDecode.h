// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// CPU decoding of block-compressed levels (spec S14 R11): the desktop fallback when a GPU
// cannot sample a cooked BC format (macOS GL has no BPTC), and the decode of previews and of
// the Game view's platform simulation. The engine decodes BC1-BC7 (bcdec); the editor
// registers its ASTC and ETC2/EAC decoders. Also the uncompressed conversions of the
// low-memory fallback (RGB565 / RGBA4).

#ifndef TEXTUREDECODE_H
#define TEXTUREDECODE_H

#include "Export.h"
#include "texture/TextureFormat.h"

#include <cstdint>
#include <vector>

namespace jasperengine {

    class JASPERENGINE_API TextureDecode {
    public:
        // a level of one face: width x height texels of `format` (blocks rounded up) -> the
        // decoded format's texels, rows top first
        using Decoder = bool (*)(GpuTextureFormat format, int width, int height, const uint8_t* data, size_t size,
                                 std::vector<uint8_t>& out);

        // what a block format decodes to: RGBA8 (colour), RG8 (BC5, EAC RG11), R8 (BC4, EAC R11),
        // RGBA16F (BC6H, RGB9E5); uncompressed formats are their own
        static GpuTextureFormat decodedFormat(GpuTextureFormat format);
        static bool canDecode(GpuTextureFormat format);
        static bool decode(GpuTextureFormat format, int width, int height, const uint8_t* data, size_t size,
                           std::vector<uint8_t>& out);
        // every face of a level (faces contiguous, as KTX2 and the uploads keep them)
        static bool decodeFaces(GpuTextureFormat format, int width, int height, int faces, const uint8_t* data, size_t size,
                                std::vector<uint8_t>& out);
        // the editor's ASTC and ETC2/EAC decoders (and the tests' fakes)
        static void registerDecoder(GpuTextureFormat format, Decoder decoder);

        // RGBA8 -> RGB565 / RGBA4 / RGBA8, R8/RG8 -> themselves; RGBA16F -> RGBA8 (display-encoded
        // values clamped): the last-resort conversions
        static bool convert(GpuTextureFormat from, GpuTextureFormat to, int width, int height, const uint8_t* data, size_t size,
                            std::vector<uint8_t>& out);
    };

}

#endif // TEXTUREDECODE_H
