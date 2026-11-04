// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The TITLE_ART program keys (docs/specs/S80-title-art-core.md §6.3): the program number in the low
// property bits, the RGBA8 encodings after it, the variants a device's caps select. That every
// program compiles for every target is the editor's TitleArtShaderBuild (an editor test: the
// editor's build has every back end).

#include <gtest/gtest.h>

#include "pool/ShaderPool.h"
#include "titleart/render/TitlePasses.h"

#include <set>

using namespace jasperengine;
using namespace jasperengine::titleart;

TEST(TitleArtShadersTest, ProgramKeys) {
    // the program is the low property bits, the RGBA8 encodings after them; names round-trip
    const ShaderKey key = titleProgramKey(TitleProgram::Outline, TITLE_VARIANT_DISTANCE_FIXED);
    EXPECT_EQ(key.type, (uint16_t)ShaderType::TITLE_ART);
    EXPECT_EQ(key.properties & 0x7Fu, (uint64_t)TitleProgram::Outline);
    EXPECT_EQ(ShaderPool::getShaderStr(ShaderType::TITLE_ART, key.properties), "titleart_Pb0Pb1Pb3Dfx");
    TitleArtCaps fixedCaps;
    fixedCaps.buffers[(int)TitleBuffer::Seeds].encoding = TitleEncoding::Fixed12_4;
    fixedCaps.buffers[(int)TitleBuffer::Distance].encoding = TitleEncoding::Fixed16;
    EXPECT_EQ(titleProgramVariant(TitleProgram::Jfa, fixedCaps), TITLE_VARIANT_SEEDS_FIXED);
    EXPECT_EQ(titleProgramVariant(TitleProgram::Distance, fixedCaps), TITLE_VARIANT_SEEDS_FIXED | TITLE_VARIANT_DISTANCE_FIXED);
    EXPECT_EQ(titleProgramVariant(TitleProgram::ColorOverlay, fixedCaps), 0u) << "programs without seeds or distances have one variant";
    std::set<std::string> names;
    for (int p = 1; p <= TITLE_PROGRAM_LAST; p++) EXPECT_TRUE(names.insert(titleProgramName((TitleProgram)p)).second);
}
