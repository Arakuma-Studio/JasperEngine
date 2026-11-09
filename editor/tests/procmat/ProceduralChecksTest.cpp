// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S90 R36, R37: the metrics and checks of a material's images, the rows the agent's tools, check
// --materials and the window's Problems strip share. Made-up images, each built to cross one threshold.

#include "../../procmat/ProceduralChecks.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <functional>

using namespace jasperengine::editor;
using namespace jasperengine::editor::procmat;

namespace {

    using Pixel = std::function<std::array<uint8_t, 4>(int x, int y)>;

    OutputImage imageOf(const std::string& name, int side, const Pixel& pixel) {
        OutputImage image;
        image.name = name;
        image.width = image.height = side;
        image.rgba.resize((size_t)side * side * 4);
        for (int y = 0; y < side; y++) {
            for (int x = 0; x < side; x++) {
                const std::array<uint8_t, 4> p = pixel(x, y);
                std::copy(p.begin(), p.end(), image.rgba.begin() + ((size_t)y * side + x) * 4);
            }
        }
        return image;
    }

    // a smooth tileable pattern that repeats nowhere inside the image: three waves of whole periods, no
    // offset but the origin lines all three up again
    uint8_t wave(int x, int y, int side) {
        const double t = 2.0 * 3.14159265358979323846 / side;
        return (uint8_t)std::lround(127.5 + 35.0 * std::sin(x * t) + 35.0 * std::cos(y * t) + 15.0 * std::sin((x + 2 * y) * t));
    }

    const qa::Json& rowOf(const qa::Json& report, const std::string& id) {
        for (const qa::Json& row : report["checks"]) {
            if (row.value("id", "") == id) return row;
        }
        static const qa::Json none;
        ADD_FAILURE() << "no row " << id;
        return none;
    }

    std::string status(const qa::Json& report, const std::string& id) {
        return rowOf(report, id).value("status", "");
    }

    // a dielectric stone: tileable colour, flat normal, varied roughness, no metal
    MaterialImages goodStone(int side = 64) {
        MaterialImages images;
        images.outputs["base_color"] = imageOf("base_color", side, [&](int x, int y) {
            const uint8_t v = wave(x, y, side);
            return std::array<uint8_t, 4>{v, (uint8_t)(v - 10), (uint8_t)(v - 20), 255};
        });
        images.outputs["normal"] = imageOf("normal", side, [](int, int) { return std::array<uint8_t, 4>{128, 128, 255, 255}; });
        images.outputs["orm"] = imageOf("orm", side, [&](int x, int y) { return std::array<uint8_t, 4>{255, wave(x, y, side), 0, 255}; });
        return images;
    }

}

TEST(ProceduralChecks, AGoodStonePassesAndEveryRowHasItsShape) {
    const qa::Json report = ProceduralChecks::report(goodStone(), CheckRequest());
    ASSERT_EQ(report["checks"].size(), ProceduralChecks::checkIds().size());
    for (const qa::Json& row : report["checks"]) {
        for (const char* key : {"id", "kind", "status", "round", "measured", "threshold", "threshold_source", "limited_by", "at", "fix_hint", "method"}) {
            EXPECT_TRUE(row.contains(key)) << row.value("id", "") << " lacks " << key;
        }
    }
    for (const char* id : {"tileability", "histogram", "base_color", "normal", "roughness", "metallic"}) EXPECT_EQ(status(report, id), "PASS") << id << ": " << rowOf(report, id).dump();
    EXPECT_EQ(status(report, "height_8bit"), "SKIP") << "no height";
    EXPECT_TRUE(report["thresholds"]["changed"].empty());
    EXPECT_TRUE(report["metrics"]["cost"].contains("mobile_mb"));
}

TEST(ProceduralChecks, ASeamFailsTileability) {
    MaterialImages images = goodStone();
    // a ramp across the image: its last column steps all the way back to its first
    images.outputs["base_color"] = imageOf("base_color", 64, [](int x, int) { return std::array<uint8_t, 4>{(uint8_t)(40 + x * 3), 90, 90, 255}; });
    const qa::Json report = ProceduralChecks::report(images, CheckRequest());
    EXPECT_EQ(status(report, "tileability"), "FAIL") << rowOf(report, "tileability").dump();
    EXPECT_NE(rowOf(report, "tileability")["limited_by"].get<std::string>().find("base_color"), std::string::npos);
    EXPECT_EQ(report["verdict"], "FAIL");
}

TEST(ProceduralChecks, ARepeatingPatternWarns) {
    MaterialImages images = goodStone();
    images.outputs["base_color"] = imageOf("base_color", 64, [](int x, int y) {
        const uint8_t v = ((x / 8 + y / 8) % 2) ? 180 : 80;
        return std::array<uint8_t, 4>{v, v, v, 255};
    });
    const qa::Json report = ProceduralChecks::report(images, CheckRequest());
    EXPECT_EQ(status(report, "repetition"), "WARN") << rowOf(report, "repetition").dump();
    EXPECT_EQ(status(ProceduralChecks::report(goodStone(), CheckRequest()), "repetition"), "PASS") << "one period of a wave repeats nowhere inside it";
}

TEST(ProceduralChecks, ClippedAndOutOfRangeAlbedoWarn) {
    MaterialImages images = goodStone();
    images.outputs["base_color"] = imageOf("base_color", 64, [](int x, int) {
        const uint8_t v = x < 16 ? 0 : 150;
        return std::array<uint8_t, 4>{v, v, v, 255};
    });
    const qa::Json report = ProceduralChecks::report(images, CheckRequest());
    EXPECT_EQ(status(report, "histogram"), "WARN");
    EXPECT_EQ(status(report, "base_color"), "WARN") << "a quarter of a dielectric is pure black";
    // the same texels count as metal when the ORM says so
    images.outputs["orm"] = imageOf("orm", 64, [](int x, int y) { return std::array<uint8_t, 4>{255, (uint8_t)(40 + y), (uint8_t)(x < 16 ? 255 : 0), 255}; });
    EXPECT_EQ(status(ProceduralChecks::report(images, CheckRequest()), "base_color"), "PASS");
}

TEST(ProceduralChecks, ANormalFacingAwayFailsAndAFlippedGreenWarns) {
    MaterialImages images = goodStone();
    // Material Maker's own convention: z toward -1
    images.outputs["normal"] = imageOf("normal", 64, [](int, int) { return std::array<uint8_t, 4>{128, 128, 0, 255}; });
    EXPECT_EQ(status(ProceduralChecks::report(images, CheckRequest()), "normal"), "FAIL");

    // a height that rises and falls down the rows: the engine's normal faces up (+Y, green) where it rises
    // toward the bottom, a DirectX one down
    const double t = 2.0 * 3.14159265358979323846 / 64.0;
    images.outputs["height"] = imageOf("height", 64, [&](int, int y) {
        const uint8_t v = (uint8_t)std::lround(127.5 + 100.0 * std::sin(y * t));
        return std::array<uint8_t, 4>{v, v, v, 255};
    });
    const auto normal = [&](double sign) {
        return imageOf("normal", 64, [=](int, int y) {
            const double ny = sign * 0.5 * std::cos(y * t), nz = std::sqrt(1.0 - ny * ny);
            return std::array<uint8_t, 4>{128, (uint8_t)std::lround(127.5 + 127.5 * ny), (uint8_t)std::lround(127.5 + 127.5 * nz), 255};
        });
    };
    images.outputs["normal"] = normal(1.0);
    const qa::Json up = ProceduralChecks::report(images, CheckRequest());
    EXPECT_EQ(status(up, "normal"), "PASS") << rowOf(up, "normal").dump();
    EXPECT_GT(up["metrics"]["normal"]["green_slope_correlation"].get<double>(), 0.9);
    images.outputs["normal"] = normal(-1.0);
    const qa::Json down = ProceduralChecks::report(images, CheckRequest());
    EXPECT_EQ(status(down, "normal"), "WARN") << rowOf(down, "normal").dump();
}

TEST(ProceduralChecks, FlatRoughnessAndHalfMetalWarn) {
    MaterialImages images = goodStone();
    images.outputs["orm"] = imageOf("orm", 64, [](int, int) { return std::array<uint8_t, 4>{255, 128, 128, 255}; });
    const qa::Json report = ProceduralChecks::report(images, CheckRequest());
    EXPECT_EQ(status(report, "roughness"), "WARN");
    EXPECT_EQ(status(report, "metallic"), "WARN");
}

TEST(ProceduralChecks, AnEightBitHeightWarnsOnlyWhenAShaderSamplesIt) {
    MaterialImages images = goodStone();
    images.outputs["height"] = imageOf("height", 64, [](int x, int) { const uint8_t v = (uint8_t)(x * 2); return std::array<uint8_t, 4>{v, v, v, 255}; });
    const qa::Json unused = ProceduralChecks::report(images, CheckRequest());
    EXPECT_EQ(status(unused, "height_8bit"), "PASS");
    EXPECT_EQ(rowOf(unused, "height_8bit")["measured"]["levels"], 64);
    images.heightReader = "the shader texture u_height";
    const qa::Json sampled = ProceduralChecks::report(images, CheckRequest());
    EXPECT_EQ(status(sampled, "height_8bit"), "WARN");
    EXPECT_NE(rowOf(sampled, "height_8bit")["limited_by"].get<std::string>().find("u_height"), std::string::npos);
}

TEST(ProceduralChecks, AChangedThresholdIsReportedAndRoundsCount) {
    MaterialImages images = goodStone();
    // stripes stepping 10 levels, and 35 across the wrap edge: a seam of about 3.5
    images.outputs["base_color"] = imageOf("base_color", 64, [](int x, int) {
        return std::array<uint8_t, 4>{(uint8_t)((x < 32 ? 100 : 125) + (x % 2) * 10), 90, 90, 255};
    });
    ASSERT_EQ(status(ProceduralChecks::report(images, CheckRequest()), "tileability"), "FAIL");
    CheckRequest request;
    request.qa.seamFail = 5.0f;
    const qa::Json loose = ProceduralChecks::report(images, request);
    EXPECT_EQ(status(loose, "tileability"), "PASS");
    ASSERT_EQ(loose["thresholds"]["changed"].size(), 1u);
    EXPECT_EQ(loose["thresholds"]["changed"][0]["key"], "materials.qa.seam_fail") << "a looser threshold never passes silently";

    request = CheckRequest();
    request.rounds = {{"tileability", 2}};
    const qa::Json third = ProceduralChecks::report(images, request);
    EXPECT_EQ(rowOf(third, "tileability")["round"], 3);
    EXPECT_TRUE(third["stop"].get<bool>()) << "past fix_rounds the agent stops";
    request.only = {"normal"};
    EXPECT_EQ(ProceduralChecks::report(images, request)["checks"].size(), 1u);
}

TEST(ProceduralChecks, AReferenceIsAdvisory) {
    const MaterialImages images = goodStone();
    const qa::Json same = ProceduralChecks::reference(images, images.outputs.at("base_color"));
    EXPECT_FALSE(same.contains("status"));
    EXPECT_NEAR(same["palette_delta_e"].get<double>(), 0.0, 1e-9);
    EXPECT_NEAR(same["spectrum"].get<double>(), 0.0, 1e-9);
    const OutputImage noise = imageOf("reference", 64, [](int x, int y) {
        const uint8_t v = (uint8_t)((x * 73 + y * 151) % 256);
        return std::array<uint8_t, 4>{v, 20, 200, 255};
    });
    const qa::Json other = ProceduralChecks::reference(images, noise);
    EXPECT_GT(other["palette_delta_e"].get<double>(), 10.0);
    EXPECT_GT(other["spectrum"].get<double>(), same["spectrum"].get<double>());
}
