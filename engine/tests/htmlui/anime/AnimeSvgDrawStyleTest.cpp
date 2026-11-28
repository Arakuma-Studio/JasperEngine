// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/runtime/SvgPreprocess.h"

#include "tinyxml2.h"
#include <lunasvg.h>

#include <gtest/gtest.h>

#include <string>

using namespace jasperengine::htmlui::compat;

namespace {

    std::string styledDrawableSvg(const char* draw, const char* dashes, const char* offset, bool marked = true){
        return std::string("<svg width='120' height='40' viewBox='0 0 120 40'><path d='M10 20L110 20' fill='none' ") +
            "stroke-linecap='butt' stroke-dasharray='" + dashes + "' stroke-dashoffset='" + offset + "' " +
            (marked ? "data-anime-drawable='' " : "") + "draw='" + draw + "' " +
            "style=\"stroke:red;stroke-width:8;stroke-dasharray:none!important;stroke-dashoffset:0!important;"
            "stroke-linecap:round;opacity:.75;--token:'a;b'\"/></svg>";
    }

    lunasvg::Bitmap styledDrawableBitmap(const SvgResult& source){
        const auto document = lunasvg::Document::loadFromData(source.xml);
        return document ? document->renderToBitmap(120, 40) : lunasvg::Bitmap();
    }

    uint8_t styledDrawableAlpha(const lunasvg::Bitmap& bitmap, int x, int y){
        return bitmap.data()[(size_t)y * bitmap.stride() + (size_t)x * 4 + 3];
    }

    std::string styledDrawableInlineStyle(const std::string& source){
        tinyxml2::XMLDocument document;
        if (document.Parse(source.c_str(), source.size()) != tinyxml2::XML_SUCCESS) return {};
        const auto* path = document.RootElement()->FirstChildElement("path");
        const char* style = path ? path->Attribute("style") : nullptr;
        return style ? style : "";
    }

}

TEST(AnimeSvgDrawStyle, DrawRangesOverrideExportedInlineDashesAndOffset){
    const SvgResult hidden = SvgPreprocess::process(styledDrawableSvg("0 0", "0 101", "0"), SvgUse::Inline);
    ASSERT_TRUE(hidden.ok);
    const auto hiddenBitmap = styledDrawableBitmap(hidden);
    ASSERT_TRUE(hiddenBitmap.valid());
    for (int y = 0; y < hiddenBitmap.height(); ++y)
        for (int x = 0; x < hiddenBitmap.width(); ++x)
            ASSERT_EQ(styledDrawableAlpha(hiddenBitmap, x, y), 0) << x << "," << y;

    const auto first = styledDrawableBitmap(SvgPreprocess::process(styledDrawableSvg("0 .5", "50 51", "0"), SvgUse::Inline));
    ASSERT_TRUE(first.valid());
    EXPECT_GT(styledDrawableAlpha(first, 20, 20), 0);
    EXPECT_EQ(styledDrawableAlpha(first, 90, 20), 0);
    const auto last = styledDrawableBitmap(SvgPreprocess::process(styledDrawableSvg(".5 1", "50 51", "-50"), SvgUse::Inline));
    ASSERT_TRUE(last.valid());
    EXPECT_EQ(styledDrawableAlpha(last, 20, 20), 0);
    EXPECT_GT(styledDrawableAlpha(last, 90, 20), 0);
}

TEST(AnimeSvgDrawStyle, OriginalCapAndUnrelatedStyleSurviveBoundedOverrides){
    const SvgResult source = SvgPreprocess::process(styledDrawableSvg("0 1", "100 0", "0"), SvgUse::Inline);
    ASSERT_TRUE(source.ok);
    const auto bitmap = styledDrawableBitmap(source);
    ASSERT_TRUE(bitmap.valid());
    EXPECT_GT(styledDrawableAlpha(bitmap, 7, 20), 0) << source.xml; // round cap extends before the path, despite the butt attribute
    const std::string style = styledDrawableInlineStyle(source.xml);
    EXPECT_NE(style.find("opacity:.75"), std::string::npos);
    EXPECT_NE(style.find("--token:'a;b'"), std::string::npos);
    EXPECT_EQ(SvgPreprocess::process(source.xml, SvgUse::Inline).xml, source.xml);
}

TEST(AnimeSvgDrawStyle, UnmarkedSvgKeepsItsAuthoredInlineStyle){
    const std::string input = styledDrawableSvg("0 0", "0 101", "0", false);
    const SvgResult source = SvgPreprocess::process(input, SvgUse::Inline);
    ASSERT_TRUE(source.ok);
    EXPECT_EQ(styledDrawableInlineStyle(source.xml), styledDrawableInlineStyle(input));
}
