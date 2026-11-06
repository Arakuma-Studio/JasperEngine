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
#include "component/UIComponent.h"
#include "component/PixelPerfectCameraComponent.h"
#include "component/TilemapComponent.h"
#include "component/Transform.h"
#include "object/Camera.h"
#include "object/PixelPerfectCamera.h"
#include "object/Sprite.h"
#include "object/ParticleSystem.h"
#include "object/Tilemap.h"
#include "object/ui/Image.h"
#include "pixelart/PixelSnap.h"
#include "pool/ShaderPool.h"
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

    // pixel art is sampled nearest (a sprite's own setting; with a texel-aligned quad linear
    // sampling reads the same texels, a rotated one blends them)
    void useNearestTexels(Mesh& mesh){
        Material material = mesh.getMaterial();
        material.baseColorTexture.setMinFilter(TextureFilter::NEAREST);
        material.baseColorTexture.setMagFilter(TextureFilter::NEAREST);
        mesh.setMaterial(material);
    }

    // a tileset from memory: a tilemap's tilesets are its submeshes' textures (addRect with a
    // path adds one, as a scene lists them), and a texture set from memory goes on submesh 0
    void tilesetFromMemory(Tilemap& tilemap, Scene& owner, const std::string& id, TextureData data){
        tilemap.setTexture(id, data);
        MeshComponent& mesh = owner.getComponent<MeshComponent>(tilemap.getEntity());
        mesh.numSubmeshes = std::max(mesh.numSubmeshes, 1u);
        useNearestTexels(tilemap);
    }

    PixelColor whiteTexel(int, int, int, int){ return kWhite; }
    PixelColor checkerTexel(int x, int y, int, int){ return ((x + y) & 1) ? kBlue : kRed; }
    // a ring of one texel around a fill: the same under any flip or quarter turn
    PixelColor ringTexel(int x, int y, int width, int height){
        return (x == 0 || y == 0 || x == width - 1 || y == height - 1) ? kGreen : kRed;
    }

    struct Shot {
        int width = 0, height = 0;
        std::vector<uint8_t> rgba;     // rows top first
        PixelColor at(int x, int y) const{
            const uint8_t* p = &rgba[((size_t)y * width + x) * 4];
            return PixelColor{p[0], p[1], p[2], p[3]};
        }
    };

    Shot readImage(){
        Shot image;
        Framebuffer& target = GpuEnvironment::target();
        image.width = (int)target.getWidth();
        image.height = (int)target.getHeight();
        readTexture(target.getRender().getColorTexture(), image.width, image.height, image.rgba);
        return image;
    }

    // the same image moved by dy rows (positive: the content down the screen), compared where both
    // have pixels
    size_t shiftedRowDifferences(const Shot& image, const Shot& reference, int dy, int x0, int x1, int y0, int y1){
        size_t differences = 0;
        for (int y = y0; y < y1; y++){
            const int sy = y - dy;
            if (sy < y0 || sy >= y1)
                continue;
            for (int x = x0; x < x1; x++){
                if (image.at(x, y) != reference.at(x, sy))
                    differences++;
            }
        }
        return differences;
    }

    // the same image moved by dx pixels (positive: the content to the right), compared where
    // both have pixels
    size_t shiftedDifferences(const Shot& image, const Shot& reference, int dx, int x0, int x1, int y0, int y1){
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

    // the GPU environment's view as the suite found it: the suites after it draw at their own sizes
    struct PixelPerfectGpuView {
        bool saved = false;
        int screenWidth = 0, screenHeight = 0, canvasWidth = 0, canvasHeight = 0;
        Scaling scaling = Scaling::FITWIDTH;
        void save(){
            if (!GpuEnvironment::ensure()) return;
            screenWidth = (int)GpuEnvironment::target().getWidth();
            screenHeight = (int)GpuEnvironment::target().getHeight();
            canvasWidth = Engine::getPreferredCanvasWidth();
            canvasHeight = Engine::getPreferredCanvasHeight();
            scaling = Engine::getScalingMode();
            saved = true;
        }
        void restore() const{
            if (saved) GpuEnvironment::setView(screenWidth, screenHeight, canvasWidth, canvasHeight, scaling);
        }
    };

    class PixelPerfectGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        PixelPerfectGpuView view;

        void SetUp() override{
            view.save();
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
            view.restore();
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
                    if (tilemaps->getComponentFromIndex(i).needUpdateTilemap)
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
            useNearestTexels(made);
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
    std::set<int> runLengths(const Shot& image, int x0, int y0, int x1, int y1){
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

    Shot composeReference(const std::vector<ReferenceSprite>& drawn, const Matrix4& viewProjection, int baseWidth, int baseHeight,
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
        Shot image;
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

    size_t differences(const Shot& a, const Shot& b){
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

// AC3 (R7, R9, §6.2): 4,096 one-pixel quads, a third at seeded random positions, a third on the
// half-pixel lattice and a third just under it (inside the rule's 2^-7 bias), each in a cell of
// its own: the pixel each lights is the CPU twin's, directly at k = 1 and through the render
// texture at k = 2. Red controls: the vertex stage rounding without the bias, and no snap at all.
TEST_F(PixelPerfectGpu, SnapParity){
    JASPER_REQUIRE_GPU();
    PixelPerfectCamera ppc = pixelPerfect(512, 512);

    enum class Kind { RANDOM, LATTICE, UNDER_HALF };
    struct Quad { float x, y; Kind kind; int cellX, cellY; };
    std::vector<Quad> quads;
    std::mt19937 random(41);
    std::uniform_real_distribution<float> offset(0.0f, 3.0f);
    for (int cy = 0; cy < 64; cy++){
        for (int cx = 0; cx < 64; cx++){
            Quad q;
            q.cellX = cx;
            q.cellY = cy;
            q.kind = (Kind)((cx + 2 * cy) % 3);
            float fx = offset(random), fy = offset(random);
            if (q.kind == Kind::LATTICE){
                fx = 0.5f + (float)(cx % 3);
                fy = 0.5f + (float)(cy % 3);
            }else if (q.kind == Kind::UNDER_HALF){
                // k + 0.5 - 2^-8: the biased rule rounds up, floor(p + 0.5) down
                fx = 0.5f - 0.00390625f + (float)(cx % 3);
                fy = 0.5f - 0.00390625f + (float)(cy % 3);
            }
            q.x = (float)(cx * 8 + 2) + fx;
            q.y = (float)(cy * 8 + 2) + fy;
            quads.push_back(q);
        }
    }
    const TextureData white = pixelArtTexture(1, 1, &whiteTexel);
    for (const Quad& q : quads)
        sprite("pixelart|parity|white", white, 1, 1, q.x, q.y);

    // the base pixel a quad lit in its cell (the k x k block's bottom-left screen pixel)
    auto lit = [&](const Shot& image, const Quad& q, int k, int& x, int& y){
        int count = 0;
        for (int dy = 0; dy < 8; dy++){
            for (int dx = 0; dx < 8; dx++){
                const int bx = q.cellX * 8 + dx;
                const int by = q.cellY * 8 + dy;
                if (image.at(bx * k, image.height - 1 - by * k) == kWhite){
                    x = bx;
                    y = by;
                    count++;
                }
            }
        }
        return count;
    };
    auto mismatches = [&](const Shot& image, int k, Kind only, bool every){
        const Matrix4 viewProjection = scene.getComponent<CameraComponent>(camera.getEntity()).viewProjectionMatrix;
        size_t count = 0;
        for (const Quad& q : quads){
            if (!every && q.kind != only)
                continue;
            int px, py, gx = -1, gy = -1;
            predictPixel(viewProjection, Matrix4::translateMatrix(q.x, q.y, 0.0f), Vector3(0, 0, 0), 512.0f, 512.0f, px, py);
            if (lit(image, q, k, gx, gy) != 1 || gx != px || gy != py)
                count++;
        }
        return count;
    };

    struct Path { int screen; bool texture; int k; };
    for (const Path& path : {Path{512, false, 1}, Path{512, true, 1}, Path{1024, true, 2}}){
        SCOPED_TRACE(::testing::Message() << "k " << path.k << (path.texture ? ", render texture" : ", direct"));
        GpuEnvironment::setView(path.screen, path.screen, 512, 512, Scaling::LETTERBOX);
        ppc.setUpscaleRenderTexture(path.texture);
        ASSERT_TRUE(render());
        ASSERT_EQ(scene.getComponent<PixelPerfectCameraComponent>(camera.getEntity()).layout.scale, path.k);
        EXPECT_EQ(mismatches(readImage(), path.k, Kind::RANDOM, true), 0u) << "quads off the CPU twin's pixel";
    }

    // D4's measure: the quads the rule without its bias rounds otherwise (the CPU), and the GPU
    // rounding without it (red control: the quads under the half then miss the CPU twin's pixel)
    size_t unbiased = 0;
    for (const Quad& q : quads){
        if (std::floor(q.x + 0.5f) != pixelart::snap(q.x))
            unbiased++;
    }
    std::printf("[S41] quads the unbiased rule rounds otherwise (CPU): %zu of %zu\n", unbiased, quads.size());
    EXPECT_GT(unbiased, 0u);
    GpuEnvironment::setView(512, 512, 512, 512, Scaling::LETTERBOX);
    ppc.setUpscaleRenderTexture(true);
    RenderSystem::setPixelSnapBias(0.0f);
    ASSERT_TRUE(render());
    const size_t unbiasedGpu = mismatches(readImage(), 1, Kind::UNDER_HALF, false);
    RenderSystem::setPixelSnapBias(pixelart::kSnapBias);
    EXPECT_GT(unbiasedGpu, 0u) << "without the bias the GPU rounds the quads under the half down";
    ASSERT_TRUE(render());
    EXPECT_EQ(mismatches(readImage(), 1, Kind::RANDOM, true), 0u) << "the bias restored";

    // red control: without snapping the lattice quads land elsewhere
    ppc.setPixelSnapping(false);
    ASSERT_TRUE(render());
    const size_t latticeMisses = mismatches(readImage(), 1, Kind::LATTICE, false);
    size_t latticeCount = 0;
    for (const Quad& q : quads)
        latticeCount += q.kind == Kind::LATTICE ? 1 : 0;
    EXPECT_GT((double)latticeMisses, 0.4 * (double)latticeCount) << "the snap did nothing";
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
    tilesetFromMemory(floor, scene, "pixelart|reference|tile", tile);
    floor.addRect(0, "tile", Rect(0, 0, 16, 16));
    for (int i = 0; i < 10; i++)
        floor.addTile(0, Vector2(16.0f * i, 0.0f), 16, 16);
    floor.setPosition(20.5f, 10.5f, 0.0f);

    PixelPerfectCamera ppc = pixelPerfect(320, 180);
    // the layouts, by hand (R1): k = floor(min(W / 320, H / 180)), the viewport centred on whole
    // pixels; Integer (Expand) grows the base to floor(W / k) x floor(H / k)
    struct Case { int screenWidth, screenHeight; PixelUpscaleMode mode; int k, baseWidth, baseHeight, viewportX, viewportY; };
    const Case cases[] = {{320, 180, PixelUpscaleMode::INTEGER, 1, 320, 180, 0, 0},
                          {960, 540, PixelUpscaleMode::INTEGER, 3, 320, 180, 0, 0},
                          {1000, 600, PixelUpscaleMode::INTEGER, 3, 320, 180, 20, 30},
                          {1280, 720, PixelUpscaleMode::INTEGER, 4, 320, 180, 0, 0},
                          {1366, 768, PixelUpscaleMode::INTEGER_EXPAND, 4, 341, 192, 1, 0}};
    for (const Case& c : cases){
        SCOPED_TRACE(::testing::Message() << c.screenWidth << "x" << c.screenHeight);
        GpuEnvironment::setView(c.screenWidth, c.screenHeight, 320, 180, Scaling::LETTERBOX);
        ppc.setUpscaleMode(c.mode);
        ppc.setPixelSnapping(true);
        ASSERT_TRUE(render());
        const Shot gpu = readImage();
        const pixelart::PixelPerfectLayout& layout = scene.getComponent<PixelPerfectCameraComponent>(camera.getEntity()).layout;
        EXPECT_EQ(layout.scale, c.k);
        EXPECT_EQ(layout.baseWidth, c.baseWidth);
        EXPECT_EQ(layout.baseHeight, c.baseHeight);
        EXPECT_EQ((int)layout.viewport.getX(), c.viewportX);
        EXPECT_EQ((int)layout.viewport.getY(), c.viewportY);
        const Matrix4 viewProjection = scene.getComponent<CameraComponent>(camera.getEntity()).viewProjectionMatrix;

        std::vector<ReferenceSprite> drawn;
        auto modelOf = [this](Entity entity){ return scene.getComponent<Transform>(entity).modelMatrix; };
        drawn.push_back({15, 16, modelOf(sprites[0]->getEntity()), Vector2(7.5f, 8.0f)});
        drawn.push_back({15, 7, modelOf(turned.getEntity()), Vector2(0.0f, 0.0f)});
        drawn.push_back({15, 7, modelOf(flipped.getEntity()), Vector2(0.0f, 0.0f)});
        for (int i = 0; i < 10; i++)
            drawn.push_back({16, 16, modelOf(floor.getEntity()) * Matrix4::translateMatrix(16.0f * i, 0.0f, 0.0f), Vector2(0.0f, 0.0f)});
        // the expected image from the hand layout, not the engine's
        const Rect viewport((float)c.viewportX, (float)c.viewportY, (float)(c.baseWidth * c.k), (float)(c.baseHeight * c.k));
        const Shot reference = composeReference(drawn, viewProjection, c.baseWidth, c.baseHeight, c.screenWidth,
                                                 c.screenHeight, viewport, c.k);
        EXPECT_EQ(differences(gpu, reference), 0u);

        // red control: unsnapped, the hero (a fractional y) and the flipped sprite (a fractional
        // x) cover whole base pixels other than the reference's
        ppc.setPixelSnapping(false);
        ASSERT_TRUE(render());
        const Shot loose = readImage();
        size_t blocks = 0;
        for (int by = 0; by < c.baseHeight; by++){
            for (int bx = 0; bx < c.baseWidth; bx++){
                const int sx = c.viewportX + bx * c.k + c.k / 2;
                const int sy = c.screenHeight - 1 - (c.viewportY + by * c.k + c.k / 2);
                if (loose.at(sx, sy) != reference.at(sx, sy))
                    blocks++;
            }
        }
        EXPECT_GT(blocks, 0u) << "snapping off draws the same base pixels";
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
        const Shot image = readImage();
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
    const Shot fitted = readImage();
    const Rect view = Engine::getViewRect();
    const int top = fitted.height - (int)(view.getY() + view.getHeight());
    const std::set<int> runs = runLengths(fitted, (int)view.getX(), top + 2, (int)(view.getX() + view.getWidth()) - 2,
                                          top + (int)view.getHeight() - 2);
    EXPECT_GT(runs.size(), 1u) << "a fractional fit should give uneven pixels";
    EXPECT_NO_ENGINE_ERRORS();
}

// AC6 (R13): a 40 x 23 floor of 8-pixel tiles (the spec's 64 x 36 is past MAX_TILEMAP_TILES)
// whose positions are 1e-4 off (and 1e-2: a gap the rasterizer's sub-pixel grid keeps), under a
// camera swept over fractional positions (around the rounding ties): no background shows between
// tiles
TEST_F(PixelPerfectGpu, TilemapSeams){
    JASPER_REQUIRE_GPU();
    GpuEnvironment::setView(320, 180, 320, 180, Scaling::LETTERBOX);
    Tilemap floor(&scene);
    tilesetFromMemory(floor, scene, "pixelart|seams|white", pixelArtTexture(8, 8, &whiteTexel));
    floor.addRect(0, "tile", Rect(0, 0, 8, 8));
    // odd columns and rows pushed by +offset, even columns by -offset: gaps of up to 2 x offset
    auto lay = [&floor](float offset){
        floor.clearTiles();
        for (int y = 0; y < 23; y++)
            for (int x = 0; x < 40; x++)
                floor.addTile(0, Vector2(8.0f * x + ((x & 1) ? offset : -offset), 8.0f * y + ((y & 1) ? offset : 0.0f)), 8, 8);
    };
    lay(1e-4f);
    // half a pixel over: every tile edge sits on a pixel centre of the (snapped) camera's grid
    floor.setPosition(0.5f, 0.5f, 0.0f);
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
            const Shot image = readImage();
            // the floor covers the target but its last column and row (the camera moved in)
            for (int y = 8; y < image.height - 8; y++)
                for (int x = 8; x < image.width - 8; x++)
                    if (image.at(x, y) == kBlack)
                        holes++;
        }
        return holes;
    };
    EXPECT_EQ(gaps(), 0u) << "1e-4 off";
    lay(1e-2f);
    EXPECT_EQ(gaps(), 0u) << "1e-2 off";

    // red control: drawn without the rule (no edge rounding, no vertex snap; the camera still snaps,
    // R8) the 0.02-pixel gaps straddle pixel centres. A gap of 2e-4 pixels never shows: the
    // rasterizer puts vertices on a 1/256-pixel grid.
    ppc.setPixelSnapping(false);
    ASSERT_TRUE(render());
    ASSERT_TRUE(render());
    positions.resize(64);
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
    const Shot straight = readImage();
    hero.setRotation(0.0f, 0.0f, 37.0f);
    ASSERT_TRUE(render());
    EXPECT_EQ(differences(readImage(), straight), 0u) << "37 degrees under 90 Degree Steps is the 0-degree step";
    const Quaternion kept = scene.getComponent<Transform>(hero.getEntity()).rotation;
    EXPECT_NEAR(kept.getRoll(), 37.0f, 1e-3f) << "the Transform keeps the authored angle";

    ppc.setRotationMode(PixelRotation::ON_GRID);
    ASSERT_TRUE(render());
    const Shot rotated = readImage();
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
    std::string offPixels;
    for (int by = 0; by < layout.baseHeight; by++){
        for (int bx = 0; bx < layout.baseWidth; bx++){
            const float wx = (float)bx + 0.5f - pivotX, wy = (float)by + 0.5f - pivotY;
            const float lx = c * wx + s * wy + 8.5f, ly = -s * wx + c * wy + 5.5f;
            PixelColor expected = kBlack;
            if (lx >= 0.0f && ly >= 0.0f && lx < 17.0f && ly < 11.0f)
                expected = ringTexel((int)lx, (int)ly, 17, 11);
            const int x0 = (int)layout.viewport.getX() + bx * k;
            const int y0 = rotated.height - (int)layout.viewport.getY() - (by + 1) * k;
            const PixelColor got = rotated.at(x0, y0);
            if (got != expected && off++ < 64)
                offPixels += " (" + std::to_string(bx) + "," + std::to_string(by) + (got == kGreen ? " green" : got == kRed ? " red" : " other") +
                             (expected == kGreen ? "/green)" : expected == kRed ? "/red)" : "/black)");
        }
    }
    EXPECT_LE(off, 8u) << "base pixels (x, y drawn/expected):" << offPixels;
    EXPECT_NO_ENGINE_ERRORS();
}

// AC8 (R12): camera at x = 10.0, 10.25, 10.5, 10.75 base pixels at k = 4: the image shifts by 0, 1,
// 2, 3 screen pixels left and stays the 10.0 frame shifted (render texture and direct mode); the
// same up the y axis moves it down the screen; a click maps to the point drawn under it
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
        const Shot reference = readImage();
        for (int step = 1; step <= 3; step++){
            camera.setPosition(10.0f + 0.25f * (float)step, 0.0f, 1.0f);
            ASSERT_TRUE(render());
            const Shot shifted = readImage();
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
        for (int step = 1; step <= 3; step++){
            camera.setPosition(10.0f, 0.25f * (float)step, 1.0f);
            ASSERT_TRUE(render());
            const Shot shifted = readImage();
            // the camera went up: the content moved down by step screen rows (rows top first)
            EXPECT_EQ(shiftedRowDifferences(shifted, reference, step, 8, shifted.width - 8, 8, shifted.height - 8), 0u) << step;
            EXPECT_GT(shiftedRowDifferences(shifted, reference, -step, 8, shifted.width - 8, 8, shifted.height - 8), 0u) << step;
            if (texture && step == 1){
                // a canvas row is counted from the top: the point under a screen row is one above
                const Rect viewport = Engine::getViewRect();
                Engine::systemMouseMove(viewport.getX() + 100.0f, viewport.getY() + 100.0f, 0);
                EXPECT_NEAR(Input::getMousePosition().y, (100.0f - 1.0f) / 4.0f, 1e-3f);
            }
        }
    }
    EXPECT_NO_ENGINE_ERRORS();
}

// AC4 (deviation 8), R9: 1-pixel UI Images at seeded random and half-pixel positions under a UI
// camera's Pixel Perfect Camera light the pixel the CPU twin puts their corner on, at k = 1 and 3
// (the UI 'Pxs' path); unsnapped, the lattice ones land elsewhere
TEST_F(PixelPerfectGpu, UiImageParity){
    JASPER_REQUIRE_GPU();
    camera.setType(CameraType::CAMERA_UI);
    PixelPerfectCamera ppc = pixelPerfect(320, 180);
    struct Cell { float x, y; bool lattice; int cellX, cellY; };
    std::vector<Cell> cells;
    std::vector<std::unique_ptr<jasperengine::Image>> images;
    std::mt19937 random(4141);
    std::uniform_real_distribution<float> offset(0.0f, 3.0f);
    const TextureData white = pixelArtTexture(1, 1, &whiteTexel);
    for (int cy = 0; cy < 22; cy++){
        for (int cx = 0; cx < 40; cx++){
            Cell c{0, 0, ((cx + cy) & 1) == 0, cx, cy};
            const float fx = c.lattice ? 0.5f + (float)(cx % 3) : offset(random);
            const float fy = c.lattice ? 0.5f + (float)(cy % 3) : offset(random);
            c.x = (float)(cx * 8 + 2) + fx;
            c.y = (float)(cy * 8 + 2) + fy;
            images.push_back(std::make_unique<jasperengine::Image>(&scene));
            images.back()->setTexture("pixelart|ui-parity|white", white);
            images.back()->setSize(1, 1);
            images.back()->setPosition(c.x, c.y, 0.0f);
            cells.push_back(c);
        }
    }
    // the base pixel a cell's image lit (base pixels y up), and the one the rule puts it on: the
    // corner of the quad nearest the grid origin, through the draw's own MVP
    auto misses = [&](const Shot& image, int k, bool latticeOnly){
        const Matrix4 viewProjection = scene.getComponent<CameraComponent>(camera.getEntity()).viewProjectionMatrix;
        const pixelart::PixelPerfectLayout& layout = scene.getComponent<PixelPerfectCameraComponent>(camera.getEntity()).layout;
        size_t count = 0;
        for (size_t i = 0; i < cells.size(); i++){
            const Cell& c = cells[i];
            if (latticeOnly && !c.lattice)
                continue;
            const Matrix4 model = scene.getComponent<Transform>(images[i]->getEntity()).modelMatrix;
            int ax, ay, bx, by;
            predictPixel(viewProjection, model, Vector3(0, 0, 0), (float)layout.baseWidth, (float)layout.baseHeight, ax, ay);
            predictPixel(viewProjection, model, Vector3(1, 1, 0), (float)layout.baseWidth, (float)layout.baseHeight, bx, by);
            const int px = std::min(ax, bx), py = std::min(ay, by);
            int lit = 0;
            bool at = false;
            for (int y = 0; y < layout.baseHeight; y++){
                for (int x = 0; x < layout.baseWidth; x++){
                    // only this cell's pixels (the rows of a UI camera's cell count down from the top)
                    const int screenX = (int)layout.viewport.getX() + x * k;
                    const int screenY = image.height - 1 - ((int)layout.viewport.getY() + y * k);
                    if (std::abs(x - px) > 4 || std::abs(y - py) > 4)
                        continue;
                    if (image.at(screenX, screenY) == kWhite){
                        lit++;
                        at = at || (x == px && y == py);
                    }
                }
            }
            if (lit != 1 || !at)
                count++;
        }
        return count;
    };
    for (int k : {1, 3}){
        SCOPED_TRACE(::testing::Message() << "k " << k);
        GpuEnvironment::setView(320 * k, 180 * k, 320, 180, Scaling::LETTERBOX);
        ppc.setPixelSnapping(true);
        ASSERT_TRUE(render());
        ASSERT_EQ(scene.getComponent<PixelPerfectCameraComponent>(camera.getEntity()).layout.scale, k);
        auto uis = scene.getComponentArray<UIComponent>();
        ASSERT_GT(uis->size(), 0u);
        EXPECT_NE(uis->getComponentFromIndex(0).shaderProperties & ShaderPool::getPixelSnapProperty(ShaderType::UI), 0u)
            << "the images draw with the UI 'Pxs' variant";
        EXPECT_EQ(misses(readImage(), k, false), 0u) << "images off the CPU twin's pixel";
        ppc.setPixelSnapping(false);
        ASSERT_TRUE(render());
        size_t lattice = 0;
        for (const Cell& c : cells)
            lattice += c.lattice ? 1 : 0;
        EXPECT_GT((double)misses(readImage(), k, true), 0.4 * (double)lattice) << "the snap did nothing";
    }
    EXPECT_NO_ENGINE_ERRORS();
}

// R9 (particles): 2-pixel square particles at fractional positions centre on the grid: each
// lights the 2 x 2 block around its snapped centre (the PARTICLES 'Pxs' path); unsnapped, the
// centres at a quarter pixel cover other blocks
TEST_F(PixelPerfectGpu, ParticleCentres){
    JASPER_REQUIRE_GPU();
    GpuEnvironment::setView(320, 180, 320, 180, Scaling::LETTERBOX);
    PixelPerfectCamera ppc = pixelPerfect(320, 180);
    const char* const effect = R"({"version": 1, "duration": 1, "looping": false, "maxParticles": 1, "modules": [
        {"type": "main", "startLifetime": 1000, "startSpeed": 0, "startSize": 2, "startColor": [1, 1, 1, 1]},
        {"type": "emission", "rateOverTime": 0, "bursts": [{"time": 0, "count": 1}]},
        {"type": "renderer", "untexturedShape": "square"}]})";
    std::vector<std::unique_ptr<ParticleSystem>> systems;
    std::vector<Vector2> centres;
    for (int i = 0; i < 24; i++){
        const Vector2 at(20.0f + 12.0f * (float)(i % 12) + 0.25f * (float)(i % 4), 40.0f + 40.0f * (float)(i / 12) + 0.25f * (float)((i / 4) % 4));
        systems.push_back(std::make_unique<ParticleSystem>(&scene));
        ASSERT_TRUE(systems.back()->setInlineEffect(effect));
        systems.back()->setPosition(at.x, at.y, 0.0f);
        centres.push_back(at);
    }
    auto misses = [&](const Shot& image){
        size_t count = 0;
        for (const Vector2& c : centres){
            // the snapped centre is a pixel corner: the block is the two pixels on each side
            const int cx = (int)pixelart::snap(c.x), cy = (int)pixelart::snap(c.y);
            for (int y = cy - 3; y < cy + 3; y++){
                for (int x = cx - 3; x < cx + 3; x++){
                    const bool inside = x >= cx - 1 && x < cx + 1 && y >= cy - 1 && y < cy + 1;
                    const bool lit = image.at(x, image.height - 1 - y) == kWhite;
                    if (lit != inside){
                        count++;
                        y = cy + 3;
                        break;
                    }
                }
            }
        }
        return count;
    };
    ASSERT_TRUE(render());
    GpuEnvironment::drawFrames(4);
    EXPECT_EQ(misses(readImage()), 0u) << "particles off their snapped centres";
    ppc.setPixelSnapping(false);
    ASSERT_TRUE(render());
    GpuEnvironment::drawFrames(4);
    EXPECT_GT(misses(readImage()), 0u) << "the snap did nothing";
    EXPECT_NO_ENGINE_ERRORS();
}

// R9 (the depth program): with the scene's depth pre-pass, the sprites' depth programs ('Pxs'
// DEPTH) put every fragment where their colour pass does: the image equals the one without the
// pre-pass pixel for pixel (a depth program off the colour's grid fails the equal test at edges)
TEST_F(PixelPerfectGpu, DepthProgramSnapsWithTheColour){
    JASPER_REQUIRE_GPU();
    GpuEnvironment::setView(960, 540, 320, 180, Scaling::LETTERBOX);
    PixelPerfectCamera ppc = pixelPerfect(320, 180);
    ppc.setUpscaleRenderTexture(false);
    std::mt19937 random(19);
    std::uniform_real_distribution<float> place(8.0f, 290.0f);
    const TextureData ring = pixelArtTexture(9, 7, &ringTexel);
    for (int i = 0; i < 48; i++)
        sprite("pixelart|depth|ring", ring, 9, 7, place(random), place(random) * 0.55f);
    ASSERT_TRUE(render());
    const Shot without = readImage();
    scene.setDepthPrepassEnabled(true);
    ASSERT_TRUE(render());
    const MeshComponent& mesh = scene.getComponent<MeshComponent>(sprites[0]->getEntity());
    ASSERT_GT(mesh.numSubmeshes, 0u);
    EXPECT_NE(mesh.submeshes[0].depthShaderProperties & ShaderPool::getPixelSnapProperty(ShaderType::DEPTH), 0u)
        << "the sprites' depth programs are the 'Pxs' variant";
    EXPECT_EQ(differences(readImage(), without), 0u) << "the pre-pass changed pixels";
    scene.setDepthPrepassEnabled(false);
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
    const Shot image = readImage();
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
    const Shot image = readImage();
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
