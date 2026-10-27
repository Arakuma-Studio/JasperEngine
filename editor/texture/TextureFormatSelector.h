// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The cooked format of a texture for one family (spec S14 R5, R6, R3, D13): the R5 table as
// data, from the type, the quality, whether it has alpha and the family; an explicit format
// overrides it. CPU-readable textures stay uncompressed, and block formats that need sizes in
// multiples of 4 rescale or (sprites, UI) stay uncompressed.

#pragma once

#include "texture/TextureImportSettings.h"
#include "texture/TextureFormat.h"

#include <string>
#include <vector>

namespace jasperengine::editor {

    // what the KTX2 holds: a raw GPU payload, or a Basis file transcoded at load (web)
    enum class TextureCodec : uint8_t { Raw, ETC1S, UASTC, UASTC_HDR };
    const char* textureCodecName(TextureCodec codec);

    struct TextureFormatChoice {
        TextureCodec codec = TextureCodec::Raw;
        // Raw: the stored format. Basis: the format the encoder works towards (reports name it)
        GpuTextureFormat format = GpuTextureFormat::RGBA8;
        // UASTC and BC7: the RDO lambda, 0 = none. ETC1S: the quality level 1-255
        float rdoLambda = 0.0f;
        int etc1sQuality = 128;
        int zstdLevel = 0;              // supercompression of UASTC and raw payloads; 0 = none
        bool opaqueBc7 = false;         // BC7 without alpha (mode 6 opaque): High on opaque textures
        // ETC2 / EAC (android_etc2) and BC6H (desktop HDR) come from a UASTC encode transcoded
        // at cook time (D1)
        bool viaUastc = false;
        int sizeMultiple = 1;           // R6: mip 0 rescaled to multiples of this
        bool uncompressed = false;
        std::vector<std::string> warnings;
        // the warnings `--check` and `check` fail on (R6's uncompressed sprite / UI fallback, a
        // format that cannot hold the data); a rescale to multiples of 4 is a warning only
        std::vector<std::string> problems;

        // "BC7 RDO 1", "ASTC 6x6", "ETC1S q128", "UASTC RDO 1 + Zstd"
        std::string label() const;
    };

    struct TextureFormatInputs {
        TextureFamily family = TextureFamily::Desktop;
        ResolvedTextureSettings settings;
        bool hasAlpha = false;          // any alpha < 255 after the alpha source
        bool floatSource = false;       // .hdr / .exr pixels
        bool sixteenBitSource = false;  // a 16-bit single-channel source (heightmaps)
        bool singleChannelSource = false;
        // mip 0 after maxSize and the project strategy, before R6
        int width = 0, height = 0;
    };

    class TextureFormatSelector {
    public:
        static TextureFormatChoice select(const TextureFormatInputs& inputs);
        // an explicit `format` value the selector accepts (GpuTextureFormat names, ETC1S, UASTC, UASTC_HDR)
        static bool isFormatName(const std::string& name);
        static std::vector<std::string> formatNames();
        // the zstd level of an effort (R5: best 19, normal 15, fast 9)
        static int zstdLevel(TextureEffort effort);
        // block formats of this family need multiples of 4 at mip 0 (BC and ETC2, D3D11; the web
        // transcodes to them)
        static bool needsMultipleOf4(TextureFamily family, const TextureFormatChoice& choice);
    };

}
