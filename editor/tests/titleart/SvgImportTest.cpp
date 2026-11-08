// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// SVG into Vector layers (docs/specs/S87-title-localisation-interchange.md R34, D12, AC18): split
// parts are files that register exactly (their composite equals the unsplit file), `auto` mode's
// colour rule, the notes of what shape mode cannot draw, the op's files and layers, create.from.

#include <gtest/gtest.h>

#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtOps.h"
#include "titleart/interchange/svg/SvgImport.h"
#include "util/LunaSVGRuntime.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using titleart::Json;
namespace fs = std::filesystem;

namespace {

    // three top-level groups: a nested transform, a gradient referenced from <defs>, a <use> of a
    // symbol and a <use> of a path that lives outside <defs>
    const char* LOGO = R"(<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="160" height="96" viewBox="0 0 160 96">
  <defs>
    <linearGradient id="sky" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#ff0000"/><stop offset="1" stop-color="#0000ff"/></linearGradient>
    <symbol id="dot" viewBox="0 0 10 10"><circle cx="5" cy="5" r="5" fill="#00ff00"/></symbol>
  </defs>
  <g id="back"><rect x="8" y="8" width="96" height="48" fill="url(#sky)"/></g>
  <g id="mid" transform="translate(40 24)">
    <g transform="scale(1.5)" fill="#ffcc00"><circle cx="20" cy="20" r="16"/><path id="tri" d="M0 40 L30 40 L15 10 Z" fill-opacity="0.75"/></g>
  </g>
  <g id="front"><use xlink:href="#dot" x="120" y="60" width="24" height="24"/><use href="#tri" x="100" y="20" fill="#3366ff"/></g>
</svg>)";

    const char* ICON = R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><path d="M2 2h20v20H2z" fill="#ffffff"/><circle cx="12" cy="12" r="4" fill="black" stroke="none"/></svg>)";
    const char* MULTI = R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><rect width="10" height="10" fill="#ff0000"/><rect x="12" width="10" height="10" fill="#00ff00"/></svg>)";
    const char* STYLED = R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><style>.a { fill: #ff0000 } .b { fill: #00ff00 }</style><rect class="a" width="10" height="10"/></svg>)";
    const char* WITH_TEXT = R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64"><defs><filter id="f"/></defs><rect width="64" height="64" filter="url(#f)"/><text x="4" y="40">Hi</text><image href="a.png" width="8" height="8"/></svg>)";

    // premultiplied RGBA8 of an SVG at its own size
    bool raster(const std::string& svg, std::vector<uint8_t>& pixels, int& w, int& h) {
        LunaSVGRuntime::ensureFonts();
        return LunaSVGRuntime::rasterize(svg.data(), svg.size(), 1.0f, true, pixels, w, h);
    }

    std::string readText(const fs::path& p) {
        std::ifstream in(p, std::ios::binary);
        std::ostringstream s;
        s << in.rdbuf();
        return s.str();
    }

    void writeText(const fs::path& p, const std::string& text) {
        fs::create_directories(p.parent_path());
        std::ofstream out(p, std::ios::binary);
        out << text;
    }

    // the largest channel difference of the composite of `split`'s parts against the whole
    int compositeDifference(const std::string& split) {
        std::vector<uint8_t> whole;
        int w = 0, h = 0;
        EXPECT_TRUE(raster(LOGO, whole, w, h));
        const SvgSplit parts = splitSvg(LOGO, split);
        EXPECT_TRUE(parts.ok) << parts.error;
        std::vector<double> acc((size_t)w * h * 4, 0.0);
        for (const SvgPart& part : parts.parts) {
            std::vector<uint8_t> px;
            int pw = 0, ph = 0;
            EXPECT_TRUE(raster(part.text, px, pw, ph)) << part.name;
            EXPECT_EQ(pw, w) << part.name << ": a part keeps the root's viewport";
            EXPECT_EQ(ph, h) << part.name;
            if (pw != w || ph != h) return 255;
            for (size_t i = 0; i < acc.size(); i += 4) {
                const double a = px[i + 3] / 255.0;
                for (int c = 0; c < 4; c++) acc[i + c] = px[i + c] / 255.0 + acc[i + c] * (1.0 - a);
            }
        }
        int worst = 0;
        for (size_t i = 0; i < acc.size(); i++) worst = std::max(worst, std::abs((int)std::lround(acc[i] * 255.0) - (int)whole[i]));
        return worst;
    }

    class SvgImportTest : public ::testing::Test {
    protected:
        void SetUp() override {
            registerSvgInterchange();
            root = fs::temp_directory_path() / ("je_svg_import_" + std::to_string(std::rand()));
            fs::create_directories(root / "assets" / "titles");
        }
        void TearDown() override { fs::remove_all(root); }

        TitleOpsOptions options() const {
            TitleOpsOptions o;
            o.documentPath = (root / "assets" / "titles" / "logo.titleart").string();
            o.projectPath = root.string();
            o.assetsPath = (root / "assets").string();
            o.idSeed = 7;
            return o;
        }

        titleart::TitleArtDoc emptyDoc() const {
            titleart::TitleArtDoc doc = titleart::makeNewTitleArt();
            doc.root["layers"] = Json::array();
            doc.root["exposed"] = Json::array();
            titleart::normalizeTitleArt(doc, nullptr);
            return doc;
        }

        fs::path root;
    };

}

TEST_F(SvgImportTest, SplitPartsRegisterExactly) {
    const SvgSplit groups = splitSvg(LOGO, "groups");
    ASSERT_TRUE(groups.ok) << groups.error;
    ASSERT_EQ(groups.parts.size(), 3u);
    EXPECT_EQ(groups.parts[0].name, "back");
    EXPECT_EQ(groups.parts[2].name, "front");
    EXPECT_DOUBLE_EQ(groups.width, 160.0);
    EXPECT_DOUBLE_EQ(groups.height, 96.0);
    // the referenced path outside <defs> travels with the part that uses it
    EXPECT_NE(groups.parts[2].text.find("id=\"tri\""), std::string::npos);
    EXPECT_NE(groups.parts[0].text.find("<linearGradient"), std::string::npos) << "the root's <defs> are in every part";
    EXPECT_LE(compositeDifference("groups"), 1);

    const SvgSplit paths = splitSvg(LOGO, "paths");
    ASSERT_TRUE(paths.ok) << paths.error;
    EXPECT_EQ(paths.parts.size(), 5u) << "rect, circle, tri, two uses";
    EXPECT_EQ(paths.parts[1].name, "Circle 1");
    // the ancestors' transforms and inherited fill ride along (ids dropped)
    EXPECT_NE(paths.parts[1].text.find("translate(40 24)"), std::string::npos);
    EXPECT_NE(paths.parts[1].text.find("scale(1.5)"), std::string::npos);
    EXPECT_NE(paths.parts[1].text.find("#ffcc00"), std::string::npos);
    EXPECT_EQ(paths.parts[1].text.find("id=\"mid\""), std::string::npos);
    EXPECT_LE(compositeDifference("paths"), 1);
}

TEST_F(SvgImportTest, AutoModePicksByColour) {
    EXPECT_EQ(svgAutoMode(ICON), "shape") << "one colour plus black";
    EXPECT_EQ(svgAutoMode(MULTI), "artwork");
    EXPECT_EQ(svgAutoMode(STYLED), "artwork") << "<style> colours count";
    EXPECT_EQ(svgAutoMode(LOGO), "artwork") << "a gradient";
    const SvgSplit parts = splitSvg(LOGO, "groups");
    EXPECT_EQ(parts.parts[1].mode, "shape") << "auto decides per part";
    EXPECT_EQ(parts.parts[0].mode, "artwork");
    EXPECT_EQ(splitSvg(MULTI, "none", "shape").parts[0].mode, "shape") << "an explicit mode wins";
}

TEST_F(SvgImportTest, ShapeModeNotes) {
    const SvgSplit s = splitSvg(WITH_TEXT, "none", "shape");
    ASSERT_TRUE(s.ok);
    const std::vector<std::string>& notes = s.parts[0].notes;
    ASSERT_EQ(notes.size(), 3u);
    EXPECT_NE(notes[0].find("<text>"), std::string::npos);
    EXPECT_NE(notes[0].find("outlines"), std::string::npos);
    EXPECT_NE(notes[1].find("<image>"), std::string::npos);
    EXPECT_NE(notes[2].find("filter"), std::string::npos);
    EXPECT_TRUE(splitSvg(WITH_TEXT, "none", "artwork").parts[0].notes.empty()) << "artwork draws them";
    EXPECT_FALSE(splitSvg("<svg", "none").ok);
    EXPECT_FALSE(splitSvg("<html/>", "none").ok);
}

TEST_F(SvgImportTest, OpWritesPartsAndLayers) {
    const fs::path outside = root / "downloads" / "logo.svg";
    writeText(outside, LOGO);
    const titleart::TitleArtDoc doc = emptyDoc();
    TitleOpsResult r = TitleArtOps::apply(doc, Json{{"op", "import_svg"}, {"file", outside.string()}, {"split", "groups"}}, options());
    ASSERT_TRUE(r.ok) << r.error();
    EXPECT_EQ(r.undoName, "Import SVG (logo.svg)");
    // the original copied into logo_assets/, then its three parts
    ASSERT_EQ(r.files.size(), 4u);
    EXPECT_EQ(fs::path(r.files[0].path).filename(), "logo.svg");
    EXPECT_EQ(fs::path(r.files[0].path).parent_path().filename(), "logo_assets");
    EXPECT_EQ(fs::path(r.files[1].path).filename(), "logo_back.svg");
    const Json& group = r.doc.root["layers"][0];
    EXPECT_EQ(group["type"], "group");
    EXPECT_EQ(group["name"], "logo");
    ASSERT_EQ(group["children"].size(), 3u);
    const Json& back = group["children"][0];
    EXPECT_EQ(back["type"], "vector");
    EXPECT_EQ(back["svg"]["path"], "titles/logo_assets/logo_back.svg");
    EXPECT_DOUBLE_EQ(back["size"].get<double>(), 160.0) << "the viewport's larger side: native size";
    EXPECT_EQ(back["mode"], "artwork");
    EXPECT_EQ(group["children"][1]["mode"], "shape");
    EXPECT_EQ(r.reports["0"]["layers"].size(), 4u);

    // inside the assets: referenced in place, parts beside it; importing again writes nothing new
    const fs::path inside = root / "assets" / "art" / "badge.svg";
    writeText(inside, LOGO);
    r = TitleArtOps::apply(doc, Json{{"op", "import_svg"}, {"file", "assets/art/badge.svg"}, {"split", "paths"}, {"name", "Badge"}}, options());
    ASSERT_TRUE(r.ok) << r.error();
    ASSERT_EQ(r.files.size(), 5u);
    for (const TitleOpFile& f : r.files) {
        EXPECT_EQ(fs::path(f.path).parent_path(), inside.parent_path());
        writeText(fs::path(f.path), f.content);
    }
    EXPECT_EQ(r.doc.root["layers"][0]["name"], "Badge");
    r = TitleArtOps::apply(doc, Json{{"op", "import_svg"}, {"file", inside.string()}, {"split", "paths"}}, options());
    ASSERT_TRUE(r.ok) << r.error();
    EXPECT_TRUE(r.files.empty()) << "the parts on disk are reused";
    EXPECT_EQ(r.doc.root["layers"][0]["children"][0]["svg"]["path"], "art/badge_Rect_1.svg");

    // one layer for split none; bad values name the valid ones
    r = TitleArtOps::apply(doc, Json{{"op", "import_svg"}, {"file", inside.string()}}, options());
    ASSERT_TRUE(r.ok) << r.error();
    EXPECT_EQ(r.doc.root["layers"][0]["svg"]["path"], "art/badge.svg");
    EXPECT_TRUE(r.files.empty());
    r = TitleArtOps::apply(doc, Json{{"op", "import_svg"}, {"file", inside.string()}, {"split", "lines"}}, options());
    ASSERT_FALSE(r.ok);
    EXPECT_EQ(r.problems.front().code, "bad_value");
    r = TitleArtOps::apply(doc, Json{{"op", "import_svg"}, {"file", "missing.svg"}}, options());
    ASSERT_FALSE(r.ok);
    EXPECT_EQ(r.problems.front().code, "file_not_found");
}

TEST_F(SvgImportTest, NotesNameTheirLayer) {
    const fs::path file = root / "assets" / "art" / "hi.svg";
    writeText(file, WITH_TEXT);
    const TitleOpsResult r = TitleArtOps::apply(emptyDoc(), Json{{"op", "import_svg"}, {"file", file.string()}, {"mode", "shape"}}, options());
    ASSERT_TRUE(r.ok) << r.error();
    const Json& notes = r.reports["0"]["notes"];
    ASSERT_EQ(notes.size(), 3u);
    EXPECT_EQ(notes[0]["layer"], "hi");
    EXPECT_EQ(notes[0]["action"], "dropped");
}

TEST_F(SvgImportTest, CreateFromMakesADocument) {
    const fs::path file = root / "downloads" / "badge.svg";
    writeText(file, LOGO);
    TitleCreateRequest request;
    request.sourcePath = file.string();
    request.documentPath = (root / "assets" / "titles" / "badge.titleart").string();
    request.projectPath = root.string();
    request.assetsPath = (root / "assets").string();
    TitleCreateResult out;
    std::string code, error;
    ASSERT_TRUE(TitleArtOps::createFrom(request, out, code, error)) << code << ": " << error;
    EXPECT_EQ(out.kind, "svg");
    EXPECT_EQ(out.doc.canvasWidth(), 160);
    EXPECT_EQ(out.doc.canvasHeight(), 96);
    ASSERT_EQ(out.doc.layers().size(), 1u);
    EXPECT_EQ(out.doc.layers()[0]["svg"]["path"], "titles/badge_assets/badge.svg");
    EXPECT_EQ(out.files.size(), 1u);
    EXPECT_TRUE(out.doc.root["exposed"].empty());
    // a file that is not SVG is not taken
    writeText(file, "{\"format\": \"something\"}");
    EXPECT_FALSE(TitleArtOps::createFrom(request, out, code, error));
}
