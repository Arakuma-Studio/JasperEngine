// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Spec S14 AC3: every cell of R5's table, the alpha / opaque cases, the explicit format
// overrides, quality none, R3's CPU-readable rule and R6's block-size rule.

#include <gtest/gtest.h>

#include "texture/TextureFormatSelector.h"

#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    using F = GpuTextureFormat;

    struct SelectorCase {
        TextureFamily family;
        TextureImportType type;
        bool alpha;
        bool floatSource;
        TextureCookQuality quality;
        TextureCodec codec;
        GpuTextureFormat format;
        float lambda;        // BC7 / UASTC lambda; ETC1S: its quality level
        bool opaqueBc7;
    };

    TextureFormatInputs selectorInputs(TextureFamily family, TextureImportType type, bool alpha, bool floatSource, TextureCookQuality quality,
                                       int width = 256, int height = 256){
        TextureFormatInputs in;
        in.family = family;
        in.settings.type = type;
        in.settings.quality = quality;
        in.hasAlpha = alpha;
        in.floatSource = floatSource;
        in.width = width;
        in.height = height;
        return in;
    }

    std::string selectorName(const SelectorCase& c){
        return std::string(textureFamilyName(c.family)) + "/" + textureImportTypeName(c.type) + (c.alpha ? "/alpha" : "/opaque") +
               (c.floatSource ? "/float" : "") + "/" + textureQualityName(c.quality);
    }

    std::vector<SelectorCase> selectorTable(){
        const auto L = TextureCookQuality::Low, N = TextureCookQuality::Normal, H = TextureCookQuality::High;
        const auto D = TextureFamily::Desktop, M = TextureFamily::Mobile, E = TextureFamily::AndroidEtc2, W = TextureFamily::Web;
        const auto def = TextureImportType::Default, nor = TextureImportType::Normal, spr = TextureImportType::Sprite,
                   ui = TextureImportType::UI, one = TextureImportType::SingleChannel, hdr = TextureImportType::HDR,
                   lmp = TextureImportType::Lightmap;
        const auto R = TextureCodec::Raw, S = TextureCodec::ETC1S, U = TextureCodec::UASTC, UH = TextureCodec::UASTC_HDR;
        std::vector<SelectorCase> cases = {
            // default, opaque
            {D, def, false, false, L, R, F::BC1, 0, false}, {D, def, false, false, N, R, F::BC1, 0, false},
            {D, def, false, false, H, R, F::BC7, 0, true},
            {M, def, false, false, L, R, F::ASTC_8x8, 0, false}, {M, def, false, false, N, R, F::ASTC_6x6, 0, false},
            {M, def, false, false, H, R, F::ASTC_4x4, 0, false},
            {E, def, false, false, N, R, F::ETC2_RGB8, 0, false},
            {W, def, false, false, L, S, F::ETC2_RGB8, 64, false}, {W, def, false, false, N, S, F::ETC2_RGB8, 128, false},
            {W, def, false, false, H, U, F::ASTC_4x4, 1, false},
            // default, alpha
            {D, def, true, false, L, R, F::BC7, 2, false}, {D, def, true, false, N, R, F::BC7, 1, false},
            {D, def, true, false, H, R, F::BC7, 0, false},
            {M, def, true, false, L, R, F::ASTC_8x8, 0, false}, {M, def, true, false, N, R, F::ASTC_6x6, 0, false},
            {M, def, true, false, H, R, F::ASTC_4x4, 0, false},
            {E, def, true, false, N, R, F::ETC2_RGBA8, 0, false},
            {W, def, true, false, L, S, F::ETC2_RGB8, 64, false}, {W, def, true, false, N, S, F::ETC2_RGB8, 128, false},
            {W, def, true, false, H, U, F::ASTC_4x4, 1, false},
            // normal (RG)
            {D, nor, false, false, L, R, F::BC5, 0, false}, {D, nor, false, false, N, R, F::BC5, 0, false},
            {D, nor, false, false, H, R, F::BC5, 0, false},
            {M, nor, false, false, L, R, F::ASTC_8x8, 0, false}, {M, nor, false, false, N, R, F::ASTC_6x6, 0, false},
            {M, nor, false, false, H, R, F::ASTC_5x5, 0, false},
            {E, nor, false, false, N, R, F::EAC_RG11, 0, false},
            {W, nor, false, false, L, U, F::ASTC_4x4, 2, false}, {W, nor, false, false, N, U, F::ASTC_4x4, 1, false},
            {W, nor, false, false, H, U, F::ASTC_4x4, 0, false},
            // sprite, ui
            {D, spr, true, false, L, R, F::BC7, 2, false}, {D, spr, true, false, N, R, F::BC7, 0.5f, false},
            {D, ui, true, false, H, R, F::BC7, 0, false},
            {M, spr, true, false, L, R, F::ASTC_8x8, 0, false}, {M, ui, true, false, N, R, F::ASTC_6x6, 0, false},
            {M, spr, true, false, H, R, F::ASTC_4x4, 0, false},
            {E, spr, false, false, N, R, F::ETC2_RGB8, 0, false}, {E, ui, true, false, N, R, F::ETC2_RGBA8, 0, false},
            {W, spr, true, false, L, S, F::ETC2_RGB8, 128, false}, {W, ui, true, false, N, U, F::ASTC_4x4, 1, false},
            {W, spr, true, false, H, U, F::ASTC_4x4, 0, false},
            // single channel
            {D, one, false, false, L, R, F::BC4, 0, false}, {D, one, false, false, H, R, F::BC4, 0, false},
            {M, one, false, false, L, R, F::ASTC_10x10, 0, false}, {M, one, false, false, N, R, F::ASTC_8x8, 0, false},
            {M, one, false, false, H, R, F::ASTC_6x6, 0, false},
            {E, one, false, false, N, R, F::EAC_R11, 0, false},
            {W, one, false, false, L, S, F::ETC2_RGB8, 128, false}, {W, one, false, false, N, S, F::ETC2_RGB8, 255, false},
            {W, one, false, false, H, U, F::ASTC_4x4, 1, false},
            // hdr, and lightmaps of float sources
            {D, hdr, false, true, L, R, F::BC6H, 0, false}, {D, lmp, false, true, H, R, F::BC6H, 0, false},
            {M, hdr, false, true, L, R, F::RGB9E5, 0, false}, {M, hdr, false, true, N, R, F::RGB9E5, 0, false},
            {M, hdr, false, true, H, R, F::RGBA16F, 0, false},
            {E, hdr, false, true, N, R, F::RGB9E5, 0, false},
            {W, hdr, false, true, N, UH, F::BC6H, 0, false}, {W, lmp, false, true, L, UH, F::BC6H, 0, false},
            // lightmaps of LDR sources: as default
            {D, lmp, false, false, N, R, F::BC1, 0, false}, {M, lmp, false, false, N, R, F::ASTC_6x6, 0, false},
            {W, lmp, true, false, N, S, F::ETC2_RGB8, 128, false},
        };
        return cases;
    }

}

TEST(TextureFormatSelector, EveryCellOfTheTable){
    for (const SelectorCase& c : selectorTable()){
        SCOPED_TRACE(selectorName(c));
        const TextureFormatChoice choice = TextureFormatSelector::select(selectorInputs(c.family, c.type, c.alpha, c.floatSource, c.quality));
        EXPECT_EQ(choice.codec, c.codec);
        EXPECT_EQ(choice.format, c.format) << gpuFormatName(choice.format);
        if (c.codec == TextureCodec::ETC1S){
            EXPECT_EQ(choice.etc1sQuality, (int)c.lambda);
            EXPECT_EQ(choice.zstdLevel, 0);   // BasisLZ
        }else{
            EXPECT_FLOAT_EQ(choice.rdoLambda, c.lambda);
            EXPECT_EQ(choice.zstdLevel, 15);  // normal effort
        }
        EXPECT_EQ(choice.opaqueBc7, c.opaqueBc7);
        EXPECT_FALSE(choice.uncompressed);
        EXPECT_TRUE(choice.warnings.empty());
        // ETC2 / EAC and BC6H come from UASTC at cook time
        const bool viaUastc = c.codec == TextureCodec::Raw &&
                              ((c.format >= F::ETC2_RGB8 && c.format <= F::EAC_RG11) || c.format == F::BC6H);
        EXPECT_EQ(choice.viaUastc, viaUastc);
    }
}

TEST(TextureFormatSelector, ZstdFollowsTheEffort){
    TextureFormatInputs in = selectorInputs(TextureFamily::Desktop, TextureImportType::Default, true, false, TextureCookQuality::Normal);
    in.settings.effort = TextureEffort::Fast;
    EXPECT_EQ(TextureFormatSelector::select(in).zstdLevel, 9);
    in.settings.effort = TextureEffort::Best;
    EXPECT_EQ(TextureFormatSelector::select(in).zstdLevel, 19);
    EXPECT_EQ(TextureFormatSelector::zstdLevel(TextureEffort::Normal), 15);
}

TEST(TextureFormatSelector, QualityNoneIsUncompressed){
    struct { TextureImportType type; bool floatSource; GpuTextureFormat format; } cases[] = {
        {TextureImportType::Default, false, F::RGBA8}, {TextureImportType::Normal, false, F::RG8},
        {TextureImportType::SingleChannel, false, F::R8}, {TextureImportType::HDR, true, F::RGBA16F},
        {TextureImportType::Sprite, false, F::RGBA8}};
    for (TextureFamily family : {TextureFamily::Desktop, TextureFamily::Mobile, TextureFamily::AndroidEtc2, TextureFamily::Web}){
        for (const auto& c : cases){
            const TextureFormatChoice choice =
                TextureFormatSelector::select(selectorInputs(family, c.type, false, c.floatSource, TextureCookQuality::None));
            EXPECT_TRUE(choice.uncompressed);
            EXPECT_EQ(choice.codec, TextureCodec::Raw);
            EXPECT_EQ(choice.format, c.format) << textureFamilyName(family) << " " << textureImportTypeName(c.type);
            EXPECT_EQ(choice.zstdLevel, 15);
        }
    }
}

TEST(TextureFormatSelector, CpuReadableTexturesStayUncompressed){
    TextureFormatInputs in = selectorInputs(TextureFamily::Mobile, TextureImportType::Default, false, false, TextureCookQuality::High);
    in.settings.cpuReadable = true;
    EXPECT_EQ(TextureFormatSelector::select(in).format, F::RGBA8);
    in.sixteenBitSource = true;
    EXPECT_EQ(TextureFormatSelector::select(in).format, F::R16);
    in.sixteenBitSource = false;
    in.singleChannelSource = true;
    EXPECT_EQ(TextureFormatSelector::select(in).format, F::R8);
    in.singleChannelSource = false;
    in.floatSource = true;
    EXPECT_EQ(TextureFormatSelector::select(in).format, F::RGBA16F);
    // large ones are warned about, not refused
    in.floatSource = false;
    in.width = in.height = 2048;
    const TextureFormatChoice big = TextureFormatSelector::select(in);
    EXPECT_TRUE(big.uncompressed);
    ASSERT_EQ(big.warnings.size(), 1u);
    EXPECT_TRUE(big.problems.empty());
}

TEST(TextureFormatSelector, ExplicitFormats){
    TextureFormatInputs in = selectorInputs(TextureFamily::Desktop, TextureImportType::Default, true, false, TextureCookQuality::Normal);
    in.settings.format = "BC3";
    TextureFormatChoice choice = TextureFormatSelector::select(in);
    EXPECT_EQ(choice.codec, TextureCodec::Raw);
    EXPECT_EQ(choice.format, F::BC3);
    EXPECT_TRUE(choice.warnings.empty());

    in.settings.format = "BC7";
    in.settings.rdo = 0.75f;
    choice = TextureFormatSelector::select(in);
    EXPECT_FLOAT_EQ(choice.rdoLambda, 0.75f);
    EXPECT_FALSE(choice.opaqueBc7);

    in.settings.format = "ETC1S";
    in.settings.rdo = 200.0f;
    choice = TextureFormatSelector::select(in);
    EXPECT_EQ(choice.codec, TextureCodec::ETC1S);
    EXPECT_EQ(choice.etc1sQuality, 200);

    in.settings.format = "UASTC";
    in.settings.rdo = -1.0f;
    choice = TextureFormatSelector::select(in);
    EXPECT_EQ(choice.codec, TextureCodec::UASTC);
    EXPECT_FLOAT_EQ(choice.rdoLambda, 1.0f);

    // a format the data does not fit, or the family's GPUs do not sample, is a problem
    in.settings.format = "BC6H";
    choice = TextureFormatSelector::select(in);
    EXPECT_EQ(choice.problems.size(), 1u);
    EXPECT_TRUE(choice.viaUastc);
    in.settings.format = "ASTC_6x6";
    choice = TextureFormatSelector::select(in);
    EXPECT_EQ(choice.problems.size(), 1u);
    in.family = TextureFamily::Mobile;
    in.settings.format = "BC1";
    EXPECT_EQ(TextureFormatSelector::select(in).problems.size(), 1u);
    in.settings.format = "ETC2_RGBA8";
    choice = TextureFormatSelector::select(in);
    EXPECT_TRUE(choice.problems.empty());
    EXPECT_TRUE(choice.viaUastc);

    EXPECT_TRUE(TextureFormatSelector::isFormatName("ASTC_4x4"));
    EXPECT_TRUE(TextureFormatSelector::isFormatName("UASTC_HDR"));
    EXPECT_FALSE(TextureFormatSelector::isFormatName("PVRTC"));
}

TEST(TextureFormatSelector, BlockFormatsNeedMultiplesOfFour){
    // default, normal, single channel, hdr: rescaled to the nearest multiple of 4
    for (TextureImportType type : {TextureImportType::Default, TextureImportType::Normal, TextureImportType::SingleChannel}){
        for (TextureFamily family : {TextureFamily::Desktop, TextureFamily::AndroidEtc2, TextureFamily::Web}){
            const TextureFormatChoice choice =
                TextureFormatSelector::select(selectorInputs(family, type, false, false, TextureCookQuality::Normal, 130, 66));
            EXPECT_EQ(choice.sizeMultiple, 4) << textureFamilyName(family) << " " << textureImportTypeName(type);
            EXPECT_FALSE(choice.uncompressed);
            ASSERT_EQ(choice.warnings.size(), 1u);
            EXPECT_NE(choice.warnings[0].find("132x68"), std::string::npos) << choice.warnings[0];
            // a rescale is no reason for --check to fail
            EXPECT_TRUE(choice.problems.empty());
        }
    }
    // sprite / ui: uncompressed instead, with the warning
    for (TextureImportType type : {TextureImportType::Sprite, TextureImportType::UI}){
        const TextureFormatChoice choice =
            TextureFormatSelector::select(selectorInputs(TextureFamily::Desktop, type, true, false, TextureCookQuality::Normal, 30, 30));
        EXPECT_TRUE(choice.uncompressed);
        EXPECT_EQ(choice.format, F::RGBA8);
        EXPECT_EQ(choice.sizeMultiple, 1);
        EXPECT_EQ(choice.problems.size(), 1u);
    }
    // ASTC takes any size
    const TextureFormatChoice astc =
        TextureFormatSelector::select(selectorInputs(TextureFamily::Mobile, TextureImportType::Sprite, true, false, TextureCookQuality::Normal, 30, 30));
    EXPECT_FALSE(astc.uncompressed);
    EXPECT_EQ(astc.format, F::ASTC_6x6);
    EXPECT_TRUE(astc.problems.empty());
    // and so do the sizes that already are multiples
    EXPECT_TRUE(TextureFormatSelector::select(selectorInputs(TextureFamily::Desktop, TextureImportType::Default, false, false,
                                                             TextureCookQuality::Normal, 128, 64)).problems.empty());
}

TEST(TextureFormatSelector, LabelsNameTheEncoding){
    TextureFormatChoice choice = TextureFormatSelector::select(
        selectorInputs(TextureFamily::Desktop, TextureImportType::Default, true, false, TextureCookQuality::Normal));
    EXPECT_EQ(choice.label(), "BC7 RDO 1 + Zstd");
    choice = TextureFormatSelector::select(selectorInputs(TextureFamily::Web, TextureImportType::Default, false, false, TextureCookQuality::Normal));
    EXPECT_EQ(choice.label(), "ETC1S q128");
    choice = TextureFormatSelector::select(selectorInputs(TextureFamily::Mobile, TextureImportType::Default, false, false, TextureCookQuality::Normal));
    EXPECT_EQ(choice.label(), "ASTC 6x6 + Zstd");
}
