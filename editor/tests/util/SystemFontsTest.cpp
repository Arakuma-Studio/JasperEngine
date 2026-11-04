// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The system CJK and emoji fallbacks of the editor's fonts (editor/util/SystemFonts.h): the
// language order, fontconfig's answer, the font checks, and the merge into fonts that already
// exist. The fonts are built here (one square glyph per code point), so no font file is shipped.

#include "util/SystemFonts.h"

#include "AppSettings.h"

#include "imgui.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace jasperengine::editor;

namespace {

    void put16(std::string& out, uint32_t value) {
        out.push_back(static_cast<char>((value >> 8) & 0xFF));
        out.push_back(static_cast<char>(value & 0xFF));
    }

    void put32(std::string& out, uint32_t value) {
        put16(out, value >> 16);
        put16(out, value & 0xFFFF);
    }

    std::string utf16Be(const std::string& ascii) {
        std::string out;
        for (char c : ascii) put16(out, static_cast<unsigned char>(c));
        return out;
    }

    // A TrueType font whose glyphs are squares, one per code point, named `family` (name ID 1) and
    // `postScript` (name ID 6). Table offsets count from `base`: where the font starts in its file.
    std::string buildFont(const std::vector<uint32_t>& codepoints, const std::string& family, const std::string& postScript,
                          uint32_t base = 0) {
        const uint32_t glyphs = static_cast<uint32_t>(codepoints.size()) + 1;   // + .notdef
        std::vector<std::pair<std::string, std::string>> tables;

        std::string cmap;
        put16(cmap, 0); put16(cmap, 1);
        put16(cmap, 3); put16(cmap, 10); put32(cmap, 12);
        put16(cmap, 12); put16(cmap, 0); put32(cmap, 16 + 12 * static_cast<uint32_t>(codepoints.size())); put32(cmap, 0);
        put32(cmap, static_cast<uint32_t>(codepoints.size()));
        for (size_t i = 0; i < codepoints.size(); i++) {
            put32(cmap, codepoints[i]); put32(cmap, codepoints[i]); put32(cmap, static_cast<uint32_t>(i + 1));
        }
        tables.push_back({"cmap", cmap});

        std::string glyph;
        put16(glyph, 1); put16(glyph, 100); put16(glyph, 0); put16(glyph, 900); put16(glyph, 700);
        put16(glyph, 3); put16(glyph, 0);
        for (int i = 0; i < 4; i++) glyph.push_back(0x01);
        for (int16_t dx : {100, 800, 0, -800}) put16(glyph, static_cast<uint16_t>(dx));
        for (int16_t dy : {0, 0, 700, 0}) put16(glyph, static_cast<uint16_t>(dy));
        while (glyph.size() % 4) glyph.push_back(0);
        std::string glyf;
        std::string loca;
        put32(loca, 0);
        put32(loca, 0);   // .notdef: empty
        for (size_t i = 0; i < codepoints.size(); i++) {
            glyf += glyph;
            put32(loca, static_cast<uint32_t>(glyf.size()));
        }
        tables.push_back({"glyf", glyf});

        std::string head;
        put32(head, 0x00010000); put32(head, 0x00010000); put32(head, 0); put32(head, 0x5F0F3CF5);
        put16(head, 0x000B); put16(head, 1000);
        for (int i = 0; i < 4; i++) put32(head, 0);   // created, modified
        put16(head, 0); put16(head, 0); put16(head, 1000); put16(head, 800);
        put16(head, 0); put16(head, 8); put16(head, 2); put16(head, 1); put16(head, 0);
        tables.push_back({"head", head});

        std::string hhea;
        put32(hhea, 0x00010000); put16(hhea, 800); put16(hhea, static_cast<uint16_t>(-200)); put16(hhea, 0);
        put16(hhea, 1000); put16(hhea, 0); put16(hhea, 0); put16(hhea, 1000);
        put16(hhea, 1); put16(hhea, 0); put16(hhea, 0);
        for (int i = 0; i < 4; i++) put16(hhea, 0);
        put16(hhea, 0); put16(hhea, glyphs);
        tables.push_back({"hhea", hhea});

        std::string hmtx;
        for (uint32_t i = 0; i < glyphs; i++) { put16(hmtx, 1000); put16(hmtx, 100); }
        tables.push_back({"hmtx", hmtx});
        tables.push_back({"loca", loca});

        std::string maxp;
        put32(maxp, 0x00010000); put16(maxp, glyphs); put16(maxp, 4); put16(maxp, 1);
        for (int i = 0; i < 4; i++) put16(maxp, 0);
        put16(maxp, 2);
        for (int i = 0; i < 6; i++) put16(maxp, 0);
        tables.push_back({"maxp", maxp});

        const std::string familyText = utf16Be(family);
        const std::string postScriptText = utf16Be(postScript);
        std::string name;
        put16(name, 0); put16(name, 2); put16(name, 6 + 12 * 2);
        put16(name, 3); put16(name, 1); put16(name, 0x409); put16(name, 1);
        put16(name, static_cast<uint32_t>(familyText.size())); put16(name, 0);
        put16(name, 3); put16(name, 1); put16(name, 0x409); put16(name, 6);
        put16(name, static_cast<uint32_t>(postScriptText.size())); put16(name, static_cast<uint32_t>(familyText.size()));
        name += familyText + postScriptText;
        tables.push_back({"name", name});

        std::string post;
        put32(post, 0x00030000); put32(post, 0); put16(post, static_cast<uint16_t>(-100)); put16(post, 50);
        for (int i = 0; i < 5; i++) put32(post, 0);
        tables.push_back({"post", post});

        std::string font;
        put32(font, 0x00010000); put16(font, static_cast<uint32_t>(tables.size()));
        put16(font, 128); put16(font, 3); put16(font, static_cast<uint32_t>(tables.size()) * 16 - 128);
        uint32_t offset = 12 + 16 * static_cast<uint32_t>(tables.size());
        std::string data;
        for (const auto& [tag, bytes] : tables) {
            font += tag;
            put32(font, 0);
            put32(font, base + offset + static_cast<uint32_t>(data.size()));
            put32(font, static_cast<uint32_t>(bytes.size()));
            data += bytes;
            while (data.size() % 4) data.push_back(0);
        }
        return font + data;
    }

    // A collection of fonts (TTC)
    std::string buildCollection(const std::vector<std::pair<std::string, std::string>>& names, const std::vector<uint32_t>& codepoints) {
        const uint32_t header = 12 + 4 * static_cast<uint32_t>(names.size());
        std::vector<std::string> fonts;
        uint32_t base = header;
        for (const auto& [family, postScript] : names) {
            fonts.push_back(buildFont(codepoints, family, postScript, base));
            base += static_cast<uint32_t>(fonts.back().size());
        }
        std::string out = "ttcf";
        put32(out, 0x00010000); put32(out, static_cast<uint32_t>(names.size()));
        base = header;
        for (const std::string& font : fonts) { put32(out, base); base += static_cast<uint32_t>(font.size()); }
        for (const std::string& font : fonts) out += font;
        return out;
    }

    struct ImGuiScope {
        ImGuiContext* previous = ImGui::GetCurrentContext();
        ImGuiContext* context = ImGui::CreateContext();
        ImGuiScope() { ImGui::SetCurrentContext(context); ImGui::GetIO().IniFilename = nullptr; }
        ~ImGuiScope() { ImGui::DestroyContext(context); ImGui::SetCurrentContext(previous); }
    };

}

TEST(SystemFonts, SearchOrder) {
    using L = CjkLanguage;
    EXPECT_EQ(SystemFonts::searchOrder(L::Auto, "ja_JP.UTF-8"),
              (std::vector<L>{L::Japanese, L::SimplifiedChinese, L::Korean}));
    EXPECT_EQ(SystemFonts::searchOrder(L::Auto, "zh_TW.UTF-8").front(), L::TraditionalChinese);
    EXPECT_EQ(SystemFonts::searchOrder(L::Auto, "zh-Hant-HK").front(), L::TraditionalChinese);
    EXPECT_EQ(SystemFonts::searchOrder(L::Auto, "zh_CN.UTF-8").front(), L::SimplifiedChinese);
    EXPECT_EQ(SystemFonts::searchOrder(L::Auto, "ko-KR").front(), L::Korean);
    EXPECT_EQ(SystemFonts::searchOrder(L::Auto, "en_US.UTF-8"),
              (std::vector<L>{L::SimplifiedChinese, L::Korean, L::Japanese}));
    // "jav" (Javanese) is not Japanese
    EXPECT_EQ(SystemFonts::searchOrder(L::Auto, "jav_ID").front(), L::SimplifiedChinese);
    // the preference wins over the locale
    EXPECT_EQ(SystemFonts::searchOrder(L::Japanese, "en_US.UTF-8"),
              (std::vector<L>{L::Japanese, L::SimplifiedChinese, L::Korean}));
    EXPECT_EQ(SystemFonts::searchOrder(L::Korean, "ja_JP"), (std::vector<L>{L::Korean, L::SimplifiedChinese, L::Japanese}));
}

TEST(SystemFonts, SettingNamesRoundTrip) {
    for (CjkLanguage language : {CjkLanguage::Auto, CjkLanguage::Japanese, CjkLanguage::Korean,
                                 CjkLanguage::SimplifiedChinese, CjkLanguage::TraditionalChinese}) {
        EXPECT_EQ(SystemFonts::fromSettingName(SystemFonts::settingName(language)), language);
        EXPECT_TRUE(SystemFonts::isLanguageSetting(SystemFonts::settingName(language)));
    }
    EXPECT_FALSE(SystemFonts::isLanguageSetting("klingon"));
    EXPECT_STREQ(SystemFonts::languageTag(CjkLanguage::TraditionalChinese), "zh-Hant");
}

TEST(SystemFonts, ParsesFontconfigOutput) {
    // as `fc-match -s -f '%{file}\t%{index}\t%{lang}\n' 'sans-serif:lang=ja'` prints it
    const std::string output =
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc\t0\tja|zh-cn|ko\n"
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf\t0\ten|fr\n"
        "/usr/share/fonts/variable/Sans.ttf\t65537\tja\n"
        "/usr/share/fonts/X11/misc/k14.pcf.gz\t0\tja\n"
        "/usr/share/fonts/opentype/noto/NotoSerifCJK-Regular.TTC\t2\tja\n"
        "\n";
    const auto fonts = SystemFonts::parseFontconfig(output, "ja");
    ASSERT_EQ(fonts.size(), 2u);
    EXPECT_EQ(fonts[0].first, "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc");
    EXPECT_EQ(fonts[0].second, 0);
    EXPECT_EQ(fonts[1].second, 2) << "an upper-case extension is a font too";
    EXPECT_EQ(SystemFonts::parseFontconfig(output, "").size(), 3u) << "no language: every loadable font";
    EXPECT_TRUE(SystemFonts::parseFontconfig("", "ja").empty());
}

TEST(SystemFonts, ChecksFontsAndCollections) {
    const std::string font = buildFont({U'A', U'中'}, "Test Sans", "TestSans-Regular");
    EXPECT_TRUE(SystemFonts::isLoadable(font.data(), font.size(), 0));
    EXPECT_FALSE(SystemFonts::isLoadable(font.data(), font.size(), 1)) << "one face only";
    const std::string junk(512, '\x5A');
    EXPECT_FALSE(SystemFonts::isLoadable(junk.data(), junk.size(), 0));
    EXPECT_FALSE(SystemFonts::isLoadable(nullptr, 0, 0));
    EXPECT_TRUE(SystemFonts::hasOutline(font.data(), font.size(), 0, 0x4E2D));
    EXPECT_FALSE(SystemFonts::hasOutline(font.data(), font.size(), 0, 0x1F600));

    const std::string collection = buildCollection({{"Noto Sans CJK JP", "NotoSansCJKjp-Regular"},
                                                    {"Noto Sans CJK KR", "NotoSansCJKkr-Regular"}}, {U'中'});
    const auto names = SystemFonts::faceNames(collection.data(), collection.size());
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[1].first, "Noto Sans CJK KR");
    EXPECT_EQ(names[1].second, "NotoSansCJKkr-Regular");
    EXPECT_TRUE(SystemFonts::isLoadable(collection.data(), collection.size(), 1));
    EXPECT_FALSE(SystemFonts::isLoadable(collection.data(), collection.size(), 2)) << "past the collection";
}

TEST(SystemFonts, MergesIntoExistingFonts) {
    ImGuiScope scope;
    ImGuiIO& io = ImGui::GetIO();
    // the editor's own font (Latin) and the system's CJK and emoji fonts found later
    std::string latin = buildFont({U'A', U'B'}, "Latin", "Latin-Regular");
    std::string cjk = buildCollection({{"Noto Sans CJK JP", "NotoSansCJKjp-Regular"}, {"Noto Sans CJK SC", "NotoSansCJKsc-Regular"}},
                                      {U'A', U'中', U'한'});
    std::string emoji = buildFont({0x1F600}, "Emoji", "Emoji-Regular");
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false;
    ImFont* ui = io.Fonts->AddFontFromMemoryTTF(latin.data(), static_cast<int>(latin.size()), 15.0f, &config);
    ImFont* code = io.Fonts->AddFontFromMemoryTTF(latin.data(), static_cast<int>(latin.size()), 16.0f, &config);
    ASSERT_NE(ui, nullptr);
    EXPECT_FALSE(ui->IsGlyphInFont(0x4E2D));

    std::vector<SystemFonts::Font> fonts(2);
    fonts[0].path = "NotoSansCJK.ttc";
    fonts[0].faceIndex = 1;
    fonts[0].covers = "zh-Hans";
    fonts[0].data = cjk.data();
    fonts[0].size = cjk.size();
    fonts[1].path = "emoji.ttf";
    fonts[1].covers = "emoji";
    fonts[1].data = emoji.data();
    fonts[1].size = emoji.size();
    SystemFonts::mergeInto(fonts, {{ui, 15.0f, 1.5f}, {code, 16.0f, 1.0f}});

    for (ImFont* font : {ui, code}) {
        EXPECT_TRUE(font->IsGlyphInFont(0x4E2D));
        EXPECT_TRUE(font->IsGlyphInFont(0xD55C));
        EXPECT_TRUE(font->IsGlyphInFont(0x1F600)) << "32-bit characters (IMGUI_USE_WCHAR32)";
        EXPECT_FALSE(font->IsGlyphInFont(0x3042));
        ASSERT_GE(font->Sources.Size, 3);
        EXPECT_EQ(font->Sources[1]->FontNo, 1u) << "the face asked for";
        EXPECT_FALSE(font->Sources[1]->FontDataOwnedByAtlas) << "the mapping is not ImGui's to free";
    }
    EXPECT_FLOAT_EQ(ui->Sources[1]->RasterizerMultiply, 1.5f) << "the weight of the font it falls back from";
    // the editor's own font still draws what it has
    EXPECT_EQ(ui->Sources[0]->FontData, static_cast<void*>(latin.data()));
}

TEST(SystemFonts, PreferenceRoundTrips) {
    const std::string before = AppSettings::getCjkFontLanguage();
    EXPECT_TRUE(SystemFonts::isLanguageSetting(before));
    AppSettings::setCjkFontLanguage("japanese");
    EXPECT_EQ(AppSettings::getCjkFontLanguage(), "japanese");
    AppSettings::setCjkFontLanguage("klingon");
    EXPECT_EQ(AppSettings::getCjkFontLanguage(), "auto") << "an unknown value is auto";
    AppSettings::setCjkFontLanguage(before);
}
