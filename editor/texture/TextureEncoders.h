// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The encoders of the texture cook (spec S14 D1, D10, D11), each behind ITextureEncoder:
//   - uncompressed: R8 / RG8 / RGBA8 / R16 / RGB565 / RGBA4 / RGB9E5 / RGBA16F + Zstandard
//   - bc7enc_rdo: BC1, BC4, BC5, BC7 (with RDO), scalar
//   - astcenc: ASTC 4x4-12x12, invariant build (byte-identical across ISAs)
//   - basisu: ETC1S, UASTC (with RDO), UASTC HDR; ETC2 / EAC and BC6H through UASTC
// Work is split in bands of block rows of a fixed size, run on the job system and polled for
// cancellation between bands, so the bytes never depend on the thread count. Each encoder
// names its version; one that is not proven ISA-invariant adds the host ISA to the cache key.

#pragma once

#include "texture/TextureFormatSelector.h"
#include "texture/TexturePrepare.h"

#include <functional>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace jasperengine::editor {

    struct TextureEncodeRequest {
        const PreparedTexture* prepared = nullptr;
        TextureFormatChoice choice;
        TextureEffort effort = TextureEffort::Normal;
        TextureImportType type = TextureImportType::Default;
        // key/values stamped on the file (JE.logical, JE.type, JE.flags)
        std::map<std::string, std::string> keyValues;
        // polled between bands; true stops the encode (the result is discarded)
        std::function<bool()> cancelled;
    };

    class ITextureEncoder {
    public:
        virtual ~ITextureEncoder() = default;
        virtual const char* name() const = 0;
        // the library version (and our settings of it): part of the cache key
        virtual std::string version() const = 0;
        // byte-identical output on every ISA (else the key carries textureEncoderIsa())
        virtual bool isaInvariant() const = 0;
        virtual bool encode(const TextureEncodeRequest& request, std::vector<uint8_t>& ktx2, std::string& error) const = 0;
    };

    const ITextureEncoder& textureEncoderFor(const TextureFormatChoice& choice);
    const std::vector<const ITextureEncoder*>& textureEncoders();
    // "x86_64", "arm64"...
    const char* textureEncoderIsa();

    // Zstandard for the KTX2 writer (Ktx2::Compressor)
    bool zstdCompress(std::span<const uint8_t> in, std::vector<uint8_t>& out, int level, void* user);

    // The editor's CPU decoders: ASTC (astcenc) and ETC2 / EAC (basisu) into TextureDecode,
    // the .exr reader (basisu's tinyexr) into TextureData. Once, at startup; idempotent.
    void registerEditorTextureCodecs();

    // the levels of a file decoded to RGBA8 / RG8 / R8 / RGBA16F (any format, Basis included):
    // previews, PSNR, the Game view's platform simulation and cook-textures --decode
    bool decodeKtx2ForPreview(const std::vector<uint8_t>& file, uint32_t level, GpuTextureFormat& format,
                              int& width, int& height, int& faces, std::vector<uint8_t>& pixels, std::string& error);
    // the same, transcoded to a target format (cook-textures --decode --platform-target)
    bool transcodeKtx2(const std::vector<uint8_t>& file, GpuTextureFormat target, std::vector<std::vector<uint8_t>>& levels,
                       int& width, int& height, std::string& error);

}
