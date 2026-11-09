// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S47 AC18 (R4): a 100-op script touching every op kind, run twice and at 1 and 8 job workers,
// gives identical file bytes and echo text.

#include "SpriteDrawTestSupport.h"

#include "jobs/JobSystem.h"
#include "pixelart/draw/GridCodec.h"

using namespace jasperengine::editor::pixelart;
using namespace jasperengine::editor::pixelart::draw;
using jasperengine::editor::pixelart::testing::json;

namespace {

    json hundredOps() {
        json ops = json::array();
        const std::vector<json> variety = {
            {{"op", "pixel"}, {"points", {{1, 1}}}, {"color", 1}},
            {{"op", "line"}, {"points", {{0, 0}, {15, 9}}}, {"color", 2}},
            {{"op", "rect"}, {"rect", {2, 2, 6, 5}}, {"color", 3}, {"radius", 1}},
            {{"op", "ellipse"}, {"rect", {3, 3, 8, 7}}, {"color", 4}, {"fill", true}},
            {{"op", "polygon"}, {"points", {{0, 0}, {8, 2}, {4, 9}}}, {"color", 5}, {"fill", true}},
            {{"op", "paste"}, {"rows", {"1.2", ".3."}}, {"at", {12, 12}}},
            {{"op", "fill"}, {"at", {31, 31}}, {"color", 1}},
            {{"op", "replace"}, {"from", 1}, {"to", 2}},
            {{"op", "dither"}, {"colors", {3, 4}}, {"level", 0.4}, {"rect", {0, 16, 16, 8}}},
            {{"op", "clear"}, {"rect", {4, 4, 3, 3}}},
            {{"op", "outline"}, {"color", 5}},
            {{"op", "inline"}, {"color", 1}},
            {{"op", "shade"}, {"mode", "ramp"}, {"ramp", {1, 2, 3}}},
            {{"op", "shift"}, {"by", {3, 2}}, {"wrap", true}},
            {{"op", "flip"}, {"axis", "y"}},
            {{"op", "rotate"}, {"angle", 37}},
            {{"op", "rotate"}, {"angle", 90}},
            {{"op", "scale"}, {"factor", 2}, {"rect", {8, 8, 8, 8}}},
            {{"op", "mirror"}, {"axis", "x"}},
            {{"op", "copy"}, {"source", {{"rect", {0, 0, 8, 8}}}}, {"at", {16, 16}}},
            {{"op", "select"}, {"shape", "ellipse"}, {"rect", {4, 4, 20, 20}}},
            {{"op", "select"}, {"shape", "none"}},
            {{"op", "layer"}, {"action", "set"}, {"name", "A"}, {"opacity", 200}},
            {{"op", "frame"}, {"action", "set"}, {"duration_ms", 90}},
            {{"op", "tag"}, {"action", "set"}, {"name", "t"}, {"from", 1}, {"to", 1}},
            {{"op", "slice"}, {"action", "set"}, {"name", "s"}, {"rect", {1, 1, 3, 3}}},
            {{"op", "cel"}, {"action", "set"}, {"opacity", 180}},
            {{"op", "palette"}, {"action", "replace"}, {"index", 5}, {"color", "#ff00aa"}},
        };
        for (int i = 0; ops.size() < 98; i++) ops.push_back(variety[static_cast<size_t>(i) % variety.size()]);
        ops.push_back({{"op", "canvas"}, {"action", "resize"}, {"size", {34, 34}}, {"anchor", "center"}});
        ops.push_back({{"op", "canvas"}, {"action", "resize"}, {"size", {32, 32}}, {"anchor", "center"}});
        return ops;
    }

    struct Outcome {
        std::vector<uint8_t> bytes;
        std::string text;
    };

    Outcome runScript() {
        draw::SpriteDoc sprite = testing::makeSprite(32, 32);
        Outcome outcome;
        const testing::Run run = testing::apply(sprite, hundredOps());
        EXPECT_TRUE(run.ok) << run.problem.kind << ": " << run.problem.message << " at " << run.opIndex;
        PixelProblem problem;
        EXPECT_TRUE(draw::encodeSprite(sprite, outcome.bytes, problem)) << problem.message;
        for (const draw::DirtyRect& d : run.result.changed) {
            outcome.text += std::to_string(d.frame) + "/" + std::to_string(d.layer) + " " + std::to_string(d.rect.x) + "," + std::to_string(d.rect.y) + "\n";
        }
        const std::vector<uint8_t> palette = sprite.doc.paletteRgba(0);
        const PixelImage canvas = draw::celCanvas(sprite.doc, 0, 0);
        const draw::EncodedGrid grid = draw::encodeRegion(canvas, canvas.bounds(), draw::GridEncoding::Auto, palette, 0);
        for (const std::string& row : grid.rows) outcome.text += row + "\n";
        return outcome;
    }

}

TEST(SpriteDrawDeterminism, SameScriptSameBytesAndEcho) {
    const Outcome first = runScript();
    const Outcome second = runScript();
    EXPECT_EQ(first.bytes, second.bytes);
    EXPECT_EQ(first.text, second.text);
}

TEST(SpriteDrawDeterminism, WorkerCountDoesNotChangeTheOutput) {
    namespace jobs = jasperengine::jobs;
    const bool hadSystem = jobs::hasSystem();
    if (hadSystem) jobs::shutdown();
    auto withWorkers = [](int workers) {
        jobs::Config config;
        config.workers = workers;
        config.reserveForFrame = 0;
        jobs::initialize(config);
        Outcome outcome = runScript();
        jobs::shutdown();
        return outcome;
    };
    const Outcome one = withWorkers(1);
    const Outcome eight = withWorkers(8);
    if (hadSystem) jobs::initialize({});
    EXPECT_EQ(one.bytes, eight.bytes);
    EXPECT_EQ(one.text, eight.text);
}
