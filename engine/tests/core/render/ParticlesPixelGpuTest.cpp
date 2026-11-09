// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S49's GPU acceptance (docs/specs/S49-pixel-particles.md AC2, AC3, AC6, AC8): pixel particles drawn
// on the headless device (glcore, gles3, Vulkan on lavapipe) by an orthographic camera one world
// unit a pixel (PPU 1, no Pixel Perfect Camera: the orthographic grid of R2), read back and
// compared with the CPU twins: the shape rules (particles/ParticlePixel), the Bayer table
// (pixelart::bayerThreshold) and the Analytic program (particles/AnalyticParticles). Each check has
// its red control in the same test where one exists.

#include "Engine.h"
#include "Scene.h"
#include "component/CameraComponent.h"
#include "component/ParticleSystemComponent.h"
#include "object/Camera.h"
#include "object/ParticleSystem.h"
#include "particles/AnalyticParticles.h"
#include "particles/ParticleEffectCodec.h"
#include "particles/ParticleEffectLibrary.h"
#include "particles/ParticlePixel.h"
#include "pixelart/PixelColor.h"
#include "pixelart/PixelSnap.h"
#include "subsystem/ParticleSubsystem.h"
#include "subsystem/ParticleSystemState.h"
#include "subsystem/RenderSystem.h"

#include "GpuEnvironment.h"
#include "ImageCompare.h"
#include "LogCapture.h"
#include "Readback.h"
#include "TestPaths.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::particles;

namespace {

    struct Rgb {
        uint8_t r = 0, g = 0, b = 0;
        bool operator==(const Rgb& o) const { return r == o.r && g == o.g && b == o.b; }
        bool operator!=(const Rgb& o) const { return !(*this == o); }
        bool operator<(const Rgb& o) const { return r != o.r ? r < o.r : (g != o.g ? g < o.g : b < o.b); }
    };

    struct Shot {
        int width = 0, height = 0;
        std::vector<uint8_t> rgba;
        // (x, y) from the bottom-left, as the grid counts
        Rgb at(int x, int y) const{
            if (x < 0 || y < 0 || x >= width || y >= height) return Rgb{};
            const uint8_t* p = &rgba[((size_t)(height - 1 - y) * width + x) * 4];
            return Rgb{p[0], p[1], p[2]};
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

    // the environment's view as the suite found it
    struct SavedView {
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

    constexpr Rgb kBlack{0, 0, 0};
    constexpr Rgb kWhite{255, 255, 255};
    constexpr Rgb kRed{255, 0, 0};

    class ParticlesPixelGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        SavedView view;
        std::vector<std::unique_ptr<ParticleSystem>> systems;

        void SetUp() override{
            view.save();
            scene.setBackgroundColor(0.0f, 0.0f, 0.0f, 1.0f);
            camera.setType(CameraType::CAMERA_ORTHO);
            scene.getComponent<CameraComponent>(camera.getEntity()).useTarget = false;
            camera.setPosition(0.0f, 0.0f, 1.0f);
            camera.activate();
            Engine::setPixelsPerUnit(1);
            Engine::setScene(&scene);
        }

        void TearDown() override{
            systems.clear();
            Engine::removeScene(&scene);
            Engine::setPixelsPerUnit(1);
            view.restore();
        }

        ParticleSystem& add(const Vector3& at, const ParticleEffectDocument& document, uint32_t seed = 1){
            systems.push_back(std::make_unique<ParticleSystem>(&scene));
            ParticleSystem& ps = *systems.back();
            ps.setPosition(at);
            ps.setSeed(seed);
            ps.setPlayOnAwake(false);
            ps.setEffectDocument(document);
            return ps;
        }

        // loaded, then every system seeked to `time` and held there
        bool render(double time){
            Scene* current = &scene;
            if (!GpuEnvironment::runFrames([current](){ return current->getSystem<RenderSystem>()->isAllLoaded(); }))
                return false;
            for (auto& ps : systems){
                ps->seek((float)time);
                ps->pause();
            }
            return GpuEnvironment::runFrames([current](){ return current->getSystem<RenderSystem>()->isAllLoaded(); });
        }
    };

    // one particle that stays where it is born (a burst of one, no motion)
    ParticleEffectDocument still(float size, float rotationDegrees){
        ParticleEffectDocument d;
        d.looping = false;
        d.duration = 1.0f;
        d.maxParticles = 4;
        d.main.startLifetime = MinMaxCurve(100.0f);
        d.main.startSpeed = MinMaxCurve(0.0f);
        d.main.startSize = MinMaxCurve(size);
        d.main.startRotation = MinMaxCurve(rotationDegrees);
        d.emission.rateOverTime = MinMaxCurve(0.0f);
        Burst b;
        b.count = MinMaxCurve(1.0f);
        d.emission.bursts = {b};
        d.shape.enabled = false;
        d.pixelArt.present = true;
        d.pixelArt.fade = PixelFade::Cutout;
        d.pixelArt.secondary = SecondaryColorMode::Color;
        d.pixelArt.secondaryColor = Vector4(1.0f, 0.0f, 0.0f, 1.0f);
        d.renderer.renderMode = RenderMode::Pixel;
        return d;
    }

}

// AC2 (R2, R3, R9, R22): every shape at sizes 1, 2, 3, 5, 8 and rotations 0, 37 and 90 degrees,
// each at a sub-pixel centre of its own cell: the read-back equals the CPU rasterisation of R9,
// pixel for pixel (primary white, secondary red, Cutout at half coverage)
TEST_F(ParticlesPixelGpu, PixelShapes){
    JASPER_REQUIRE_GPU();
    GpuEnvironment::setView(256, 256, 256, 256, Scaling::LETTERBOX);
    struct Case { PixelShape shape; float size; float rotation; Vector2 centre; };
    std::vector<Case> cases;
    const int sizes[] = {1, 2, 3, 5, 8};
    const float rotations[] = {0.0f, 37.0f, 90.0f};
    int cell = 0;
    for (int s = 0; s <= (int)PixelShape::Glow; s++){
        for (int size : sizes){
            for (float rotation : rotations){
                // cells of 21 pixels, 12 to a row; the centre a quarter and three fifths of a pixel off
                const Vector2 centre(10.0f + 21.0f * (float)(cell % 12) + 0.25f, 10.0f + 21.0f * (float)(cell / 12) + 0.6f);
                cases.push_back({(PixelShape)s, (float)size, rotation, centre});
                ParticleEffectDocument d = still((float)size, rotation);
                d.renderer.pixelShape = (PixelShape)s;
                if ((PixelShape)s == PixelShape::Streak){
                    // its length from the speed (L = 1 + speed x scale), its direction the rotation's
                    const float a = rotation * 3.14159265f / 180.0f;
                    const float speed = (float)(size - 1) / 0.5f;
                    d.renderer.streakLengthScale = 0.5f;
                    d.startVelocity.present = true;
                    d.startVelocity.min = d.startVelocity.max = Vector3(std::cos(a) * speed, std::sin(a) * speed, 0.0f);
                    d.forceOverLifetime.present = false;
                    d.main.startSize = MinMaxCurve(1.0f);
                }
                add(Vector3(centre.x, centre.y, 0.0f), d);
                cell++;
            }
        }
    }
    ASSERT_TRUE(render(0.05));
    GpuEnvironment::drawFrames(2);
    const Shot image = readImage();

    size_t mismatches = 0, lit = 0;
    std::string first;
    for (size_t i = 0; i < cases.size(); i++){
        const Case& c = cases[i];
        RendererModule renderer = still(c.size, c.rotation).renderer;
        renderer.pixelShape = c.shape;
        PixelShapeSpec spec = PixelShapeSpec::fromRenderer(renderer);
        Vector2 velocity(0.0f, 0.0f);
        float size = c.size;
        if (c.shape == PixelShape::Streak){
            spec.streakLengthScale = 0.5f;
            const float a = c.rotation * 3.14159265f / 180.0f;
            const float speed = (c.size - 1.0f) / 0.5f;
            velocity = Vector2(std::cos(a) * speed, std::sin(a) * speed);
            size = 1.0f;
        }
        const float rotation = c.rotation * 3.14159265f / 180.0f;
        const PixelLayout layout = ParticlePixel::layout(spec, c.centre, Vector2(size, size), velocity, rotation, true);
        // the expected colour of each pixel of the cell
        const int x0 = (int)std::floor(c.centre.x) - 10, y0 = (int)std::floor(c.centre.y) - 10;
        for (int y = y0; y < y0 + 21; y++){
            for (int x = x0; x < x0 + 21; x++){
                Rgb expected = kBlack;
                // the box's local pixel holding this pixel's centre
                const float fx = (float)x + 0.5f - layout.cornerX, fy = (float)y + 0.5f - layout.cornerY;
                if (fx >= 0.0f && fy >= 0.0f && fx < layout.boxW && fy < layout.boxH){
                    const PixelSample sample = ParticlePixel::sample(spec, layout, (int)std::floor(fx), (int)std::floor(fy), 0.0f, 0);
                    if (sample.drawn){
                        if (sample.cover >= 0.5f) expected = sample.tone == 1 ? kRed : kWhite;
                        else if (sample.tone2 >= 0 && sample.cover2 >= 0.5f) expected = sample.tone2 == 1 ? kRed : kWhite;
                    }
                }
                const Rgb actual = image.at(x, y);
                if (expected != kBlack) lit++;
                if (actual != expected){
                    mismatches++;
                    if (first.empty()){
                        char text[200];
                        std::snprintf(text, sizeof(text), "shape %d size %g rotation %g: pixel (%d, %d) is (%d, %d, %d), the CPU rule (%d, %d, %d)",
                                      (int)c.shape, (double)c.size, (double)c.rotation, x, y, actual.r, actual.g, actual.b, expected.r, expected.g, expected.b);
                        first = text;
                    }
                }
            }
        }
    }
    EXPECT_GT(lit, 1000u);
    EXPECT_EQ(mismatches, 0u) << first;
    CompareOptions exact;
    exact.pixelThreshold = 0;
    exact.maxBadFraction = 0.0;
    exact.maxMeanDiff = 0.0;
    EXPECT_GOLDEN_OPTIONS("particles_pixel_shapes", exact);
    EXPECT_NO_ENGINE_ERRORS();
}

// AC3 (R4, D5): a 16x16 Dot at alpha k/16 lights k pixels of each 4x4 cell (Bayer 4) and k/4 of
// each 2x2 (Bayer 2); Screen space holds the pattern when the particle moves a pixel, Particle space
// moves it with it; every drawn pixel is the particle's colour exactly (alpha 1, never blended)
TEST_F(ParticlesPixelGpu, DitherFade){
    JASPER_REQUIRE_GPU();
    GpuEnvironment::setView(128, 128, 128, 128, Scaling::LETTERBOX);
    auto dot = [&](float alpha, int ditherSize, DitherSpace space){
        ParticleEffectDocument d = still(16.0f, 0.0f);
        d.pixelArt.fade = PixelFade::Dither;
        d.pixelArt.ditherSize = ditherSize;
        d.pixelArt.ditherSpace = space;
        d.main.startColor = MinMaxGradient(Vector4(1.0f, 1.0f, 1.0f, alpha));
        d.renderer.pixelShape = PixelShape::Dot;
        return d;
    };
    // a column of dots, k = 1..15 at Bayer 4, boxes on multiples of 4 (centres at 8 + 32 i)
    for (int k = 1; k <= 3; k++){
        for (int j = 0; j < 3; j++){
            const int index = k * 4 + j;   // k/16 = 4/16 .. 14/16
            add(Vector3(8.0f + 32.0f * (float)j, 8.0f + 32.0f * (float)(k - 1), 0.0f), dot((float)(index) / 16.0f, 4, DitherSpace::Screen));
        }
    }
    // Bayer 2 at k = 4, 8, 12 (k / 4 of each 2x2)
    for (int j = 0; j < 3; j++)
        add(Vector3(8.0f + 32.0f * (float)j, 104.0f, 0.0f), dot((float)(4 * (j + 1)) / 16.0f, 2, DitherSpace::Screen));
    ASSERT_TRUE(render(0.05));
    GpuEnvironment::drawFrames(2);
    const Shot image = readImage();
    for (int k = 1; k <= 3; k++){
        for (int j = 0; j < 3; j++){
            const int index = k * 4 + j;
            const int bx = 32 * j, by = 32 * (k - 1);
            for (int cy = 0; cy < 4; cy++){
                for (int cx = 0; cx < 4; cx++){
                    int count = 0;
                    for (int y = 0; y < 4; y++){
                        for (int x = 0; x < 4; x++){
                            const Rgb c = image.at(bx + cx * 4 + x, by + cy * 4 + y);
                            EXPECT_TRUE(c == kWhite || c == kBlack) << "a blended pixel under Dither";
                            count += c == kWhite ? 1 : 0;
                            // the cell is the table's: drawn where the threshold is below k/16
                            EXPECT_EQ(c == kWhite, (float)index / 16.0f > pixelart::bayerThreshold(4, bx + cx * 4 + x, by + cy * 4 + y));
                        }
                    }
                    EXPECT_EQ(count, index) << "a 4x4 cell at alpha " << index << "/16";
                }
            }
        }
    }
    for (int j = 0; j < 3; j++){
        for (int cy = 0; cy < 8; cy++){
            for (int cx = 0; cx < 8; cx++){
                int count = 0;
                for (int y = 0; y < 2; y++)
                    for (int x = 0; x < 2; x++) count += image.at(32 * j + cx * 2 + x, 96 + cy * 2 + y) == kWhite ? 1 : 0;
                EXPECT_EQ(count, j + 1) << "a 2x2 cell at alpha " << 4 * (j + 1) << "/16";
            }
        }
    }

    // moved by one pixel: Screen holds the pattern, Particle moves it
    systems.clear();
    ParticleSystem& screen = add(Vector3(40.0f, 40.0f, 0.0f), dot(0.5f, 4, DitherSpace::Screen));
    ParticleSystem& particle = add(Vector3(80.0f, 40.0f, 0.0f), dot(0.5f, 4, DitherSpace::Particle));
    ASSERT_TRUE(render(0.05));
    GpuEnvironment::drawFrames(2);
    const Shot before = readImage();
    screen.setPosition(41.0f, 40.0f, 0.0f);
    particle.setPosition(81.0f, 40.0f, 0.0f);
    ASSERT_TRUE(render(0.05));
    GpuEnvironment::drawFrames(2);
    const Shot after = readImage();
    size_t screenSame = 0, particleShifted = 0, particleSame = 0;
    for (int y = 33; y < 47; y++){
        for (int x = 34; x < 47; x++){
            screenSame += before.at(x, y) == after.at(x, y) ? 1 : 0;
            particleShifted += before.at(x + 40, y) == after.at(x + 41, y) ? 1 : 0;
            particleSame += before.at(x + 41, y) == after.at(x + 41, y) ? 1 : 0;
        }
    }
    EXPECT_EQ(screenSame, (size_t)(14 * 13)) << "Screen space: the pattern holds still under a moving particle";
    EXPECT_EQ(particleShifted, (size_t)(14 * 13)) << "Particle space: the pattern moves with it";
    EXPECT_LT(particleSame, (size_t)(14 * 13)) << "red control: Particle space at the same screen pixels differs";
    EXPECT_NO_ENGINE_ERRORS();
}

// AC6 (R12, R13): every sRGB8 value comes out of the particle colour path as it went in (a ramp's
// bytes, the record's UBYTE4N, the shader's decode and the output's encode), and a ramp-coloured
// Pixel effect draws only its ramp's colours (neutral post-processing)
TEST_F(ParticlesPixelGpu, ExactColors){
    JASPER_REQUIRE_GPU();
    GpuEnvironment::setView(128, 128, 128, 128, Scaling::LETTERBOX);
    for (int v = 0; v < 256; v++){
        ParticleEffectDocument d = still(4.0f, 0.0f);
        d.renderer.pixelShape = PixelShape::Dot;
        d.main.startColor.mode = GradientValueMode::Ramp;
        // grey, and the other channels apart (all 256 values on each)
        d.main.startColor.rampColors = {pixelart::Rgba8{(uint8_t)v, (uint8_t)(255 - v), (uint8_t)((v * 7) & 255), 255}};
        add(Vector3(2.0f + 8.0f * (float)(v % 16), 2.0f + 8.0f * (float)(v / 16), 0.0f), d);
    }
    ASSERT_TRUE(render(0.05));
    GpuEnvironment::drawFrames(2);
    const Shot image = readImage();
    int wrong = 0;
    for (int v = 0; v < 256; v++){
        const Rgb expected{(uint8_t)v, (uint8_t)(255 - v), (uint8_t)((v * 7) & 255)};
        const Rgb actual = image.at(1 + 8 * (v % 16), 1 + 8 * (v / 16));
        if (actual != expected){
            wrong++;
            ADD_FAILURE() << "value " << v << " drew (" << (int)actual.r << ", " << (int)actual.g << ", " << (int)actual.b << ")";
        }
    }
    EXPECT_EQ(wrong, 0);

    // a Pixel effect on a ramp: nothing but the ramp's colours (and the background)
    systems.clear();
    const std::vector<pixelart::Rgba8> ramp = {{255, 248, 214, 255}, {255, 174, 53, 255}, {242, 112, 31, 255}, {204, 58, 26, 255},
                                               {140, 29, 28, 255}, {74, 17, 24, 255}};
    ParticleEffectDocument fire;
    fire.duration = 2.0f;
    fire.maxParticles = 500;
    fire.main.startLifetime = MinMaxCurve::twoConstants(0.5f, 1.5f);
    fire.main.startSpeed = MinMaxCurve::twoConstants(5.0f, 30.0f);
    fire.main.startSize = MinMaxCurve::twoConstants(2.0f, 9.0f);
    fire.emission.rateOverTime = MinMaxCurve(200.0f);
    fire.shape.shape = ShapeType::Circle;
    fire.shape.radius = 20.0f;
    fire.shape.rotation = Vector3(90.0f, 0.0f, 0.0f);
    fire.colorOverLifetime.present = true;
    fire.colorOverLifetime.color.mode = GradientValueMode::Ramp;
    fire.colorOverLifetime.color.rampColors = ramp;
    fire.pixelArt.present = true;
    fire.pixelArt.fade = PixelFade::Dither;
    fire.pixelArt.secondaryOffset = 2;
    fire.pixelArt.fadeOverLifetime = MinMaxCurve::fromCurve(Curve::linear(1.0f, 0.2f));
    fire.renderer.renderMode = RenderMode::Pixel;
    fire.renderer.pixelShape = PixelShape::Disc;
    fire.renderer.discRim = true;
    add(Vector3(64.0f, 64.0f, 0.0f), fire, 3);
    ASSERT_TRUE(render(1.3));
    GpuEnvironment::drawFrames(2);
    const Shot fireImage = readImage();
    std::set<Rgb> allowed = {kBlack};
    for (const pixelart::Rgba8& c : ramp) allowed.insert(Rgb{c.r, c.g, c.b});
    size_t drawn = 0, off = 0;
    for (int y = 0; y < fireImage.height; y++){
        for (int x = 0; x < fireImage.width; x++){
            const Rgb c = fireImage.at(x, y);
            if (c == kBlack) continue;
            drawn++;
            if (!allowed.count(c)) off++;
        }
    }
    EXPECT_GT(drawn, 200u);
    EXPECT_EQ(off, 0u) << "pixels off the ramp";
    EXPECT_NO_ENGINE_ERRORS();
}

// AC8 (R18, R19): about 6,000 live Analytic dots over a 120 x 120 box (a third of its pixels lit)
// drawn at 6 times, at a drag where the closed form holds and at a small one (k t well under 1, where
// it cancels): the lit pixels are the CPU twin's snapped particles, pixel for pixel. A difference is
// allowed on 0.1 % of them, and only one pixel off
TEST_F(ParticlesPixelGpu, AnalyticMatchesCpu){
    JASPER_REQUIRE_GPU();
    GpuEnvironment::setView(256, 256, 256, 256, Scaling::LETTERBOX);
    // the random directions are 3D: depth wide enough for every particle the twin places
    camera.setNearClip(-1000.0f);
    camera.setFarClip(1000.0f);
    for (float drag : {0.7f, 0.0006f}){
        ParticleEffectDocument d;
        d.simulationMode = SimulationMode::Analytic;
        d.looping = true;
        d.prewarm = true;
        d.duration = 4.0f;
        d.main.startLifetime = MinMaxCurve::twoConstants(1.0f, 3.0f);
        d.main.startSpeed = MinMaxCurve::twoConstants(5.0f, 40.0f);
        d.main.startSize = MinMaxCurve(1.0f);
        d.emission.rateOverTime = MinMaxCurve(3000.0f);
        d.shape.shape = ShapeType::Box;
        d.shape.scale = Vector3(120.0f, 120.0f, 0.0f);
        d.shape.randomizeDirection = 1.0f;
        d.limitVelocity.present = true;
        d.limitVelocity.dampen = 0.0f;
        d.limitVelocity.multiplyBySize = false;
        d.limitVelocity.drag = MinMaxCurve(drag);
        d.forceOverLifetime.present = true;
        d.forceOverLifetime.y = MinMaxCurve(-12.0f);
        d.pixelArt.present = true;
        d.pixelArt.fade = PixelFade::Cutout;
        d.renderer.renderMode = RenderMode::Pixel;
        d.renderer.pixelShape = PixelShape::Dot;
        systems.clear();
        ParticleSystem& ps = add(Vector3(128.0f, 128.0f, 0.0f), d, 21);
        ASSERT_TRUE(render(0.05));
        const ParticleSystemState* state = scene.getComponent<ParticleSystemComponent>(ps.getEntity()).state.get();
        ASSERT_TRUE(state && state->runtime.isAnalytic());

        size_t particles = 0, pixels = 0, lit = 0, unlit = 0, stray = 0, far = 0;
        for (int i = 0; i < 6; i++){
            const double t = 0.37 + 1.13 * i;
            ASSERT_TRUE(render(t));
            GpuEnvironment::drawFrames(2);
            const Shot image = readImage();
            const int w = image.width, h = image.height;
            std::vector<uint8_t> expected((size_t)w * h, 0), drawn((size_t)w * h, 0);
            const AnalyticProgram& program = *state->runtime.analytic();
            for (uint32_t s = 0; s < program.slotCount; s++){
                const AnalyticParticle p = AnalyticParticles::evaluate(program, s);
                if (!p.alive) continue;
                // the emitter's transform (a translation), then the corner snap of a 1-px box
                const int x = (int)pixelart::snap(128.0f + p.position.x - 0.5f), y = (int)pixelart::snap(128.0f + p.position.y - 0.5f);
                if (x < 0 || y < 0 || x >= w || y >= h) continue;
                expected[(size_t)y * w + x] = 1;
                particles++;
            }
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++)
                    drawn[(size_t)y * w + x] = image.at(x, y) != kBlack ? 1 : 0;
            auto near = [&](const std::vector<uint8_t>& set, int x, int y){
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++){
                        const int nx = x + dx, ny = y + dy;
                        if (nx >= 0 && ny >= 0 && nx < w && ny < h && set[(size_t)ny * w + nx]) return true;
                    }
                return false;
            };
            for (int y = 0; y < h; y++){
                for (int x = 0; x < w; x++){
                    const bool e = expected[(size_t)y * w + x] != 0, g = drawn[(size_t)y * w + x] != 0;
                    pixels += e ? 1 : 0;
                    lit += g ? 1 : 0;
                    if (e && !g){
                        unlit++;
                        far += near(drawn, x, y) ? 0 : 1;
                    }else if (g && !e){
                        stray++;
                        far += near(expected, x, y) ? 0 : 1;
                    }
                }
            }
            if (i == 0 && std::getenv("JASPER_TEST_WRITE_ACTUAL")){
                // the read-back and the twin's pixels side by side, for a failure's evidence
                std::vector<uint8_t> both((size_t)w * 2 * h * 4, 0);
                for (int y = 0; y < h; y++){
                    for (int x = 0; x < w; x++){
                        uint8_t* left = &both[((size_t)(h - 1 - y) * w * 2 + x) * 4];
                        uint8_t* right = left + (size_t)w * 4;
                        left[0] = left[1] = left[2] = drawn[(size_t)y * w + x] ? 255 : 0;
                        right[0] = right[1] = right[2] = expected[(size_t)y * w + x] ? 255 : 0;
                        left[3] = right[3] = 255;
                    }
                }
                writePng(getOutputDirectory() + "/particles_analytic_vs_cpu_" + std::to_string(drag) + ".png", both, w * 2, h);
            }
        }
        const std::string counts = "drag " + std::to_string(drag) + ": " + std::to_string(particles) + " particles on " + std::to_string(pixels) +
                                   " pixels, " + std::to_string(lit) + " lit, " + std::to_string(unlit) + " unlit, " + std::to_string(stray) + " stray, " +
                                   std::to_string(far) + " more than a pixel off";
        std::printf("[gpu] AnalyticMatchesCpu %s\n", counts.c_str());
        EXPECT_GT(particles, 30000u) << counts;
        EXPECT_GT((double)pixels, 0.25 * 6 * 120 * 120) << counts;
        EXPECT_LE((double)(unlit + stray), 0.001 * (double)pixels) << counts;
        EXPECT_EQ(far, 0u) << counts;
    }
    EXPECT_NO_ENGINE_ERRORS();
}

namespace {

    // a pixel preset as the editor ships it (editor/pixelart/particles; S49 R20)
    ParticleEffectDocument presetDocument(const std::string& name){
        ParticleEffectDocument d;
        std::ifstream file(testPaths().sourceDir + "/../../../editor/pixelart/particles/" + name + ".particles");
        std::stringstream text;
        text << file.rdbuf();
        // a project gets a copy of builtin:pixelfx-effects (the editor's): here the ramps are inline
        const std::string json = std::regex_replace(text.str(), std::regex("\"ramp\": \"builtin:pixelfx-effects#ramp:[a-z_]+\""),
                                                    "\"rampColors\": [\"#fff3a6\", \"#ffb03b\", \"#c8481c\", \"#5a1e12\"]");
        std::vector<Diagnostic> diagnostics;
        EXPECT_TRUE(ParticleEffectCodec::read(json, d, diagnostics)) << name << ": " << ParticleEffectCodec::format(diagnostics);
        return d;
    }

    // the drawn pixels in columns [x0, x1)
    size_t litBetween(const Shot& image, int x0, int x1){
        size_t n = 0;
        for (int y = 0; y < image.height; y++)
            for (int x = x0; x < x1; x++)
                n += image.at(x, y) != kBlack ? 1 : 0;
        return n;
    }

    ParticleEffectDocument withoutSubEmitters(ParticleEffectDocument d){
        d.subEmittersModule.enabled = false;
        return d;
    }

}

// S20 R10 through the renderer (S49 review): a sub-emitter's particles draw, with their own effect's
// renderer. A user effect whose emitter draws nothing (Render Mode None) and spawns Pixel dots at
// birth: drawn beside the same effect without its sub-emitter, which draws nothing at all
TEST_F(ParticlesPixelGpu, SubEmittersDraw){
    JASPER_REQUIRE_GPU();
    GpuEnvironment::setView(256, 256, 256, 256, Scaling::LETTERBOX);
    ParticleEffectDocument child = still(3.0f, 0.0f);
    child.emission.bursts.clear();
    child.main.startSpeed = MinMaxCurve::twoConstants(10.0f, 30.0f);
    child.shape.enabled = true;
    child.shape.shape = ShapeType::Sphere;
    child.maxParticles = 64;
    ParticleEffectLibrary::set("tests/s49_child.particles", child);
    ParticleEffectDocument parent;
    parent.looping = false;
    parent.duration = 1.0f;
    parent.main.startLifetime = MinMaxCurve(10.0f);
    parent.main.startSpeed = MinMaxCurve(0.0f);
    parent.emission.rateOverTime = MinMaxCurve(0.0f);
    Burst b;
    b.count = MinMaxCurve(6.0f);
    parent.emission.bursts = {b};
    parent.shape.enabled = false;
    parent.renderer.renderMode = RenderMode::None;
    parent.subEmittersModule.present = true;
    parent.subEmittersModule.enabled = true;
    SubEmitterEntry entry;
    entry.trigger = SubEmitterTrigger::Birth;
    entry.effect = "tests/s49_child.particles";
    entry.emitCount = 4;
    parent.subEmitters = {entry};
    add(Vector3(64.0f, 128.0f, 0.0f), parent, 3);
    add(Vector3(192.0f, 128.0f, 0.0f), withoutSubEmitters(parent), 3);
    ASSERT_TRUE(render(0.5));
    const Shot image = readImage();
    EXPECT_GT(litBetween(image, 0, 128), 20u) << "the sub-emitter's dots draw";
    EXPECT_EQ(litBetween(image, 128, 256), 0u) << "without it nothing draws";
    EXPECT_NO_ENGINE_ERRORS();
}

// The presets that draw through their sub-emitters (S49 R20): Fire's smoke, Debris's shards and
// chunks; each beside the same preset without its sub-emitters
TEST_F(ParticlesPixelGpu, PresetSubEmittersDraw){
    JASPER_REQUIRE_GPU();
    GpuEnvironment::setView(256, 256, 256, 256, Scaling::LETTERBOX);
    for (const char* name : {"pixel_smoke", "pixel_debris_shard", "pixel_debris_chunk"})
        ParticleEffectLibrary::set(std::string("preset:") + name, presetDocument(name));

    // Debris draws nothing of its own
    add(Vector3(64.0f, 128.0f, 0.0f), presetDocument("pixel_debris"), 5);
    add(Vector3(192.0f, 128.0f, 0.0f), withoutSubEmitters(presetDocument("pixel_debris")), 5);
    ASSERT_TRUE(render(0.08));
    Shot image = readImage();
    EXPECT_GT(litBetween(image, 0, 128), 10u) << "Debris's shards and chunks draw";
    EXPECT_EQ(litBetween(image, 128, 256), 0u) << "Debris without its pieces draws nothing";

    // Fire's smoke adds to the fire (the same seed: the same flames)
    systems.clear();
    add(Vector3(64.0f, 60.0f, 0.0f), presetDocument("pixel_fire"), 7);
    add(Vector3(192.0f, 60.0f, 0.0f), withoutSubEmitters(presetDocument("pixel_fire")), 7);
    ASSERT_TRUE(render(2.0));
    image = readImage();
    const size_t withSmoke = litBetween(image, 0, 128), flames = litBetween(image, 128, 256);
    EXPECT_GT(flames, 0u);
    EXPECT_GT(withSmoke, flames + 10) << "the smoke draws: " << withSmoke << " pixels against the flames' " << flames;
    EXPECT_NO_ENGINE_ERRORS();
}
