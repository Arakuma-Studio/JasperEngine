// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// export_title's sheet, atlas and sequence writers over frames already rendered
// (docs/specs/S87-title-localisation-interchange.md AC22, R42, §6.6): the grid's cells, its
// `@WxH` name and JSON; deterministic atlas packing with padding and the extruded edge; trimmed
// frames with their source boxes; per-language names; the frame-time rules and the 1,024 cap.
// Rendering the frames needs S85's frame source: tools/mcp/tests covers the job once it exists.

#include <gtest/gtest.h>

#include "stb_image.h"
#include "titleart/TitleArtIO.h"
#include "titleart/interchange/sheet/TitleSheetExport.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>

using namespace jasperengine;
using namespace jasperengine::editor;
using titleart::Json;
namespace fs = std::filesystem;

namespace {

    class TitleSheetExportTest : public ::testing::Test {
    protected:
        void SetUp() override {
            dir = fs::temp_directory_path() / ("jasper-title-sheet-" + std::to_string(::getpid()) + "-" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
            fs::remove_all(dir);
            fs::create_directories(dir);
        }
        void TearDown() override { fs::remove_all(dir); }

        std::unique_ptr<TitleExportContext> context(const std::string& format, Json options = Json::object()) {
            auto ctx = std::make_unique<TitleExportContext>();
            ctx->request.document = dir / "win.titleart";
            ctx->request.relative = "win.titleart";
            ctx->request.doc = titleart::parseTitleArtJson(Json{{"format", titleart::TITLE_ART_FORMAT}, {"version", 1}}).doc;
            ctx->request.format = format;
            ctx->request.options = std::move(options);
            ctx->request.out = dir;
            ctx->request.insideProjectOnly = false;
            ctx->image.metadata = false;
            return ctx;
        }

        // `n` frames of one size: a filled box whose place and colour depend on the frame
        static TitleSheetFrames frames(int n, int width, int height, int inset) {
            TitleSheetFrames f;
            f.info.name = "idle";
            f.info.fps = 30.0;
            f.info.duration = n / 30.0;
            f.fps = 30.0;
            for (int i = 0; i < n; i++) {
                texsynth::Image image = texsynth::Image::make(width, height, 4, 8);
                for (int y = inset; y < height - inset; y++) {
                    for (int x = inset + i % 3; x < width - inset; x++) {
                        uint8_t* p = image.pixel(x, y);
                        p[0] = (uint8_t)(40 + i * 20);
                        p[1] = 200;
                        p[2] = 90;
                        p[3] = 255;
                    }
                }
                f.times.push_back(i / 30.0);
                f.pixels.push_back(std::move(image));
            }
            return f;
        }

        Json readJson(const std::string& name) const {
            std::ifstream in(dir / name);
            return Json::parse(in);
        }

        std::string readAll(const std::string& name) const {
            std::ifstream in(dir / name, std::ios::binary);
            std::ostringstream out;
            out << in.rdbuf();
            return out.str();
        }

        struct Png {
            int width = 0, height = 0;
            std::vector<uint8_t> rgba;
            const uint8_t* at(int x, int y) const { return rgba.data() + (y * width + x) * 4; }
        };

        Png readPng(const std::string& name) const {
            Png out;
            int n = 0;
            unsigned char* data = stbi_load((dir / name).string().c_str(), &out.width, &out.height, &n, 4);
            EXPECT_TRUE(data) << name;
            if (data) {
                out.rgba.assign(data, data + out.width * out.height * 4);
                stbi_image_free(data);
            }
            return out;
        }

        fs::path dir;
    };

}

TEST_F(TitleSheetExportTest, SheetIsAGridNamedByItsCell) {
    auto ctx = context("sheet");
    const TitleSheetFrames f = frames(5, 16, 8, 1);
    ASSERT_TRUE(writeTitleSheetFrames(*ctx, "sheet", f)) << ctx->result.error;
    EXPECT_EQ(ctx->result.files.size(), 2u);
    EXPECT_TRUE(fs::exists(dir / "win@16x8.png")) << "S45's grid convention";
    const Png png = readPng("win@16x8.png");
    EXPECT_EQ(png.width, 48) << "ceil(sqrt(5)) columns of 16";
    EXPECT_EQ(png.height, 16) << "two rows of 8";
    const Json j = readJson("win.sheet.json");
    EXPECT_EQ(j["format"], "jasperengine.titleart.sheet");
    ASSERT_EQ(j["frames"].size(), 5u);
    EXPECT_EQ(j["frames"][4]["filename"], "win_0004");
    EXPECT_EQ(j["frames"][4]["frame"], (Json{{"x", 16}, {"y", 8}, {"w", 16}, {"h", 8}}));
    EXPECT_EQ(j["frames"][0]["duration"], 33) << "1000 / 30 ms, rounded";
    EXPECT_EQ(j["frames"][0]["sourceSize"], (Json{{"w", 16}, {"h", 8}}));
    EXPECT_EQ(j["meta"]["image"], "win@16x8.png");
    EXPECT_EQ(j["meta"]["size"], (Json{{"w", 48}, {"h", 16}}));
    EXPECT_EQ(j["meta"]["frameTags"][0], (Json{{"name", "idle"}, {"from", 0}, {"to", 4}, {"direction", "forward"}}));
    // the frame's pixels are in its cell: frame 4 (colour 120) starts one column past its inset
    EXPECT_EQ(png.at(16 + 1 + 1, 8 + 1)[0], 120);
    EXPECT_EQ(png.at(16 + 1 + 1, 8 + 1)[3], 255);
    EXPECT_EQ(png.at(16, 8)[3], 0) << "the inset is transparent";
}

TEST_F(TitleSheetExportTest, ColumnsAndLanguageNames) {
    auto ctx = context("sheet", Json{{"columns", 2}});
    TitleSheetFrames f = frames(3, 10, 10, 0);
    f.language = "pt-BR";
    ASSERT_TRUE(writeTitleSheetFrames(*ctx, "sheet", f)) << ctx->result.error;
    EXPECT_TRUE(fs::exists(dir / "win@10x10.pt-br.png")) << "the code lowercase, as bake sets";
    EXPECT_TRUE(fs::exists(dir / "win.pt-br.sheet.json"));
    const Png png = readPng("win@10x10.pt-br.png");
    EXPECT_EQ(png.width, 20);
    EXPECT_EQ(png.height, 20);
}

TEST_F(TitleSheetExportTest, SequenceNamesEveryFrame) {
    auto ctx = context("sequence");
    TitleSheetFrames f = frames(3, 8, 8, 0);
    f.language = "de";
    ASSERT_TRUE(writeTitleSheetFrames(*ctx, "sequence", f)) << ctx->result.error;
    EXPECT_EQ(ctx->result.files.size(), 3u);
    for (const char* name : {"win_0000.de.png", "win_0001.de.png", "win_0002.de.png"}) EXPECT_TRUE(fs::exists(dir / name)) << name;
}

TEST_F(TitleSheetExportTest, AtlasPacksTrimmedFramesDeterministically) {
    const TitleSheetFrames f = frames(6, 32, 24, 3);
    auto first = context("atlas");
    ASSERT_TRUE(writeTitleSheetFrames(*first, "atlas", f)) << first->result.error;
    const std::string json = readAll("win.atlas.json");
    const std::string page = readAll("win_page0.png");
    auto second = context("atlas", Json{{"overwrite", true}});
    ASSERT_TRUE(writeTitleSheetFrames(*second, "atlas", f)) << second->result.error;
    EXPECT_EQ(readAll("win.atlas.json"), json) << "the same frames pack the same";
    EXPECT_EQ(readAll("win_page0.png"), page);

    const Json j = readJson("win.atlas.json");
    EXPECT_EQ(j["format"], "jasperengine.titleart.atlas");
    ASSERT_EQ(j["frames"].size(), 6u);
    const Png png = readPng("win_page0.png");
    struct Box {
        int x, y, w, h;
    };
    std::vector<Box> boxes;
    for (auto it = j["frames"].begin(); it != j["frames"].end(); ++it) {
        const Json& e = it.value();
        const Box b{e["frame"]["x"].get<int>(), e["frame"]["y"].get<int>(), e["frame"]["w"].get<int>(), e["frame"]["h"].get<int>()};
        boxes.push_back(b);
        EXPECT_TRUE(e["trimmed"].get<bool>()) << it.key();
        EXPECT_EQ(e["sourceSize"], (Json{{"w", 32}, {"h", 24}}));
        EXPECT_EQ(e["spriteSourceSize"]["w"], b.w);
        EXPECT_EQ(e["spriteSourceSize"]["y"], 3) << "the inset trimmed";
        EXPECT_EQ(e["duration"], 33);
        EXPECT_EQ(e["page"], 0);
        // the edge is extruded one pixel, so bilinear sampling stays inside the frame
        ASSERT_GE(b.x, 2);
        ASSERT_GE(b.y, 2);
        EXPECT_EQ(png.at(b.x - 1, b.y + 1)[3], png.at(b.x, b.y + 1)[3]) << it.key();
        EXPECT_EQ(png.at(b.x - 1, b.y + 1)[0], png.at(b.x, b.y + 1)[0]) << it.key();
        EXPECT_EQ(png.at(b.x + 1, b.y - 1)[3], png.at(b.x + 1, b.y)[3]) << it.key();
    }
    // padding: two frames never touch, with the extruded edges between them
    for (size_t a = 0; a < boxes.size(); a++) {
        for (size_t b = a + 1; b < boxes.size(); b++) {
            const bool apart = boxes[a].x + boxes[a].w + 2 <= boxes[b].x || boxes[b].x + boxes[b].w + 2 <= boxes[a].x ||
                               boxes[a].y + boxes[a].h + 2 <= boxes[b].y || boxes[b].y + boxes[b].h + 2 <= boxes[a].y;
            EXPECT_TRUE(apart) << a << " and " << b;
        }
    }
}

TEST_F(TitleSheetExportTest, AtlasSplitsPagesAndRefusesWhatCannotFit) {
    auto ctx = context("atlas", Json{{"max_atlas", 64}});
    ASSERT_TRUE(writeTitleSheetFrames(*ctx, "atlas", frames(8, 40, 40, 0))) << ctx->result.error;
    const Json j = readJson("win.atlas.json");
    EXPECT_GT(j["meta"]["pages"].size(), 1u) << "eight 44 px frames do not fit one 64 px page";
    for (const Json& p : j["meta"]["pages"]) {
        EXPECT_TRUE(fs::exists(dir / p["image"].get<std::string>()));
        EXPECT_LE(p["size"]["w"].get<int>(), 64);
    }
    auto refused = context("atlas", Json{{"max_atlas", 64}, {"overwrite", true}});
    EXPECT_FALSE(writeTitleSheetFrames(*refused, "atlas", frames(1, 100, 10, 0)));
    EXPECT_EQ(refused->result.code, "atlas_too_small");
    auto manyPages = context("atlas", Json{{"max_atlas", 64}, {"overwrite", true}});
    EXPECT_FALSE(writeTitleSheetFrames(*manyPages, "atlas", frames(17, 60, 60, 0)));
    EXPECT_EQ(manyPages->result.code, "atlas_too_many_pages");
}

TEST(TitleSheetFrameTimes, OnceIncludesItsEndAndALoopDoesNot) {
    std::vector<double> times;
    std::string error;
    TitleAnimInfo info;
    info.duration = 1.0;
    info.loop = false;
    ASSERT_TRUE(titleAnimFrameTimes(info, 4.0, Json(), times, error)) << error;
    EXPECT_EQ(times, (std::vector<double>{0.0, 0.25, 0.5, 0.75, 1.0}));
    info.loop = true;
    ASSERT_TRUE(titleAnimFrameTimes(info, 4.0, Json(), times, error)) << error;
    EXPECT_EQ(times, (std::vector<double>{0.0, 0.25, 0.5, 0.75}));
    ASSERT_TRUE(titleAnimFrameTimes(info, 4.0, Json(2), times, error)) << error;
    EXPECT_EQ(times.size(), 2u);
    ASSERT_TRUE(titleAnimFrameTimes(info, 4.0, Json::array({1, 2}), times, error)) << error;
    EXPECT_EQ(times, (std::vector<double>{0.25, 0.5}));
    EXPECT_FALSE(titleAnimFrameTimes(info, 4.0, Json(0), times, error));
    EXPECT_FALSE(titleAnimFrameTimes(info, 4.0, Json::array({2, 1}), times, error));
    EXPECT_FALSE(titleAnimFrameTimes(info, 4.0, Json("all"), times, error));
}

TEST(TitleSheetFrameTimes, AtMost1024Frames) {
    std::vector<double> times;
    std::string error;
    TitleAnimInfo info;
    info.duration = 100.0;
    info.loop = true;
    ASSERT_TRUE(titleAnimFrameTimes(info, 30.0, Json(1024), times, error)) << error;
    EXPECT_EQ(times.size(), 1024u);
    EXPECT_FALSE(titleAnimFrameTimes(info, 30.0, Json(1025), times, error));
    EXPECT_NE(error.find("at most 1024"), std::string::npos) << error;
    EXPECT_FALSE(titleAnimFrameTimes(info, 30.0, Json(), times, error)) << "3,000 frames by default";
}
