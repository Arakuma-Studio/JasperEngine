// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S14's test project: cooked textures through the runtime of a platform. assets/je-cooked/ holds
// the desktop, mobile, android_etc2 and web cooks of source/ (make_assets.sh), as an export with
// the ETC2 fallback ships them; the build loads its family's (ETC1S / UASTC transcoded at load on
// the web, ASTC on Android, ETC2 on an Android GPU without ASTC such as the emulator's
// SwiftShader, BC on desktop), falls back where the GPU cannot sample a format, and draws them. A solid
// texture fills the canvas centre: the gate checks its colour (WEB_PROJECT=texture-formats
// tools/build.sh web, GATE=texture-formats tools/build.sh web-gate; ANDROID_PROJECT=texture-formats
// tools/build.sh android with android-expect.txt).
//
// With ?transcode_bench=1 (a Release web build on the benchmark server E, docs/textures.md) it
// first times spec S14 R19's web row on the page's one thread: a 2048² UASTC λ1 file with its mips
// transcoded to BC7 and to ASTC 4x4 (median of 15 runs; budget 40 ms each in desktop Chrome), plus
// the transcoder's first-use set-up, the Zstandard inflate of the source file and the longest band
// of the main thread's time slices. A budget missed is an error line (GATE_ARGS='--query=transcode_bench=1
// --expect=transcode_web_budgets=pass'); ?transcode_bench=report prints the figures without a
// verdict (indicative runs on a shared build server). The 2048² file is assets/bench/'s 512² cook
// of the corpus photo (make_assets.sh), its blocks tiled: UASTC blocks transcode independently,
// so the per-block work is a 2048² photo's.

#include "JasperEngine.h"
#include "io/Data.h"
#include "subsystem/RenderSystem.h"
#include "texture/BasisTranscode.h"
#include "texture/CookedTextures.h"
#include "texture/Ktx2.h"
#include "../../../engine/tests/core/bench/BenchUtil.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
// 0: no bench, 1: bench and budgets, 2: bench only
EM_JS(int, transcodeBenchRequested, (), {
    const value = new URLSearchParams(globalThis.location ? globalThis.location.search : "").get("transcode_bench");
    return value === "1" ? 1 : value === "report" ? 2 : 0;
});
#endif

using namespace jasperengine;

Scene scene;
Camera camera(&scene);
Primitive solid(&scene);
std::vector<std::unique_ptr<Primitive>> tiles;
bool reported = false;

namespace {
    const char* const kTiles[] = {"textures/photo.png", "textures/normal.png", "textures/sprite.png", "textures/sky.hdr"};
}

namespace transcodebench {

    constexpr const char* kSource = "bench/uastc-photo-512.ktx2";
    constexpr uint32_t kSize = 2048;
    constexpr int kRuns = 15;
    // R19, desktop Chrome: a 2048² UASTC to BC7 or ASTC 4x4
    constexpr double kBudgetMs = 40.0;

    void report(const char* metric, double value){
        Log::print("[bench] transcode web %s %.3f", metric, value);
    }

    bool withinBudget(const char* metric, double value, double budget){
        if (value <= budget)
            return true;
        Log::error("[bench] transcode web %s %.3f misses its budget (<= %.3f)", metric, value, budget);
        return false;
    }

    // the 2048² chain from the 512² one: levels 0 and 1 tile level 0's blocks 4x4 and 2x2, the
    // levels below are the source's from level 0 on
    bool tiledChain(const Ktx2Image& source, Ktx2Image& out){
        const uint32_t sourceBlocks = (source.info.width + 3) / 4;
        if (source.info.codec != Ktx2Codec::UASTC || source.info.width != source.info.height || source.levels.empty())
            return false;
        out.info = source.info;
        out.metadata = source.metadata;
        out.firstLevel = 0;
        out.info.width = out.info.height = kSize;
        out.info.levelCount = fullMipCount((int)kSize, (int)kSize);
        out.levels.clear();
        for (uint32_t size = kSize; size > source.info.width; size /= 2){
            const uint32_t blocks = size / 4;
            std::vector<uint8_t> level((size_t)blocks * blocks * 16);
            for (uint32_t y = 0; y < blocks; y++)
                for (uint32_t x = 0; x < blocks; x++)
                    std::memcpy(&level[((size_t)y * blocks + x) * 16], &source.levels[0][((size_t)(y % sourceBlocks) * sourceBlocks + x % sourceBlocks) * 16], 16);
            out.levels.push_back(std::move(level));
        }
        for (const std::vector<uint8_t>& level : source.levels)
            out.levels.push_back(level);
        out.info.levels.assign(out.levels.size(), Ktx2LevelIndex());
        return out.levels.size() == out.info.levelCount;
    }

    // milliseconds of one whole chain to `target`
    double chainMs(BasisTranscode::Session& session, GpuTextureFormat target, std::vector<uint8_t>& out){
        const auto start = bench::Clock::now();
        std::string error;
        for (uint32_t level = 0; level < session.levelCount(); level++){
            if (!session.transcodeLevel(level, target, TextureContent::ColorOpaque, out, error)){
                Log::error("[bench] transcode web: %s", error.c_str());
                return 1e9;
            }
        }
        return bench::secondsSince(start) * 1000.0;
    }

    void run(bool budgets){
        // the transcoder's tables (the ETC1S -> BC1 ones computed): the first Basis file pays it
        const auto initStart = bench::Clock::now();
        BasisTranscode::warmUp();
        const double initMs = bench::secondsSince(initStart) * 1000.0;

        Data file;
        if (file.open(kSource) != FileErrors::FILEDATA_OK){
            Log::error("[bench] transcode web: no %s", kSource);
            return;
        }
        const std::vector<uint8_t> bytes(file.getMemPtr(), file.getMemPtr() + file.length());
        Ktx2MemorySource memory(bytes);
        Ktx2Image source;
        std::string error;
        std::vector<double> inflate;
        for (int run = 0; run < kRuns; run++){
            const auto start = bench::Clock::now();
            if (!Ktx2::read(memory, 0, source, error)){
                Log::error("[bench] transcode web: %s", error.c_str());
                return;
            }
            inflate.push_back(bench::secondsSince(start) * 1000.0);
        }
        Ktx2Image chain;
        if (!tiledChain(source, chain)){
            Log::error("[bench] transcode web: %s is not a square UASTC file", kSource);
            return;
        }
        BasisTranscode::Session session;
        if (!session.open(std::move(chain), error)){
            Log::error("[bench] transcode web: %s", error.c_str());
            return;
        }

        std::vector<uint8_t> out;
        double medians[2] = {};
        const GpuTextureFormat targets[2] = {GpuTextureFormat::BC7, GpuTextureFormat::ASTC_4x4};
        for (int t = 0; t < 2; t++){
            std::vector<double> runs;
            chainMs(session, targets[t], out);      // warm: the output buffer and the caches
            for (int run = 0; run < kRuns; run++)
                runs.push_back(chainMs(session, targets[t], out));
            medians[t] = bench::percentile(runs, 0.5);
        }
        // the longest band of the main thread's slices (TextureDataPool: 32 block rows) on level 0
        double longestBand = 0.0;
        std::vector<uint8_t> level(session.levelBytes(0, GpuTextureFormat::BC7));
        for (uint32_t row = 0; row < session.blockRows(0); row += 32){
            const auto start = bench::Clock::now();
            session.transcodeRows(0, row, std::min(32u, session.blockRows(0) - row), GpuTextureFormat::BC7, TextureContent::ColorOpaque, level, error);
            longestBand = std::max(longestBand, bench::secondsSince(start) * 1000.0);
        }

        report("init_ms", initMs);
        report("zstd_inflate_512_ms", bench::percentile(inflate, 0.5));
        report("uastc_2048_bc7_ms", medians[0]);
        report("uastc_2048_astc4x4_ms", medians[1]);
        report("band_32_rows_bc7_ms", longestBand);
        if (!budgets){
            Log::print("[bench] transcode_web_budgets=report (no verdict)");
            return;
        }
        bool pass = withinBudget("uastc_2048_bc7_ms", medians[0], kBudgetMs);
        pass = withinBudget("uastc_2048_astc4x4_ms", medians[1], kBudgetMs) && pass;
        Log::print("[bench] transcode_web_budgets=%s", pass ? "pass" : "fail");
    }

}

JASPERENGINE_INIT void init(){
#ifdef __EMSCRIPTEN__
    if (const int bench = transcodeBenchRequested())
        transcodebench::run(bench == 1);
#endif

    Engine::setCanvasSize(320, 240);
    scene.setBackgroundColor(0.0f, 0.0f, 0.0f, 1.0f);

    // the canvas centre: a cooked solid colour (200, 60, 30)
    solid.createPlane(2.0f, 2.0f);
    solid.setTexture("textures/solid.png");
    solid.setReceiveLights(false);

    // one corner each: colour, normal map, sprite with alpha, HDR
    const float corners[4][2] = {{-1.6f, -1.1f}, {1.6f, -1.1f}, {-1.6f, 1.1f}, {1.6f, 1.1f}};
    for (int i = 0; i < 4; i++){
        tiles.push_back(std::make_unique<Primitive>(&scene));
        tiles.back()->createPlane(0.9f, 0.9f);
        tiles.back()->setPosition(corners[i][0], 0.0f, corners[i][1]);
        tiles.back()->setTexture(kTiles[i]);
        tiles.back()->setReceiveLights(false);
    }

    camera.setType(CameraType::CAMERA_ORTHO);
    camera.setOrtho(-2.0f, 2.0f, -1.5f, 1.5f, 0.1f, 100.0f);
    camera.setPosition(0.0f, 10.0f, 0.0f);
    camera.setTarget(0.0f, 0.0f, 0.0f);
    camera.setUp(0.0f, 0.0f, -1.0f);
    camera.activate();

    Engine::setScene(&scene);
    Engine::onUpdate.add("textureFormatsTest", [](){
        if (reported || !scene.getSystem<RenderSystem>()->isAllLoaded())
            return;
        reported = true;
        int cooked = 0;
        for (const char* path : kTiles){
            Texture texture(path);
            texture.load();
            Log::print("[gate] %s %s %dx%d stored %dx%d", path, texture.getGpuFormat().c_str(), texture.getWidth(), texture.getHeight(),
                       texture.getStoredWidth(), texture.getStoredHeight());
            cooked += texture.isCompressed() ? 1 : 0;
        }
        Log::print("[gate] texture family %s", CookedTextures::activeFamily().c_str());
        Log::print("[gate] textures loaded %d compressed %s", (int)(sizeof(kTiles) / sizeof(kTiles[0])), cooked > 0 ? "yes" : "no");
    });
}
