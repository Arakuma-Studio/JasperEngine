// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// W3: screenshots, Scene view camera, window focus, Game view settings
#include "McpTools.h"
#include "PlayViewCommon.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpValues.h"

#include "App.h"
#include "AppSettings.h"
#include "Engine.h"
#include "Project.h"
#include "mcp/McpActivityWindow.h"
#include "render/SceneRender.h"
#include "render/SceneRender2D.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/UISystem.h"
#include "object/ui/UIDocument.h"
#include "util/Base64.h"
#include "util/FileUtils.h"
#include "util/GraphicUtils.h"
#include "window/ConsoleWindow.h"
#include "window/GameWindow.h"
#include "window/SceneWindow.h"

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
#include <chrono>
#include <cmath>
#include <fstream>
#include <memory>

namespace fs = std::filesystem;

namespace jasperengine::editor::mcp {

namespace {

using Clock = std::chrono::steady_clock;

bool sceneContentLoaded(SceneProject* sp) {
    if (!sp || !sp->scene) return false;
    auto rs = sp->scene->getSystem<RenderSystem>();
    if (rs && !rs->isAllLoaded()) return false;
    auto us = sp->scene->getSystem<UISystem>();
    if (us && us->hasPendingImages()) return false;
    // pinned stateful UI effects still replaying their state (phase 6): drawn a few frames later
    if (UIDocument::getPendingEffectReplays() > 0) return false;
    auto ms = sp->scene->getSystem<MeshSystem>();
    if (ms && ms->hasPendingAsyncModelLoads()) return false;
    return true;
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

// How long a screenshot waits for loading assets before it shoots anyway
constexpr long long LOAD_WAIT_MS = 6000;

struct CaptureJob {
    CallPtr call;
    w3::CaptureRequest req;
    std::function<void(w3::CaptureResult)> done;
    Clock::time_point deadline;
    bool finished = false;
    // assets were still loading when the wait for them gave up (some never report loaded)
    bool stillLoading = false;
    Clock::time_point loadWaitStart;
    bool loadWaitStarted = false;

    // True once the scene's content is loaded, or after waiting long enough for it
    bool contentReady(SceneProject* sp) {
        if (sceneContentLoaded(sp)) {
            stillLoading = false;
            return true;
        }
        if (!loadWaitStarted) {
            loadWaitStarted = true;
            loadWaitStart = Clock::now();
        }
        const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - loadWaitStart).count();
        stillLoading = true;
        return waited >= LOAD_WAIT_MS;
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
bool encodePixels(const uint8_t* pixels, int width, int height, int channels, int maxWidth,
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

// True when every pixel of the PNG is the same colour (a blank render)
bool isFlatPng(const std::vector<uint8_t>& png, std::string& colour) {
    int w = 0, h = 0, n = 0;
    unsigned char* pixels = stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &w, &h, &n, 3);
    if (!pixels) return false;
    bool flat = true;
    for (int i = 1; i < w * h && flat; i++) {
        if (pixels[i * 3] != pixels[0] || pixels[i * 3 + 1] != pixels[1] || pixels[i * 3 + 2] != pixels[2]) flat = false;
    }
    if (flat && w * h > 0) {
        char buffer[16];
        snprintf(buffer, sizeof(buffer), "#%02x%02x%02x", pixels[0], pixels[1], pixels[2]);
        colour = buffer;
    }
    stbi_image_free(pixels);
    return flat;
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
    if (isFlatPng(png, colour)) {
        info["flat_colour"] = colour;
        result.note = "The image is one flat colour (" + colour + "): nothing visible rendered there yet?";
    }
    if (job->stillLoading) {
        info["assets_loading"] = true;
        result.note += std::string(result.note.empty() ? "" : " ") +
                       "Some assets were still loading after " + std::to_string(LOAD_WAIT_MS / 1000) +
                       " s (missing files or a slow load); parts of the scene may be missing.";
    }

    if (!job->req.savePath.empty()) {
        Project* project = job->call->ctx().project();
        fs::path target;
        std::string error;
        const fs::path requested(job->req.savePath);
        if (requested.is_absolute()) {
            target = requested;
        } else if (!McpRefs::resolveProjectPath(project, job->req.savePath, target, error)) {
            job->fail("save_path: " + error);
            return;
        }
        std::error_code ec;
        if (target.has_parent_path()) fs::create_directories(target.parent_path(), ec);
        std::ofstream file(target, std::ios::binary | std::ios::trunc);
        if (!file || !file.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()))) {
            job->fail("Could not write the PNG to " + FileUtils::pathToGenericUtf8(target) + ".");
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
            return ++*frames >= 2 && c.gameWindow()->getFramebuffer()->isCreated();
        }, job->remainingMs(), [job, playId](bool timedOut) {
            McpContext& c = job->call->ctx();
            if (c.project()->getPlaySessionSceneId() != playId) {
                // play ended meanwhile (a crash?): shoot the stopped view
                captureGame(job);
                return;
            }
            if (timedOut) {
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
            else job->fail("The Game view preview did not finish rendering in time (assets still loading?).");
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
                job->fail(std::string(sceneViewShown(cx, sc) ? "The Scene view did not finish rendering in time."
                                                             : "The Scene view is not shown.") +
                          " (visible " + std::to_string(sc->isVisible) + ", size " + std::to_string(sw->getWidth(sceneId)) + "x" +
                          std::to_string(sw->getHeight(sceneId)) + ", assets loaded " + std::to_string(sceneContentLoaded(sc)) + ")");
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
    }, std::min(job->remainingMs(), 10000), [job, readback](bool timedOut) {
        if (timedOut || !readback->done) {
            job->fail("The editor window did not render a frame to read (minimized?).");
            return;
        }
        const int width = readback->width;
        const int height = readback->height;
        // GL rows are bottom first
        std::vector<uint8_t> flipped(readback->pixels.size());
        const size_t row = static_cast<size_t>(width) * 4;
        for (int y = 0; y < height; y++) {
            std::copy_n(readback->pixels.data() + (height - 1 - y) * row, row, flipped.data() + y * row);
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
    if (request.view == "game") captureGame(job);
    else if (request.view == "scene") captureScene(job);
    else if (request.view == "editor") captureEditor(job);
    else job->fail("view must be game, scene or editor.");
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
    if (request.view != "game" && request.view != "scene" && request.view != "editor") {
        call->fail("view must be \"game\" (the Game view: the running game, or the scene through its game camera), "
                   "\"scene\" (a Scene view with gizmos) or \"editor\" (the whole editor window).");
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
        out["distance"] = camera->getDistanceFromTarget();
        out["mode"] = "3d";
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
        {"hide_camera_view", &SceneDisplaySettings::hideCameraView},
        {"hide_light_icons", &SceneDisplaySettings::hideLightIcons},
        {"hide_sound_icons", &SceneDisplaySettings::hideSoundIcons},
        {"hide_container_guides", &SceneDisplaySettings::hideContainerGuides},
        {"hide_selection_outline", &SceneDisplaySettings::hideSelectionOutline},
        {"disable_fog", &SceneDisplaySettings::disableFog},
        {"show_post_processing", &SceneDisplaySettings::showPostProcessing},
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
            if (!displayFlags().count(it.key()) || !it.value().is_boolean()) {
                std::string valid;
                for (const auto& [name, member] : displayFlags()) valid += " " + name;
                call->fail("display." + it.key() + " is not a boolean display flag. Valid:" + valid +
                           ", grid_spacing_2d, grid_spacing_3d (numbers).");
                return;
            }
        }
    }

    const uint32_t sceneId = sp->id;
    whenSceneViewReady(call, sceneId, true, 10000, [call, sceneId, is3D, position, target, center](bool ready, const std::string& err) {
        const Json& a = call->args();
        Project* p = call->ctx().project();
        SceneProject* s = p->getScene(sceneId);
        if (!s || !s->sceneRender || !s->sceneRender->getCamera()) {
            call->fail("The scene closed.");
            return;
        }
        Camera* camera = s->sceneRender->getCamera();
        std::vector<std::string> changed;
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
            {
                const Vector3 dir = (camera->getTarget() - camera->getPosition()).normalize();
                if (std::abs(dir.y) < 0.99f) camera->setUp(0, 1, 0);
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
        if (a.contains("display")) {
            for (auto it = a["display"].begin(); it != a["display"].end(); ++it) {
                if (it.key() == "grid_spacing_2d" && it.value().is_number()) s->displaySettings.gridSpacing2D = it.value().get<float>();
                else if (it.key() == "grid_spacing_3d" && it.value().is_number()) s->displaySettings.gridSpacing3D = it.value().get<float>();
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
        data["display"] = displayJson;
        std::string text = (changed.empty() ? "Scene view camera of \"" : "Moved the Scene view camera of \"") + s->name +
                           "\": " + data["camera"].dump() + ".";
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
        s->needUpdateRender = true;
        Json data = {{"scene", McpRefs::sceneJson(p, *s)}, {"camera", sceneCameraJson(s)}};
        data["framed"] = entities.empty() ? Json("selection") : framed;
        replyMaybeWithScreenshot(call, sceneId,
            "Framed " + (entities.empty() ? std::string("the selection") : std::to_string(entities.size()) + " GameObject(s)") +
            " in the Scene view of \"" + s->name + "\".", data);
    });
}

void focusWindowTool(const CallPtr& call) {
    const Json& args = call->args();
    McpContext& ctx = call->ctx();
    App& app = ctx.app();
    Project* project = ctx.project();
    const std::string window = args.value("window", "");
    std::string imguiName;

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
        ctx.consoleWindow()->setOpen(true);
        imguiName = ConsoleWindow::WINDOW_NAME;
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
    } else if (window == "terrain") {
        ctx.terrainEditWindow()->setOpen(true);
        imguiName = TerrainEditWindow::WINDOW_NAME;
    } else if (window == "mcp") {
        imguiName = McpActivityWindow::WINDOW_NAME;
    } else if (window == "texture_synthesis") {
        app.getTextureSynthesisWindow()->open();
        imguiName = TextureSynthesisWindow::WINDOW_NAME;
    } else {
        call->fail("window must be one of: scene, game, console, hierarchy, inspector, project, animation, terrain, mcp, "
                   "texture_synthesis.");
        return;
    }
    // A window opened just now exists after its next draw
    call->afterFrames(1, [call, imguiName, window]() {
        if (window != "game") ImGui::SetWindowFocus(imguiName.c_str());
        call->afterFrames(2, [call, imguiName, window]() {
            ImGuiWindow* w = ImGui::FindWindowByName(imguiName.c_str());
            const bool shown = w && w->WasActive && !w->Hidden;
            Json data = {{"window", window}, {"shown", shown}};
            if (w) data["size"] = {w->Size.x, w->Size.y};
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
        changed.push_back("aspect");
    }
    if (args.contains("scale")) {
        settings.scale = std::clamp(args.value("scale", 1.0f), 1.0f, 5.0f);
        changed.push_back("scale");
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
    if (!changed.empty()) gw->applySettings(settings);

    Json data = {
        {"aspect", aspectNames()[std::clamp(settings.aspect, 0, (int)aspectNames().size() - 1)]},
        {"scale", settings.scale},
        {"maximize_on_play", settings.maximizeOnPlay},
        {"show_stats", settings.showStats},
        {"simulate_touch", project->isSimulateTouch()},
        {"open", gw->isOpen()},
        {"visible", gw->isVisible()},
        {"view_size", {gw->getWidth(), gw->getHeight()}},
        {"render_size", {gw->getRenderWidth(), gw->getRenderHeight()}},
        {"canvas", {project->getCanvasWidth(), project->getCanvasHeight()}},
        {"changed", changed}
    };
    call->ok(std::string(changed.empty() ? "Game view settings: " : "Updated the Game view: ") + data.dump(), data);
}

} // namespace

void registerViewTools(McpRegistry& registry) {
    using namespace schema;

    const Json viewScene = sceneRef("Default: playing, else selected.");
    const Json sceneShot = withDefault(boolean("Also return a Scene view image."), false);
    const Json maxWidth = w3::maxWidthArgSchema(8192, 768);

    {
        ToolDef tool;
        tool.name = "screenshot";
        tool.title = "Screenshot";
        tool.description =
            "Capture a view as PNG. game: the running game, or (stopped) the selected scene through its camera. "
            "scene: the Scene view with gizmos (aim: set_scene_camera / frame_entity). editor: the whole window. "
            "canvas_rect_in_image maps image -> send_input coords; warns on a flat image.";
        tool.inputSchema = object({
            {"view", withDefault(stringEnum("", {"game", "scene", "editor"}), "game")},
            {"scene", sceneRef("scene view: whose tab; stopped game view: preview it (selects it).")},
            {"max_width", w3::maxWidthArgSchema(8192, 1024)},
            {"save_path", string("Also write the PNG (project-relative or absolute).")},
            {"focus", withDefault(boolean("Front the view's tab if needed."), true)},
            {"timeout_seconds", withDefault(number("", 1, 600), 30)}
        });
        tool.annotations.readOnly = true;
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
            {"target", vec3("Look-at point.")},
            {"orbit", object({{"yaw", number("")}, {"pitch", number("")}})},
            {"distance", number("", 0.2, 2000)},
            {"center", vec2("World point at the view centre.")},
            {"zoom", number("World units per pixel.", 0.01, 1000)},
            {"display", anyObject("Overlay flags, e.g. {show_grid_3d: false, show_post_processing: true}; saved with the workspace.")},
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
        tool.description =
            "Open and bring an editor panel to the front, e.g. to show a watching human. Not needed before "
            "screenshot.";
        tool.inputSchema = object({
            {"window", stringEnum("", {"scene", "game", "console", "hierarchy", "inspector", "project", "animation", "terrain", "mcp",
                                       "texture_synthesis"})},
            {"scene", sceneRef("window scene: whose tab.")}
        }, {"window"});
        tool.annotations.idempotent = true;
        tool.handler = focusWindowTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "configure_game_view";
        tool.title = "Configure Game View";
        tool.description =
            "Read (no arguments) or change the Game view toolbar settings (editor preferences, not undoable). aspect "
            "project = the canvas size (a project setting); the game renders at render_size.";
        tool.inputSchema = object({
            {"aspect", stringEnum("", {"free", "project", "16:9", "16:10", "4:3", "9:16"})},
            {"scale", number("Zoom.", 1, 5)},
            {"maximize_on_play", boolean("")},
            {"show_stats", boolean("")},
            {"simulate_touch", boolean("Mouse = one finger. Not while playing.")}
        });
        tool.annotations.idempotent = true;
        tool.handler = configureGameViewTool;
        registry.addTool(std::move(tool));
    }
}

}
