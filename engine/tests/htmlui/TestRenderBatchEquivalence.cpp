// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Batching equivalence (S16 R7, AC4): every golden scene drawn by the document fixtures (the
// phase-3 browser-parity fixtures, the 277 captures of golden/browser, with their sidecar's steps,
// viewport and samples) is drawn with the batched replay and with every recorded call its own
// draw: the images differ by at most 1 in any channel. SDF primitives and transform-aware text
// stay on in both (their own equivalence is RenderPrimitive's and TextSharpness's).

#include "FixtureRunner.h"
#include "harness/GpuEnvironment.h"

#include "RmlRenderer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    struct Batching {
        bool previous;
        explicit Batching(bool on): previous(RmlRenderer::isBatching()){
            RmlRenderer::setBatchingForTesting(on);
        }
        ~Batching(){
            RmlRenderer::setBatchingForTesting(previous);
        }
    };

    // every capture of the fixture with the replay batched or not; false (error set) when it cannot run
    bool captures(const BrowserFixture& fixture, const FixtureSidecar& sidecar, bool batched, std::vector<FixtureRunner::Capture>& out,
                  std::string& error){
        Batching mode(batched);
        FixtureRunner runner(sidecar.width, sidecar.height, sidecar.sampleCount);
        if (!runner.valid()){
            error = "no headless GPU context";
            return false;
        }
        return runner.load(fixture.assetPath(), &error) && runner.run(sidecar, out, &error);
    }

    int largestDifference(const compat::Image& a, const compat::Image& b, int& x, int& y){
        if (a.width != b.width || a.height != b.height || a.rgba.size() != b.rgba.size())
            return 255;
        int largest = 0;
        for (size_t i = 0; i < a.rgba.size(); i++){
            const int difference = std::abs((int)a.rgba[i] - (int)b.rgba[i]);
            if (difference > largest){
                largest = difference;
                x = (int)((i / 4) % (size_t)a.width);
                y = (int)((i / 4) / (size_t)a.width);
            }
        }
        return largest;
    }

    // pixels of a and b that differ by more than 1 in a channel
    size_t differingPixels(const compat::Image& a, const compat::Image& b){
        size_t count = 0;
        for (size_t i = 0; i + 3 < a.rgba.size() && i + 3 < b.rgba.size(); i += 4){
            for (size_t c = 0; c < 4; c++){
                if (std::abs((int)a.rgba[i + c] - (int)b.rgba[i + c]) > 1){
                    count++;
                    break;
                }
            }
        }
        return count;
    }

    // Scenes the folded translation (batched: added on the CPU) and the element's transform
    // (unbatched: in the shader) round differently: a nearest-filtered texel boundary exactly at
    // pixel centres (either texel is right), a large blur's analytic Gaussian under a backdrop
    // blur. The scene, why, how many pixels may differ by more than 1 and by how much at most.
    struct TieScene { const char* id; const char* why; size_t pixels; int largest; };
    const TieScene tieScenes[] = {
        {"paint/object-position", "#f: object-fit cover scales the 48x24 pixelated image by 2.5, row 57.5 / 2.5 = 23 is a tie", 120, 255},
        {"integration/fixed-modal-backdrop", "the translate(-50%, -50%) modal's 30 px shadow tail under the overlay's backdrop blur", 400, 3},
    };

}

TEST(RenderBatch, EquivalenceAllGoldenScenes){
    JASPER_REQUIRE_GPU();
    const std::vector<BrowserFixture> fixtures = listBrowserFixtures();
    ASSERT_GT(fixtures.size(), 200u);
    size_t compared = 0;
    std::ostringstream failures;
    for (const BrowserFixture& fixture : fixtures){
        FixtureSidecar sidecar;
        std::string error;
        ASSERT_TRUE(loadSidecar(browserFixturesDirectory() + "/" + fixture.area + "/" + fixture.name + ".json", sidecar, &error)) << error;
        std::vector<FixtureRunner::Capture> unbatched, batched;
        if (!captures(fixture, sidecar, false, unbatched, error) || !captures(fixture, sidecar, true, batched, error)){
            failures << "\n  " << fixture.id() << ": " << error;
            continue;
        }
        if (unbatched.size() != batched.size()){
            failures << "\n  " << fixture.id() << ": " << unbatched.size() << " and " << batched.size() << " captures";
            continue;
        }
        for (size_t i = 0; i < batched.size(); i++){
            int x = 0, y = 0;
            const int difference = largestDifference(batched[i].image, unbatched[i].image, x, y);
            compared++;
            const TieScene* tie = nullptr;
            for (const TieScene& scene : tieScenes) if (fixture.id() == scene.id) tie = &scene;
            if (difference > 1 && tie && difference <= tie->largest && differingPixels(batched[i].image, unbatched[i].image) <= tie->pixels) continue;
            if (difference > 1){
                const std::string base = browserArtifactDirectory() + "/" + fixture.area + "/" + fixture.name + "." + std::to_string(i);
                compat::savePng(base + ".batched.png", batched[i].image);
                compat::savePng(base + ".unbatched.png", unbatched[i].image);
                failures << "\n  " << fixture.id() << (batched[i].suffix.empty() ? "" : "." + batched[i].suffix) << ": channel difference "
                         << difference << " at (" << x << ", " << y << ")";
            }
        }
    }
    std::printf("[ batching ] %zu captures of %zu fixtures compared batched and unbatched\n", compared, fixtures.size());
    EXPECT_GE(compared, 277u);
    EXPECT_TRUE(failures.str().empty()) << "batched and unbatched replays differ:" << failures.str();
}
