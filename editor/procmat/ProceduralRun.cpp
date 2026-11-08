// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ProceduralRun.h"

#include "ProceduralGpuEvaluator.h"

#include "Engine.h"
#include "render/SystemRender.h"
#include "render/preview/MaterialRender.h"
#include "texture/Framebuffer.h"
#include "texture/TextureData.h"
#include "util/GraphicUtils.h"

#include <algorithm>
#include <cctype>
#include <chrono>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::procmat;

namespace {

    double runNow() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    std::vector<ProceduralRun*>& liveRuns() {
        static std::vector<ProceduralRun*> runs;
        return runs;
    }

    std::function<void()>& runFrameRequester() {
        static std::function<void()> requester;
        return requester;
    }

    // every run's ball textures get ids no other run has had: the texture pools answer by id
    uint64_t nextBallSerial() {
        static uint64_t serial = 0;
        return ++serial;
    }

}

struct ProceduralRun::Impl {
    enum class Stage { Evaluate, Sphere, Done };
    Stage stage = Stage::Evaluate;
    double deadline = 0.0;
    std::unique_ptr<ProceduralGpuEvaluator> evaluator;
    bool prepared = false;
    std::string device;
    EvaluatorTier tier = EvaluatorTier::Gpu;
    // the pixels the ball's textures read (TextureData does not own them): declared before the ball, so
    // they outlive it
    std::vector<std::vector<uint8_t>> pixels;
    std::unique_ptr<MaterialRender> ball;
    int draws = 0;

    void releaseBall() {
        if (!ball) return;
        Engine::removeScene(ball->getScene());
        ball.reset();
    }
};

ProceduralRun::ProceduralRun(ProceduralRunRequest request) : impl(std::make_unique<Impl>()), request(std::move(request)) {
    impl->device = SystemRender::getDeviceName();
    impl->tier = editorTier(impl->device);
}

ProceduralRun::~ProceduralRun() {
    auto& runs = liveRuns();
    runs.erase(std::remove(runs.begin(), runs.end(), this), runs.end());
    if (impl) impl->releaseBall();
}

EvaluatorTier ProceduralRun::editorTier(const std::string& deviceName) {
    std::string name = deviceName;
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    for (const char* software : {"llvmpipe", "lavapipe", "softpipe", "swiftshader", "swrast", "microsoft basic render", "apple software renderer"}) {
        if (name.find(software) != std::string::npos) return EvaluatorTier::Software;
    }
    return EvaluatorTier::Gpu;
}

EvaluatorTier ProceduralRun::tier() const {
    return impl->tier;
}

std::string ProceduralRun::deviceName() const {
    return impl->device;
}

std::string ProceduralRun::getStage() const {
    switch (impl->stage) {
        case Impl::Stage::Evaluate: return "evaluating the graph";
        case Impl::Stage::Sphere: return "drawing the lit ball";
        case Impl::Stage::Done: return "done";
    }
    return "done";
}

void ProceduralRun::start() {
    if (started) return;
    started = true;
    impl->deadline = runNow() + request.timeoutSeconds;
    impl->stage = request.plan ? Impl::Stage::Evaluate : (request.sphere ? Impl::Stage::Sphere : Impl::Stage::Done);
    if (!request.plan) outputs = request.images;
    liveRuns().push_back(this);
    if (runFrameRequester()) runFrameRequester()();
}

void ProceduralRun::cancel() {
    if (finished) return;
    if (impl->evaluator) impl->evaluator->cancel();
    finish("Cancelled.");
}

void ProceduralRun::finish(const std::string& failure) {
    if (finished) return;
    error = failure;
    finished = true;
    impl->stage = Impl::Stage::Done;
    impl->evaluator.reset();
    impl->releaseBall();
    auto& runs = liveRuns();
    runs.erase(std::remove(runs.begin(), runs.end(), this), runs.end());
    if (onFinished) onFinished();
}

void ProceduralRun::advance(double budgetMs) {
    if (runNow() > impl->deadline) {
        const std::string waited = impl->stage == Impl::Stage::Sphere ? "the lit ball still waited for its shader or textures" : "the graph was still evaluating";
        finish("Not ready within " + std::to_string((int)request.timeoutSeconds) + " s (" + waited +
               "): no image is returned rather than an older one. Call again, with a larger timeout_seconds.");
        return;
    }
    if (impl->stage == Impl::Stage::Evaluate) {
        if (!impl->prepared) {
            impl->evaluator = std::make_unique<ProceduralGpuEvaluator>(impl->tier);
            std::string prepareError;
            if (!impl->evaluator->prepare(*request.plan, prepareError)) {
                finish("The " + impl->device + " device cannot run this graph: " + prepareError);
                return;
            }
            impl->prepared = true;
        }
        EvaluationResult result;
        if (!impl->evaluator->advance(budgetMs, result)) return;
        if (!result.ok) {
            std::string failure = result.error;
            if (result.failedPass >= 0 && (size_t)result.failedPass < request.plan->passes.size()) {
                failure = "pass " + std::to_string(result.failedPass) + " (" + request.plan->passes[(size_t)result.failedPass].label + ") failed on " +
                          impl->device + ": " + result.error;
            }
            if (!result.log.empty()) failure += "\n" + result.log;
            finish(failure);
            return;
        }
        outputs = std::move(result.outputs);
        impl->evaluator.reset();
        impl->stage = request.sphere ? Impl::Stage::Sphere : Impl::Stage::Done;
    }
    if (impl->stage == Impl::Stage::Sphere) {
        if (!impl->ball) {
            Material material = request.material;
            if (request.plan) {
                material = Material();
                const uint64_t serial = nextBallSerial();
                for (const OutputImage& image : outputs) {
                    impl->pixels.push_back(image.rgba);
                    std::vector<uint8_t>& bytes = impl->pixels.back();
                    Texture texture;
                    texture.setData("procmat-run|" + std::to_string(serial) + "|" + image.name,
                                    TextureData(image.width, image.height, (unsigned int)bytes.size(), ColorFormat::RGBA, 4, bytes.data()));
                    if (image.name == "base_color") material.baseColorTexture = texture;
                    else if (image.name == "normal") material.normalTexture = texture;
                    else if (image.name == "emission") {
                        material.emissiveTexture = texture;
                        material.emissiveFactor = Vector3(1.0f, 1.0f, 1.0f);
                    } else if (image.name == "orm") {
                        material.metallicRoughnessTexture = texture;
                        material.occlusionTexture = texture;
                    }
                }
            }
            impl->ball = std::make_unique<MaterialRender>();
            impl->ball->applyMaterial(material);
            Engine::executeSceneOnce(impl->ball->getScene());
            impl->draws = 1;
            return;
        }
        if (Engine::isSceneRunning(impl->ball->getScene())) return;
        std::error_code ec;
        const bool stalled = !request.pendingMarker.empty() && std::filesystem::exists(request.pendingMarker, ec);
        if (impl->ball->isPending() || stalled) {
            Engine::executeSceneOnce(impl->ball->getScene());
            impl->draws++;
            return;
        }
        Framebuffer* framebuffer = impl->ball->getFramebuffer();
        std::vector<uint8_t> rgba;
        if (!framebuffer || !GraphicUtils::readFramebufferRGBA(framebuffer, rgba) || rgba.size() != (size_t)framebuffer->getWidth() * framebuffer->getHeight() * 4u) {
            finish("The lit ball could not be read back from the " + impl->device + " device.");
            return;
        }
        sphereImage.name = "sphere";
        sphereImage.width = (int)framebuffer->getWidth();
        sphereImage.height = (int)framebuffer->getHeight();
        sphereImage.rgba = std::move(rgba);
        impl->stage = Impl::Stage::Done;
    }
    if (impl->stage == Impl::Stage::Done) finish(std::string());
}

bool ProceduralRun::processFrame() {
    // a copy: a finished run's callback may destroy runs
    const std::vector<ProceduralRun*> runs = liveRuns();
    const double frameStart = runNow();
    for (ProceduralRun* run : runs) {
        auto& live = liveRuns();
        if (std::find(live.begin(), live.end(), run) == live.end() || run->finished) continue;
        const double left = FRAME_BUDGET_MS - (runNow() - frameStart) * 1000.0;
        run->advance(std::max(left, 1.0));
    }
    const bool running = !liveRuns().empty();
    // the passes drawn here are committed as a view's are: a read back waits for the commit of the frame
    // that drew, and an editor whose scenes are idle commits nothing else
    if (running && Engine::isViewLoaded()) SystemRender::commit();
    if (running && runFrameRequester()) runFrameRequester()();
    return running;
}

void ProceduralRun::setFrameRequester(std::function<void()> requester) {
    runFrameRequester() = std::move(requester);
}

void ProceduralRun::releaseGpu() {
    const std::vector<ProceduralRun*> runs = liveRuns();
    for (ProceduralRun* run : runs) {
        auto& live = liveRuns();
        if (std::find(live.begin(), live.end(), run) != live.end()) run->finish("The editor is closing.");
    }
}
