// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// How a cooked texture reaches the GPU when the device cannot sample its stored format
// (spec S14 R11, D5): native upload, a CPU decode (BC -> RGBA8/RG8/R8/RGBA16F on desktop), or a
// Basis transcode to the first format of the content's priority list the device samples; then
// the last resort (RGB565 / RGBA4 with the low-memory fallback on, else RGBA8; R8 / RG8 for
// single-channel and normal textures).

#ifndef TEXTUREFALLBACK_H
#define TEXTUREFALLBACK_H

#include "Export.h"
#include "texture/Ktx2.h"
#include "texture/TextureFormat.h"

#include <string>
#include <vector>

namespace jasperengine {

    // what a texture holds, for the transcode lists (from the KTX2 JE.type / JE.flags)
    enum class TextureContent : uint8_t { ColorOpaque, ColorAlpha, Normal, Single, Hdr };

    struct TextureUploadPlan {
        enum class Path : uint8_t { Native, CpuDecode, Transcode, Unavailable };
        Path path = Path::Native;
        GpuTextureFormat upload = GpuTextureFormat::RGBA8;
        // after a CPU decode: the decoded format is converted to `upload` (RGB565/RGBA4)
        GpuTextureFormat decoded = GpuTextureFormat::RGBA8;
        std::string reason;     // why it is not native ("BC7 is not supported by this GPU")
    };

    class JASPERENGINE_API TextureFallback {
    public:
        static TextureContent contentOf(const Ktx2Info& info);
        // the KTX2 JE.flags of a texture, as the cook writes them ("alpha,normalRG")
        static bool hasFlag(const Ktx2Info& info, const char* flag);

        // D5: the Basis transcode targets of a codec and content, in priority order, then R11's
        // last resort
        static std::vector<GpuTextureFormat> transcodeCandidates(Ktx2Codec codec, TextureContent content, bool lowMemoryFallback);
        static TextureUploadPlan plan(const Ktx2Info& info, const TextureFormatSupport& support, bool lowMemoryFallback);
        // a raw-payload format alone (the uncompressed loads and the tests)
        static TextureUploadPlan planRaw(GpuTextureFormat stored, TextureContent content, const TextureFormatSupport& support,
                                         bool lowMemoryFallback);

        // R11: the first fallback of each stored format (one for all Basis files) logs once,
        // at info level, with its reason
        static void logOnce(GpuTextureFormat stored, bool basis, const std::string& reason);
        static void resetLogOnce();   // tests
    };

}

#endif // TEXTUREFALLBACK_H
