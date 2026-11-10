// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// S81 R53, R55, AC15 (the Characters part), AC16 (the check part): the presets' exact counts, the
// set type, a document's Characters (presets, Custom, Custom File, the document's texts at any
// depth, exposed defaults, S87 translations), the missing-glyph check against a font chain, the
// best-effort Lua literal scan and check_title's `characters` check with its "Add to Custom" fix.

#include <gtest/gtest.h>

#include "titleart/TitleArtAssets.h"
#include "titleart/TitleArtBuiltinChecks.h"
#include "titleart/TitleArtChecks.h"
#include "titleart/TitleArtIO.h"
#include "titleart/runtime/TitleCharacterChecks.h"
#include "titleart/runtime/TitleCharacters.h"
#include "util/FontFace.h"
#include "util/FontRegistry.h"

#include <filesystem>
#include <map>
#include <memory>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;
using TJson = titleart::Json;

namespace {

    const fs::path& fontDir() {
        static const fs::path dir = fs::path(JASPER_TEST_REPO_DIR) / "engine" / "tests" / "core" / "titleart" / "fonts";
        return dir;
    }

    // fonts by path under the fixture folder, the engine's built-in face at the end of chains
    class FixtureFonts : public titleart::AssetResolver {
    public:
        std::vector<std::string> project;       // what projectFonts() lists

        titleart::ResolvedTitleAsset resolve(const titleart::TitleAssetRef& ref, titleart::TitleAssetKind) const override {
            titleart::ResolvedTitleAsset out;
            const fs::path file = fontDir() / ref.path;
            std::error_code ec;
            if (ref.path.empty() || !fs::is_regular_file(file, ec)) return out;
            out.found = true;
            out.path = file.string();
            out.assetPath = ref.path;
            return out;
        }
        std::shared_ptr<FontFace> loadFont(const titleart::ResolvedTitleAsset& asset, int faceIndex, std::string* error) const override {
            auto& face = faces[asset.path];
            if (!face) face = FontFace::loadFile(asset.path, faceIndex, error, FontFaceRasterizer::Optional);
            return face;
        }
        bool readBytes(const titleart::ResolvedTitleAsset&, std::vector<uint8_t>&, std::string* error) const override {
            if (error) *error = "no files here";
            return false;
        }
        bool loadImage(const titleart::ResolvedTitleAsset&, titleart::TitleImageData&, std::string*) const override { return false; }
        std::shared_ptr<FontFace> builtinFont() const override { return FontRegistry::getBuiltInLatin(); }
        std::vector<titleart::ResolvedTitleAsset> projectFonts() const override {
            std::vector<titleart::ResolvedTitleAsset> out;
            for (const std::string& path : project) out.push_back(resolve({path, ""}, titleart::TitleAssetKind::Font));
            return out;
        }

    private:
        mutable std::map<std::string, std::shared_ptr<FontFace>> faces;
    };

    TitleCharacterSet preset(const std::string& name) {
        TitleCharacterSet set;
        EXPECT_TRUE(titleCharacterPreset(name, set)) << name;
        return set;
    }

    std::string utf8(const std::vector<uint32_t>& cps) {
        std::string out;
        for (uint32_t cp : cps) out += titleUtf8Of(cp);
        return out;
    }

    // a new document whose text layer uses `font`, with the given Characters
    titleart::TitleArtDoc document(const std::string& font, const TJson& characters) {
        titleart::TitleArtDoc doc = titleart::makeNewTitleArt();
        if (!font.empty()) doc.root["layers"][0]["font"] = TJson{{"path", font}, {"guid", ""}};
        doc.root["runtime"]["characters"] = characters;
        titleart::normalizeTitleArt(doc, nullptr);
        return doc;
    }

    const TitleCheckResult* findResult(const TitleCheckReport& report, const std::string& id) {
        for (const TitleCheckResult& r : report.checks) {
            if (r.id == id) return &r;
        }
        return nullptr;
    }

}

// R53: each preset is exactly its list. Digits is the 23 characters R53 writes plus space: 24
// code points (R53 says 25; the list is authoritative, recorded as a deviation).
TEST(TitleCharacters, PresetCounts) {
    EXPECT_EQ(titleCharacterPresetNames(), std::vector<std::string>({"digits", "latin", "latin_extended", "kana", "joyo_kanji"}));
    const TitleCharacterSet digits = preset("digits");
    EXPECT_EQ(digits.size(), 24u);
    EXPECT_EQ(digits.toUtf8(), " $%+,-./0123456789:x\u00a3\u00a5\u00d7\u20ac");
    EXPECT_EQ(preset("latin").size(), 95u);
    EXPECT_TRUE(preset("latin").contains(0x20) && preset("latin").contains(0x7e) && !preset("latin").contains(0x7f));
    // 95 + 432 (U+00A0-U+024F) + 256 + 112 + 48
    EXPECT_EQ(preset("latin_extended").size(), 943u);
    // 64 + 96 + 96 + 16 + 240
    EXPECT_EQ(preset("kana").size(), 512u);
    const TitleCharacterSet joyo = preset("joyo_kanji");
    EXPECT_EQ(joyo.size(), 2136u);
    EXPECT_TRUE(joyo.contains(0x4e00)) << "一";
    EXPECT_TRUE(joyo.contains(0x20b9f)) << "the 2010 list's form of shikaru, outside the BMP";
    EXPECT_FALSE(joyo.contains(0x53f1)) << "the common form is not the list's";
    EXPECT_FALSE(joyo.contains('#')) << "the file's comments are not characters";
    TitleCharacterSet unknown;
    EXPECT_FALSE(titleCharacterPreset("latn", unknown));
}

TEST(TitleCharacters, SetOperations) {
    TitleCharacterSet a;
    a.addUtf8("cab\xc3\xa9");
    EXPECT_EQ(a.size(), 4u);
    EXPECT_EQ(a.toUtf8(), "abc\xc3\xa9");
    EXPECT_FALSE(a.add(0xd800)) << "a surrogate is not a character";
    EXPECT_FALSE(a.add(0x110000));
    EXPECT_TRUE(a.add('a'));
    EXPECT_EQ(a.size(), 4u) << "unique";
    TitleCharacterSet b;
    b.addUtf8("\xc3\xa9" "bac");
    EXPECT_EQ(a.hash(), b.hash()) << "the hash depends on the code points only";
    EXPECT_EQ(a.hash().size(), 16u);
    b.add('z');
    EXPECT_NE(a.hash(), b.hash());
    a.merge(b);
    EXPECT_TRUE(a.contains('z'));
    EXPECT_EQ(a, b);
    EXPECT_EQ(utf8(a.missingFrom("zebra!?!")), "er!?") << "in order of appearance, once each";
    TitleCharacterSet file;
    file.addUtf8("\xef\xbb\xbf" "a\r\n\tb", true);
    EXPECT_EQ(file.toUtf8(), "ab") << "a text file's BOM, line ends and tabs are not characters";
}

// R53: presets ∪ Custom ∪ Custom File ∪ the document's texts (every depth, exposed defaults,
// translations when present)
TEST(TitleCharacters, DocumentCharacters) {
    titleart::TitleArtDoc doc = document("", TJson{{"presets", TJson::array({"digits"})}, {"custom", "\xc3\x86\xc3\x98"},
                                                   {"custom_file", TJson{{"path", "strings.csv"}, {"guid", ""}}}, {"document_text", true}});
    doc.root["layers"][0]["text"] = "\xe7\xa2\xba\xe5\xae\x9a";      // 確定
    doc.root["layers"][0]["translations"] = TJson{{"de", TJson{{"text", "Gr\xc3\xb6\xc3\x9f" "e"}}}};
    doc.root["layers"].push_back(TJson{{"id", "a1b2c3d4"}, {"type", "group"}, {"name", "Group"},
                                       {"children", TJson::array({TJson{{"id", "a1b2c3d5"}, {"type", "text"}, {"name", "Inner"}, {"text", "Nest"}}})}});
    titleart::normalizeTitleArt(doc, nullptr);
    ASSERT_EQ(doc.root["layers"][0]["translations"]["de"]["text"], "Gr\xc3\xb6\xc3\x9f" "e") << "normalize keeps S87's member";
    std::string asked;
    const TitleCharacterFileReader reader = [&](const titleart::TitleAssetRef& ref, std::string& text, std::string&) {
        asked = ref.path;
        text = "\xef\xbb\xbfid,text\r\nwin,\xe5\xa4\xa7\xe5\xbd\x93\xe3\x82\x8a\r\n";   // BOM, 大当り
        return true;
    };
    TitleCharactersReport report;
    const TitleCharacterSet set = titleDocumentCharacters(doc, reader, &report);
    EXPECT_EQ(asked, "strings.csv");
    EXPECT_EQ(report.presets, std::vector<std::string>({"digits"}));
    EXPECT_EQ(report.customCount, 2u);
    for (const char* text : {"0123456789", "\xc3\x86\xc3\x98", "\xe5\xa4\xa7\xe5\xbd\x93\xe3\x82\x8a", "id,textwn",
                             "\xe7\xa2\xba\xe5\xae\x9a", "Gr\xc3\xb6\xc3\x9f" "e", "Nest"}) {
        EXPECT_TRUE(set.missingFrom(text).empty()) << text << " misses " << utf8(set.missingFrom(text));
    }
    EXPECT_FALSE(set.contains(0xfeff));
    EXPECT_FALSE(set.contains('\r'));
    EXPECT_FALSE(set.contains('Q'));
    EXPECT_TRUE(report.warnings.empty());

    // the exposed text's default: its target's current value (one more text layer, exposed)
    doc.root["layers"].push_back(TJson{{"id", "a1b2c3d6"}, {"type", "text"}, {"name", "Score"}, {"text", "Q"}});
    doc.root["exposed"].push_back(TJson{{"name", "score"}, {"type", "text"}, {"targets", TJson::array({TJson{{"layer", "a1b2c3d6"}, {"field", "text"}}})}});
    titleart::normalizeTitleArt(doc, nullptr);
    EXPECT_TRUE(titleDocumentCharacters(doc, reader).contains('Q'));

    // Characters Used by the Document off: only presets, Custom and the file
    doc.root["runtime"]["characters"]["document_text"] = false;
    const TitleCharacterSet without = titleDocumentCharacters(doc, reader);
    EXPECT_FALSE(without.contains(0x78ba)) << "確 came from the layer";
    EXPECT_FALSE(without.contains('N'));
    EXPECT_TRUE(without.contains(0x5927)) << "大 came from the file";
    EXPECT_EQ(titleDocumentCharacterCount(doc, reader), without.size());

    // an unreadable file is a warning, not a failure
    const TitleCharacterSet unread = titleDocumentCharacters(doc, nullptr, &report);
    ASSERT_EQ(report.warnings.size(), 1u);
    EXPECT_NE(report.warnings[0].find("strings.csv"), std::string::npos) << report.warnings[0];
    EXPECT_TRUE(unread.contains('0'));
}

// R55: the characters a dynamic layer's chain cannot draw, naming the character, title and font
TEST(TitleCharacters, MissingGlyphs) {
    FixtureFonts fonts;
    const titleart::TitleArtDoc doc = document("Oswald-wght.ttf", TJson{{"presets", TJson::array({"latin"})}});
    TitleCharacterSet chars = preset("latin");
    chars.addUtf8("\xe7\xa2\xba \xe3\x80\x80");      // 確, a space and an ideographic space
    const std::vector<TitleMissingCharacters> missing = findTitleMissingCharacters(doc, chars, fonts, "titles/logo.titleart");
    ASSERT_EQ(missing.size(), 1u);
    EXPECT_EQ(missing[0].code, "W-TITLEART-MISSING-GLYPH");
    EXPECT_EQ(missing[0].codepoints, std::vector<uint32_t>({0x78ba})) << "spaces are never missing";
    EXPECT_EQ(missing[0].font, "Oswald-wght.ttf");
    EXPECT_NE(missing[0].message.find("titles/logo.titleart"), std::string::npos) << missing[0].message;
    EXPECT_NE(missing[0].message.find("\xe7\xa2\xba"), std::string::npos) << missing[0].message;
    EXPECT_NE(missing[0].message.find("U+78BA"), std::string::npos) << missing[0].message;
    EXPECT_NE(missing[0].message.find("Oswald-wght.ttf, then the built-in face"), std::string::npos) << missing[0].message;

    // a fallback that has it covers it; unassigned code points of a preset are never missing
    titleart::TitleArtDoc covered = doc;
    covered.root["layers"][0]["fallback_fonts"] = TJson::array({TJson{{"path", "DelaGothicOne-Regular.ttf"}, {"guid", ""}}});
    EXPECT_TRUE(findTitleMissingCharacters(covered, chars, fonts, "logo").empty());
    TitleCharacterSet unassigned;
    unassigned.add(0x3040);
    unassigned.add(0xffef);
    EXPECT_TRUE(titleChainMissing(titleLayerFontChain(doc.root["layers"][0], fonts), unassigned).empty());
}

TEST(TitleCharacters, LuaLiteralScan) {
    const std::string source =
        "local logo = TitleArt(scene, entity)\n"                                   // 1
        "logo.text = \"WIN \\u{C6}\"\n"                                            // 2
        "self.counter = self.gameObject:getTitleArt()\n"                           // 3
        "self.counter:setText(\"12\\x2C345\") -- \"comment\"\n"                    // 4
        "self.counter:setParameter(\"label\", 'Gr\xc3\xb6\xc3\x9f" "e')\n"         // 5
        "self.counter:setParameter(\"tint\", \"#ff8800\")\n"                       // 6
        "local label = Text(scene, e)\n"                                           // 7
        "label.text = \"not a title\"\n"                                           // 8
        "go:getTitleArt().text = [[\nlong\nstring]]\n"                             // 9-11
        "--[[ logo.text = \"commented\" ]]\n"                                      // 12
        "if logo == TitleArt(scene, e) then logo:setText \"\\65\\066\" end\n";     // 13
    const std::vector<TitleLuaLiteral> literals = scanTitleLuaLiterals(source, "scripts/hud.lua");
    ASSERT_EQ(literals.size(), 5u);
    EXPECT_EQ(literals[0].line, 2);
    EXPECT_EQ(literals[0].call, "text =");
    EXPECT_EQ(literals[0].value, "WIN \xc3\x86");
    EXPECT_EQ(literals[1].call, "setText");
    EXPECT_EQ(literals[1].value, "12,345");
    EXPECT_EQ(literals[1].line, 4);
    EXPECT_EQ(literals[2].call, "setParameter");
    EXPECT_EQ(literals[2].parameter, "label");
    EXPECT_EQ(literals[2].value, "Gr\xc3\xb6\xc3\x9f" "e");
    EXPECT_EQ(literals[3].line, 9);
    EXPECT_EQ(literals[3].value, "long\nstring");
    EXPECT_EQ(literals[4].line, 13);
    EXPECT_EQ(literals[4].value, "AB");
    EXPECT_EQ(literals[4].parameter, "text");

    const std::vector<TitleCharsWarning> warnings = titleLiteralCharsWarnings(literals, preset("latin"), "titles/logo.titleart");
    ASSERT_EQ(warnings.size(), 2u);
    EXPECT_EQ(warnings[0].code, "W-TITLEART-CHARS");
    EXPECT_EQ(warnings[0].file, "scripts/hud.lua");
    EXPECT_EQ(warnings[0].line, 2);
    EXPECT_EQ(utf8(warnings[0].codepoints), "\xc3\x86");
    EXPECT_EQ(warnings[0].message.rfind("scripts/hud.lua:2: ", 0), 0u) << warnings[0].message;
    EXPECT_NE(warnings[0].message.find("add them to Characters > Custom"), std::string::npos) << warnings[0].message;
    EXPECT_EQ(utf8(warnings[1].codepoints), "\xc3\xb6\xc3\x9f");
    EXPECT_EQ(warnings[1].parameter, "label");
}

// AC16's check part: check_title's `characters` reports the coverage and the script literals, and
// its "Add to Custom" fix makes the literals rule pass
TEST(TitleCharacters, CharactersCheck) {
    registerBuiltinTitleChecks();
    const CheckDef* def = TitleArtChecks::find("characters");
    ASSERT_TRUE(def);
    EXPECT_EQ(def->owner, "S81");
    EXPECT_EQ(def->kind, TitleCheckKind::Document);

    FixtureFonts fonts;
    fonts.project = {"Oswald-wght.ttf", "Stick-Regular.ttf"};
    std::vector<TitleLuaLiteral> literals = {{"scripts/hud.lua", 7, "text =", "text", "\xc3\x86\xc3\x98 WIN"},
                                             {"scripts/hud.lua", 9, "setParameter", "unexposed", "\xe2\x98\x85"}};
    setTitleLuaLiteralProvider([&](const fs::path& root) {
        EXPECT_EQ(root, fs::path("/projects/demo"));
        return literals;
    });
    auto run = [&](const titleart::TitleArtDoc& doc) {
        TitleCheckContext context(doc, "assets/titles/logo.titleart");
        context.file = "/projects/demo/assets/titles/logo.titleart";
        context.assets = &fonts;
        return TitleArtChecks::run({def}, context);
    };

    // Custom 確 on a Latin font: coverage warns, naming a project font that has it; the Kana
    // preset's gaps are counted, not warned about (a preset is a broad range)
    titleart::TitleArtDoc doc = document("Oswald-wght.ttf", TJson{{"presets", TJson::array({"latin", "kana"})}, {"custom", "\xe7\xa2\xba"}});
    TitleCheckReport report = run(doc);
    const TitleCheckResult* coverage = findResult(report, "characters.coverage");
    ASSERT_TRUE(coverage);
    EXPECT_EQ(coverage->status, TitleCheckStatus::Warn);
    EXPECT_EQ(coverage->measured["missing"], 1);
    EXPECT_GT(coverage->measured["preset_gaps"].get<int>(), 100);
    EXPECT_NE(coverage->message.find("Oswald-wght.ttf"), std::string::npos) << coverage->message;
    EXPECT_NE(coverage->message.find("\xe7\xa2\xba"), std::string::npos) << coverage->message;
    ASSERT_TRUE(coverage->fix.is_object()) << coverage->toJson().dump();
    const TJson& op = coverage->fix["ops"][0];
    EXPECT_EQ(op["op"], "set");
    EXPECT_EQ(op["values"]["fallback_fonts"].back()["path"], "Stick-Regular.ttf");

    // the literal for the exposed `text` falls outside Latin; the unexposed parameter is not this title's
    const TitleCheckResult* lits = findResult(report, "characters.literals");
    ASSERT_TRUE(lits);
    EXPECT_EQ(lits->status, TitleCheckStatus::Warn);
    EXPECT_EQ(lits->measured["literals"], 1);
    EXPECT_NE(lits->message.find("scripts/hud.lua:7"), std::string::npos) << lits->message;
    ASSERT_TRUE(lits->fix.is_object());
    const TJson& add = lits->fix["ops"][0];
    EXPECT_EQ(add["op"], "set");
    EXPECT_FALSE(add.contains("layer")) << "a document member";
    EXPECT_EQ(add["values"]["runtime.characters.custom"], "\xe7\xa2\xba\xc3\x86\xc3\x98") << "Custom keeps what it had";

    // applying the fixes: the fallback font and the Custom characters
    titleart::FieldSetResult fallback = titleart::setTitleArtField(doc.root["layers"][0], "layer", "fallback_fonts", op["values"]["fallback_fonts"]);
    ASSERT_TRUE(fallback.ok) << fallback.error;
    titleart::FieldSetResult custom = titleart::setTitleArtField(doc.root, "document", "runtime.characters.custom", add["values"]["runtime.characters.custom"]);
    ASSERT_TRUE(custom.ok) << custom.error;
    report = run(doc);
    EXPECT_EQ(findResult(report, "characters.literals")->status, TitleCheckStatus::Pass) << findResult(report, "characters.literals")->message;
    EXPECT_EQ(findResult(report, "characters.coverage")->status, TitleCheckStatus::Pass) << findResult(report, "characters.coverage")->message;

    // without a resolver the check is not measured
    TitleCheckContext bare(doc, "assets/titles/logo.titleart");
    EXPECT_EQ(TitleArtChecks::run({def}, bare).checks.at(0).status, TitleCheckStatus::Skip);
    setTitleLuaLiteralProvider(nullptr);
}
