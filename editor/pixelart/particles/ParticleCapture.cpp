// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ParticleCapture.h"

#include "Engine.h"
#include "Scene.h"
#include "component/ParticleSystemComponent.h"
#include "object/Camera.h"
#include "object/ParticleSystem.h"
#include "particles/AnalyticParticles.h"
#include "particles/ParticleChannels.h"
#include "particles/ParticleProgram.h"
#include "subsystem/ParticleSubsystem.h"
#include "subsystem/ParticleSystemState.h"
#include "subsystem/RenderSystem.h"
#include "util/GraphicUtils.h"

#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <cstdio>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::pixelart;

namespace jasperengine::editor::pixelart::captureio {
namespace {

    // a 3x5 font for the sheet's labels: rows top first, bit 2 the left column
    struct SheetGlyph { char c; uint8_t rows[5]; };
    constexpr SheetGlyph kSheetGlyphs[] = {
        {'0', {7, 5, 5, 5, 7}}, {'1', {2, 6, 2, 2, 7}}, {'2', {7, 1, 7, 4, 7}}, {'3', {7, 1, 7, 1, 7}}, {'4', {5, 5, 7, 1, 1}},
        {'5', {7, 4, 7, 1, 7}}, {'6', {7, 4, 7, 5, 7}}, {'7', {7, 1, 1, 1, 1}}, {'8', {7, 5, 7, 5, 7}}, {'9', {7, 5, 7, 1, 7}},
        {'#', {5, 7, 5, 7, 5}}, {'.', {0, 0, 0, 0, 2}}, {'t', {2, 7, 2, 2, 1}}, {'s', {0, 3, 6, 1, 6}}, {'=', {0, 7, 0, 7, 0}},
    };

    void drawSheetText(PixelImage& image, const std::string& text, int x, int y, int scale, PixelRgba color){
        for (size_t i = 0; i < text.size(); i++){
            const SheetGlyph* glyph = nullptr;
            for (const SheetGlyph& g : kSheetGlyphs){
                if (g.c == text[i]) glyph = &g;
            }
            if (!glyph) continue;
            for (int row = 0; row < 5; row++){
                for (int col = 0; col < 3; col++){
                    if (!(glyph->rows[row] & (4 >> col))) continue;
                    for (int sy = 0; sy < scale; sy++){
                        for (int sx = 0; sx < scale; sx++)
                            image.set(x + ((int)i * 4 + col) * scale + sx, y + row * scale + sy, color);
                    }
                }
            }
        }
    }

}
}

namespace cio = jasperengine::editor::pixelart::captureio;

namespace {

    // an emitter and its sub-emitters, depth first
    void forEachEmitter(const jasperengine::particles::EmitterRuntime& emitter,
                        const std::function<void(const jasperengine::particles::EmitterRuntime&)>& visit){
        visit(emitter);
        for (size_t i = 0; const jasperengine::particles::EmitterRuntime* child = emitter.child(i); i++)
            forEachEmitter(*child, visit);
    }

}

ParticleCapture::ParticleCapture() = default;

ParticleCapture::~ParticleCapture(){
    release();
}

void ParticleCapture::release(){
    system.reset();
    delete camera;
    delete scene;
    camera = nullptr;
    scene = nullptr;
}

void ParticleCapture::seekTo(double time){
    std::shared_ptr<ParticleSubsystem> particles = scene->getSystem<ParticleSubsystem>();
    particles->seek(system->getEntity(), time);
    particles->pause(system->getEntity());
}

bool ParticleCapture::begin(const ParticleCaptureSettings& settings, std::string& error){
    release();
    captured.clear();
    failure.clear();
    next = 0;
    warmFrames = 0;
    rendering = false;
    config = settings;
    if (config.times.empty()){
        error = "no times to capture";
        return false;
    }
    const float ppu = (float)std::max(config.pixelsPerUnit, 1);

    scene = new Scene();
    scene->setDefaultEntityPool(EntityPool::System);
    camera = new Camera(scene);
    scene->setDefaultEntityPool(EntityPool::User);
    // drawn on transparent black: a baked strip keeps exactly what the effect drew (an additive or
    // Alpha-faded pixel is never blended with a key colour); the contact sheet composites the
    // frames over the background colour
    scene->setBackgroundColor(0.0f, 0.0f, 0.0f, 0.0f);
    scene->setCamera(camera);

    system = std::make_unique<ParticleSystem>(scene);
    system->setPlayOnAwake(false);
    system->setSeed(config.seed);
    system->setEffectDocument(config.document);
    std::shared_ptr<ParticleSubsystem> particles = scene->getSystem<ParticleSubsystem>();
    particles::ParticleSystemState* state = particles->getState(system->getEntity());
    if (!state || !state->effect || !state->effect->valid){
        error = "the effect does not compile";
        if (state && state->effect){
            for (const particles::Diagnostic& d : state->effect->diagnostics){
                if (d.severity == particles::Diagnostic::Severity::Error){
                    error += ": " + d.path + ": " + d.message;
                    break;
                }
            }
        }
        release();
        return false;
    }

    // the effect's reach over the times (simulation space is the world here: the emitter sits at
    // the origin, unturned)
    float lo[2] = {0.0f, 0.0f}, hi[2] = {0.0f, 0.0f};
    for (double t : config.times){
        seekTo(t);
        // the sub-emitters' particles too (Debris draws only its children)
        forEachEmitter(state->runtime, [&](const particles::EmitterRuntime& emitter){
            if (!emitter.hasBounds() || (!emitter.isAnalytic() && emitter.count() == 0))
                return;
            float bmin[3], bmax[3];
            emitter.bounds(bmin, bmax);
            for (int k = 0; k < 2; k++){
                lo[k] = std::min(lo[k], bmin[k]);
                hi[k] = std::max(hi[k], bmax[k]);
            }
        });
    }
    // whole base pixels, an even count each way so the camera's centre is a pixel corner
    const float m = (float)config.margin;
    int minX = (int)std::floor(lo[0] * ppu - m), maxX = (int)std::ceil(hi[0] * ppu + m);
    int minY = (int)std::floor(lo[1] * ppu - m), maxY = (int)std::ceil(hi[1] * ppu + m);
    auto fit = [&](int& a, int& b){
        if ((b - a) % 2) b++;
        if (b - a > config.maxSide){
            const int centre = (a + b) / 2;
            a = centre - config.maxSide / 2;
            b = a + config.maxSide;
        }
        if (b - a < 2) b = a + 2;
    };
    fit(minX, maxX);
    fit(minY, maxY);
    frameWidth = maxX - minX;
    frameHeight = maxY - minY;
    frameOrigin = Vector2((float)minX / ppu, (float)minY / ppu);
    reachPx[0] = lo[0] * ppu - (float)minX;
    reachPx[1] = lo[1] * ppu - (float)minY;
    reachPx[2] = hi[0] * ppu - (float)minX;
    reachPx[3] = hi[1] * ppu - (float)minY;

    const float cx = (float)(minX + maxX) * 0.5f / ppu, cy = (float)(minY + maxY) * 0.5f / ppu;
    camera->setType(CameraType::CAMERA_ORTHO);
    camera->setOrtho(-(float)frameWidth * 0.5f / ppu, (float)frameWidth * 0.5f / ppu, -(float)frameHeight * 0.5f / ppu,
                     (float)frameHeight * 0.5f / ppu, 0.1f, 1000.0f);
    camera->setPosition(cx, cy, 100.0f);
    camera->setTarget(cx, cy, 0.0f);
    camera->setFramebufferSize(frameWidth * kProbeScale, frameHeight * kProbeScale);
    camera->setFramebufferFilter(TextureFilter::NEAREST);
    camera->setRenderToTexture(true);

    seekTo(config.times[0]);
    Engine::executeSceneOnce(scene);
    rendering = true;
    return true;
}

void ParticleCapture::collectParticles(CapturedFrame& frame){
    std::shared_ptr<ParticleSubsystem> particles = scene->getSystem<ParticleSubsystem>();
    const particles::ParticleSystemState* state = particles->getState(system->getEntity());
    if (!state)
        return;
    const float ppu = (float)std::max(config.pixelsPerUnit, 1);
    // every emitter of the effect: its sub-emitters draw too
    forEachEmitter(state->runtime, [&](const particles::EmitterRuntime& runtime){
        const particles::ParticleProgram* program = runtime.program();
        const particles::PixelParams* pixel = program ? &program->render.pixel : nullptr;
        const bool pixelSizes = pixel && pixel->active && pixel->pixelSizes;
        auto add = [&](float x, float y, float size){
            CapturedParticle p;
            p.x = (x - frameOrigin.x) * ppu;
            p.y = (y - frameOrigin.y) * ppu;
            p.size = pixelSizes ? size : size * ppu;
            if (pixel){
                p.pixel = pixel->active != 0;
                p.snap = pixel->module && pixel->snap;
                p.integerSize = !pixel->module || pixel->integerSize;
                p.streak = pixel->shapes && pixel->shape == particles::PixelShape::Streak;
            }
            frame.particles.push_back(p);
        };
        if (runtime.isAnalytic()){
            const particles::AnalyticProgram& analytic = *runtime.analytic();
            for (uint32_t s = 0; s < analytic.slotCount; s++){
                const particles::AnalyticParticle p = particles::AnalyticParticles::evaluate(analytic, s);
                if (p.alive) add(p.position.x + p.wobble.x / ppu, p.position.y + p.wobble.y / ppu, p.size.x);
            }
            return;
        }
        const particles::ParticleChannels& ch = runtime.particleChannels();
        const float* xs = ch.get(particles::ChPosX);
        const float* ys = ch.get(particles::ChPosY);
        for (uint32_t i = 0; i < runtime.count(); i++){
            float color[4], size = 1.0f, rotation = 0.0f;
            runtime.appearance(i, color, size, rotation);
            add(xs[i], ys[i], size);
        }
    });
    frame.count = (uint32_t)frame.particles.size();
}

bool ParticleCapture::step(){
    if (!failure.empty() || !scene)
        return true;
    if (next >= config.times.size())
        return true;
    if (rendering && Engine::isSceneRunning(scene))
        return false;
    rendering = false;
    // the first frames load the variants (asynchronous shader builds): drawn again until loaded
    RenderSystem* render = scene->getSystem<RenderSystem>().get();
    if (warmFrames < 2 || (render && !render->isAllLoaded())){
        if (++warmFrames > 1200){
            failure = "the effect's shaders did not load within 1200 frames";
            return true;
        }
        Engine::executeSceneOnce(scene);
        rendering = true;
        return false;
    }
    CapturedFrame frame;
    frame.time = config.times[next];
    std::vector<uint8_t> rgba;
    const int probeWidth = frameWidth * kProbeScale;
    if (!GraphicUtils::readFramebufferRGBA(camera->getFramebuffer(), rgba) || rgba.size() < (size_t)probeWidth * frameHeight * kProbeScale * 4){
        failure = "the capture could not be read back";
        return true;
    }
    // each base pixel from its block's first screen pixel; a block that is not one colour is a base
    // pixel drawn between grid lines
    frame.image = PixelImage(frameWidth, frameHeight);
    for (int y = 0; y < frameHeight; y++){
        for (int x = 0; x < frameWidth; x++){
            const uint8_t* first = &rgba[((size_t)(y * kProbeScale) * probeWidth + (size_t)(x * kProbeScale)) * 4];
            bool even = true;
            for (int sy = 0; sy < kProbeScale && even; sy++){
                for (int sx = 0; sx < kProbeScale && even; sx++){
                    const uint8_t* q = &rgba[((size_t)(y * kProbeScale + sy) * probeWidth + (size_t)(x * kProbeScale + sx)) * 4];
                    even = std::memcmp(q, first, 4) == 0;
                }
            }
            frame.image.set(x, y, PixelRgba{first[0], first[1], first[2], first[3]});
            if (!even){
                frame.offGrid++;
                if (frame.offGridAt.size() < 16) frame.offGridAt.push_back({x, y});
            }
        }
    }
    collectParticles(frame);
    captured.push_back(std::move(frame));
    next++;
    if (next >= config.times.size()){
        release();
        return true;
    }
    seekTo(config.times[next]);
    Engine::executeSceneOnce(scene);
    rendering = true;
    return false;
}

PixelImage editor::pixelart::particleContactSheet(const std::vector<CapturedFrame>& frames, int frameWidth, int frameHeight, int scale, int columns,
                                                  jasperengine::pixelart::Rgba8 background, bool labels){
    const int n = (int)frames.size();
    if (n == 0 || frameWidth <= 0 || frameHeight <= 0)
        return PixelImage();
    scale = std::clamp(scale, 1, 8);
    if (columns <= 0) columns = (int)std::ceil(std::sqrt((double)n));
    columns = std::min(columns, n);
    const int rows = (n + columns - 1) / columns;
    const int labelScale = std::max(1, scale / 2);
    const int labelBand = labels ? 5 * labelScale + 2 : 0;
    const int cellW = frameWidth * scale, cellH = frameHeight * scale + labelBand;
    const int width = columns * cellW + (columns - 1), height = rows * cellH + (rows - 1);
    PixelImage sheet(width, height);
    sheet.fill(PixelRgba{32, 32, 32, 255});
    for (int f = 0; f < n; f++){
        const int ox = (f % columns) * (cellW + 1), oy = (f / columns) * (cellH + 1);
        const PixelImage& image = frames[(size_t)f].image;
        for (int y = 0; y < frameHeight * scale; y++){
            for (int x = 0; x < frameWidth * scale; x++){
                PixelRgba c = PixelRgba{background.r, background.g, background.b, 255};
                if (!image.empty()){
                    // the frame's premultiplied colour over the background
                    const PixelRgba f = image.at(x / scale, y / scale);
                    const int keep = 255 - f[3];
                    for (int k = 0; k < 3; k++){
                        const int bg = k == 0 ? background.r : (k == 1 ? background.g : background.b);
                        c[k] = (uint8_t)std::min(255, f[k] + (bg * keep + 127) / 255);
                    }
                }
                sheet.set(ox + x, oy + y, c);
            }
        }
        if (labels){
            char text[32];
            std::snprintf(text, sizeof(text), "#%d t=%.2fs", f + 1, frames[(size_t)f].time);
            cio::drawSheetText(sheet, text, ox + 1, oy + frameHeight * scale + 1, labelScale, PixelRgba{230, 230, 230, 255});
        }
    }
    return sheet;
}

PixelImage editor::pixelart::particleSpriteStrip(const std::vector<CapturedFrame>& frames, int frameWidth, int frameHeight, int scale, int padding){
    const int n = (int)frames.size();
    if (n == 0 || frameWidth <= 0 || frameHeight <= 0)
        return PixelImage();
    scale = std::clamp(scale, 1, 8);
    padding = std::max(padding, 0);
    const int cellW = frameWidth * scale, cellH = frameHeight * scale;
    PixelImage strip(n * cellW + (n - 1) * padding, cellH);
    for (int f = 0; f < n; f++){
        const PixelImage& image = frames[(size_t)f].image;
        for (int y = 0; y < cellH; y++){
            for (int x = 0; x < cellW; x++){
                // keyed on alpha (the frames are drawn on transparent black): the colour back from
                // premultiplied; an additive pixel that wrote no alpha has its brightest channel's
                const PixelRgba c = image.at(x / scale, y / scale);
                const int brightest = std::max<int>({c[0], c[1], c[2]});
                const int alpha = c[3] > 0 ? c[3] : brightest;
                PixelRgba out{0, 0, 0, 0};
                if (alpha > 0){
                    for (int k = 0; k < 3; k++) out[k] = (uint8_t)std::min(255, (c[k] * 255 + alpha / 2) / alpha);
                    out[3] = (uint8_t)alpha;
                }
                strip.set(f * (cellW + padding) + x, y, out);
            }
        }
    }
    return strip;
}

std::string editor::pixelart::particleSpriteStripJson(const std::string& imageName, const std::vector<CapturedFrame>& frames, int frameWidth,
                                                      int frameHeight, int scale, int padding){
    using Ordered = nlohmann::ordered_json;
    const int n = (int)frames.size();
    const int cellW = frameWidth * scale, cellH = frameHeight * scale;
    std::string stem = imageName;
    const size_t dot = stem.rfind('.');
    if (dot != std::string::npos) stem.resize(dot);
    Ordered list = Ordered::object();
    for (int f = 0; f < n; f++){
        // a frame lasts until the next one's time (the last as long as the one before it)
        const double next = f + 1 < n ? frames[(size_t)f + 1].time : (n > 1 ? frames[(size_t)f].time * 2.0 - frames[(size_t)f - 1].time : frames[(size_t)f].time + 0.1);
        const int duration = std::max(1, (int)std::lround((next - frames[(size_t)f].time) * 1000.0));
        Ordered frame = Ordered::object();
        frame["frame"] = {{"x", f * (cellW + padding)}, {"y", 0}, {"w", cellW}, {"h", cellH}};
        frame["rotated"] = false;
        frame["trimmed"] = false;
        frame["spriteSourceSize"] = {{"x", 0}, {"y", 0}, {"w", cellW}, {"h", cellH}};
        frame["sourceSize"] = {{"w", cellW}, {"h", cellH}};
        frame["duration"] = duration;
        list[stem + " " + std::to_string(f) + ".aseprite"] = frame;
    }
    Ordered json = Ordered::object();
    json["frames"] = list;
    json["meta"] = {{"app", "JasperEngine (Bake to Sprite Sheet)"}, {"version", "1"}, {"image", imageName}, {"format", "RGBA8888"},
                    {"size", {{"w", n * cellW + (n - 1) * padding}, {"h", cellH}}}, {"scale", "1"}, {"frameTags", Ordered::array()},
                    {"layers", Ordered::array()}, {"slices", Ordered::array()}};
    return json.dump(1, ' ') + "\n";
}
