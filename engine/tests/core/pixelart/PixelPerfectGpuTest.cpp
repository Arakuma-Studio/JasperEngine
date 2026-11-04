// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S41's GPU acceptance (docs/specs/S41-pixel-perfect-camera.md AC3-AC10): what a Pixel Perfect
// Camera draws, read back from the headless device (glcore, and Vulkan on lavapipe) and compared
// with the CPU twin of the pixel-snap rule (pixelart/PixelSnap.h) or with a CPU compositor of the
// same scene. Each behaviour has its red control in the same test: the scene drawn without the
// feature misses the prediction.

#include "Engine.h"
#include "Input.h"
#include "Scene.h"
#include "component/CameraComponent.h"
#include "component/MeshComponent.h"
#include "component/PixelPerfectCameraComponent.h"
#include "component/TilemapComponent.h"
#include "component/Transform.h"
#include "object/Camera.h"
#include "object/PixelPerfectCamera.h"
#include "object/Sprite.h"
#include "object/Tilemap.h"
#include "pixelart/PixelSnap.h"
#include "render/RenderStats.h"
#include "render/SystemRender.h"
#include "subsystem/RenderSystem.h"
#include "texture/TextureData.h"

#include "GpuEnvironment.h"
#include "LogCapture.h"
#include "Readback.h"
#include "support/AllocationCounter.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    struct PixelColor {
        uint8_t r = 0, g = 0, b = 0, a = 255;
        bool operator==(const PixelColor& other) const { return r == other.r && g == other.g && b == other.b; }
        bool operator!=(const PixelColor& other) const { return !(*this == other); }
    };

    // pure channels: they survive any sRGB encode and decode exactly
    constexpr PixelColor kBlack{0, 0, 0, 255};
    constexpr PixelColor kWhite{255, 255, 255, 255};
    constexpr PixelColor kRed{255, 0, 0, 255};
    constexpr PixelColor kBlue{0, 0, 255, 255};
    constexpr PixelColor kGreen{0, 255, 0, 255};

    // pixels kept alive for every texture made of them (TextureData does not own them)
    std::vector<uint8_t>& keepPixels(std::vector<uint8_t> pixels){
        static std::vector<std::unique_ptr<std::vector<uint8_t>>> kept;
        kept.push_back(std::make_unique<std::vector<uint8_t>>(std::move(pixels)));
        return *kept.back();
    }

    TextureData pixelArtTexture(int width, int height, PixelColor (*texel)(int x, int y, int width, int height)){
        std::vector<uint8_t> rgba((size_t)width * height * 4);
        for (int y = 0; y < height; y++){
            for (int x = 0; x < width; x++){
                const PixelColor c = texel(x, y, width, height);
                uint8_t* p = &rgba[((size_t)y * width + x) * 4];
                p[0] = c.r; p[1] = c.g; p[2] = c.b; p[3] = c.a;
            }
        }
        std::vector<uint8_t>& kept = keepPixels(std::move(rgba));
        return TextureData(width, height, (unsigned int)kept.size(), ColorFormat::RGBA, 4, kept.data());
    }

    PixelColor whiteTexel(int, int, int, int){ return kWhite; }
    PixelColor checkerTexel(int x, int y, int, int){ return ((x + y) & 1) ? kBlue : kRed; }
    // a ring of one texel around a fill: the same under any flip or quarter turn
    PixelColor ringTexel(int x, int y, int width, int height){
        return (x == 0 || y == 0 || x == width - 1 || y == height - 1) ? kGreen : kRed;
    }

    struct Image {
        int width = 0, height = 0;
        std::vector<uint8_t> rgba;     // rows top first
        PixelColor at(int x, int y) const{
            const uint8_t* p = &rgba[((size_t)y * width + x) * 4];
            return PixelColor{p[0], p[1], p[2], p[3]};
        }
    };

    Image readImage(){
        Image image;
        Framebuffer& target = GpuEnvironment::target();
        image.width = (int)target.getWidth();
        image.height = (int)target.getHeight();
        readTexture(target.getRender().getColorTexture(), image.width, image.height, image.rgba);
        return image;
    }

    // the same image moved by dx pixels (positive: the content to the right), compared where
    // both have pixels
    size_t shiftedDifferences(const Image& image, const Image& reference, int dx, int x0, int x1, int y0, int y1){
        size_t differences = 0;
        for (int y = y0; y < y1; y++){
            for (int x = x0; x < x1; x++){
                const int sx = x - dx;
                if (sx < x0 || sx >= x1)
                    continue;
                if (image.at(x, y) != reference.at(sx, y))
                    differences++;
            }
        }
        return differences;
    }

    class PixelPerfectGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};

        void SetUp() override{
            scene.setBackgroundColor(0.0f, 0.0f, 0.0f, 1.0f);
            camera.setType(CameraType::CAMERA_ORTHO);
            CameraComponent& component = scene.getComponent<CameraComponent>(camera.getEntity());
            component.useTarget = false;
            camera.setPosition(0.0f, 0.0f, 1.0f);
            camera.activate();
            Engine::setScene(&scene);
        }

        void TearDown() override{
            Engine::removeScene(&scene);
            Engine::setPixelPerfectForcedScale(0);
            Engine::setPixelsPerUnit(1);
        }

        PixelPerfectCamera pixelPerfect(int referenceWidth, int referenceHeight){
            PixelPerfectCamera ppc(&scene, camera.getEntity());
            ppc.setReferenceResolution(Vector2((float)referenceWidth, (float)referenceHeight));
            return ppc;
        }

        // until everything is loaded, and every tilemap built (it waits for its texture's size)
        bool render(){
            Scene* current = &scene;
            return GpuEnvironment::runFrames([current](){
                auto tilemaps = current->getComponentArray<TilemapComponent>();
                for (size_t i = 0; i < tilemaps->size(); i++){
                    const MeshComponent* mesh = current->findComponent<MeshComponent>(tilemaps->getEntity(i));
                    if (mesh && mesh->buffer.getCount() == 0)
                        return false;
                }
                return current->getSystem<RenderSystem>()->isAllLoaded();
            });
        }

        // a sprite at a world position (its pivot), size in pixels
        Sprite& sprite(const std::string& id, TextureData data, int width, int height, float x, float y,
                       PivotPreset pivot = PivotPreset::BOTTOM_LEFT){
            sprites.push_back(std::make_unique<Sprite>(&scene));
            Sprite& made = *sprites.back();
            made.setTexture(id, data);
            made.setSize((unsigned int)width, (unsigned int)height);
            made.setPivotPreset(pivot);
            made.setPosition(x, y, 0.0f);
            return made;
        }

        std::vector<std::unique_ptr<Sprite>> sprites;
    };

    // the base pixel the CPU twin puts a draw's local point on: the draw's MVP times the point,
    // snapped (an unflipped grid: the logical orientation every backend rounds in)
    void predictPixel(const Matrix4& viewProjection, const Matrix4& model, const Vector3& local, float targetWidth, float targetHeight,
                      int& x, int& y){
        pixelart::SnapGrid grid;
        grid.width = targetWidth;
        grid.height = targetHeight;
        const Matrix4 mvp = viewProjection * model;
        const Vector4 clip = mvp * Vector4(local.x, local.y, local.z, 1.0f);
        const Vector2 p = pixelart::toPixels(pixelart::snapClip(clip, grid), grid);
        x = (int)std::lround(p.x);
        y = (int)std::lround(p.y);
    }

    // the runs of equal colour along each row and column of a rect of the image
    std::set<int> runLengths(const Image& image, int x0, int y0, int x1, int y1){
        std::set<int> runs;
        for (int y = y0; y < y1; y++){
            int run = 1;
            for (int x = x0 + 1; x < x1; x++){
                if (image.at(x, y) == image.at(x - 1, y)){
                    run++;
                }else{
                    runs.insert(run);
                    run = 1;
                }
            }
        }
        for (int x = x0; x < x1; x++){
            int run = 1;
            for (int y = y0 + 1; y < y1; y++){
                if (image.at(x, y) == image.at(x, y - 1)){
                    run++;
                }else{
                    runs.insert(run);
                    run = 1;
                }
            }
        }
        return runs;
    }

    // ---- the CPU compositor of AC4 (the method of spryte's parity checks): orientation-free
    // ring sprites blitted at their snapped corners, nearest, then the base image scaled by k
    struct ReferenceSprite {
        int width, height;   // texels
        Matrix4 model;       // the draw's model (a 90-degree turn or a flip at most)
        Vector2 pivot;       // the local origin's offset from the quad's bottom-left, pixels
    };

    Image composeReference(const std::vector<ReferenceSprite>& drawn, const Matrix4& viewProjection, int baseWidth, int baseHeight,
                           int screenWidth, int screenHeight, const Rect& viewport, int scale){
        std::vector<PixelColor> base((size_t)baseWidth * baseHeight, kBlack);
        for (const ReferenceSprite& s : drawn){
            // the four corners through the draw's MVP and the rule: an axis-aligned box of whole pixels
            const Vector3 corners[4] = {Vector3(-s.pivot.x, -s.pivot.y, 0), Vector3((float)s.width - s.pivot.x, -s.pivot.y, 0),
                                        Vector3((float)s.width - s.pivot.x, (float)s.height - s.pivot.y, 0),
                                        Vector3(-s.pivot.x, (float)s.height - s.pivot.y, 0)};
            int minX = INT32_MAX, minY = INT32_MAX, maxX = INT32_MIN, maxY = INT32_MIN;
            for (const Vector3& corner : corners){
                int px, py;
                predictPixel(viewProjection, s.model, corner, (float)baseWidth, (float)baseHeight, px, py);
                minX = std::min(minX, px); maxX = std::max(maxX, px);
                minY = std::min(minY, py); maxY = std::max(maxY, py);
            }
            for (int y = minY; y < maxY; y++){
                for (int x = minX; x < maxX; x++){
                    if (x < 0 || y < 0 || x >= baseWidth || y >= baseHeight)
                        continue;
                    const bool ring = x == minX || y == minY || x == maxX - 1 || y == maxY - 1;
                    base[(size_t)y * baseWidth + x] = ring ? kGreen : kRed;
                }
            }
        }
        Image image;
        image.width = screenWidth;
        image.height = screenHeight;
        image.rgba.assign((size_t)screenWidth * screenHeight * 4, 0);
        for (int sy = 0; sy < screenHeight; sy++){
            for (int sx = 0; sx < screenWidth; sx++){
                PixelColor c = kBlack;
                // screen rows are top first, viewports and base pixels count from the bottom
                const int fromBottom = screenHeight - 1 - sy;
                const int bx = (int)std::floor((sx - viewport.getX()) / scale);
                const int by = (int)std::floor((fromBottom - viewport.getY()) / scale);
                if (sx >= viewport.getX() && fromBottom >= viewport.getY() && bx >= 0 && by >= 0 && bx < baseWidth && by < baseHeight)
                    c = base[(size_t)by * baseWidth + bx];
                uint8_t* p = &image.rgba[((size_t)sy * screenWidth + sx) * 4];
                p[0] = c.r; p[1] = c.g; p[2] = c.b; p[3] = 255;
            }
        }
        return image;
    }

    size_t differences(const Image& a, const Image& b){
        if (a.width != b.width || a.height != b.height)
            return SIZE_MAX;
        size_t count = 0;
        for (int y = 0; y < a.height; y++)
            for (int x = 0; x < a.width; x++)
                if (a.at(x, y) != b.at(x, y))
                    count++;
        return count;
    }

}

// AC3 (R7, R9, §6.2): 4,096 one-pixel quads, half at seeded random positions and half on the
// half-pixel lattice, each in a cell of its own: the pixel each lights is the CPU twin's
TEST_F(PixelPerfectGpu, SnapParity){
    JASPER_REQUIRE_GPU();
    GpuEnvironment::setView(512, 512, 512, 512, Scaling::LETTERBOX);
    PixelPerfectCamera ppc = pixelPerfect(512, 512);

    struct Quad { float x, y; bool lattice; int cellX, cellY; };
    std::vector<Quad> quads;
    std::mt19937 random(41);
    std::uniform_real_distribution<float> offset(0.0f, 3.0f);
    for (int cy = 0; cy < 64; cy++){
        for (int cx = 0; cx < 64; cx++){
            Quad q;
            q.cellX = cx;
            q.cellY = cy;
            q.lattice = ((cx + cy) & 1) == 0;
            const float fx = q.lattice ? 0.5f + (float)(cx % 3) : offset(random);
            const float fy = q.lattice ? 0.5f + (float)(cy % 3) : offset(random);
            q.x = (float)(cx * 8 + 2) + fx;
            q.y = (float)(cy * 8 + 2) + fy;
            quads.push_back(q);
        }
    }
    const TextureData white = pixelArtTexture(1, 1, &whiteTexel);
    for (const Quad& q : quads)
        sprite("pixelart|parity|white", white, 1, 1, q.x, q.y);

    auto lit = [&](const Image& image, const Quad& q, int& x, int& y){
        int count = 0;
        for (int dy = 0; dy < 8; dy++){
            for (int dx = 0; dx < 8; dx++){
                const int bx = q.cellX * 8 + dx;
                const int by = q.cellY * 8 + dy;
                if (image.at(bx, image.height - 1 - by) == kWhite){
                    x = bx;
                    y = by;
                    count++;
                }
            }
        }
        return count;
    };

    ASSERT_TRUE(render());
    const Image snapped = readImage();
    const Matrix4 viewProjection = scene.getComponent<CameraComponent>(camera.getEntity()).viewProjectionMatrix;
    size_t mismatches = 0;
    for (const Quad& q : quads){
        int px, py, gx = -1, gy = -1;
        predictPixel(viewProjection, Matrix4::translateMatrix(q.x, q.y, 0.0f), Vector3(0, 0, 0), 512.0f, 512.0f, px, py);
        if (lit(snapped, q, gx, gy) != 1 || gx != px || gy != py)
            mismatches++;
    }
    EXPECT_EQ(mismatches, 0u) << "quads off the CPU twin's pixel";

    // D4's measure, on the CPU: how many lattice quads the rule without the bias rounds otherwise
    size_t unbiased = 0;
    for (const Quad& q : quads){
        if (q.lattice && std::floor(q.x + 0.5f) != pixelart::snap(q.x))
            unbiased++;
    }
    std::printf("[S41] lattice quads the unbiased rule rounds otherwise (CPU): %zu of %zu\n", unbiased, quads.size() / 2);

    // red control: without snapping the lattice quads land elsewhere
    ppc.setPixelSnapping(false);
    ASSERT_TRUE(render());
    const Image free = readImage();
    size_t latticeHits = 0, latticeCount = 0;
    for (const Quad& q : quads){
        if (!q.lattice)
            continue;
        latticeCount++;
        int px, py, gx = -1, gy = -1;
        predictPixel(viewProjection, Matrix4::translateMatrix(q.x, q.y, 0.0f), Vector3(0, 0, 0), 512.0f, 512.0f, px, py);
        if (lit(free, q, gx, gy) == 1 && gx == px && gy == py)
            latticeHits++;
    }
    EXPECT_LT((double)latticeHits, 0.6 * (double)latticeCount) << "the snap did nothing";
    EXPECT_NO_ENGINE_ERRORS();
}

// AC4 (R3, R9, R13): ring sprites (a CENTER pivot at a half pixel, an odd size, a quarter turn, a
// flip) and a tile floor equal the CPU compositor at k = 1, 3, 4 and in Integer (Expand)
TEST_F(PixelPerfectGpu, ReferenceParity){
    JASPER_REQUIRE_GPU();
    const TextureData hero = pixelArtTexture(15, 16, &ringTexel);
    const TextureData wide = pixelArtTexture(15, 7, &ringTexel);
    const TextureData tile = pixelArtTexture(16, 16, &ringTexel);
    sprite("pixelart|reference|hero", hero, 15, 16, 100.5f, 60.25f, PivotPreset::CENTER);
    Sprite& turned = sprite("pixelart|reference|wide", wide, 15, 7, 200.0f, 90.0f);
    turned.setRotation(0.0f, 0.0f, 90.0f);
    Sprite& flipped = sprite("pixelart|reference|wide", wide, 15, 7, 260.25f, 120.75f);
    flipped.setScale(Vector3(-1.0f, 1.0f, 1.0f));
    Tilemap floor(&scene);
    floor.setTexture("pixelart|reference|tile", tile);
    floor.addRect(0, "tile", Rect(0, 0, 16, 16));
    for (int i = 0; i < 10; i++)
        floor.addTile(0, Vector2(16.0f * i, 0.0f), 16, 16);
    floor.setPosition(20.5f, 10.5f, 0.0f);

    PixelPerfectCamera ppc = pixelPerfect(320, 180);
    struct Case { int screenWidth, screenHeight; PixelUpscaleMode mode; };
    const Case cases[] = {{320, 180, PixelUpscaleMode::INTEGER}, {960, 540, PixelUpscaleMode::INTEGER},
                          {1280, 720, PixelUpscaleMode::INTEGER}, {1366, 768, PixelUpscaleMode::INTEGER_EXPAND}};
    for (const Case& c : cases){
        SCOPED_TRACE(::testing::Message() << c.screenWidth << "x" << c.screenHeight);
        GpuEnvironment::setView(c.screenWidth, c.screenHeight, 320, 180, Scaling::LETTERBOX);
        ppc.setUpscaleMode(c.mode);
        ppc.setPixelSnapping(true);
        ASSERT_TRUE(render());
        const Image gpu = readImage();
        const pixelart::PixelPerfectLayout& layout = scene.getComponent<PixelPerfectCameraComponent>(camera.getEntity()).layout;
        const Matrix4 viewProjection = scene.getComponent<CameraComponent>(camera.getEntity()).viewProjectionMatrix;

        std::vector<ReferenceSprite> drawn;
        auto modelOf = [this](Entity entity){ return scene.getComponent<Transform>(entity).modelMatrix; };
        drawn.push_back({15, 16, modelOf(sprites[0]->getEntity()), Vector2(7.5f, 8.0f)});
        drawn.push_back({15, 7, modelOf(turned.getEntity()), Vector2(0.0f, 0.0f)});
        drawn.push_back({15, 7, modelOf(flipped.getEntity()), Vector2(0.0f, 0.0f)});
        for (int i = 0; i < 10; i++)
            drawn.push_back({16, 16, modelOf(floor.getEntity()) * Matrix4::translateMatrix(16.0f * i, 0.0f, 0.0f), Vector2(0.0f, 0.0f)});
        const Image reference = composeReference(drawn, viewProjection, layout.baseWidth, layout.baseHeight, c.screenWidth,
                                                 c.screenHeight, layout.viewport, layout.scale);
        EXPECT_EQ(differences(gpu, reference), 0u);

        // red control: unsnapped, some column differs
        ppc.setPixelSnapping(false);
        ASSERT_TRUE(render());
        EXPECT_GT(differences(readImage(), reference), 0u);
    }
    EXPECT_NO_ENGINE_ERRORS();
}

// AC5 (R1): a one-texel checker through the component has every run exactly k wide and tall;
// the scene's Fixed Resolution under a fractional fit has runs of two lengths (the red control)
TEST_F(PixelPerfectGpu, EvenColumns){
    JASPER_REQUIRE_GPU();
    sprite("pixelart|even|checker", pixelArtTexture(320, 180, &checkerTexel), 320, 180, 0.0f, 0.0f);
    PixelPerfectCamera ppc = pixelPerfect(320, 180);
    for (const auto& screen : {std::pair<int, int>{1366, 768}, std::pair<int, int>{1920, 1080}}){
        SCOPED_TRACE(::testing::Message() << screen.first << "x" << screen.second);
        GpuEnvironment::setView(screen.first, screen.second, 320, 180, Scaling::LETTERBOX);
        ASSERT_TRUE(render());
        const pixelart::PixelPerfectLayout& layout = scene.getComponent<PixelPerfectCameraComponent>(camera.getEntity()).layout;
        const Image image = readImage();
        const Rect v = layout.viewport;
        const int top = image.height - (int)(v.getY() + v.getHeight());
        const std::set<int> runs = runLengths(image, (int)v.getX(), top, (int)(v.getX() + v.getWidth()), top + (int)v.getHeight());
        EXPECT_EQ(runs, std::set<int>{layout.scale});
    }

    // red control: today's fixed resolution, 640 x 360 fitted to the width
    scene.removeComponent<PixelPerfectCameraComponent>(camera.getEntity());
    sprites.clear();
    sprite("pixelart|even|checker640", pixelArtTexture(640, 360, &checkerTexel), 640, 360, 0.0f, 0.0f);
    scene.setFixedResolutionEnabled(true);
    scene.setFixedResolutionWidth(640);
    scene.setFixedResolutionHeight(360);
    scene.setFixedResolutionFilter(TextureFilter::NEAREST);
    GpuEnvironment::setView(1366, 768, 640, 360, Scaling::FITWIDTH);
    CameraComponent& component = scene.getComponent<CameraComponent>(camera.getEntity());
    component.autoResize = false;
    camera.setOrtho(0.0f, 640.0f, 0.0f, 360.0f, -10.0f, 10.0f);
    ASSERT_TRUE(render());
    const Image fitted = readImage();
    const Rect view = Engine::getViewRect();
    const int top = fitted.height - (int)(view.getY() + view.getHeight());
    const std::set<int> runs = runLengths(fitted, (int)view.getX(), top + 2, (int)(view.getX() + view.getWidth()) - 2,
                                          top + (int)view.getHeight() - 2);
    EXPECT_GT(runs.size(), 1u) << "a fractional fit should give uneven pixels";
    EXPECT_NO_ENGINE_ERRORS();
}

// AC6 (R13): a 40 x 23 floor of 8-pixel tiles (the spec's 64 x 36 is past MAX_TILEMAP_TILES)
// whose positions are 1e-4 off, under a camera swept over fractional positions (around the
// rounding ties): no background shows between tiles
TEST_F(PixelPerfectGpu, TilemapSeams){
    JASPER_REQUIRE_GPU();
    GpuEnvironment::setView(320, 180, 320, 180, Scaling::LETTERBOX);
    Tilemap floor(&scene);
    floor.setTexture("pixelart|seams|white", pixelArtTexture(8, 8, &whiteTexel));
    floor.addRect(0, "tile", Rect(0, 0, 8, 8));
    for (int y = 0; y < 23; y++)
        for (int x = 0; x < 40; x++)
            floor.addTile(0, Vector2(8.0f * x + ((x & 1) ? 1e-4f : -1e-4f), 8.0f * y + ((y & 1) ? 1e-4f : 0.0f)), 8, 8);
    PixelPerfectCamera ppc = pixelPerfect(320, 180);

    // sub-pixel camera positions, many of them where the edges round (the tie at k + 0.5 - 2^-7)
    std::vector<float> positions;
    for (int i = 0; i < 64; i++)
        positions.push_back(2.0f + (float)i / 64.0f);
    for (float d : {-2e-4f, -5e-5f, 0.0f, 5e-5f, 2e-4f})
        positions.push_back(2.5f - 0.0078125f + d);
    for (float d : {-2e-4f, -5e-5f, 0.0f, 5e-5f, 2e-4f})
        positions.push_back(2.5f + d);

    auto gaps = [&](){
        size_t holes = 0;
        for (float p : positions){
            camera.setPosition(p, p * 0.5f, 1.0f);
            if (!render())
                return SIZE_MAX;
            const Image image = readImage();
            // the floor covers the target but its last column and row (the camera moved in)
            for (int y = 8; y < image.height - 8; y++)
                for (int x = 8; x < image.width - 8; x++)
                    if (image.at(x, y) == kBlack)
                        holes++;
        }
        return holes;
    };
    EXPECT_EQ(gaps(), 0u);

    // red control: drawn without the rule (unsnapped edges 1e-4 apart leave holes at pixel centres)
    ppc.setPixelSnapping(false);
    positions.clear();
    for (float d : {-1e-4f, -5e-5f, 0.0f, 5e-5f})
        positions.push_back(2.5f + d);
    EXPECT_GT(gaps(), 0u);
    EXPECT_NO_ENGINE_ERRORS();
}

// AC7 (R10): under 90 Degree Steps a sprite at 37 degrees draws as the 0-degree one (its Transform
// keeps 37); under Rotate On Grid with the render texture it is a nearest rotation on base pixels
TEST_F(PixelPerfectGpu, Rotation){
    JASPER_REQUIRE_GPU();
    GpuEnvironment::setView(1280, 720, 320, 180, Scaling::LETTERBOX);
    PixelPerfectCamera ppc = pixelPerfect(320, 180);
    Sprite& hero = sprite("pixelart|rotation|ring", pixelArtTexture(17, 11, &ringTexel), 17, 11, 160.0f, 90.0f, PivotPreset::CENTER);

    ASSERT_TRUE(render());
    const Image straight = readImage();
    hero.setRotation(0.0f, 0.0f, 37.0f);
    ASSERT_TRUE(render());
    EXPECT_EQ(differences(readImage(), straight), 0u) << "37 degrees under 90 Degree Steps is the 0-degree step";
    const Quaternion kept = scene.getComponent<Transform>(hero.getEntity()).rotation;
    EXPECT_NEAR(kept.getRoll(), 37.0f, 1e-3f) << "the Transform keeps the authored angle";

    ppc.setRotationMode(PixelRotation::ON_GRID);
    ASSERT_TRUE(render());
    const Image rotated = readImage();
    EXPECT_GT(differences(rotated, straight), 0u);
    const pixelart::PixelPerfectLayout& layout = scene.getComponent<PixelPerfectCameraComponent>(camera.getEntity()).layout;
    const int k = layout.scale;
    // no partially covered base pixel: every k x k block is one colour
    size_t mixed = 0;
    for (int by = 0; by < layout.baseHeight; by++){
        for (int bx = 0; bx < layout.baseWidth; bx++){
            const int x0 = (int)layout.viewport.getX() + bx * k;
            const int y0 = rotated.height - (int)layout.viewport.getY() - (by + 1) * k;
            const PixelColor first = rotated.at(x0, y0);
            for (int dy = 0; dy < k; dy++)
                for (int dx = 0; dx < k; dx++)
                    if (rotated.at(x0 + dx, y0 + dy) != first)
                        mixed++;
        }
    }
    EXPECT_EQ(mixed, 0u);
    // the CPU's nearest rotation about the snapped pivot, within 8 edge pixels (GPU trigonometry)
    const float radians = 37.0f * 3.14159265f / 180.0f;
    const float c = std::cos(radians), s = std::sin(radians);
    const float pivotX = pixelart::snap(160.0f), pivotY = pixelart::snap(90.0f);
    size_t off = 0;
    for (int by = 0; by < layout.baseHeight; by++){
        for (int bx = 0; bx < layout.baseWidth; bx++){
            const float wx = (float)bx + 0.5f - pivotX, wy = (float)by + 0.5f - pivotY;
            const float lx = c * wx + s * wy + 8.5f, ly = -s * wx + c * wy + 5.5f;
            PixelColor expected = kBlack;
            if (lx >= 0.0f && ly >= 0.0f && lx < 17.0f && ly < 11.0f)
                expected = ringTexel((int)lx, (int)ly, 17, 11);
            const int x0 = (int)layout.viewport.getX() + bx * k;
            const int y0 = rotated.height - (int)layout.viewport.getY() - (by + 1) * k;
            if (rotated.at(x0, y0) != expected)
                off++;
        }
    }
    EXPECT_LE(off, 8u);
    EXPECT_NO_ENGINE_ERRORS();
}

// AC8 (R12): camera at x = 10.0, 10.25, 10.5, 10.75 base pixels at k = 4: the image shifts by 0, 1,
// 2, 3 screen pixels and stays the 10.0 frame shifted (render texture and direct mode); a click
// maps to the point drawn under it
TEST_F(PixelPerfectGpu, Smoothing){
    JASPER_REQUIRE_GPU();
    GpuEnvironment::setView(1280, 720, 320, 180, Scaling::LETTERBOX);
    sprite("pixelart|smoothing|checker", pixelArtTexture(400, 200, &checkerTexel), 400, 200, -40.0f, -10.0f);
    PixelPerfectCamera ppc = pixelPerfect(320, 180);
    ppc.setSubpixelSmoothing(true);
    for (bool texture : {true, false}){
        SCOPED_TRACE(texture ? "render texture" : "direct");
        ppc.setUpscaleRenderTexture(texture);
        camera.setPosition(10.0f, 0.0f, 1.0f);
        ASSERT_TRUE(render());
        const Image reference = readImage();
        for (int step = 1; step <= 3; step++){
            camera.setPosition(10.0f + 0.25f * (float)step, 0.0f, 1.0f);
            ASSERT_TRUE(render());
            const Image shifted = readImage();
            // the content moved left by step screen pixels: shifted(x) == reference(x + step)
            EXPECT_EQ(shiftedDifferences(shifted, reference, -step, 8, shifted.width - 8, 8, shifted.height - 8), 0u) << step;
            EXPECT_GT(shiftedDifferences(shifted, reference, 0, 8, shifted.width - 8, 8, shifted.height - 8), 0u) << step;
            if (texture && step == 1){
                // input: the canvas point under a screen pixel is the one drawn there
                const Rect viewport = Engine::getViewRect();
                const float screenX = viewport.getX() + 100.0f;
                Engine::systemMouseMove(screenX, viewport.getY() + 100.0f, 0);
                EXPECT_NEAR(Input::getMousePosition().x, (100.0f + 1.0f) / 4.0f, 1e-3f);
            }
        }
    }
    EXPECT_NO_ENGINE_ERRORS();
}

// AC9 (R11): Stretch Fill at 1366 x 768: each base pixel's interior is flat, its seams blend only
// the two neighbours
TEST_F(PixelPerfectGpu, StretchFill){
    JASPER_REQUIRE_GPU();
    GpuEnvironment::setView(1366, 768, 320, 180, Scaling::LETTERBOX);
    sprite("pixelart|stretch|checker", pixelArtTexture(320, 180, &checkerTexel), 320, 180, 0.0f, 0.0f);
    PixelPerfectCamera ppc = pixelPerfect(320, 180);
    ppc.setUpscaleMode(PixelUpscaleMode::STRETCH_FILL);
    ASSERT_TRUE(render());
    const Image image = readImage();
    const pixelart::PixelPerfectLayout& layout = scene.getComponent<PixelPerfectCameraComponent>(camera.getEntity()).layout;
    const Rect v = layout.viewport;
    const float scaleX = v.getWidth() / 320.0f;
    size_t interiorWrong = 0, seamWrong = 0, interior = 0;
    const int y = image.height - (int)v.getY() - (int)(v.getHeight() * 0.5f);
    // base rows count from the bottom; the texture's first row is the sprite's top
    const int row = 179 - (int)std::floor(((float)(image.height - 1 - y) - v.getY()) / (v.getHeight() / 180.0f));
    for (int x = (int)v.getX() + 1; x < (int)(v.getX() + v.getWidth()) - 1; x++){
        const float left = ((float)x - v.getX()) / scaleX;
        const float right = ((float)x + 1.0f - v.getX()) / scaleX;
        const int texel = (int)std::floor(left);
        const PixelColor got = image.at(x, y);
        if ((int)std::floor(right - 1e-4f) == texel){
            interior++;
            if (got != checkerTexel(texel, row, 0, 0))
                interiorWrong++;
        }else{
            // a blend of this texel and the next: each channel between theirs
            const PixelColor a = checkerTexel(texel, row, 0, 0), b = checkerTexel(texel + 1, row, 0, 0);
            auto between = [](uint8_t value, uint8_t p, uint8_t q){ return value + 1 >= std::min(p, q) && value <= std::max(p, q) + 1; };
            if (!between(got.r, a.r, b.r) || !between(got.g, a.g, b.g) || !between(got.b, a.b, b.b))
                seamWrong++;
        }
    }
    EXPECT_GT(interior, 1000u);
    EXPECT_EQ(interiorWrong, 0u);
    EXPECT_EQ(seamWrong, 0u);
    EXPECT_NO_ENGINE_ERRORS();
}

// AC10 (R4, R5): the component drives the fixed-resolution target (its colour pass at 320 x 180 over
// the scene's 640 x 360, which comes back when it goes); a layer scene's Pixel Perfect Camera draws
// on the main scene's base grid
TEST_F(PixelPerfectGpu, FixedResolutionDriven){
    JASPER_REQUIRE_GPU();
    GpuEnvironment::setView(1280, 720, 320, 180, Scaling::LETTERBOX);
    scene.setFixedResolutionEnabled(true);
    scene.setFixedResolutionWidth(640);
    scene.setFixedResolutionHeight(360);
    sprite("pixelart|driven|white", pixelArtTexture(1, 1, &whiteTexel), 4, 4, 10.0f, 10.0f);
    auto cameraPassSize = [](int& width, int& height){
        // a reader asks for the passes' details: the next frames record them
        SystemRender::getPassList(SystemRender::FRAME_STATS_FRAME, false);
        GpuEnvironment::drawFrames(2);
        const PassList passes = SystemRender::getPassList(SystemRender::FRAME_STATS_FRAME, false);
        for (const PassRecord& pass : passes.passes){
            if (std::string(pass.label) == "camera"){
                width = pass.width;
                height = pass.height;
                return true;
            }
        }
        return false;
    };
    {
        PixelPerfectCamera ppc = pixelPerfect(320, 180);
        ASSERT_TRUE(render());
        int width = 0, height = 0;
        ASSERT_TRUE(cameraPassSize(width, height));
        EXPECT_EQ(width, 320);
        EXPECT_EQ(height, 180);
        EXPECT_EQ(scene.getFixedResolutionWidth(), 640u) << "the scene's setting is kept";
    }
    scene.removeComponent<PixelPerfectCameraComponent>(camera.getEntity());
    ASSERT_TRUE(render());
    int width = 0, height = 0;
    ASSERT_TRUE(cameraPassSize(width, height));
    EXPECT_EQ(width, 640);
    EXPECT_EQ(height, 360);

    // a HUD layer with its own Pixel Perfect Camera: a one-pixel column at a half pixel lands on
    // the main scene's grid, k screen pixels wide, at the snapped base pixel
    scene.setFixedResolutionEnabled(false);
    PixelPerfectCamera mainPixelPerfect = pixelPerfect(320, 180);
    Scene hud;
    Camera hudCamera(&hud);
    hudCamera.setType(CameraType::CAMERA_ORTHO);
    hud.getComponent<CameraComponent>(hudCamera.getEntity()).useTarget = false;
    hudCamera.setPosition(0.0f, 0.0f, 1.0f);
    hudCamera.activate();
    PixelPerfectCamera hudPixelPerfect(&hud, hudCamera.getEntity());
    hudPixelPerfect.setReferenceResolution(Vector2(320.0f, 180.0f));
    Sprite column(&hud);
    column.setTexture("pixelart|driven|white", pixelArtTexture(1, 1, &whiteTexel));
    column.setSize(1, 180);
    column.setPosition(40.5f, 0.0f, 0.0f);
    Engine::addSceneLayer(&hud);
    Scene* layers[2] = {&scene, &hud};
    ASSERT_TRUE(GpuEnvironment::runFrames([&layers](){
        return layers[0]->getSystem<RenderSystem>()->isAllLoaded() && layers[1]->getSystem<RenderSystem>()->isAllLoaded();
    }));
    const Image image = readImage();
    Engine::removeScene(&hud);
    const Rect viewport = Engine::getViewRect();
    const int k = Engine::getPixelPerfectLayout().scale;
    ASSERT_EQ(k, 4);
    const int y = image.height / 2;
    std::vector<int> lit;
    for (int x = 0; x < image.width; x++)
        if (image.at(x, y) == kWhite)
            lit.push_back(x);
    ASSERT_EQ(lit.size(), (size_t)k);
    EXPECT_EQ(lit.front(), (int)viewport.getX() + (int)pixelart::snap(40.5f) * k);
    EXPECT_NO_ENGINE_ERRORS();
}

namespace {
    // the benchmark of R18 (the spec's pixel_perfect_bench): its fixture is the suite's
    class PixelPerfectBenchGpu : public PixelPerfectGpu {};

    // the least wall time of a few frames (the least is the frame without a neighbour's load)
    double leastFrameMs(int frames){
        double least = 1e30;
        for (int i = 0; i < frames; i++){
            const auto start = std::chrono::steady_clock::now();
            Engine::systemDraw();
            finishGpu();
            least = std::min(least, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }
        return least;
    }
}

// R18: 10,000 sprites at 320 x 180 through the render texture: the frame with snapping costs at
// most 102 % of the frame without it (enforced on the benchmark server, JASPER_TEST_PERF_SLACK;
// printed elsewhere)
TEST_F(PixelPerfectBenchGpu, SnappingFrameCost){
    JASPER_REQUIRE_GPU();
    GpuEnvironment::setView(1280, 720, 320, 180, Scaling::LETTERBOX);
    const TextureData white = pixelArtTexture(1, 1, &whiteTexel);
    std::mt19937 random(18);
    std::uniform_real_distribution<float> x(0.0f, 316.0f), y(0.0f, 176.0f);
    for (int i = 0; i < 10000; i++)
        sprite("pixelart|bench|white", white, 4, 4, x(random), y(random));
    PixelPerfectCamera ppc = pixelPerfect(320, 180);
    ppc.setPixelSnapping(false);
    ASSERT_TRUE(render());
    const double off = leastFrameMs(10);
    ppc.setPixelSnapping(true);
    ASSERT_TRUE(render());
    const double on = leastFrameMs(10);
    std::printf("[bench] S41 pixel_perfect_bench: 10000 sprites, frame %.3f ms snapping off, %.3f ms on (%.1f %%)\n", off, on,
                100.0 * on / off);
    const double slack = testsupport::perfSlack();
    if (slack > 0.0){
        EXPECT_LE(on, off * 1.02 * slack);
    }
}
