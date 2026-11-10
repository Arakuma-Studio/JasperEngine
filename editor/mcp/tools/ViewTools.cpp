// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// W3: screenshots, Scene view camera, window focus, Game view settings
#include "Screen.h"
#include "window/widget/InputTextContextMenu.h"
#include "window/AnimationWindow.h"
#include "window/AnimatorWindow.h"
#include "window/GraphEditorWindow.h"
#include "graph/GraphEditorHooks.h"
#include "graph/GraphRegistry.h"
#include "window/ParticleEffectWindow.h"
#include "window/HierarchyWindow.h"
#include "window/InspectorWindow.h"
#include "window/ProjectWindow.h"
#include "window/SpriteNormalsWindow.h"
#include "window/TitleDesignerWindow.h"
#include "titleart/anim/TitleAnimationPanel.h"
#include "window/TerrainEditWindow.h"
#include "window/TextureSynthesisWindow.h"
#include "window/VertexAnimationBakerWindow.h"
#include "registry/PhysicsLayers.h"
#include "McpTools.h"
#include "texture/TextureCookQueue.h"
#include "window/dialog/EditorSettingsWindow.h"
#include "Out.h"
#include "io/AtomicFile.h"
#include "PlayViewCommon.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpValues.h"

#include "App.h"
#include "Backend.h"
#include "AppSettings.h"
#include "Engine.h"
#include "Screen.h"
#include "Platform.h"
#include "Project.h"
#include "net/Http.h"
#include "mcp/McpActivityWindow.h"
#include "component/CameraComponent.h"
#include "render/SceneRender.h"
#include "render/SceneRender2D.h"
#include "pool/ShaderPool.h"
#include "pool/TextureDataPool.h"
#include "render/SystemRender.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/UISystem.h"
#include "subsystem/UIDocumentSystem.h"
#include "object/ui/UIDocument.h"
#include "util/Base64.h"
#include "util/FileUtils.h"
#include "util/SafeWrite.h"
#include "util/GraphicUtils.h"
#include "util/PlatformRules.h"
#include "window/DeviceProfiles.h"
#include "window/GameResolutions.h"
#include "window/ConsoleWindow.h"
#include "window/GameWindow.h"
#include "window/JobsMemoryWindow.h"
#include "window/profiler/FrameDebuggerWindow.h"
#include "window/profiler/ProfilerWindow.h"
#include "window/PlayModeChangesWindow.h"
#include "window/PoseAnimationWindow.h"
#include "window/SceneWindow.h"
#include "window/SystemsWindow.h"
#include "window/UndoHistoryWindow.h"
#include "util/UiItemLocator.h"
#include "pose/PoseAnimationDocument.h"
#include "pose/PoseViewport.h"

#include "imgui.h"
#include "imgui_internal.h"

#include "stb_image.h"
#include "stb_image_resize2.h"
#include "stb_image_write.h"

#if defined(SOKOL_GLCORE) || defined(SOKOL_GLES3)
    #if defined(_WIN32)
        #include <windows.h>
        #include <GL/gl.h>
    #elif defined(__APPLE__)
        #include <OpenGL/gl.h>
    #else
        #include <GL/gl.h>
    #endif
#endif

#include <algorithm>
#include <cstdio>
#include <chrono>
#include <cmath>
#include <fstream>
#include <memory>

namespace fs = std::filesystem;

namespace jasperengine::editor::mcp {

bool w3::isFlatPng(const std::vector<uint8_t>& png, std::string& colour) {
    // what dithering and a resize leave on a blank render, in levels of 255. The whole image's
    // range counts: a slow gradient is close between neighbours and far from end to end.
    constexpr int FLAT_TOLERANCE = 2;
    int w = 0, h = 0, n = 0;
    unsigned char* pixels = stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &w, &h, &n, 3);
    if (!pixels) return false;
    int low[3] = {255, 255, 255};
    int high[3] = {0, 0, 0};
    bool flat = true;
    for (int i = 0; i < w * h && flat; i++) {
        for (int c = 0; c < 3; c++) {
            low[c] = std::min<int>(low[c], pixels[i * 3 + c]);
            high[c] = std::max<int>(high[c], pixels[i * 3 + c]);
            if (high[c] - low[c] > FLAT_TOLERANCE) flat = false;
        }
    }
    if (flat && w * h > 0) {
        char buffer[16];
        snprintf(buffer, sizeof(buffer), "#%02x%02x%02x", pixels[0], pixels[1], pixels[2]);
        colour = buffer;
    }
    stbi_image_free(pixels);
    return flat;
}

namespace {

using Clock = std::chrono::steady_clock;

bool sceneContentLoaded(SceneProject* sp) {
    if (!sp || !sp->scene) return false;
    auto rs = sp->scene->getSystem<RenderSystem>();
    if (rs && !rs->isAllLoaded()) return false;
    auto us = sp->scene->getSystem<UISystem>();
    if (us && us->hasPendingImages()) return false;
    // its HTML documents loaded (or failed) and drawn once: the first frame waits for the HTML UI
    // shaders, which are made when a document first draws (on a software rasterizer, seconds)
    auto ds = sp->scene->getSystem<UIDocumentSystem>();
    if (ds && !ds->isAllLoaded()) return false;
    // pinned stateful UI effects still replaying their state (phase 6): drawn a few frames later
    if (UIDocument::getPendingEffectReplays() > 0) return false;
    auto ms = sp->scene->getSystem<MeshSystem>();
    if (ms && ms->hasPendingAsyncModelLoads()) return false;
    // a frame drawn while a texture loads shows a stand-in for it (the texture before, or none)
    if (TextureDataPool::getLoadingCount() > 0) return false;
    // and while a queued upload waits for the upload budget (spec S56 R8), its object is not drawn
    if (SystemRender::getDeferredUploadCount() > 0) return false;
    return true;
}

// The asset loads a screenshot always waits for (a texture, a model): it never shows a stand-in
// for one, and past the call's timeout it fails naming them
bool assetsLoading(SceneProject* sp) {
    if (TextureDataPool::getLoadingCount() > 0) return true;
    if (SystemRender::getDeferredUploadCount() > 0) return true;
    auto ms = sp && sp->scene ? sp->scene->getSystem<MeshSystem>() : nullptr;
    return ms && ms->hasPendingAsyncModelLoads();
}

// The shaders, textures and scene objects a screenshot is still waiting for, for its error
std::string describeLoading(SceneProject* sp) {
    std::string out;
    auto add = [&out](const std::string& part) { out += (out.empty() ? "" : "; ") + part; };
    if (!ShaderPool::isCreationIdle()) add("shaders or pipelines being made");
    if (const size_t uploads = SystemRender::getDeferredUploadCount())
        add(std::to_string(uploads) + " queued upload(s) waiting for the upload budget");
    if (const size_t textures = TextureDataPool::getLoadingCount()) {
        std::string list;
        for (const std::string& id : TextureDataPool::getLoadingIds(8)) list += (list.empty() ? "" : ", ") + id;
        add(std::to_string(textures) + " texture load(s): " + list);
    }
    if (sp && sp->scene) {
        if (auto rs = sp->scene->getSystem<RenderSystem>()) {
            const std::vector<std::string> waiting = rs->describeNotLoaded();
            std::string list;
            for (size_t i = 0; i < waiting.size() && i < 8; i++) list += (i ? ", " : "") + waiting[i];
            if (waiting.size() > 8) list += " and " + std::to_string(waiting.size() - 8) + " more";
            if (!list.empty()) add("not loaded: " + list);
        }
        if (auto us = sp->scene->getSystem<UISystem>(); us && us->hasPendingImages()) add("UI images loading");
        if (auto ds = sp->scene->getSystem<UIDocumentSystem>(); ds && !ds->isAllLoaded()) add("HTML UI documents loading or not drawn yet");
        if (UIDocument::getPendingEffectReplays() > 0) add("UI effects replaying their state");
        if (auto ms = sp->scene->getSystem<MeshSystem>(); ms && ms->hasPendingAsyncModelLoads()) add("models loading");
    }
    return out.empty() ? std::string("nothing (it settled as the wait ended)") : out;
}

bool isTransitional(ScenePlayState state) {
    return state == ScenePlayState::LOADING || state == ScenePlayState::SAVING || state == ScenePlayState::CANCELLING;
}

std::string sceneTabName(Project* project, uint32_t sceneId) {
    if (project->isPrefabStage(sceneId)) return SceneWindow::PREFAB_STAGE_WINDOW_ID;
    return "###Scene" + std::to_string(sceneId);
}

void focusSceneTab(Project* project, uint32_t sceneId) {
    ImGui::SetWindowFocus(sceneTabName(project, sceneId).c_str());
}

bool sceneViewShown(McpContext& ctx, SceneProject* sp) {
    SceneWindow* sw = ctx.sceneWindow();
    return sp && sw && sp->isVisible && sw->getWidth(sp->id) > 0 && sw->getHeight(sp->id) > 0;
}

// The scene the Scene view tools act on: given, else the playing one, else the selected one
SceneProject* resolveViewScene(Project* project, const Json& ref, std::string& error) {
    if (!project) {
        error = "No project is open.";
        return nullptr;
    }
    if (ref.is_null()) {
        const uint32_t playId = project->getPlaySessionSceneId();
        if (playId != NULL_PROJECT_SCENE) return project->getScene(playId);
    }
    SceneProject* sp = McpRefs::resolveScene(project, ref, error);
    if (!sp) return nullptr;
    if (!sp->opened && !project->isPrefabStage(sp->id)) {
        error = "Scene " + std::to_string(sp->id) + " \"" + sp->name + "\" is not open in a Scene view tab; open it first.";
        return nullptr;
    }
    if (!sp->scene || !sp->sceneRender) {
        error = "Scene " + std::to_string(sp->id) + " \"" + sp->name + "\" is not loaded.";
        return nullptr;
    }
    return sp;
}

// Brings sceneId's Scene view to the front if needed and runs then() once it is laid out
void whenSceneViewReady(const CallPtr& call, uint32_t sceneId, bool focus, int timeoutMs,
                        std::function<void(bool ready, const std::string& error)> then) {
    McpContext& ctx = call->ctx();
    Project* project = ctx.project();
    SceneProject* sp = project->getScene(sceneId);
    if (sceneViewShown(ctx, sp)) {
        then(true, {});
        return;
    }
    if (!focus) {
        then(false, "The Scene view of \"" + (sp ? sp->name : std::string("?")) +
                    "\" is hidden behind another tab; pass focus: true (or call focus_window) to show it.");
        return;
    }
    focusSceneTab(project, sceneId);
    auto frames = std::make_shared<int>(0);
    call->waitUntil([call, sceneId, frames]() {
        McpContext& c = call->ctx();
        // ImGui docks a newly focused tab a frame later; ask again while it isn't shown
        if (++*frames % 10 == 5) focusSceneTab(c.project(), sceneId);
        return sceneViewShown(c, c.project()->getScene(sceneId));
    }, timeoutMs, [then](bool timedOut) {
        if (timedOut) then(false, "The Scene view did not become visible (is the editor window minimized or the tab closed?).");
        else then(true, {});
    });
}

// ------------------------------------------------------------------ capture

// How long a screenshot waits for the rest of the scene's content to settle (objects, HTML UI
// documents drawn once, stateful effects replaying: some never report it) before it shoots
constexpr long long SETTLE_WAIT_MS = 6000;

struct CaptureJob {
    CallPtr call;
    w3::CaptureRequest req;
    std::function<void(w3::CaptureResult)> done;
    Clock::time_point deadline;
    bool finished = false;
    // the rest of the content had not settled when the wait for it gave up
    bool stillSettling = false;
    Clock::time_point settleWaitStart;
    bool settleWaitStarted = false;

    // Its asset loads done (always waited for: up to the call's timeout_seconds, then the
    // screenshot fails naming them), and the rest settled or waited for SETTLE_WAIT_MS
    bool contentReady(SceneProject* sp) {
        if (assetsLoading(sp)) return false;
        if (sceneContentLoaded(sp)) {
            stillSettling = false;
            return true;
        }
        if (!settleWaitStarted) {
            settleWaitStarted = true;
            settleWaitStart = Clock::now();
        }
        const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - settleWaitStart).count();
        stillSettling = true;
        return waited >= SETTLE_WAIT_MS;
    }

    int remainingMs() const {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        return static_cast<int>(std::max<long long>(left, 50));
    }
    void finish(w3::CaptureResult result) {
        if (finished) return;
        finished = true;
        done(std::move(result));
    }
    void fail(const std::string& error) {
        w3::CaptureResult result;
        result.ok = false;
        result.error = error;
        finish(std::move(result));
    }
};
using CaptureJobPtr = std::shared_ptr<CaptureJob>;

void captureGame(const CaptureJobPtr& job);
void captureScene(const CaptureJobPtr& job);

// RGB(A) rows → PNG, scaled down to maxWidth; analyses flatness on the way
[[maybe_unused]] bool encodePixels(const uint8_t* pixels, int width, int height, int channels, int maxWidth,
                  std::vector<uint8_t>& png, int& outWidth, int& outHeight) {
    std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3);
    for (int i = 0; i < width * height; i++) {
        rgb[i * 3 + 0] = pixels[i * channels + 0];
        rgb[i * 3 + 1] = pixels[i * channels + 1];
        rgb[i * 3 + 2] = pixels[i * channels + 2];
    }
    outWidth = width;
    outHeight = height;
    if (maxWidth > 0 && width > maxWidth) {
        outWidth = maxWidth;
        outHeight = std::max(1, static_cast<int>(static_cast<long long>(height) * maxWidth / width));
        std::vector<uint8_t> scaled(static_cast<size_t>(outWidth) * outHeight * 3);
        if (!stbir_resize_uint8_srgb(rgb.data(), width, height, 0, scaled.data(), outWidth, outHeight, 0, STBIR_RGB)) {
            return false;
        }
        rgb.swap(scaled);
    }
    png.clear();
    auto append = [](void* context, void* data, int size) {
        auto* bytes = static_cast<std::vector<uint8_t>*>(context);
        const uint8_t* begin = static_cast<const uint8_t*>(data);
        bytes->insert(bytes->end(), begin, begin + size);
    };
    return stbi_write_png_to_func(append, &png, outWidth, outHeight, 3, rgb.data(), outWidth * 3) != 0;
}

// Fills image + common info, writes save_path, and finishes the job
void finishWithPng(const CaptureJobPtr& job, std::vector<uint8_t> png, int width, int height, int sourceWidth,
                   int sourceHeight, Json info) {
    w3::CaptureResult result;
    result.ok = true;
    info["view"] = job->req.view;
    info["width"] = width;
    info["height"] = height;
    info["source_width"] = sourceWidth;
    info["source_height"] = sourceHeight;
    info["bytes"] = png.size();

    std::string colour;
    if (w3::isFlatPng(png, colour)) {
        info["flat_colour"] = colour;
        result.note = "The image is one flat colour (" + colour + "): nothing visible rendered there yet?";
    }
    if (job->stillSettling) {
        info["assets_loading"] = true;
        result.note += std::string(result.note.empty() ? "" : " ") +
                       "Some content was still settling after " + std::to_string(SETTLE_WAIT_MS / 1000) +
                       " s (objects, HTML UI documents or effects not drawn yet); parts of the scene may be missing.";
    }

    if (!job->req.savePath.empty()) {
        Project* project = job->call->ctx().project();
        fs::path target;
        std::string error;
        // Inside the project only, like every file a tool writes (also once links are resolved),
        // and written beneath it: no link planted on the way since the check is followed
        if (!McpRefs::resolveProjectWritePath(project, job->req.savePath, target, error)) {
            job->fail("save_path: " + error);
            return;
        }
        // not an editor-owned or hidden file (project.yaml, .jasperengine, CMakeLists.txt ...): a
        // screenshot is an asset, never a path the editor owns
        if (const std::string owned = McpRefs::protectedPlace(project, target); !owned.empty()) {
            job->fail("save_path: " + owned);
            return;
        }
        // beneath the project: no link planted on the way since the check is followed
        if (!safewrite::writeFileResolvedBeneath(McpRefs::rootFor(project, target), target, std::string(png.begin(), png.end()), error)) {
            job->fail("Could not write the PNG to " + FileUtils::pathToGenericUtf8(target) + ": " + error);
            return;
        }
        info["saved_to"] = FileUtils::pathToGenericUtf8(target);
    }

    result.image.data = Base64::encode(png.data(), png.size());
    result.image.mimeType = "image/png";
    result.info = std::move(info);
    job->finish(std::move(result));
}

void readFramebuffer(const CaptureJobPtr& job, Framebuffer* framebuffer, Json info) {
    std::vector<uint8_t> png;
    int width = 0, height = 0;
    if (!framebuffer || !framebuffer->isCreated()) {
        job->fail("The view has not rendered yet (no framebuffer).");
        return;
    }
    const int sourceWidth = static_cast<int>(framebuffer->getWidth());
    const int sourceHeight = static_cast<int>(framebuffer->getHeight());
    if (!GraphicUtils::encodeFramebufferPng(framebuffer, png, false, job->req.maxWidth, &width, &height)) {
        job->fail("Reading the view's framebuffer back failed.");
        return;
    }
    finishWithPng(job, std::move(png), width, height, sourceWidth, sourceHeight, std::move(info));
}

Json rectJson(const Rect& rect, double scale) {
    return {{"x", std::round(rect.getX() * scale)}, {"y", std::round(rect.getY() * scale)},
            {"width", std::round(rect.getWidth() * scale)}, {"height", std::round(rect.getHeight() * scale)}};
}

// Where the game canvas sits in the image (letterbox bars around it), for mapping image
// points to send_input coordinates
void addCanvasInfo(Json& info, int canvasWidth, int canvasHeight, const Rect& viewRect, int sourceWidth, int maxWidth) {
    const double scale = (maxWidth > 0 && sourceWidth > maxWidth) ? double(maxWidth) / sourceWidth : 1.0;
    info["canvas"] = {{"width", canvasWidth}, {"height", canvasHeight}};
    info["canvas_rect_in_image"] = rectJson(viewRect, scale);
}

void readGameView(const CaptureJobPtr& job, uint32_t sceneId, bool running) {
    McpContext& ctx = job->call->ctx();
    Project* project = ctx.project();
    GameWindow* gw = ctx.gameWindow();
    SceneProject* sp = project->getScene(sceneId);
    Framebuffer* fb = gw->getFramebuffer();
    Json info = {{"scene", sp ? McpRefs::sceneJson(project, *sp) : Json(nullptr)},
                 {"playing", running}};
    if (fb && fb->isCreated()) {
        const int fbWidth = static_cast<int>(fb->getWidth());
        const int fbHeight = static_cast<int>(fb->getHeight());
        if (running) {
            addCanvasInfo(info, Engine::getCanvasWidth(), Engine::getCanvasHeight(), Engine::getViewRect(), fbWidth,
                          job->req.maxWidth);
        } else {
            int canvasWidth = 0, canvasHeight = 0;
            Rect viewRect;
            Engine::computeViewLayout(fbWidth, fbHeight, (int)project->getCanvasWidth(), (int)project->getCanvasHeight(),
                                      project->getScalingMode(), canvasWidth, canvasHeight, viewRect);
            addCanvasInfo(info, canvasWidth, canvasHeight, viewRect, fbWidth, job->req.maxWidth);
        }
        // a simulated device: where its safe area is in the image, and its screen drawn over it
        const DeviceProfile* device = Platform::getSimulation().device;
        Rect safe;
        if (device && gw->getSafeAreaInImage((float)fbWidth, (float)fbHeight, safe)) {
            const double scale = (job->req.maxWidth > 0 && fbWidth > job->req.maxWidth) ? double(job->req.maxWidth) / fbWidth : 1.0;
            info["device"] = device->id;
            info["safe_area_in_image"] = rectJson(safe, scale);
            if (job->req.deviceOverlay && (gw->getSettings().deviceOverlay || gw->getSettings().safeAreaOutline)) {
                std::vector<uint8_t> rgba;
                if (!GraphicUtils::readFramebufferRGBA(fb, rgba)) {
                    job->fail("Reading the view's framebuffer back failed.");
                    return;
                }
                rasterizeDeviceOverlay(rgba.data(), fbWidth, fbHeight, *device, gw->getLayoutOrientation(),
                                       gw->getSettings().safeAreaOutline, &safe, gw->getSettings().deviceOverlay);
                std::vector<uint8_t> png;
                int width = 0, height = 0;
                if (!encodePixels(rgba.data(), fbWidth, fbHeight, 4, job->req.maxWidth, png, width, height)) {
                    job->fail("Encoding the screenshot failed.");
                    return;
                }
                // the device's screen drawn (false: the safe-area outline only)
                info["device_overlay"] = gw->getSettings().deviceOverlay;
                finishWithPng(job, std::move(png), width, height, fbWidth, fbHeight, std::move(info));
                return;
            }
        }
    }
    readFramebuffer(job, fb, std::move(info));
}

void captureGame(const CaptureJobPtr& job) {
    const CallPtr& call = job->call;
    McpContext& ctx = call->ctx();
    Project* project = ctx.project();
    GameWindow* gw = ctx.gameWindow();
    if (!project || !gw) {
        job->fail("No project or Game view.");
        return;
    }

    const uint32_t playId = project->getPlaySessionSceneId();
    if (playId != NULL_PROJECT_SCENE) {
        SceneProject* sp = project->getScene(playId);
        if (!sp) {
            job->fail("The playing scene is gone.");
            return;
        }
        if (isTransitional(sp->playState)) {
            // Starting (maybe compiling C++) or stopping: shoot once it settles
            call->waitUntil([call, playId]() {
                Project* p = call->ctx().project();
                SceneProject* s = p->getScene(playId);
                return p->getPlaySessionSceneId() != playId || !s || !isTransitional(s->playState);
            }, job->remainingMs(), [job](bool timedOut) {
                if (timedOut) job->fail("Play mode is still starting or stopping; call play with wait (or get_play_state) first.");
                else captureGame(job);
            });
            return;
        }
        // PLAYING redraws every frame; PAUSED only on demand
        if (sp->playState == ScenePlayState::PAUSED) sp->needUpdateRender = true;
        auto frames = std::make_shared<int>(0);
        call->waitUntil([call, frames, playId]() {
            McpContext& c = call->ctx();
            SceneProject* s = c.project()->getScene(playId);
            if (!s || c.project()->getPlaySessionSceneId() != playId) return true;
            if (s->playState == ScenePlayState::PAUSED) s->needUpdateRender = true;
            // (two frames after its textures and shaders are in: drawn with them)
            const bool loading = TextureDataPool::getLoadingCount() > 0 || !ShaderPool::isCreationIdle();
            *frames = loading ? 0 : *frames + 1;
            return *frames >= 2 && c.gameWindow()->getFramebuffer()->isCreated();
        }, job->remainingMs(), [job, playId](bool timedOut) {
            McpContext& c = job->call->ctx();
            if (c.project()->getPlaySessionSceneId() != playId) {
                // play ended meanwhile (a crash?): shoot the stopped view
                captureGame(job);
                return;
            }
            if (timedOut) {
                if (TextureDataPool::getLoadingCount() > 0 || !ShaderPool::isCreationIdle())
                    job->fail("The running game was not shot: still loading after the call's timeout_seconds (a screenshot "
                              "never shows a stand-in for it): " + describeLoading(nullptr) + ". Retry, or raise timeout_seconds.");
                else
                    job->fail("The running game did not render a frame in time.");
                return;
            }
            readGameView(job, playId, true);
        });
        return;
    }

    // Stopped: the Game view previews the selected scene through its game camera, only
    // while the view is shown
    SceneProject* sp = nullptr;
    if (!job->req.scene.is_null()) {
        std::string error;
        sp = McpRefs::resolveScene(project, job->req.scene, error);
        if (!sp) {
            job->fail(error);
            return;
        }
        if (sp->id != project->getSelectedSceneId()) {
            if (!sp->opened) {
                job->fail("Scene \"" + sp->name + "\" is not open; open it before previewing it in the Game view.");
                return;
            }
            project->setSelectedSceneId(sp->id);
        }
    } else {
        sp = project->getScene(project->getSelectedSceneId());
    }
    if (!sp || !sp->scene) {
        job->fail("No scene is open: the Game view has nothing to show.");
        return;
    }
    if (project->getGameCamera(sp->id) == NULL_ENTITY) {
        job->fail("Scene \"" + sp->name + "\" has no camera rendering (the Game view shows 'No cameras rendering'). "
                  "Add a Camera GameObject or set the scene's main camera.");
        return;
    }
    if (!gw->isVisible()) {
        if (!job->req.focus) {
            job->fail("The Game view is hidden (closed or behind another tab) and only renders while shown; "
                      "pass focus: true or call focus_window {window: \"game\"}.");
            return;
        }
        gw->requestFocus();
    }
    const uint32_t sceneId = sp->id;
    sp->needUpdateRender = true;
    auto frames = std::make_shared<int>(0);
    auto stable = std::make_shared<int>(0);
    call->waitUntil([call, job, sceneId, frames, stable]() {
        McpContext& c = call->ctx();
        GameWindow* g = c.gameWindow();
        SceneProject* s = c.project()->getScene(sceneId);
        if (!s || c.project()->getPlaySessionSceneId() != NULL_PROJECT_SCENE) return true;
        // drawn again each frame of the wait: a texture that finished loading shows from the
        // frame that draws it next (an idle editor would not)
        s->needUpdateRender = true;
        if (++*frames < 2) return false;
        if (*frames % 15 == 0 && !g->isVisible()) g->requestFocus();
        const bool ready = g->isVisible() && g->getWidth() > 0 && g->getHeight() > 0 && !g->isEditPassDirty() &&
                           job->contentReady(s);
        *stable = ready ? *stable + 1 : 0;
        return *stable >= 3;
    }, job->remainingMs(), [job, sceneId](bool timedOut) {
        McpContext& c = job->call->ctx();
        if (c.project()->getPlaySessionSceneId() != NULL_PROJECT_SCENE) {
            captureGame(job);  // play started meanwhile
            return;
        }
        if (timedOut) {
            GameWindow* g = c.gameWindow();
            if (!g->isVisible()) job->fail("The Game view did not become visible.");
            else if (g->getWidth() <= 0 || g->getHeight() <= 0) job->fail("The Game view has no size (too small or collapsed).");
            else {
                SceneProject* s = c.project()->getScene(sceneId);
                if (assetsLoading(s))
                    job->fail("The Game view was not shot: still loading after the call's timeout_seconds (a screenshot never "
                              "shows a stand-in for it): " + describeLoading(s) + ". Retry, or raise timeout_seconds.");
                else
                    job->fail("The Game view preview did not finish rendering in time (preview current " +
                              std::to_string(!g->isEditPassDirty()) + ", content loaded " + std::to_string(sceneContentLoaded(s)) +
                              ", UI effect replays pending " + std::to_string(UIDocument::getPendingEffectReplays()) + ")");
            }
            return;
        }
        readGameView(job, sceneId, false);
    });
}

void captureScene(const CaptureJobPtr& job) {
    const CallPtr& call = job->call;
    McpContext& ctx = call->ctx();
    Project* project = ctx.project();
    std::string error;
    SceneProject* sp = resolveViewScene(project, job->req.scene, error);
    if (!sp) {
        job->fail(error);
        return;
    }
    const uint32_t sceneId = sp->id;
    if (isTransitional(sp->playState)) {
        call->waitUntil([call, sceneId]() {
            SceneProject* s = call->ctx().project()->getScene(sceneId);
            return !s || !isTransitional(s->playState);
        }, job->remainingMs(), [job](bool timedOut) {
            if (timedOut) job->fail("The scene is still starting or stopping Play mode.");
            else captureScene(job);
        });
        return;
    }

    whenSceneViewReady(call, sceneId, job->req.focus, job->remainingMs(), [job, sceneId](bool ready, const std::string& err) {
        if (!ready) {
            job->fail(err);
            return;
        }
        const CallPtr& c = job->call;
        SceneProject* s = c->ctx().project()->getScene(sceneId);
        if (!s) {
            job->fail("The scene closed.");
            return;
        }
        s->needUpdateRender = true;
        auto frames = std::make_shared<int>(0);
        auto stable = std::make_shared<int>(0);
        c->waitUntil([c, job, sceneId, frames, stable]() {
            McpContext& cx = c->ctx();
            SceneProject* sc = cx.project()->getScene(sceneId);
            if (!sc) return true;
            // drawn again each frame of the wait: a texture that finished loading shows from the
            // frame that draws it next (an idle editor would not)
            sc->needUpdateRender = true;
            if (++*frames < 2) return false;
            // The view redraws on the frame after it is flagged (needUpdateRender) and keeps
            // redrawing for a few settle frames, so a few shown frames in a row mean the
            // framebuffer holds a fresh image. (The flag itself is not a signal: editor
            // windows raise it again every frame in places.)
            const bool ready = sceneViewShown(cx, sc) && job->contentReady(sc);
            *stable = ready ? *stable + 1 : 0;
            return *stable >= 3;
        }, job->remainingMs(), [job, sceneId](bool timedOut) {
            McpContext& cx = job->call->ctx();
            Project* p = cx.project();
            SceneProject* sc = p->getScene(sceneId);
            if (!sc || !sc->sceneRender) {
                job->fail("The scene closed.");
                return;
            }
            if (timedOut) {
                SceneWindow* sw = cx.sceneWindow();
                if (sceneViewShown(cx, sc) && assetsLoading(sc)) {
                    job->fail("The Scene view was not shot: still loading after the call's timeout_seconds (a screenshot never "
                              "shows a stand-in for it): " + describeLoading(sc) + ". Retry, or raise timeout_seconds.");
                    return;
                }
                job->fail(std::string(sceneViewShown(cx, sc) ? "The Scene view did not finish rendering in time."
                                                             : "The Scene view is not shown.") +
                          " (visible " + std::to_string(sc->isVisible) + ", size " + std::to_string(sw->getWidth(sceneId)) + "x" +
                          std::to_string(sw->getHeight(sceneId)) + ", content loaded " + std::to_string(sceneContentLoaded(sc)) + ")");
                return;
            }
            Json info = {{"scene", McpRefs::sceneJson(p, *sc)}};
            if (Camera* camera = sc->sceneRender->getCamera()) {
                info["camera"] = {{"position", McpValues::toJson(camera->getPosition())},
                                  {"target", McpValues::toJson(camera->getTarget())}};
                if (sc->sceneType != SceneType::SCENE_3D) {
                    info["camera"]["zoom"] = static_cast<SceneRender2D*>(sc->sceneRender)->getZoom();
                }
            }
            // where the view sits in the editor window: maps the image to pointer positions
            float vx = 0.0f, vy = 0.0f, vw = 0.0f, vh = 0.0f;
            if (SceneWindow* sw = cx.sceneWindow(); sw && sw->getViewRect(sceneId, vx, vy, vw, vh)) {
                info["view_rect_in_window"] = {{"x", std::round(vx)}, {"y", std::round(vy)},
                                               {"width", std::round(vw)}, {"height", std::round(vh)}};
            }
            readFramebuffer(job, sc->sceneRender->getFramebuffer(), std::move(info));
        });
    });
}

#if defined(SOKOL_GLCORE) || defined(SOKOL_GLES3)

// Filled by an ImGui draw callback while the backend renders the frame: at that point the
// window's framebuffer holds the whole editor UI and nothing has been presented yet
struct EditorReadback {
    bool done = false;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;     // RGBA, bottom row first
};

void editorReadbackCallback(const ImDrawList*, const ImDrawCmd* cmd) {
    auto* readback = static_cast<EditorReadback*>(cmd->UserCallbackData);
    if (!readback || readback->done) return;
    GLint viewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, viewport);
    const int width = viewport[2];
    const int height = viewport[3];
    if (width <= 0 || height <= 0) return;
    GLint alignment = 4;
    glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    readback->pixels.resize(static_cast<size_t>(width) * height * 4);
    glReadPixels(viewport[0], viewport[1], width, height, GL_RGBA, GL_UNSIGNED_BYTE, readback->pixels.data());
    glPixelStorei(GL_PACK_ALIGNMENT, alignment);
    if (glGetError() != GL_NO_ERROR) {
        readback->pixels.clear();
        return;
    }
    readback->width = width;
    readback->height = height;
    readback->done = true;
}
#endif

void captureEditor(const CaptureJobPtr& job) {
#if defined(SOKOL_GLCORE) || defined(SOKOL_GLES3)
    // Kept alive by the waits below; the callback only runs inside this frame's render
    auto readback = std::make_shared<EditorReadback>();
    auto attempts = std::make_shared<int>(0);
    auto request = [readback]() {
        ImGui::GetForegroundDrawList()->AddCallback(editorReadbackCallback, readback.get());
        ImGui::GetForegroundDrawList()->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    };
    request();
    job->call->waitUntil([readback, attempts, request]() {
        if (readback->done) return true;
        // a frame that was not rendered (idle, minimized) drops the callback: ask again
        if (++*attempts % 2 == 0) request();
        return false;
    // up to the call's own timeout (timeout_seconds): a loaded host can take seconds per frame
    }, job->remainingMs(), [job, readback](bool timedOut) {
        if (timedOut || !readback->done) {
            job->fail("The editor window did not render a frame to read within the call's timeout_seconds (minimized?).");
            return;
        }
        int width = readback->width;
        int height = readback->height;
        // GL rows are bottom first
        std::vector<uint8_t> flipped(readback->pixels.size());
        const size_t row = static_cast<size_t>(width) * 4;
        for (int y = 0; y < height; y++) {
            std::copy_n(readback->pixels.data() + (height - 1 - y) * row, row, flipped.data() + y * row);
        }
        // a window's rect (title_designer): the framebuffer cropped to it, so an agent sees what a person sees
        if (job->req.cropWidth > 0.0f && job->req.cropHeight > 0.0f) {
            const float scale = ImGui::GetIO().DisplayFramebufferScale.x > 0.0f ? ImGui::GetIO().DisplayFramebufferScale.x : 1.0f;
            const int x0 = std::clamp((int)std::lround(job->req.cropX * scale), 0, width - 1);
            const int y0 = std::clamp((int)std::lround(job->req.cropY * scale), 0, height - 1);
            const int cw = std::clamp((int)std::lround(job->req.cropWidth * scale), 1, width - x0);
            const int ch = std::clamp((int)std::lround(job->req.cropHeight * scale), 1, height - y0);
            std::vector<uint8_t> cropped(static_cast<size_t>(cw) * ch * 4);
            for (int y = 0; y < ch; y++) {
                std::copy_n(flipped.data() + (static_cast<size_t>(y0 + y) * width + x0) * 4, static_cast<size_t>(cw) * 4, cropped.data() + static_cast<size_t>(y) * cw * 4);
            }
            flipped = std::move(cropped);
            width = cw;
            height = ch;
        }
        std::vector<uint8_t> png;
        int outWidth = 0, outHeight = 0;
        if (!encodePixels(flipped.data(), width, height, 4, job->req.maxWidth, png, outWidth, outHeight)) {
            job->fail("Encoding the editor screenshot failed.");
            return;
        }
        finishWithPng(job, std::move(png), outWidth, outHeight, width, height, Json::object());
    });
#else
    job->fail("Whole-editor screenshots need the OpenGL renderer; use view game or scene.");
#endif
}

} // namespace

// ------------------------------------------------------------------ shared entry points

namespace w3 {

void captureView(const CallPtr& call, const CaptureRequest& request, std::function<void(CaptureResult)> done) {
    auto job = std::make_shared<CaptureJob>();
    job->call = call;
    job->req = request;
    job->done = std::move(done);
    job->deadline = Clock::now() + std::chrono::milliseconds(std::max(1000, request.timeoutMs));
    if (!call->ctx().project()) {
        job->fail("No project is open.");
        return;
    }
    if (request.view != "game" && request.view != "scene" && request.view != "editor" && request.view != "title_designer") {
        job->fail("view must be game, scene, editor or title_designer.");
        return;
    }
    if (request.view == "title_designer") {
        // the editor framebuffer cropped to the Title Designer's rect, its tab fronted first (S82 R79)
        TitleDesignerWindow* designer = call->ctx().app().getTitleDesignerWindow();
        designer->setOpen(true);
        designer->requestFocus();
        call->afterFrames(3, [job, designer]() {
            ImVec2 min, max;
            if (!designer->windowRect(min, max)) {
                job->fail("The Title Designer is not shown (minimized editor?).");
                return;
            }
            const ImVec2 origin = ImGui::GetMainViewport()->Pos;
            job->req.cropX = min.x - origin.x;
            job->req.cropY = min.y - origin.y;
            job->req.cropWidth = max.x - min.x;
            job->req.cropHeight = max.y - min.y;
            captureEditor(job);
        });
        return;
    }
    // nothing is drawn: the capture would wait for a frame that never comes
    if (!call->ctx().app().isWindowVisible()) {
        job->fail("The editor window is minimized or hidden, so nothing is drawn (the game keeps updating). Restore the "
                  "window, or run the editor with mcp --headless.");
        return;
    }
    if (request.view == "game" && Platform::isGameHiddenSimulated() && call->ctx().project()->isPlaySessionActive()) {
        job->fail("The game is hidden (send_input app hide): it updates without drawing. Send {type: \"app\", action: "
                  "\"show\"} first.");
        return;
    }
    auto capture = [job]() {
        if (job->req.view == "game") captureGame(job);
        else if (job->req.view == "scene") captureScene(job);
        else captureEditor(job);
    };
    // shaders still building, shaders and pipelines still being created off the render thread
    // (spec S32) and textures still loading are not drawn yet (stand-ins are): the image waits
    // for them, up to the call's timeout_seconds, then fails naming them
    auto idle = []() { return ShaderPool::isCreationIdle() && TextureDataPool::getLoadingCount() == 0; };
    if (idle()) {
        capture();
        return;
    }
    call->waitUntil(idle, job->remainingMs(), [job, capture](bool timedOut) {
        if (timedOut) {
            job->fail("The " + job->req.view + " view was not shot: still loading after the call's timeout_seconds (a "
                      "screenshot never shows a stand-in for it): " + describeLoading(nullptr) + ". Retry, or raise timeout_seconds.");
            return;
        }
        capture();
    });
}

bool parseScreenshotOption(const Json& args, CaptureRequest& request, bool& wanted, std::string& error) {
    wanted = false;
    if (!args.contains("screenshot") || args["screenshot"].is_null()) return true;
    const Json& value = args["screenshot"];
    if (value.is_boolean()) {
        wanted = value.get<bool>();
        request.view = "game";
    } else if (value.is_string() && (value == "game" || value == "scene" || value == "editor")) {
        wanted = true;
        request.view = value.get<std::string>();
    } else {
        error = "screenshot must be true (Game view), \"game\", \"scene\" or \"editor\".";
        return false;
    }
    request.maxWidth = std::clamp(args.value("max_width", 768), 0, 4096);
    return true;
}

} // namespace w3

// ------------------------------------------------------------------ tools

namespace {

void screenshotTool(const CallPtr& call) {
    const Json& args = call->args();
    w3::CaptureRequest request;
    request.view = args.value("view", "game");
    if (args.contains("scene")) request.scene = args["scene"];
    request.maxWidth = std::clamp(args.value("max_width", 1024), 0, 8192);
    request.savePath = args.value("save_path", "");
    request.focus = args.value("focus", true);
    request.timeoutMs = static_cast<int>(std::clamp(args.value("timeout_seconds", 30.0), 1.0, 600.0) * 1000.0);
    request.deviceOverlay = args.value("device_overlay", true);
    if (request.view != "game" && request.view != "scene" && request.view != "editor" && request.view != "title_designer") {
        call->fail("view must be \"game\" (the Game view: the running game, or the scene through its game camera), "
                   "\"scene\" (a Scene view with gizmos), \"editor\" (the whole editor window) or \"title_designer\" (that window).");
        return;
    }
    w3::captureView(call, request, [call](w3::CaptureResult result) {
        if (!result.ok) {
            call->fail(result.error);
            return;
        }
        std::string text = result.info.value("view", std::string("view")) + " view screenshot " +
                           std::to_string(result.info.value("width", 0)) + "x" + std::to_string(result.info.value("height", 0));
        if (result.info.value("source_width", 0) != result.info.value("width", 0)) {
            text += " (scaled from " + std::to_string(result.info.value("source_width", 0)) + "x" +
                    std::to_string(result.info.value("source_height", 0)) + ")";
        }
        if (result.info.contains("scene") && result.info["scene"].is_object()) {
            text += " of scene \"" + result.info["scene"].value("name", std::string()) + "\"";
        }
        if (result.info.value("playing", false)) text += ", playing";
        text += ".";
        if (result.info.contains("canvas_rect_in_image")) {
            const Json& r = result.info["canvas_rect_in_image"];
            text += " Game canvas " + std::to_string(result.info["canvas"].value("width", 0)) + "x" +
                    std::to_string(result.info["canvas"].value("height", 0)) + " drawn at x=" + r["x"].dump() +
                    " y=" + r["y"].dump() + " w=" + r["width"].dump() + " h=" + r["height"].dump() + " in the image.";
        }
        if (result.info.contains("safe_area_in_image")) {
            const Json& r = result.info["safe_area_in_image"];
            text += " Safe area at x=" + r["x"].dump() + " y=" + r["y"].dump() + " w=" + r["width"].dump() + " h=" +
                    r["height"].dump() + " (" + result.info.value("device", std::string()) + ").";
        }
        if (result.info.contains("saved_to")) text += " Saved to " + result.info["saved_to"].get<std::string>() + ".";
        if (!result.note.empty()) text += " Note: " + result.note;
        ToolResult reply = ToolResult::success(text, result.info);
        reply.images.push_back(std::move(result.image));
        call->reply(std::move(reply));
    });
}

Json sceneCameraJson(SceneProject* sp) {
    Json out = Json::object();
    if (!sp || !sp->sceneRender) return out;
    Camera* camera = sp->sceneRender->getCamera();
    if (!camera) return out;
    out["position"] = McpValues::toJson(camera->getPosition());
    out["target"] = McpValues::toJson(camera->getTarget());
    if (sp->sceneType == SceneType::SCENE_3D) {
        out["up"] = McpValues::toJson(camera->getUp());
        out["distance"] = camera->getDistanceFromTarget();
        out["mode"] = "3d";
        // S26: projection, axis view and the Scene Camera settings
        if (SceneWindow* sw = Backend::getApp().getSceneWindow()) {
            SceneViewController& view = sw->getViewController();
            const SceneCameraPose pose = view.currentPose(*sp);
            const SceneCameraSettings& settings = sp->viewState.camera;
            out["projection"] = pose.orthographic ? "orthographic" : "perspective";
            out["ortho_size"] = pose.orthoSize;
            out["fov"] = settings.fieldOfView;
            out["near"] = camera->getNearClip();
            out["far"] = camera->getFarClip();
            const SceneViewAxis axis = view.viewAxis(*sp);
            out["view"] = SceneViewStateStream::axisName(axis);
            out["camera_settings"] = {{"speed", settings.speed}, {"speed_min", settings.speedMin}, {"speed_max", settings.speedMax},
                                      {"shift_multiplier", settings.shiftMultiplier}, {"acceleration", settings.acceleration},
                                      {"easing", settings.easing}, {"dynamic_clipping", settings.dynamicClipping},
                                      {"near", settings.nearClip}, {"far", settings.farClip}};
            out["layout"] = SceneViewStateStream::layoutName(sp->viewState.layout);
            out["active_pane"] = sp->viewState.activePane;
        }
    } else {
        out["zoom"] = static_cast<SceneRender2D*>(sp->sceneRender)->getZoom();
        out["mode"] = "2d";
    }
    return out;
}

// Replies with text + structured, adding a Scene view screenshot when asked
void replyMaybeWithScreenshot(const CallPtr& call, uint32_t sceneId, const std::string& text, Json data) {
    const Json& args = call->args();
    bool wanted = args.value("screenshot", false);
    if (!wanted) {
        call->ok(text, data);
        return;
    }
    w3::CaptureRequest request;
    request.view = "scene";
    request.scene = sceneId;
    request.maxWidth = std::clamp(args.value("max_width", 768), 0, 8192);
    w3::captureView(call, request, [call, text, data](w3::CaptureResult result) mutable {
        if (!result.ok) {
            call->ok(text + " (Screenshot failed: " + result.error + ")", data);
            return;
        }
        ToolResult reply = ToolResult::success(text, data);
        reply.images.push_back(std::move(result.image));
        call->reply(std::move(reply));
    });
}

const std::map<std::string, bool SceneDisplaySettings::*>& displayFlags() {
    static const std::map<std::string, bool SceneDisplaySettings::*> flags = {
        {"show_grid_2d", &SceneDisplaySettings::showGrid2D},
        {"show_grid_3d", &SceneDisplaySettings::showGrid3D},
        {"show_origin", &SceneDisplaySettings::showOrigin},
        {"show_all_bodies", &SceneDisplaySettings::showAllBodies},
        {"show_all_joints", &SceneDisplaySettings::showAllJoints},
        {"show_all_bones", &SceneDisplaySettings::showAllBones},
        {"show_all_lattices", &SceneDisplaySettings::showAllLattices},
        {"hide_camera_view", &SceneDisplaySettings::hideCameraView},
        {"hide_light_icons", &SceneDisplaySettings::hideLightIcons},
        {"hide_sound_icons", &SceneDisplaySettings::hideSoundIcons},
        {"hide_container_guides", &SceneDisplaySettings::hideContainerGuides},
        {"hide_selection_outline", &SceneDisplaySettings::hideSelectionOutline},
        {"disable_fog", &SceneDisplaySettings::disableFog},
        {"disable_clouds", &SceneDisplaySettings::disableClouds},
        {"show_post_processing", &SceneDisplaySettings::showPostProcessing},
        {"animate_water", &SceneDisplaySettings::animateWater},
        {"show_stats", &SceneDisplaySettings::showStats},
        {"show_ik", &SceneDisplaySettings::showIK},
        {"show_root_motion", &SceneDisplaySettings::showRootMotion},
        {"show_foot_trails", &SceneDisplaySettings::showFootTrails},
        // S41 (in a scene with a Pixel Perfect Camera)
        {"pixel_zoom", &SceneDisplaySettings::pixelZoom},
        {"pixel_grid", &SceneDisplaySettings::pixelGrid},
        {"pixel_snap_preview", &SceneDisplaySettings::pixelSnapPreview},
        {"pixel_move_snap", &SceneDisplaySettings::pixelMoveSnap},
    };
    return flags;
}

void setSceneCameraTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    std::string error;
    SceneProject* sp = resolveViewScene(project, args.contains("scene") ? args["scene"] : Json(), error);
    if (!sp) {
        call->fail(error);
        return;
    }
    const bool is3D = sp->sceneType == SceneType::SCENE_3D;
    for (const char* key : {"position", "target", "orbit", "distance"}) {
        if (!is3D && args.contains(key)) {
            call->fail(std::string(key) + " is for 3D scenes; \"" + sp->name + "\" is " + McpRefs::sceneTypeName(sp->sceneType) +
                       ": use center and zoom.");
            return;
        }
    }
    for (const char* key : {"center", "zoom"}) {
        if (is3D && args.contains(key)) {
            call->fail(std::string(key) + " is for 2D/UI scenes; \"" + sp->name + "\" is 3D: use position, target, orbit, distance.");
            return;
        }
    }
    Vector3 position, target;
    Vector2 center;
    if (args.contains("position") && !McpValues::parseVector3(args["position"], position)) {
        call->fail("position must be [x,y,z].");
        return;
    }
    if (args.contains("target") && !McpValues::parseVector3(args["target"], target)) {
        call->fail("target must be [x,y,z].");
        return;
    }
    if (args.contains("center") && !McpValues::parseVector2(args["center"], center)) {
        call->fail("center must be [x,y] (world units).");
        return;
    }
    if (args.contains("display")) {
        if (!args["display"].is_object()) {
            call->fail("display must be an object of flags, e.g. {\"show_grid_2d\": true}.");
            return;
        }
        for (auto it = args["display"].begin(); it != args["display"].end(); ++it) {
            if (it.key() == "grid_spacing_2d" || it.key() == "grid_spacing_3d") continue;
            if (it.key() == "draw_mode") {
                ViewDrawMode mode;
                if (!it.value().is_string() || !drawModeFromKey(it.value().get<std::string>(), mode)) {
                    call->fail("display.draw_mode must be shaded, wireframe, shaded_wireframe, lod_level or overdraw.");
                    return;
                }
                if (!is3D && mode != ViewDrawMode::SHADED) {
                    call->fail("display.draw_mode is for 3D scenes; \"" + sp->name + "\" is " + McpRefs::sceneTypeName(sp->sceneType) + ".");
                    return;
                }
                continue;
            }
            if (!displayFlags().count(it.key()) || !it.value().is_boolean()) {
                std::string valid;
                for (const auto& [name, member] : displayFlags()) valid += " " + name;
                call->fail("display." + it.key() + " is not a boolean display flag. Valid:" + valid +
                           ", grid_spacing_2d, grid_spacing_3d (numbers).");
                return;
            }
        }
    }

    for (const char* key : {"projection", "ortho_size", "view", "camera_settings"}) {
        if (!is3D && args.contains(key)) {
            call->fail(std::string(key) + " is for 3D scenes; \"" + sp->name + "\" is " + McpRefs::sceneTypeName(sp->sceneType) + ".");
            return;
        }
    }
    SceneViewAxis viewAxis = SceneViewAxis::FREE;
    if (args.contains("view") && (!args["view"].is_string() || !SceneViewStateStream::parseAxis(args["view"].get<std::string>(), viewAxis) ||
                                  viewAxis == SceneViewAxis::FREE)) {
        call->fail("view must be top, bottom, front, back, left or right.");
        return;
    }
    if (args.contains("projection") && args["projection"] != "perspective" && args["projection"] != "orthographic") {
        call->fail("projection must be \"perspective\" or \"orthographic\".");
        return;
    }
    // the Scene Camera settings (Unity's camera popup): one object, checked here
    if (args.contains("camera_settings")) {
        const Json& cs = args["camera_settings"];
        if (!cs.is_object()) {
            call->fail("camera_settings must be an object {fov, near, far, speed, acceleration, easing, dynamic_clipping}.");
            return;
        }
        struct Range {
            const char* key;
            double min;
            double max;
        };
        static const Range ranges[] = {{"fov", 4, 120}, {"near", 0.001, 1000}, {"far", 1, 1000000}, {"speed", 0.01, 1000}};
        for (auto it = cs.begin(); it != cs.end(); ++it) {
            const std::string& key = it.key();
            const Range* range = nullptr;
            for (const Range& r : ranges) {
                if (key == r.key) range = &r;
            }
            if (range) {
                if (!it.value().is_number() || it.value().get<double>() < range->min || it.value().get<double>() > range->max) {
                    call->fail("camera_settings." + key + " must be a number in " + Json(range->min).dump() + ".." + Json(range->max).dump() + ".");
                    return;
                }
            } else if (key == "acceleration" || key == "easing" || key == "dynamic_clipping") {
                if (!it.value().is_boolean()) {
                    call->fail("camera_settings." + key + " must be true or false.");
                    return;
                }
            } else {
                call->fail("camera_settings: unknown \"" + key + "\" (fov, near, far, speed, acceleration, easing, dynamic_clipping).");
                return;
            }
        }
        if (cs.contains("near") && cs.contains("far") && cs.value("near", 0.0) >= cs.value("far", 0.0)) {
            call->fail("camera_settings: near must be less than far.");
            return;
        }
    }

    const uint32_t sceneId = sp->id;
    whenSceneViewReady(call, sceneId, true, 10000, [call, sceneId, is3D, position, target, center, viewAxis](bool ready, const std::string& err) {
        const Json& a = call->args();
        McpContext& c = call->ctx();
        Project* p = c.project();
        SceneProject* s = p->getScene(sceneId);
        if (!s || !s->sceneRender || !s->sceneRender->getCamera()) {
            call->fail("The scene closed.");
            return;
        }
        Camera* camera = s->sceneRender->getCamera();
        std::vector<std::string> changed;
        if (is3D && (a.contains("position") || a.contains("target"))) {
            const Vector3 eye = a.contains("position") ? position : camera->getPosition();
            const Vector3 look = a.contains("target") ? target : camera->getTarget();
            if ((look - eye).length() < 1e-4f) {
                call->fail("target must differ from position: it is the point the camera looks at (nothing was changed).");
                return;
            }
        }
        // a camera preview (the Inspector's Preview) shows another camera: a move leaves it, or it would not show
        static const char* const moves[] = {"position", "target", "orbit", "distance", "view", "frame_all"};
        const bool moving = is3D && std::any_of(std::begin(moves), std::end(moves), [&a](const char* key) {
            return a.contains(key) && !(a[key].is_boolean() && !a[key].get<bool>());
        });
        if (moving && s->sceneRender->isPreviewCameraActive()) {
            s->sceneRender->clearPreviewCamera();
            changed.push_back("left_preview");
        }
        if (is3D) {
            if (a.contains("position")) {
                camera->setPosition(position);
                changed.push_back("position");
            }
            if (a.contains("target")) {
                camera->setTarget(target);
                changed.push_back("target");
            }
            if (a.contains("orbit")) {
                const Json& orbit = a["orbit"];
                const float yaw = orbit.is_object() ? orbit.value("yaw", 0.0f) : 0.0f;
                const float pitch = orbit.is_object() ? orbit.value("pitch", 0.0f) : 0.0f;
                if (yaw != 0.0f) camera->rotatePosition(yaw);
                if (pitch != 0.0f) camera->elevatePosition(pitch);
                changed.push_back("orbit");
            }
            if (a.contains("distance")) {
                // from position and target: the camera's world direction updates only when it draws
                const float distance = std::clamp(a.value("distance", 10.0f), MIN_EDITOR_CAMERA_DISTANCE, MAX_EDITOR_CAMERA_DISTANCE);
                const Vector3 t = camera->getTarget();
                Vector3 offset = camera->getPosition() - t;
                if (offset.length() < 1e-5f) offset = Vector3(0, 0, 1);
                const Vector3 newPos = t + offset.normalize() * distance;
                camera->setPosition(newPos);
                camera->setTarget(t);
                changed.push_back("distance");
            }
            // straight up or down the world up is parallel to the view: the Top/Bottom views' up instead
            camera->setUp(cameraUpFor(camera->getTarget() - camera->getPosition(), Vector3(0, 1, 0)));
            // S26 (R17, R18, R21): the Scene Camera settings, projection, axis view, framing;
            // an agent's camera moves land at once (Camera Easing is for the user's)
            SceneViewController& view = c.sceneWindow()->getViewController();
            view.ensureApplied(*s);
            SceneCameraSettings settings = s->viewState.camera;
            bool settingsChanged = false;
            if (a.contains("camera_settings") && a["camera_settings"].is_object()) {
                const Json& cs = a["camera_settings"];
                settings.fieldOfView = cs.value("fov", settings.fieldOfView);
                settings.nearClip = cs.value("near", settings.nearClip);
                settings.farClip = cs.value("far", settings.farClip);
                settings.speed = cs.value("speed", settings.speed);
                settings.acceleration = cs.value("acceleration", settings.acceleration);
                settings.easing = cs.value("easing", settings.easing);
                settings.dynamicClipping = cs.value("dynamic_clipping", settings.dynamicClipping);
                settingsChanged = true;
            }
            if (settingsChanged) {
                view.applyCameraSettings(*s, settings);
                changed.push_back("camera_settings");
            }
            if (a.contains("projection")) {
                view.setProjection(*s, a["projection"] == "orthographic");
                changed.push_back("projection");
            }
            if (a.contains("view")) {
                view.setAxisView(*s, viewAxis);
                view.finishTween(*s);
                changed.push_back("view");
            }
            if (a.contains("ortho_size")) {
                view.setOrthoSize(*s, a.value("ortho_size", 10.0f));
                changed.push_back("ortho_size");
            }
        } else {
            auto* render2D = static_cast<SceneRender2D*>(s->sceneRender);
            SceneWindow* sw = call->ctx().sceneWindow();
            const int viewWidth = sw ? sw->getWidth(sceneId) : 0;
            const int viewHeight = sw ? sw->getHeight(sceneId) : 0;
            float zoom = render2D->getZoom();
            // the view's centre before the change, so a zoom alone keeps it
            const Vector3 oldPos = camera->getPosition();
            Vector2 c(oldPos.x + viewWidth * zoom * 0.5f, oldPos.y + viewHeight * zoom * 0.5f);
            if (a.contains("zoom")) {
                zoom = std::clamp(a.value("zoom", zoom), 0.01f, 1000.0f);
                render2D->setZoom(zoom);
                changed.push_back("zoom");
            }
            if (a.contains("center")) {
                c = center;
                changed.push_back("center");
            }
            if (!changed.empty()) {
                if (!ready || viewWidth <= 0 || viewHeight <= 0) {
                    call->fail("The Scene view is not laid out, so the 2D camera can't be centred: " + err);
                    return;
                }
                const Vector3 t = camera->getTarget();
                const float x = c.x - viewWidth * zoom * 0.5f;
                const float y = c.y - viewHeight * zoom * 0.5f;
                camera->setPosition(x, y, oldPos.z);
                camera->setTarget(x, y, t.z);
            }
        }
        if (a.value("frame_all", false)) {
            SceneViewController& view = c.sceneWindow()->getViewController();
            view.ensureApplied(*s);
            if (!view.frameAll(*s)) {
                call->fail("Nothing in \"" + s->name + "\" to frame.");
                return;
            }
            view.finishTween(*s);
            changed.push_back("frame_all");
        }
        if (!changed.empty()) p->saveWorkspaceFile();
        if (a.contains("display")) {
            for (auto it = a["display"].begin(); it != a["display"].end(); ++it) {
                if (it.key() == "grid_spacing_2d" && it.value().is_number()) s->displaySettings.gridSpacing2D = it.value().get<float>();
                else if (it.key() == "grid_spacing_3d" && it.value().is_number()) s->displaySettings.gridSpacing3D = it.value().get<float>();
                else if (it.key() == "draw_mode") drawModeFromKey(it.value().get<std::string>(), s->displaySettings.drawMode);
                else if (displayFlags().count(it.key())) s->displaySettings.*(displayFlags().at(it.key())) = it.value().get<bool>();
            }
            changed.push_back("display");
            // a view preference, kept per scene in the workspace file (as the Gizmos menu does)
            p->saveWorkspaceFile();
        }
        s->needUpdateRender = true;
        Json data = {{"scene", McpRefs::sceneJson(p, *s)}, {"camera", sceneCameraJson(s)}, {"changed", changed}};
        Json displayJson = Json::object();
        for (const auto& [name, member] : displayFlags()) displayJson[name] = s->displaySettings.*member;
        displayJson["draw_mode"] = drawModeKey(s->displaySettings.drawMode);
        data["display"] = displayJson;
        std::string text = (changed.empty() ? "Scene view camera of \"" : "Moved the Scene view camera of \"") + s->name +
                           "\": " + data["camera"].dump() + ".";
        if (std::find(changed.begin(), changed.end(), "left_preview") != changed.end()) {
            text += " Left the camera preview, which showed another camera.";
        }
        replyMaybeWithScreenshot(call, sceneId, text, data);
    });
}

void frameEntityTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    std::string error;
    SceneProject* sp = resolveViewScene(project, args.contains("scene") ? args["scene"] : Json(), error);
    if (!sp) {
        call->fail(error);
        return;
    }
    std::vector<Entity> entities;
    Json framed = Json::array();
    Json refs = Json::array();
    if (args.contains("entity")) refs.push_back(args["entity"]);
    if (args.contains("entities") && args["entities"].is_array()) {
        for (const Json& ref : args["entities"]) refs.push_back(ref);
    }
    for (const Json& ref : refs) {
        Entity entity = w3::resolveLiveEntity(project, sp, ref, error);
        if (entity == NULL_ENTITY) {
            call->fail(error);
            return;
        }
        entities.push_back(entity);
        framed.push_back(McpRefs::entityJson(sp, entity));
    }
    SceneWindow* sw = call->ctx().sceneWindow();
    if (entities.empty() && (!sw || !sw->hasFrameableSelection(sp->id))) {
        call->fail("Nothing selected in \"" + sp->name + "\" to frame; pass entity (id or path) or entities.");
        return;
    }
    const uint32_t sceneId = sp->id;
    whenSceneViewReady(call, sceneId, true, 10000, [call, sceneId, entities, framed](bool ready, const std::string& err) {
        if (!ready) {
            call->fail(err);
            return;
        }
        Project* p = call->ctx().project();
        SceneWindow* w = call->ctx().sceneWindow();
        SceneProject* s = p->getScene(sceneId);
        if (!s) {
            call->fail("The scene closed.");
            return;
        }
        if (entities.empty()) w->frameSelection(sceneId);
        else w->frameEntities(sceneId, sceneId, entities);
        // an agent's framing lands at once (Camera Easing animates the user's)
        w->getViewController().finishTween(*s);
        s->needUpdateRender = true;
        Json data = {{"scene", McpRefs::sceneJson(p, *s)}, {"camera", sceneCameraJson(s)}};
        data["framed"] = entities.empty() ? Json("selection") : framed;
        replyMaybeWithScreenshot(call, sceneId,
            "Framed " + (entities.empty() ? std::string("the selection") : std::to_string(entities.size()) + " GameObject(s)") +
            " in the Scene view of \"" + s->name + "\".", data);
    });
}

// The Console as it shows: rows after its filters (the newest 50), categories, the selection's origin
Json consoleStateJson(ConsoleWindow& console) {
    const ConsoleWindow::ViewState state = console.viewState(50);
    Json rows = Json::array();
    for (const ConsoleWindow::ViewRow& row : state.rows) {
        Json item = {{"id", row.id}, {"type", Out::logTypeName(row.type)}, {"category", row.category}, {"text", row.text}};
        if (row.grey) item["grey"] = true;
        if (row.count > 1) item["count"] = row.count;
        rows.push_back(std::move(item));
    }
    Json json = {{"rows_shown", state.rowCount}, {"rows", rows}, {"categories", state.categories},
                 {"hidden_categories", state.hiddenCategories}, {"collapse", state.collapse},
                 {"clear_on_play", state.clearOnPlay}, {"error_pause", state.errorPause}};
    if (state.hasSelection) json["selected"] = {{"id", state.selectedId}, {"origin", state.selectedOrigin}};
    return json;
}

// Where an editor control is drawn now, for tests that click it with real input: the items of
// the next frames drawn with the label (UiItemLocator)
void locateUiItemTool(const CallPtr& call) {
    const Json& args = call->args();
    const std::string label = args.value("label", std::string());
    if (label.empty()) {
        call->fail("label: the item's label (its text, or the whole label with its ## id).");
        return;
    }
    UiItemLocator::start(label, args.value("window", std::string()));
    call->afterFrames(2, [call, label]() {
        Json items = Json::array();
        for (const UiItemLocator::Item& item : UiItemLocator::stop()) {
            items.push_back({{"label", item.label}, {"window", item.window}, {"visible", item.visible}, {"enabled", item.enabled},
                             {"rect", {{"x", std::round(item.x)}, {"y", std::round(item.y)},
                                       {"width", std::round(item.width)}, {"height", std::round(item.height)}}},
                             {"window_rect", {{"x", std::round(item.windowX)}, {"y", std::round(item.windowY)},
                                              {"width", std::round(item.windowWidth)}, {"height", std::round(item.windowHeight)}}}});
        }
        const std::string text = items.empty() ? "No item \"" + label + "\" is drawn (scrolled away, closed or another label)."
                                               : std::to_string(items.size()) + " item(s) \"" + label + "\".";
        call->ok(text, Json{{"items", items}});
    });
}

// The Scene view of the posed scene and its pose handles (phase 5), null when not posing there
PoseViewport* posedViewport(Project* project, PoseEditSession& session) {
    if (!session.bound()) return nullptr;
    SceneProject* sp = project->getScene(session.sceneId());
    if (!sp || !sp->sceneRender) return nullptr;
    return sp->sceneRender->getPoseViewport();
}

// focus_window pose_animation's `pose` object: {entity, path, index, play}, applied in that order
bool focusPoseAnimation(const CallPtr& call, PoseAnimationWindow* window) {
    const Json& args = call->args();
    const Json& pose = args["pose"];
    if (!pose.is_object()) {
        call->fail("pose must be an object {entity, path, index, play, tab}.");
        return false;
    }
    for (auto it = pose.begin(); it != pose.end(); ++it) {
        const std::string& key = it.key();
        if (key != "entity" && key != "path" && key != "index" && key != "play" && key != "tab") {
            call->fail("pose." + key + ": unknown key (valid: entity, path, index, play, tab).");
            return false;
        }
    }
    Project* project = call->ctx().project();
    std::string error;
    if (pose.contains("entity")) {
        SceneProject* sp = resolveViewScene(project, args.contains("scene") ? args["scene"] : Json(), error);
        if (!sp) {
            call->fail(error);
            return false;
        }
        const Entity entity = w3::resolveLiveEntity(project, sp, pose["entity"], error);
        if (entity == NULL_ENTITY) {
            call->fail("pose.entity: " + error);
            return false;
        }
        if (!window->bindCharacter(sp->id, entity, &error)) {
            call->fail("pose.entity: " + error);
            return false;
        }
    }
    if (pose.contains("path")) {
        if (!pose["path"].is_string() || !window->openAnimation(pose["path"].get<std::string>(), &error)) {
            call->fail("pose.path: " + (error.empty() ? std::string("a .poseanim path (string)") : error));
            return false;
        }
    }
    PoseEditSession& session = window->getSession();
    if (pose.contains("index")) {
        const int count = session.document() ? static_cast<int>(session.document()->data().poses.size()) : 0;
        if (!pose["index"].is_number_integer() || pose["index"].get<int>() < 0 || pose["index"].get<int>() >= count) {
            call->fail("pose.index: a pose index 0.." + std::to_string(count - 1) + " of the open animation.");
            return false;
        }
        window->selectPose(pose["index"].get<int>());
    }
    if (pose.contains("play")) {
        if (!pose["play"].is_boolean()) {
            call->fail("pose.play: true plays the preview, false stops it.");
            return false;
        }
        if (!session.bound() || !session.document()) {
            call->fail("pose.play: bind a character (pose.entity) and open an animation (pose.path) first.");
            return false;
        }
        if (pose["play"].get<bool>()) window->play();
        else window->stop();
    }
    if (pose.contains("tab")) {
        if (!pose["tab"].is_string()) {
            call->fail("pose.tab: Pose, Feel, Humanize, Templates or map_bones (the bone mapping popup).");
            return false;
        }
        const std::string tab = pose["tab"].get<std::string>();
        if (tab == "map_bones") {
            if (!window->openMappingFor(&error)) {
                call->fail("pose.tab: " + error);
                return false;
            }
        } else {
            window->selectTab(tab);
        }
    }
    return true;
}

Json poseWindowState(const CallPtr& call) {
    PoseAnimationWindow::State state = call->ctx().app().getPoseAnimationWindow()->getState();
    Json data = {{"character", state.character.empty() ? Json() : Json(state.character)},
                 {"rig", state.rig.empty() ? Json() : Json(state.rig)},
                 {"animation", state.animation.empty() ? Json() : Json(state.animation)},
                 {"pose_count", state.poseCount}, {"primary", state.primary}, {"selection", state.selection},
                 {"mode", state.mode}, {"time", state.time}, {"length", state.length}, {"playing", state.playing},
                 {"tab", state.tab}, {"scene_keys", state.sceneKeys}, {"editable", state.editable},
                 {"layout", state.layout}, {"thumbnail_height", state.thumbnailHeight},
                 {"menu", state.menu.empty() ? Json() : Json(state.menu)}};
    if (state.entity != NULL_ENTITY) {
        if (SceneProject* sp = call->ctx().project()->getScene(state.sceneId)) {
            if (sp->scene && sp->scene->isEntityCreated(state.entity)) data["entity"] = McpRefs::entityJson(sp, state.entity);
        }
    }
    if (!state.emptyState.empty()) data["empty_state"] = state.emptyState;
    if (!state.status.empty()) data["status"] = state.status;
    // the Templates tab (phase 6) while it shows: the cards after the filter
    if (state.templates.shown) {
        data["templates"] = {{"cards", state.templates.cards}, {"user", state.templates.user},
                             {"visible", state.templates.visible}, {"thumbnails", state.templates.thumbnails}, {"hint", state.templates.hint},
                             {"category", state.templates.category}, {"search", state.templates.search}};
    }
    // the Scene view posing the character (phase 5): handles in Scene view pixels, ghosts, floor
    PoseEditSession& session = call->ctx().app().getPoseAnimationWindow()->getSession();
    if (PoseViewport* viewport = posedViewport(call->ctx().project(), session)) {
        const PoseViewport::State view = viewport->getState();
        data["viewport"] = {{"posing", view.posing}, {"previewing", view.previewing}, {"handle_count", view.visibleHandles},
                            {"active_joint", view.activeJoint.empty() ? Json() : Json(view.activeJoint)},
                            {"ghosts", view.ghosts}, {"floor", view.floor}, {"contacts", view.contacts},
                            {"balanced", view.balanceShown ? Json(view.balanced) : Json()},
                            {"global_edit", session.globalEditActive()},
                            {"view", Json::array({view.viewWidth, view.viewHeight})}};
    }
    return data;
}

// focus_window title_designer's object (S82 R78): its keys applied in this order, each through the
// window's scripted API; an unknown key fails listing the valid ones
bool focusTitleDesigner(const CallPtr& call, TitleDesignerWindow* designer) {
    static const std::vector<std::string> keys = {"path", "select", "panel", "search", "library", "animation", "preview_as", "background", "device", "zoom", "add"};
    const Json& args = call->args();
    const Json options = args.value("title_designer", Json::object());
    if (!options.is_object()) {
        call->fail("title_designer must be an object with any of: path, select, panel, search, library, animation, preview_as, background, device, zoom, add.");
        return false;
    }
    for (auto it = options.begin(); it != options.end(); ++it) {
        if (std::find(keys.begin(), keys.end(), it.key()) == keys.end()) {
            call->fail("title_designer." + it.key() + " is not a key: valid are path, select, panel, search, library, preview_as, background, device, zoom, add.");
            return false;
        }
    }
    designer->setOpen(true);
    designer->requestFocus();
    std::string error;
    auto fail = [&](const std::string& message) {
        call->fail("title_designer: " + message, {{"state", designer->state()}});
        return false;
    };
    if (options.contains("path")) {
        if (!options["path"].is_string() || !designer->open(FileUtils::pathFromUtf8(options["path"].get<std::string>()), &error)) {
            return fail(error.empty() ? "path must be a .titleart path" : error);
        }
    }
    if (options.contains("select")) {
        std::vector<std::string> paths;
        if (options["select"].is_string()) {
            if (!options["select"].get<std::string>().empty()) paths.push_back(options["select"].get<std::string>());
        } else if (options["select"].is_array()) {
            for (const Json& p : options["select"]) {
                if (p.is_string()) paths.push_back(p.get<std::string>());
            }
        }
        if (!designer->select(paths, &error)) return fail(error);
    }
    if (options.contains("panel") && (!options["panel"].is_string() || !designer->setPanel(options["panel"].get<std::string>(), &error))) return fail(error);
    if (options.contains("search")) designer->setSearch(options.value("search", std::string()));
    if (options.contains("library")) {
        const Json& lib = options["library"];
        if (!lib.is_object()) return fail("library must be {tab, family, query}");
        if (!designer->setLibrary(lib.value("tab", std::string()), lib.value("family", std::string()), lib.value("query", std::string()), &error)) return fail(error);
    }
    // S85: the Animation panel's preview at a time ({name, time}; null stops it), as a scrub does
    if (auto animation = options.find("animation"); animation != options.end()) {
        const bool stop = animation->is_null();
        if (!stop && (!animation->is_object() || !(*animation)["time"].is_number())) {
            call->fail("title_designer.animation is {name, time} or null.");
            return false;
        }
        if (!titledesigner::previewTitleAnimation(stop ? std::string() : animation->value("name", std::string()), stop ? -1.0 : (*animation)["time"].get<double>(), &error)) {
            call->fail(error);
            return false;
        }
    }
    if (options.contains("preview_as") && !designer->setPreviewAs(options.value("preview_as", std::string()), &error)) return fail(error);
    if (options.contains("background")) {
        titledesigner::BackdropChoice backdrop;
        const Json& bg = options["background"];
        if (bg.is_string()) {
            backdrop.kind = bg.get<std::string>();
        } else if (bg.is_object()) {
            backdrop.kind = bg.value("kind", std::string("checker"));
            backdrop.color = bg.value("color", backdrop.color);
            backdrop.image = bg.value("image", std::string());
            backdrop.scene = bg.value("scene", std::string());
        }
        if (!designer->setBackground(backdrop, &error)) return fail(error);
    }
    if (options.contains("device")) {
        const Json& d = options["device"];
        titledesigner::DeviceChoice device;
        if (d.is_string()) device.profile = d.get<std::string>();
        else if (d.is_object()) {
            device.profile = d.value("profile", std::string());
            device.orientation = d.value("orientation", device.orientation);
            device.displayHeight = d.value("display_height", device.displayHeight);
        } else if (!d.is_null()) {
            return fail("device must be a profile id or {profile, orientation, display_height}");
        }
        if (!designer->setDevice(device, &error)) return fail(error);
    }
    if (options.contains("add")) {
        // the Add palette (S82 R17): {filter, query} opens it as a person would, false closes it
        const Json& a = options["add"];
        if (a.is_boolean() && !a.get<bool>()) designer->closeAddPalette();
        else if (a.is_object()) designer->openAddPalette(a.value("filter", std::string()), a.value("query", std::string()));
        else return fail("add must be {filter, query} or false");
    }
    if (options.contains("zoom")) {
        const Json& z = options["zoom"];
        if (z.is_string() && z.get<std::string>() == "fit") designer->fit();
        else if (z.is_number()) designer->setZoom(z.get<float>());
        else return fail("zoom must be a number (1 = 100 %) or \"fit\"");
    }
    return true;
}

void focusWindowTool(const CallPtr& call) {
    const Json& args = call->args();
    McpContext& ctx = call->ctx();
    App& app = ctx.app();
    Project* project = ctx.project();
    const std::string window = args.value("window", "");
    std::string imguiName;
    // refused before any window opens: a failing call leaves the layout as it was
    if (args.contains("pose") && window != "pose_animation") {
        call->fail("pose: only for window \"pose_animation\".");
        return;
    }
    if (args.contains("path") && window != "graph_editor") {
        call->fail("path: only for window \"graph_editor\".");
        return;
    }
    if (args.contains("title_designer") && window != "title_designer") {
        call->fail("title_designer: only for window \"title_designer\".");
        return;
    }

    if (window == "game") {
        ctx.gameWindow()->requestFocus();
        imguiName = GameWindow::WINDOW_NAME;
    } else if (window == "scene") {
        std::string error;
        SceneProject* sp = resolveViewScene(project, args.contains("scene") ? args["scene"] : Json(), error);
        if (!sp) {
            call->fail(error);
            return;
        }
        imguiName = sceneTabName(project, sp->id);
    } else if (window == "console") {
        ConsoleWindow* console = ctx.consoleWindow();
        console->setOpen(true);
        imguiName = ConsoleWindow::WINDOW_NAME;
        // what a user does in the Console: the Category menu, Collapse, a click on a row
        const Json options = args.value("console", Json::object());
        if (options.contains("hidden_categories") && options["hidden_categories"].is_array()) {
            std::vector<std::string> hidden;
            for (const Json& item : options["hidden_categories"]) {
                if (item.is_string()) hidden.push_back(item.get<std::string>());
            }
            console->setHiddenCategories(hidden);
        }
        if (options.contains("collapse") && options["collapse"].is_boolean()) {
            console->setCollapse(options["collapse"].get<bool>());
        }
        if (options.contains("select") && options["select"].is_number_integer() && !console->selectLog(options["select"].get<uint64_t>())) {
            call->fail("console.select: no row with id " + options["select"].dump() + " is shown (filtered out, or cleared).");
            return;
        }
    } else if (window == "preferences") {
        EditorSettingsWindow& preferences = app.getEditorSettingsWindow();
        if (args.value("close", false)) {
            preferences.close();
            call->ok("Closed Preferences.", Json{{"window", window}, {"shown", false}});
            return;
        }
        if (!preferences.isOpen()) preferences.open(project);
        const std::string page = args.value("page", "");
        if (!page.empty() && !preferences.showPage(page)) {
            call->fail("page must be one of: General, Code Editor, Undo, External Tools, Logging, Shader Cache.");
            return;
        }
        imguiName = "Preferences##EditorSettingsModal";
    } else if (window == "hierarchy") {
        ctx.hierarchyWindow()->setOpen(true);
        imguiName = HierarchyWindow::WINDOW_NAME;
    } else if (window == "inspector") {
        ctx.inspectorWindow()->setOpen(true);
        imguiName = InspectorWindow::WINDOW_NAME;
    } else if (window == "project") {
        ctx.projectWindow()->setOpen(true);
        imguiName = ProjectWindow::WINDOW_NAME;
    } else if (window == "animation") {
        ctx.animationWindow()->setOpen(true);
        imguiName = AnimationWindow::WINDOW_NAME;
    } else if (window == "animator") {
        // it shows the selected GameObject's controller (or the .controller opened last)
        app.getAnimatorWindow()->setOpen(true);
        imguiName = AnimatorWindow::WINDOW_NAME;
    } else if (window == "particle_effect") {
        // it shows the .particles or the subtree opened last
        app.getParticleEffectWindow()->setOpen(true);
        imguiName = ParticleEffectWindow::WINDOW_NAME;
    } else if (window == "graph_editor") {
        // `path` opens that graph where a double-click on it does (its kind's window: a .procmat's is
        // Procedural Material, a .noisegraph's Noise Graph); without it, the one open
        GraphEditorWindow* graphs = app.getGraphEditorWindow();
        const std::string path = args.value("path", std::string());
        const auto kind = args.contains("path") ? graph::GraphRegistry::get().kindForFile(path) : nullptr;
        const auto own = kind ? graphEditorHooks().kindWindows.find(kind->id) : graphEditorHooks().kindWindows.end();
        if (own != graphEditorHooks().kindWindows.end()) {
            if (!own->second.open(path)) {
                call->fail("path: " + path + " is not a file of the project.");
                return;
            }
            imguiName = own->second.windowName;
        } else {
            if (args.contains("path")) {
                if (GraphEditorWindow* kindWindow = GraphEditorWindow::windowFor(path)) graphs = kindWindow;
                if (!graphs->openGraph(path)) {
                    call->fail("path: " + graphs->getStatus() + ".");
                    return;
                }
            }
            graphs->setOpen(true);
            imguiName = graphs->windowName();
        }
    } else if (window == "terrain") {
        ctx.terrainEditWindow()->setOpen(true);
        imguiName = TerrainEditWindow::WINDOW_NAME;
    } else if (window == "mcp") {
        imguiName = McpActivityWindow::WINDOW_NAME;
    } else if (window == "systems") {
        ctx.systemsWindow()->setOpen(true);
        imguiName = SystemsWindow::WINDOW_NAME;
    } else if (window == "jobs_memory") {
        app.getJobsMemoryWindow()->setOpen(true);
        imguiName = JobsMemoryWindow::WINDOW_NAME;
    } else if (window == "profiler") {
        ProfilerWindow* profiler = app.getProfilerWindow();
        profiler->setOpen(true);
        imguiName = ProfilerWindow::WINDOW_NAME;
        // what Load does, without the file dialog
        const Json options = args.value("profiler", Json::object());
        if (options.contains("load") && options["load"].is_string()) {
            std::string error;
            if (!profiler->loadCapture(options["load"].get<std::string>(), error)) {
                call->fail("profiler.load: " + error);
                return;
            }
        }
    } else if (window == "frame_debugger") {
        app.getFrameDebuggerWindow()->setOpen(true);
        imguiName = FrameDebuggerWindow::WINDOW_NAME;
    } else if (window == "undo_history") {
        app.getUndoHistoryWindow()->setOpen(true);
        imguiName = UndoHistoryWindow::WINDOW_NAME;
    } else if (window == "play_mode_changes") {
        app.getPlayModeChangesWindow()->setOpen(true);
        imguiName = PlayModeChangesWindow::WINDOW_NAME;
    } else if (window == "pose_animation") {
        PoseAnimationWindow* pw = app.getPoseAnimationWindow();
        pw->requestFocus();
        imguiName = PoseAnimationWindow::WINDOW_NAME;
        if (args.contains("pose") && !focusPoseAnimation(call, pw)) return;
    } else if (window == "texture_synthesis") {
        app.getTextureSynthesisWindow()->open();
        imguiName = TextureSynthesisWindow::WINDOW_NAME;
    } else if (window == "vertex_animation_baker") {
        app.getVertexAnimationBakerWindow()->open();
        imguiName = VertexAnimationBakerWindow::WINDOW_NAME;
    } else if (window == "sprite_normals") {
        app.getSpriteNormalsWindow()->open();
        imguiName = SpriteNormalsWindow::WINDOW_NAME;
    } else if (window == "title_designer") {
        imguiName = TitleDesignerWindow::WINDOW_NAME;
        if (!focusTitleDesigner(call, app.getTitleDesignerWindow())) return;
    } else {
        call->fail("window must be one of: scene, game, console, hierarchy, inspector, project, animation, animator, particle_effect, pose_animation, terrain, mcp, "
                   "systems, jobs_memory, profiler, frame_debugger, preferences, undo_history, play_mode_changes, texture_synthesis, "
                   "vertex_animation_baker, sprite_normals, title_designer, graph_editor.");
        return;
    }
    // A window opened just now exists after its next draw.
    call->afterFrames(1, [call, imguiName, window]() {
        if (window != "game") ImGui::SetWindowFocus(imguiName.c_str());
        call->afterFrames(2, [call, imguiName, window]() {
            ImGuiWindow* w = ImGui::FindWindowByName(imguiName.c_str());
            const bool shown = w && w->WasActive && !w->Hidden;
            Json data = {{"window", window}, {"shown", shown}};
            if (w) {
                data["size"] = {w->Size.x, w->Size.y};
                // in the editor window's client area, as the OS pointer sees it
                const ImVec2 origin = ImGui::GetMainViewport()->Pos;
                data["rect"] = {{"x", std::round(w->Pos.x - origin.x)}, {"y", std::round(w->Pos.y - origin.y)},
                                {"width", std::round(w->Size.x)}, {"height", std::round(w->Size.y)}};
            }
            if (window == "console") data["console"] = consoleStateJson(*call->ctx().consoleWindow());
            if (window == "graph_editor") {
                const GraphEditorWindow* shownGraphs = GraphEditorWindow::focused();
                if (!shownGraphs) shownGraphs = call->ctx().app().getGraphEditorWindow();
                const auto& document = shownGraphs->getDocument();
                data["graph"] = document ? Json(document->projectPath()) : Json();
                if (!document) data["next"] = "No graph is open: focus_window {window: \"graph_editor\", path} opens one.";
            }
            if (window == "preferences") data["page"] = call->ctx().app().getEditorSettingsWindow().pageName();
            if (window == "profiler") {
                const ProfilerWindow* profiler = call->ctx().app().getProfilerWindow();
                data["profiler"] = {{"target", profiler->getTarget()}, {"frames", profiler->targetFrameCount()},
                                    {"recording", profiler->isRecording()}};
            }
            if (window == "systems") {
                // each row's Enabled checkbox, as the OS pointer sees it; visible: inside the table's scroll view
                const ImVec2 origin = ImGui::GetMainViewport()->Pos;
                Json rows = Json::array();
                for (const SystemsWindow::RowRect& row : call->ctx().systemsWindow()->getRowRects()) {
                    rows.push_back({{"phase", row.phase}, {"system", row.system}, {"visible", row.visible},
                                    {"enabled_checkbox", {{"x", std::round(row.x - origin.x)}, {"y", std::round(row.y - origin.y)},
                                                          {"width", std::round(row.width)}, {"height", std::round(row.height)}}}});
                }
                data["rows"] = rows;
            }
            if (window == "title_designer") data["state"] = call->ctx().app().getTitleDesignerWindow()->state();
            if (window == "pose_animation") {
                data["state"] = poseWindowState(call);
                if (w) data["pos"] = {w->Pos.x - ImGui::GetMainViewport()->Pos.x, w->Pos.y - ImGui::GetMainViewport()->Pos.y};
            }
            if (!w) {
                call->fail("The " + window + " window does not exist (closed panel?).", data);
                return;
            }
            call->ok("Focused the " + window + " window" + (shown ? "." : " (it is not shown: minimized editor?)."), data);
        });
    });
}

const std::vector<std::string>& aspectNames() {
    // GameWindow's ASPECT_PRESETS order
    static const std::vector<std::string> names = {"free", "project", "16:9", "16:10", "4:3", "9:16"};
    return names;
}

void replyGameViewSettings(const CallPtr& call, const std::vector<std::string>& changed);

void configureGameViewTool(const CallPtr& call) {
    const Json& args = call->args();
    McpContext& ctx = call->ctx();
    GameWindow* gw = ctx.gameWindow();
    Project* project = ctx.project();
    GameViewSettings settings = gw->getSettings();
    std::vector<std::string> changed;

    if (args.contains("aspect")) {
        const std::string aspect = args.value("aspect", "");
        auto it = std::find(aspectNames().begin(), aspectNames().end(), aspect);
        if (it == aspectNames().end() || (int)(it - aspectNames().begin()) >= GameWindow::getAspectCount()) {
            call->fail("aspect must be free, project (the project's canvas size), 16:9, 16:10, 4:3 or 9:16.");
            return;
        }
        settings.aspect = static_cast<int>(it - aspectNames().begin());
        // an aspect replaces a fixed resolution, as the toolbar's Aspect entries do (a
        // resolution given in the same call still wins: it is applied below)
        settings.resolution.clear();
        settings.resolutionWidth = 0;
        settings.resolutionHeight = 0;
        settings.resolutionDpi = 0.0f;
        changed.push_back("aspect");
    }
    if (args.contains("scale")) {
        settings.scale = std::clamp(args.value("scale", 1.0f), 1.0f, 5.0f);
        changed.push_back("scale");
    }
    if (args.contains("pixel_scale")) {
        if (!args["pixel_scale"].is_number_integer() || args["pixel_scale"].get<int>() < 0 || args["pixel_scale"].get<int>() > 8) {
            call->fail("pixel_scale must be an integer 0 (Auto) to 8.");
            return;
        }
        settings.pixelScale = args["pixel_scale"].get<int>();
        changed.push_back("pixel_scale");
    }
    if (args.contains("maximize_on_play")) {
        settings.maximizeOnPlay = args.value("maximize_on_play", false);
        changed.push_back("maximize_on_play");
    }
    if (args.contains("show_stats")) {
        settings.showStats = args.value("show_stats", false);
        changed.push_back("show_stats");
    }
    if (args.contains("simulate_touch")) {
        const bool touch = args.value("simulate_touch", false);
        if (touch != project->isSimulateTouch()) {
            if (project->isAnyScenePlaying()) {
                call->fail("simulate_touch can't change while playing; stop play first.");
                return;
            }
            project->setSimulateTouch(touch);
            project->saveWorkspaceFile();
        }
        changed.push_back("simulate_touch");
    }
    static const std::vector<std::string> atmosphereNames = {"project", "off", "low", "medium", "high"};
    if (args.contains("atmosphere_quality")) {
        const std::string quality = args.value("atmosphere_quality", "");
        auto it = std::find(atmosphereNames.begin(), atmosphereNames.end(), quality);
        if (it == atmosphereNames.end()) {
            call->fail("atmosphere_quality must be project, off, low, medium or high.");
            return;
        }
        settings.atmosphereQuality = (int)(it - atmosphereNames.begin()) - 1;
        changed.push_back("atmosphere_quality");
    }
    // S14 R26: the family of cooked textures every view renders with
    if (args.contains("simulate_textures")) {
        TextureFamily family;
        if (!args["simulate_textures"].is_string() || !textureFamilyFromName(args["simulate_textures"].get<std::string>(), family)) {
            call->fail("simulate_textures must be desktop, mobile, android_etc2 or web.");
            return;
        }
        TextureCookQueue::get().setViewFamily(family);
        changed.push_back("simulate_textures");
    }
    if (args.contains("custom_device")) {
        const Json& custom = args["custom_device"];
        if (!custom.is_object()) {
            call->fail("custom_device must be {width, height, insets: {top, right, bottom, left}, corner_radius} (pixels, portrait).");
            return;
        }
        CustomDeviceSettings& c = settings.custom;
        auto number = [&](const Json& object, const char* key, float& out, float low, float high) {
            if (!object.contains(key)) return true;
            if (!object[key].is_number() || !std::isfinite(object[key].get<double>())) return false;
            out = std::clamp(object[key].get<float>(), low, high);
            return true;
        };
        float width = (float)c.width, height = (float)c.height;
        bool ok = number(custom, "width", width, 16.0f, 16384.0f) && number(custom, "height", height, 16.0f, 16384.0f) &&
                  number(custom, "corner_radius", c.cornerRadius, 0.0f, 8192.0f) && number(custom, "dpi", c.dpi, 0.0f, 2000.0f);
        if (ok && custom.contains("insets")) {
            const Json& insets = custom["insets"];
            ok = insets.is_object() && number(insets, "top", c.top, 0.0f, 16384.0f) && number(insets, "right", c.right, 0.0f, 16384.0f) &&
                 number(insets, "bottom", c.bottom, 0.0f, 16384.0f) && number(insets, "left", c.left, 0.0f, 16384.0f);
        }
        if (!ok) {
            call->fail("custom_device: width, height, corner_radius, dpi and insets {top, right, bottom, left} are numbers (pixels).");
            return;
        }
        c.width = (int)std::lround(width);
        c.height = (int)std::lround(height);
        // a custom screen is meant to be shown
        if (!args.contains("device")) settings.device = "custom";
        changed.push_back("custom_device");
    }
    // the runtime collider / joint wireframes (Physics.debugDraw): every scene, Play and exports
    if (args.contains("physics_debug")) {
        Physics::setDebugDraw(args.value("physics_debug", false));
        changed.push_back("physics_debug");
    }
    if (args.contains("physics_debug_contacts")) {
        Physics::setDebugDrawContacts(args.value("physics_debug_contacts", false));
        changed.push_back("physics_debug_contacts");
    }
    if (args.contains("network")) {
        const std::string network = args.value("network", "");
        if (network != "online" && network != "offline") {
            call->fail("network must be online or offline (simulated: Http requests to hosts other than localhost fail with "
                       "errorCode \"offline\").");
            return;
        }
        Http::setOfflineSimulated(network == "offline");
        changed.push_back("network");
    }

    if (args.contains("device")) {
        const std::string device = args["device"].is_string() ? args["device"].get<std::string>() : std::string();
        if (device == "none" || device.empty()) {
            settings.device.clear();
        } else if (findDeviceProfile(device)) {
            settings.device = device;
        } else {
            std::string ids;
            for (const DeviceProfile& profile : deviceProfiles()) ids += std::string(" ") + profile.id;
            call->fail("device must be none, custom (with custom_device) or one of:" + ids + ".");
            return;
        }
        changed.push_back("device");
    }
    if (args.contains("device_overlay")) {
        settings.deviceOverlay = args.value("device_overlay", true);
        changed.push_back("device_overlay");
    }
    if (args.contains("safe_area_outline")) {
        settings.safeAreaOutline = args.value("safe_area_outline", false);
        changed.push_back("safe_area_outline");
    }
    if (args.contains("language")) {
        const std::string language = args["language"].is_string() ? args["language"].get<std::string>() : std::string();
        const std::string tag = (language == "system" || language.empty()) ? std::string() : LocaleRules::normalize(language);
        if (!language.empty() && language != "system" && LocaleRules::languageCode(tag).size() < 2) {
            call->fail("language must be \"system\" or a BCP 47 tag like es-MX, pt-BR, zh-Hant.");
            return;
        }
        settings.language = tag;
        changed.push_back("language");
    }

    if (!changed.empty()) gw->applySettings(settings);

    // a fixed resolution: a preset or custom name, "WxH[@dpi]", {width, height, dpi[, name]} (a
    // name saves it as a custom one), "free" back to the aspect
    if (args.contains("resolution")) {
        const Json& value = args["resolution"];
        std::string error;
        bool ok = false;
        if (value.is_string()) {
            ok = gw->setResolution(value.get<std::string>(), &error);
        } else if (value.is_object()) {
            GameResolution resolution;
            resolution.width = value.value("width", 0);
            resolution.height = value.value("height", 0);
            resolution.dpi = value.value("dpi", 0.0f);
            resolution.name = value.value("name", std::string());
            if (!isValidGameResolution(resolution.width, resolution.height, resolution.dpi)) {
                error = "resolution {width, height} must be 16..16384, dpi 0..2000";
            } else if (!resolution.name.empty()) {
                ok = gw->addCustomResolution(resolution, &error) && gw->setResolution(resolution.name, &error);
            } else {
                char text[64];
                std::snprintf(text, sizeof(text), "%dx%d@%g", resolution.width, resolution.height, resolution.dpi);
                ok = gw->setResolution(text, &error);
            }
        } else {
            error = "resolution must be a string (\"1920x1080\", a preset name, \"free\") or {width, height, dpi, name}";
        }
        if (!ok) {
            call->fail("resolution: " + error + ".");
            return;
        }
        changed.push_back("resolution");
    }

    if (args.contains("orientation")) {
        ScreenOrientation orientation;
        const std::string name = args.value("orientation", "");
        if (!PlatformRules::parseOrientationName(name, orientation) || !PlatformRules::isFixedOrientation(orientation)) {
            call->fail("orientation must be portrait, portrait_upside_down, landscape_left or landscape_right.");
            return;
        }
        const DeviceProfile* device = Platform::getSimulation().device;
        if (!device) {
            call->fail("orientation turns a simulated device: pass device too (e.g. {device: \"iphone_15_pro\", "
                       "orientation: \"" + name + "\"}).");
            return;
        }
        if (!(gw->getRotatableOrientations() & PlatformRules::orientationBit(orientation))) {
            call->fail(std::string(device->name) + " cannot show " + name + " now" +
                       (project->isAnyScenePlaying() ? " (the playing game allows other orientations: Screen.orientation / "
                                                       "Screen.autorotateTo*)." : " (iPhones do not turn upside down)."));
            return;
        }
        // as Rotate: the toolbar's orientation, and the device turned now
        gw->turnDevice(orientation);
        changed.push_back("orientation");
    }
    // the Game view lays the new screen out and hands its insets to Screen in the next frames
    if (changed.empty()) {
        replyGameViewSettings(call, changed);
        return;
    }
    auto frames = std::make_shared<int>(0);
    call->waitUntil([frames]() { return ++*frames >= 3; }, 5000, [call, changed](bool) {
        replyGameViewSettings(call, changed);
    });
}

void replyGameViewSettings(const CallPtr& call, const std::vector<std::string>& changed) {
    McpContext& ctx = call->ctx();
    GameWindow* gw = ctx.gameWindow();
    Project* project = ctx.project();
    const GameViewSettings& settings = gw->getSettings();

    const PlatformSimulation& simulation = Platform::getSimulation();
    Json devices = Json::array();
    for (const DeviceProfile& profile : deviceProfiles()) devices.push_back(profile.id);
    devices.push_back("custom");
    auto rectArray = [](const Rect& rect) {
        auto r = [](float v) { return std::round(v * 100.0f) / 100.0f; };
        return Json::array({r(rect.getX()), r(rect.getY()), r(rect.getWidth()), r(rect.getHeight())});
    };
    Json resolutions = Json::array();
    for (const GameResolution& r : builtInGameResolutions()) resolutions.push_back(r.name);
    Json customResolutions = Json::array();
    for (const GameResolution& r : project->getCustomResolutions())
        customResolutions.push_back({{"name", r.name}, {"width", r.width}, {"height", r.height}, {"dpi", r.dpi}});
    int fixedWidth = 0, fixedHeight = 0;
    float fixedDpi = 0.0f;
    Json resolution = "free";
    if (gw->getFixedResolution(fixedWidth, fixedHeight, fixedDpi)) {
        resolution = {{"name", simulation.device ? std::string(simulation.device->id) : settings.resolution},
                      {"width", fixedWidth}, {"height", fixedHeight}, {"dpi", fixedDpi}};
    }

    // the lines the playing scenes drew last frame (0 until a frame is drawn after turning it on)
    size_t physicsDebugLines = 0;
    for (const SceneProject& sceneProject : project->getScenes()) {
        if (sceneProject.playState == ScenePlayState::STOPPED || !sceneProject.scene) continue;
        if (auto physics = sceneProject.scene->getSystem<PhysicsSystem>()) physicsDebugLines += physics->getDebugLineCount();
    }

    Json data = {
        {"aspect", aspectNames()[std::clamp(settings.aspect, 0, (int)aspectNames().size() - 1)]},
        {"scale", settings.scale},
        {"pixel_scale", settings.pixelScale},
        {"maximize_on_play", settings.maximizeOnPlay},
        {"maximized", gw->isMaximizedNow()},
        {"show_stats", settings.showStats},
        {"simulate_touch", project->isSimulateTouch()},
        {"simulate_textures", textureFamilyName(TextureCookQueue::get().getViewFamily())},
        {"atmosphere_quality", std::vector<std::string>{"project", "off", "low", "medium", "high"}[(size_t)std::clamp(settings.atmosphereQuality + 1, 0, 4)]},
        {"physics_debug", Physics::isDebugDraw()},
        {"physics_debug_contacts", Physics::isDebugDrawContacts()},
        {"physics_debug_lines", physicsDebugLines},
        {"network", Http::isOfflineSimulated() ? "offline" : "online"},
        {"open", gw->isOpen()},
        {"visible", gw->isVisible()},
        {"view_size", {gw->getWidth(), gw->getHeight()}},
        {"render_size", {gw->getRenderWidth(), gw->getRenderHeight()}},
        {"canvas", {project->getCanvasWidth(), project->getCanvasHeight()}},
        {"device", simulation.device ? Json(simulation.device->id) : Json("none")},
        {"orientation", PlatformRules::orientationName(simulation.orientation)},
        {"language", settings.language.empty() ? std::string("system") : settings.language},
        {"device_overlay", settings.deviceOverlay},
        {"safe_area_outline", settings.safeAreaOutline},
        // Screen.safeArea (canvas) and Screen.safeAreaPixels (render pixels) as the game sees them now
        {"safe_area", rectArray(Screen::getSafeArea())},
        {"safe_area_pixels", rectArray(Screen::getSafeAreaPixels())},
        {"devices", devices},
        {"resolution", resolution},
        {"resolutions", resolutions},
        {"custom_resolutions", customResolutions},
        // Screen.dpi as Play reads it: the device's, a resolution preset's, else the host's
        {"screen_dpi", Screen::getDpi()},
        {"changed", changed}
    };
    if (simulation.device && std::string(simulation.device->id) == "custom") {
        const CustomDeviceSettings& c = settings.custom;
        data["custom_device"] = {{"width", c.width}, {"height", c.height}, {"corner_radius", c.cornerRadius},
                                 {"dpi", c.dpi}, {"insets", {{"top", c.top}, {"right", c.right}, {"bottom", c.bottom}, {"left", c.left}}}};
    }
    call->ok(std::string(changed.empty() ? "Game view settings: " : "Updated the Game view: ") + data.dump(), data);
}

} // namespace

namespace w3 {

SceneProject* resolveSceneViewScene(Project* project, const Json& ref, std::string& error) {
    return resolveViewScene(project, ref, error);
}

void whenSceneViewShown(const CallPtr& call, uint32_t sceneId, bool focus, int timeoutMs,
                        std::function<void(bool ready, const std::string& error)> then) {
    whenSceneViewReady(call, sceneId, focus, timeoutMs, std::move(then));
}

void replyWithSceneShot(const CallPtr& call, uint32_t sceneId, const std::string& text, Json data) {
    replyMaybeWithScreenshot(call, sceneId, text, std::move(data));
}

Json sceneViewCameraJson(SceneProject* sceneProject) {
    return sceneCameraJson(sceneProject);
}

} // namespace w3

void registerViewTools(McpRegistry& registry) {
    using namespace schema;

    const Json viewScene = sceneRef();
    const Json sceneShot = withDefault(boolean("Also return a Scene view image."), false);
    const Json maxWidth = w3::maxWidthArgSchema(8192, 768);

    {
        ToolDef tool;
        tool.name = "screenshot";
        tool.title = "Screenshot";
        tool.description =
            "Capture a view as PNG. game: the running game, or (stopped) the selected scene through its "
            "camera. scene: the Scene view with gizmos (aim: set_scene_camera / frame_entity). editor: the "
            "whole window. canvas_rect_in_image maps image -> send_input coords; a simulated device: its "
            "overlay, safe_area_in_image. Warns on a flat image.";
        tool.inputSchema = object({
            {"view", withDefault(stringEnum("", {"game", "scene", "editor", "title_designer"}), "game")},
            {"scene", sceneRef("scene view: whose tab; stopped game view: preview it (selects it).")},
            {"max_width", w3::maxWidthArgSchema(8192, 1024)},
            {"save_path", string("Also write the PNG to this project-relative path.")},
            {"focus", withDefault(boolean("Front the view's tab if needed."), true)},
            {"timeout_seconds", withDefault(number("", 1, 600), 30)},
            {"device_overlay", withDefault(boolean(""), true)}
        });
        tool.annotations.readOnly = true;
        tool.writesFiles = [](const Json& args) {
            return args.contains("save_path") && !(args["save_path"].is_string() && args["save_path"].get<std::string>().empty());
        };
        tool.handler = screenshotTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "set_scene_camera";
        tool.title = "Set Scene View Camera";
        tool.description =
            "Aim the editor's Scene view camera (not a game camera; no undo). 3D: position, target, orbit (yaw/pitch "
            "degrees around the target), distance; 2D/UI: center, zoom. No arguments: reads.";
        tool.inputSchema = object({
            {"scene", viewScene},
            {"position", vec3("")},
            {"target", vec3("")},
            {"orbit", object({{"yaw", number("")}, {"pitch", number("")}})},
            {"distance", number("", 0.2, 2000)},
            {"center", vec2("World point at the view centre.")},
            {"zoom", number("World units per pixel.", 0.01, 1000)},
            {"display", anyObject("Overlay flags, e.g. {show_grid_3d: false, draw_mode: \"lod_level\"}; saved with the workspace.")},
            {"projection", stringEnum("", {"perspective", "orthographic"})},
            {"ortho_size", number("Half height.", 0.001, 100000)},
            {"view", stringEnum("", {"top", "bottom", "front", "back", "left", "right"})},
            {"frame_all", boolean("As Alt+F.")},
            {"camera_settings", anyObject("fov, near, far, speed, acceleration, easing, dynamic_clipping.")},
            {"screenshot", sceneShot},
            {"max_width", maxWidth}
        });
        tool.handler = setSceneCameraTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "frame_entity";
        tool.activity = ToolActivity::Control;  // busy policy (McpServer::busyRefusal)
        tool.title = "Frame GameObjects";
        tool.description =
            "Frame GameObjects in the Scene view like pressing F (runtime-spawned too; none given: the selection). "
            "Changes neither scene nor selection.";
        tool.inputSchema = object({
            {"scene", viewScene},
            {"entity", entityRef()},
            {"entities", entities()},
            {"screenshot", sceneShot},
            {"max_width", maxWidth}
        });
        tool.annotations.idempotent = true;
        tool.handler = frameEntityTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "focus_window";
        tool.activity = ToolActivity::Control;  // busy policy (McpServer::busyRefusal)
        tool.title = "Focus Window";
        // pose: {entity (bind), path (.poseanim), index (pose it), play (preview), tab (Pose, Feel,
        // Humanize, an added tab, or map_bones: the mapping popup)}; the result has the window's
        // state. Budget-neutral with the pose_animation window added. title_designer (S82 R78): its keys
        // are in jasper://docs/title-art and an unknown key fails listing them, which pays for its schema;
        // its words were cut again when the wave-2 merges took core 16 characters over its limit.
        tool.description =
            "Fronts a panel (screenshot needs none). console: "
            "its rows, categories, selection; preferences: opens a page; systems: each row's Enabled rect; "
            "pose_animation: pose {entity, path, index, play, tab}.";
        tool.inputSchema = object({
            {"window", stringEnum("", {"scene", "game", "console", "hierarchy", "inspector", "project", "animation", "animator", "particle_effect", "pose_animation",
                                       "terrain", "mcp", "systems", "jobs_memory", "profiler", "frame_debugger", "preferences",
                                       "undo_history", "play_mode_changes", "texture_synthesis", "vertex_animation_baker", "sprite_normals",
                                       "title_designer", "graph_editor"})},
            {"scene", sceneRef("window scene: whose tab.")},
            {"console", object({
                {"hidden_categories", array(string(""), "Categories the filter hides.")},
                {"collapse", boolean("")},
                {"select", integer("Click this message id's row.")}
            })},
            {"page", string("preferences: a page title.")},
            {"close", boolean("preferences: close it.")},
            {"profiler", object({{"load", string("A .jetrace.json to load.")}})},
            {"path", string("graph_editor: file to open")},
            {"pose", anyObject("")},
            {"title_designer", anyObject("")}
        }, {"window"});
        tool.annotations.idempotent = true;
        tool.handler = focusWindowTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "find_ui_item";
        tool.group = "editor_ux";  // S31 tool groups: listed once enabled, callable always
        tool.title = "Find UI Item";
        tool.description =
            "Where editor controls with this label are drawn now (window coords, as focus_window's rect; enabled false: "
            "greyed out), to click them with send_input or real input. Only drawn items: scroll or open their panel first.";
        tool.inputSchema = object({
            {"label", string("Text (also after its icons), its ## id (these two only while in view), or the whole label.")},
            {"window", string("Only in windows whose name contains this.")}
        }, {"label"});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = locateUiItemTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "configure_game_view";
        tool.title = "Configure Game View";
        tool.description =
            "Read (no arguments) or change the Game view toolbar (editor preferences, not undoable): aspect (project = "
            "the canvas size), resolution (the game and screenshots render at it; its dpi is Screen.dpi), a simulated "
            "device, orientation and language (editor only; safe_area is Screen.safeArea). Options: jasper://docs/ui.";
        tool.inputSchema = object({
            {"aspect", stringEnum("", {"free", "project", "16:9", "16:10", "4:3", "9:16"})},
            {"scale", number("", 1, 5)},
            {"pixel_scale", integer("Pixel Perfect scale; 0 auto.", 0, 8)},
            {"maximize_on_play", boolean("")},
            {"show_stats", boolean("")},
            {"simulate_touch", boolean("")},
            {"simulate_textures", stringEnum("", {"desktop", "mobile", "android_etc2", "web"})},
            {"atmosphere_quality", stringEnum("", {"project", "off", "low", "medium", "high"})},
            {"physics_debug", boolean("")},
            {"physics_debug_contacts", boolean("")},
            {"network", stringEnum("", {"online", "offline"})},
            {"device", string("A devices id, custom or none.")},
            {"orientation", stringEnum("", {"portrait", "portrait_upside_down", "landscape_left", "landscape_right"})},
            {"language", string("es-MX..., or system.")},
            {"device_overlay", boolean("")},
            {"safe_area_outline", boolean("")},
            {"custom_device", anyObject("")},
            {"resolution", any("\"WxH\", \"WxH@dpi\", a name, free, or {width, height, dpi, name}.")}
        });
        tool.annotations.idempotent = true;
        // the Game view's editor preferences (S31: outside the default listing)
        tool.group = "editor_ux";
        tool.handler = configureGameViewTool;
        registry.addTool(std::move(tool));
    }
}

}
