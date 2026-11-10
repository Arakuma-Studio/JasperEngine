// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SpriteNormalsWindow.h"
#include "component/TilemapComponent.h"

#include "App.h"
#include "Backend.h"
#include "EditorHost.h"
#include "Out.h"
#include "Project.h"
#include "Scene.h"
#include "Theme.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/PropertyCmd.h"
#include "component/Light2DComponent.h"
#include "component/MeshComponent.h"
#include "component/SpriteComponent.h"
#include "component/Transform.h"
#include "external/IconsFontAwesome6.h"
#include "mcp/McpValues.h"
#include "spritenormals/SpriteNormalsWatcher.h"
#include "texsynth/SynthesisSession.h"
#include "util/EntityPath.h"
#include "util/EntityPayload.h"
#include "util/ProjectChecks.h"
#include "util/UIProbe.h"
#include "util/UIUtils.h"
#include "util/Util.h"
#include "window/ProjectWindow.h"
#include "widget/ColorField.h"
#include "window/Widgets.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <mutex>
#include <thread>
#include "util/FileUtils.h"

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;
namespace sn = jasperengine::editor::spritenormals;

namespace {

    // a source above this does not regenerate live (spec 3.1): the Generate button instead
    constexpr int64_t LIVE_PIXELS = 1048576;
    constexpr int LIVE_FRAMES = 64;
    constexpr double DEBOUNCE_SECONDS = 0.15;
    constexpr int MAX_REGIONS = 16;
    constexpr int MAX_REGION_COLOURS = 16;
    // the zoom steps of the mouse wheel (screen pixels per image pixel)
    const float ZOOM_STEPS[] = {0.25f, 0.5f, 1.0f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f, 12.0f, 16.0f, 24.0f, 32.0f, 48.0f, 64.0f};

    std::string lower(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    std::string fileName(const std::string& path) {
        const std::string name = jasperengine::editor::FileUtils::pathToUtf8(fs::u8path(path).filename());
        return name.empty() ? path : name;
    }

    // a number as people write it: 1.5, 4, 0.6
    std::string trimmed(double v, int decimals = 2) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, v);
        std::string text = buffer;
        if (text.find('.') != std::string::npos) {
            while (!text.empty() && text.back() == '0') text.pop_back();
            if (!text.empty() && text.back() == '.') text.pop_back();
        }
        return text;
    }

    std::string joined(const std::vector<std::string>& list, const char* separator = ", ") {
        std::string text;
        for (const std::string& item : list) text += (text.empty() ? "" : separator) + item;
        return text;
    }

    bool isAnimated(sn::SourceKind kind) {
        return kind == sn::SourceKind::Gif || kind == sn::SourceKind::Apng;
    }

    // One core left for the editor (the output never depends on the thread count)
    int workerThreads() {
        const unsigned n = std::thread::hardware_concurrency();
        return static_cast<int>(std::max(1u, n > 1 ? n - 1 : 1u));
    }

    // A crop of an RGBA8 image (clipped to it)
    sn::Image crop(const sn::Image& image, int x, int y, int w, int h) {
        x = std::clamp(x, 0, std::max(0, image.width));
        y = std::clamp(y, 0, std::max(0, image.height));
        w = std::clamp(w, 0, image.width - x);
        h = std::clamp(h, 0, image.height - y);
        if (w <= 0 || h <= 0 || image.empty()) return sn::Image();
        if (x == 0 && y == 0 && w == image.width && h == image.height) return image;
        sn::Image out = sn::Image::make(w, h, image.channels, image.bitDepth);
        const size_t bpp = image.bytesPerPixel();
        for (int row = 0; row < h; row++) std::memcpy(out.pixel(0, row), image.pixel(x, y + row), static_cast<size_t>(w) * bpp);
        return out;
    }

    // the image a project-relative path names, absolute
    fs::path absoluteOf(Project* project, const std::string& rel) {
        const fs::path path = fs::u8path(rel);
        return path.is_absolute() ? path.lexically_normal() : (project->getProjectPath() / path).lexically_normal();
    }

    // The base colour texture of a GameObject's submesh, project-relative ("" when none or not
    // an image file: framebuffers, camera and fluid links have no file)
    std::string baseTextureOf(Project* project, Scene* scene, Entity entity, unsigned submesh) {
        if (!scene || entity == NULL_ENTITY || !scene->isEntityCreated(entity)) return std::string();
        MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
        if (!mesh || submesh >= std::max(mesh->numSubmeshes, 1u)) return std::string();
        const Texture& base = mesh->submeshes[submesh].material.baseColorTexture;
        if (base.getPath().empty() || base.isFramebuffer()) return std::string();
        const std::string rel = mcp::McpValues::assetPathToJson(project, base.getPath());
        std::error_code ec;
        if (!Util::isImageFile(rel) || !fs::is_regular_file(absoluteOf(project, rel), ec)) return std::string();
        return jasperengine::editor::FileUtils::pathToGenericUtf8(fs::u8path(rel).lexically_normal());
    }

    // Whether the ImGui frame belongs to the real editor (a platform backend): the Project
    // window's thumbnails and selection exist only there (headless tests draw without one)
    bool editorUi() {
        return ImGui::GetCurrentContext() && ImGui::GetIO().BackendPlatformName != nullptr;
    }

    const char* viewName(SpriteNormalsWindow::View v) {
        switch (v) {
            case SpriteNormalsWindow::View::Color: return "Color";
            case SpriteNormalsWindow::View::Normal: return "Normal";
            case SpriteNormalsWindow::View::Lit: return "Lit";
            case SpriteNormalsWindow::View::Split: return "Split";
        }
        return "Lit";
    }

}

// ====================================================================== background jobs

// A Shift-click generation or a Regenerate: every check on the main thread (sn::preparePlan reads
// the project, the Platform's asset path and the scene), then the decode (sn::prepareLoad), the
// generation and the encoding on its own thread, which owns the plain data it reads and never
// touches System, Project or a scene; the main thread writes the files (an undoable command) and
// assigns.
struct SpriteNormalsWindow::QuickWork {
    std::atomic<bool> cancel{false};
    std::atomic<bool> done{false};
    int threads = 0;
    uint64_t generation = 0;             // texsynth::projectGeneration() when it started
    sn::Prepared prepared;
    sn::Result result;
    std::vector<sn::OutputFile> files;
    std::string error;
    std::mutex mutex;
    std::condition_variable finished;
};

void SpriteNormalsWindow::startWork(const std::shared_ptr<QuickWork>& work) {
    work->generation = texsynth::projectGeneration();
    auto run = [work]() {
        try {
            std::vector<sn::Problem> problems;
            if (!sn::prepareLoad(work->prepared, problems)) {
                work->error = problems.empty() ? std::string("the source can't be read") : problems.front().message;
            } else {
                work->prepared.request.threads = work->threads;
                sn::Control control;
                control.cancel = &work->cancel;
                // the map's paint (kept from it) composited again
                work->result = sn::generatePrepared(work->prepared, control);
                if (work->result.status == sn::Status::Ok && !work->cancel) {
                    sn::encodeOutputs(work->prepared, work->result, work->files, work->error);
                } else if (work->result.status != sn::Status::Ok && work->result.status != sn::Status::Cancelled) {
                    work->error = work->result.errors.empty() ? std::string("the generation failed") : work->result.errors.front().message;
                }
            }
        } catch (const std::exception& e) {
            work->error = std::string("the generation failed: ") + e.what();
        } catch (...) {
            work->error = "the generation failed";
        }
        {
            std::lock_guard<std::mutex> lock(work->mutex);
            work->done = true;
        }
        work->finished.notify_all();
    };
    try {
        std::thread(std::move(run)).detach();
    } catch (const std::system_error& e) {
        work->error = std::string("could not start a worker thread: ") + e.what();
        work->done = true;
    }
}

void SpriteNormalsWindow::shutdownJobs(double timeoutSeconds) {
    for (QuickJob& job : quickJobs) {
        if (job.work) job.work->cancel = true;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeoutSeconds);
    for (QuickJob& job : quickJobs) {
        if (!job.work) continue;
        std::unique_lock<std::mutex> lock(job.work->mutex);
        job.work->finished.wait_until(lock, deadline, [&job]() { return job.work->done.load(); });
    }
    quickJobs.clear();
}

void SpriteNormalsWindow::waitJobWorkers() {
    for (QuickJob& job : quickJobs) {
        if (!job.work) continue;
        std::unique_lock<std::mutex> lock(job.work->mutex);
        job.work->finished.wait(lock, [&job]() { return job.work->done.load(); });
    }
}

SpriteNormalsWindow::RegenerateOutcome SpriteNormalsWindow::regenerateOutcome(const fs::path& generatedPng) const {
    const fs::path png = generatedPng.is_absolute() ? generatedPng : project->getProjectPath() / generatedPng;
    auto it = regenerateOutcomes.find(jasperengine::editor::FileUtils::pathToGenericUtf8(png.lexically_normal()));
    return it == regenerateOutcomes.end() ? RegenerateOutcome() : it->second;
}

// ====================================================================== lifecycle

SpriteNormalsWindow::SpriteNormalsWindow(Project* project)
    : project(project) {
    settings = sn::presetSettings(sn::Preset::Default);
    // the Angle brush starts visibly tilted (up-left), so it never looks like Flatten
    brush.direction = sn::directionFromAngles(135.0f, 40.0f);
}

SpriteNormalsWindow::~SpriteNormalsWindow() {
    if (run) run->cancel();
    for (QuickJob& job : quickJobs) {
        if (job.work) job.work->cancel = true;
    }
}

void SpriteNormalsWindow::open() {
    loadStored();
    setOpen(true);
    focusRequested = true;
}

void SpriteNormalsWindow::setOpen(bool openIt) {
    if (openIt) {
        loadStored();
        if (!windowOpen) focusRequested = true;
        windowOpen = true;
        return;
    }
    windowOpen = false;
    focusRequested = false;
    focused = false;
    // closing frees the GPU copies and stops a live run (a Shift-click generation goes on)
    stopRun();
    saveAll = SaveAllJob();
    changedAt = -1.0;
    pickingRegion = -1;
    playing = false;
    retire(colorGpu);
    retire(normalGpu);
    retire(litGpu);
    retire(hemisphereGpu);
    endStroke();
}

bool SpriteNormalsWindow::isOpen() const {
    return windowOpen;
}

bool SpriteNormalsWindow::isFocused() const {
    return windowOpen && focused;
}

void SpriteNormalsWindow::resetForProjectSwitch() {
    stopRun();
    run.reset();
    for (QuickJob& job : quickJobs) {
        if (job.work) job.work->cancel = true;
    }
    quickJobs.clear();
    regenerateOutcomes.clear();
    saveAll = SaveAllJob();
    items.clear();
    current = 0;
    settings = sn::presetSettings(sn::Preset::Default);
    target = AssignTarget();
    assign = false;
    runErrors.clear();
    changedAt = -1.0;
    statusMessage.clear();
    statusError = false;
    notices.clear();
    lastWritten.clear();
    warnings.clear();
    warningsDirty = true;
    lastConfirm = nullptr;
    assignRecorded = false;
    assignSerial = 0;
    writeRecorded = false;
    writeSerial = 0;
    storedLoaded = false;
    storedProject.clear();
    view = View::Lit;
    frame = -1;
    playing = false;
    orbit = false;
    light = LightState();
    pickingRegion = -1;
    lastEditedRegion = -1;
    projectImages.clear();
    projectImagesScannedAt = -1.0;
    sourceText.clear();
    colorRgba = sn::Image();
    colorLoadedKey.clear();
    litRgba = sn::Image();
    litDirty = true;
    retire(colorGpu);
    retire(normalGpu);
    retire(litGpu);
    retire(hemisphereGpu);
    paint = sn::PaintOverlay();
    paintSaved = sn::PaintOverlay();
    paintTarget.clear();
    paintProblem.clear();
    paintUndo.clear();
    paintRedo.clear();
    paintUndoBytes = 0;
    paintStrokes = 0;
    paintTool = sn::PaintTool::Navigate;
    stroking = false;
    paintOpenSet = false;
    paintChanged();
}

void SpriteNormalsWindow::loadStored() {
    // a project's settings exist once it is loaded (startup restores the window before that;
    // a switch resets them): taken once per project, before anything stores over them
    const fs::path path = project->getProjectPath();
    if (path.empty() || (storedLoaded && storedProject == path)) return;
    storedLoaded = true;
    storedProject = path;
    const SpriteNormalsWindowSettings& s = project->getSpriteNormalsSettings();
    settings = s.settings;
    view = static_cast<View>(std::clamp(s.view, 0, 3));
    light.colour = s.lightColour;
    light.ambient = s.ambient;
    orbit = s.orbit;
    grid = s.grid;
    advancedOpen = s.advancedOpen;
    light.placed = false;
}

void SpriteNormalsWindow::storeSettings() {
    // never over a project's stored settings not read yet
    loadStored();
    if (!storedLoaded) return;
    SpriteNormalsWindowSettings& s = project->getSpriteNormalsSettings();
    s.settings = settings;
    // regions without colours yet are the window's, not settings (validateSettings refuses them)
    auto& regions = s.settings.regions;
    regions.erase(std::remove_if(regions.begin(), regions.end(), [](const sn::Region& r) { return r.colors.empty(); }), regions.end());
    s.view = static_cast<int>(view);
    s.lightHeight = light.userHeight ? light.height : 0.0f;
    s.lightColour = light.colour;
    s.ambient = light.ambient;
    s.orbit = orbit;
    s.grid = grid;
    s.advancedOpen = advancedOpen;
}

void SpriteNormalsWindow::setStatus(const std::string& message, bool error) {
    statusMessage = message;
    statusError = error;
}

double SpriteNormalsWindow::now() const {
    return (ImGui::GetCurrentContext() ? ImGui::GetTime() : 0.0) + clockOffset;
}

// ====================================================================== items

SpriteNormalsWindow::Item SpriteNormalsWindow::makeItem(const sn::Spec& spec) const {
    Item item;
    item.spec = spec;
    item.spec.overwrite = false;
    if (spec.hasSprite()) {
        const SceneProject* sp = project->getScene(spec.sceneId);
        item.label = sp && sp->scene && sp->scene->isEntityCreated(spec.entity) ? sp->scene->getEntityName(spec.entity) : std::string("GameObject");
    } else {
        item.label = fileName(spec.source);
    }
    return item;
}

const Scene* SpriteNormalsWindow::sceneFor(const sn::Spec& spec) const {
    if (!spec.hasSprite()) return nullptr;
    const SceneProject* sp = project->getScene(spec.sceneId);
    return sp ? sp->scene : nullptr;
}

sn::Spec SpriteNormalsWindow::specOf(const Item& item) const {
    sn::Spec spec = item.spec;
    spec.settings = settings;
    // a region still without colours (just added, its colours not picked yet) matches nothing:
    // left out of the run instead of refusing it
    auto& regions = spec.settings.regions;
    regions.erase(std::remove_if(regions.begin(), regions.end(), [](const sn::Region& r) { return r.colors.empty(); }), regions.end());
    // single mode: the window keeps the paint layer itself (loaded from the target, composited
    // over every result, written by Save), so the runs make the plain generation; a batch keeps
    // each map's own paint (the session composites it into the result it writes)
    if (!isBatch()) {
        spec.paintMode = sn::PaintMode::Replace;
        spec.paint.clear();
    }
    return spec;
}

std::string SpriteNormalsWindow::keyOf(const Item& item) const {
    return specOf(item).fingerprint();
}

void SpriteNormalsWindow::probe(Item& item, bool force) {
    if (item.probed && !force) return;
    item.probed = true;
    item.probeOk = false;
    item.probeError.clear();
    item.info = sn::SourceInfo();
    item.spriteCells.clear();
    std::string texture = item.spec.source;
    if (item.spec.hasSprite()) {
        std::string error;
        if (!sn::framesFromSprite(sceneFor(item.spec), item.spec.entity, item.spec.submesh, texture, item.spriteCells, error)) {
            item.texturePath.clear();
            item.probeError = error;
            return;
        }
    }
    item.texturePath = texture;
    if (texture.empty()) return;
    const fs::path abs = absoluteOf(project, texture);
    std::error_code ec;
    item.stampTime = fs::last_write_time(abs, ec);
    item.stampSize = ec ? 0 : fs::file_size(abs, ec);
    item.changedOnDisk = false;
    std::vector<sn::Problem> problems;
    item.probeOk = sn::probeSource(abs, item.info, problems);
    (void)ec;
    if (!item.probeOk && !problems.empty()) item.probeError = problems.front().message;
}

void SpriteNormalsWindow::refreshProblems(Item& item) {
    // outputs that exist are a row of their own (Save asks): checked as overwriting
    sn::Spec spec = specOf(item);
    spec.overwrite = true;
    // what the full check looks at besides the settings: a slider drag only re-validates the
    // settings (checkSpec probes the source file)
    std::string key = spec.source + "|" + std::to_string(spec.sceneId) + ":" + std::to_string(spec.entity) + ":" + std::to_string(spec.submesh);
    const sn::FramesSpec& f = spec.frames;
    const sn::Grid& g = f.grid;
    key += "|" + std::to_string(static_cast<int>(f.mode)) + ":" + std::to_string(static_cast<int>(f.from));
    for (int v : {g.tileWidth, g.tileHeight, g.columns, g.rows, g.count, g.offsetX, g.offsetY, g.paddingX, g.paddingY}) key += "," + std::to_string(v);
    for (const sn::Cell& c : f.cells) key += ";" + std::to_string(c.x) + "," + std::to_string(c.y) + "," + std::to_string(c.width) + "," + std::to_string(c.height);
    key += "|" + spec.output.normal + "|" + spec.output.atlas + "|" + spec.output.height + "|" + (spec.settings.height ? "h" : "-");
    key += "|" + std::to_string(static_cast<int64_t>(item.stampTime.time_since_epoch().count())) + ":" + std::to_string(item.stampSize);
    // a GameObject source: whether it is there, the texture it draws and its frame rects
    if (spec.hasSprite()) {
        const Scene* scene = sceneFor(spec);
        if (!scene || !scene->isEntityCreated(spec.entity)) {
            key += "|gone";
        } else {
            Scene* s = const_cast<Scene*>(scene);
            if (MeshComponent* mesh = s->findComponent<MeshComponent>(spec.entity)) {
                key += "|" + (spec.submesh < std::max(mesh->numSubmeshes, 1u) ? mesh->submeshes[spec.submesh].material.baseColorTexture.getPath() : std::string());
            }
            if (SpriteComponent* sprite = s->findComponent<SpriteComponent>(spec.entity)) {
                key += "|" + std::to_string(sprite->numFramesRect);
                for (unsigned i = 0; i < sprite->numFramesRect; i++) {
                    const Rect& r = sprite->framesRect[i].rect;
                    char rect[96];
                    std::snprintf(rect, sizeof(rect), ";%g,%g,%g,%g", r.getX(), r.getY(), r.getWidth(), r.getHeight());
                    key += rect;
                }
            }
        }
    }
    if (key == item.problemsKey) {
        // only the settings changed: their problems again, first as checkSpec lists them
        std::vector<sn::Problem> problems = sn::validateSettings(spec.settings);
        for (const sn::Problem& p : item.problems) {
            if (p.kind != "invalid_settings") problems.push_back(p);
        }
        item.problems = std::move(problems);
        return;
    }
    item.problemsKey = key;
    item.problems = sn::checkSpec(project->getProjectPath(), spec, sceneFor(spec));
}

bool SpriteNormalsWindow::liveAllowed(const Item& item) const {
    if (!item.probeOk) return true;   // the run reports what is wrong
    const int64_t pixels = static_cast<int64_t>(item.info.width) * item.info.height;
    const int frames = std::max<int>(item.info.frameCount, static_cast<int>(item.spriteCells.size()));
    return pixels <= LIVE_PIXELS && frames <= LIVE_FRAMES;
}

void SpriteNormalsWindow::markChanged() {
    changedAt = now();
    warningsDirty = true;
    litDirty = true;
    if (current < items.size()) refreshProblems(items[current]);
}

void SpriteNormalsWindow::resetPreviewFor(bool keepLight) {
    frame = -1;
    playing = false;
    playElapsed = 0.0;
    fitRequested = true;
    pan = ImVec2(0, 0);
    pickingRegion = -1;
    colorLoadedKey.clear();
    litDirty = true;
    if (!keepLight) light.placed = false;
    sourceText = current < items.size() ? (items[current].spec.hasSprite() ? items[current].label : items[current].spec.source) : std::string();
}

std::shared_ptr<sn::SpriteNormalsRun> SpriteNormalsWindow::shown() const {
    if (current >= items.size()) return nullptr;
    const std::shared_ptr<sn::SpriteNormalsRun>& r = items[current].result;
    return r && r->finished() && r->result().status == sn::Status::Ok ? r : nullptr;
}

// ====================================================================== entry points

bool SpriteNormalsWindow::openSources(const std::vector<fs::path>& images, std::string& error) {
    const fs::path root = project->getProjectPath();
    std::vector<Item> next;
    std::vector<std::string> skipped;
    for (const fs::path& given : images) {
        const fs::path abs = given.is_absolute() ? given.lexically_normal() : (root / given).lexically_normal();
        const std::string ext = lower(jasperengine::editor::FileUtils::pathToUtf8(abs.extension()));
        std::error_code ec;
        if (!Util::isImageFile(ext) || ext == ".svg" || !fs::is_regular_file(abs, ec)) {
            skipped.push_back(jasperengine::editor::FileUtils::pathToUtf8(abs.filename()));
            continue;
        }
        sn::Spec spec;
        spec.source = sn::projectRelative(root, abs);
        next.push_back(makeItem(spec));
    }
    if (next.empty()) {
        error = skipped.empty() ? "no image was given" : joined(skipped) + " is not a PNG, GIF, APNG, JPEG, TGA or BMP image of the project";
        return false;
    }
    // unsaved strokes: asked first (Discard opens the images)
    if (!paintDiscardOk([this, images]() {
            std::string e;
            if (!openSources(images, e)) setStatus(e, true);
        })) {
        return true;
    }
    loadStored();
    stopRun();
    saveAll = SaveAllJob();
    items = std::move(next);
    current = 0;
    notices.clear();
    runErrors.clear();
    lastWritten.clear();
    setStatus("", false);
    target = AssignTarget();
    assign = false;
    for (Item& item : items) {
        probe(item, true);
        refreshProblems(item);
    }
    if (items.size() == 1) autoTargetFor(items[0].texturePath);
    resetPreviewFor(false);
    loadPaintForTarget();
    markChanged();
    open();
    return true;
}

void SpriteNormalsWindow::autoTargetFor(const std::string& imagePath) {
    // the selected scene's only selected GameObject drawing this image (Assign stays off)
    const uint32_t sceneId = project->getSelectedSceneId();
    SceneProject* sp = project->getScene(sceneId);
    if (!sp || !sp->scene || imagePath.empty()) return;
    const std::vector<Entity> selection = project->getSelectedEntities(sceneId);
    if (selection.size() != 1) return;
    MeshComponent* mesh = sp->scene->findComponent<MeshComponent>(selection[0]);
    if (!mesh) return;
    const unsigned count = std::max(mesh->numSubmeshes, sp->scene->findComponent<SpriteComponent>(selection[0]) ? 1u : 0u);
    for (unsigned s = 0; s < count; s++) {
        if (baseTextureOf(project, sp->scene, selection[0], s) == imagePath) {
            setAssignTarget(sceneId, selection[0], s);
            assign = false;
            return;
        }
    }
}

bool SpriteNormalsWindow::openRecipe(const fs::path& generatedPng, std::string& error) {
    const fs::path root = project->getProjectPath();
    const fs::path png = generatedPng.is_absolute() ? generatedPng.lexically_normal() : (root / generatedPng).lexically_normal();
    std::string readError;
    const std::optional<nlohmann::json> recipe = sn::readRecipeHead(png, &readError);
    if (!recipe) {
        error = readError.empty() ? "it carries no Sprite Normal Map recipe (it was not generated by the Sprite Normal Map)" : readError;
        return false;
    }
    sn::Spec spec;
    std::vector<std::string> notes;
    if (!sn::specFromRecipe(*recipe, png, root, spec, notes, error)) return false;
    if (!paintDiscardOk([this, png]() {
            std::string e;
            if (!openRecipe(png, e)) setStatus(e, true);
        })) {
        return true;
    }
    loadStored();
    stopRun();
    saveAll = SaveAllJob();
    items = {makeItem(spec)};
    current = 0;
    settings = spec.settings;
    notices = notes;
    runErrors.clear();
    lastWritten.clear();
    setStatus("", false);
    const sn::Staleness s = sn::checkStaleness(root, png);
    const std::string source = fileName(s.sourcePath);
    if (s.state == sn::Staleness::State::Changed) {
        notices.push_back(source + " changed since this normal map was generated.");
    } else if (s.state == sn::Staleness::State::Missing) {
        notices.push_back("The source " + source + " is missing.");
    } else {
        notices.push_back("Restored from " + sn::projectRelative(root, png) + "; its source is unchanged, so Regenerate reproduces it.");
    }
    probe(items[0], true);
    refreshProblems(items[0]);
    // another source: a target set before is not this map's (the selection may offer one, Assign off)
    target = AssignTarget();
    assign = false;
    autoTargetFor(items[0].texturePath);
    resetPreviewFor(false);
    loadPaintForTarget();
    markChanged();
    storeSettings();
    open();
    return true;
}

bool SpriteNormalsWindow::openForEntity(uint32_t sceneId, Entity entity, unsigned submesh, bool now, std::string& error) {
    const fs::path root = project->getProjectPath();
    SceneProject* sp = project->getScene(sceneId);
    if (!sp || !sp->scene) {
        error = "the scene is not open";
        return false;
    }
    std::string texture;
    std::vector<sn::Cell> cells;
    if (!sn::framesFromSprite(sp->scene, entity, submesh, texture, cells, error)) return false;
    if (now && project->isAnyScenePlaying()) {
        error = "stop the scene to generate a normal map: it writes files and changes the scene";
        return false;
    }
    loadStored();
    sn::Spec spec;
    spec.sceneId = sceneId;
    spec.entity = entity;
    spec.submesh = submesh;
    spec.settings = project->getSpriteNormalsSettings().settings;

    // the current Normal Texture's recipe: its settings and outputs come back (Edit / regenerate)
    std::vector<std::string> recipeOutputs;
    std::vector<std::string> notes;
    fs::path recipePng;
    MeshComponent* mesh = sp->scene->findComponent<MeshComponent>(entity);
    if (mesh && submesh < std::max(mesh->numSubmeshes, 1u)) {
        const std::string stored = mesh->submeshes[submesh].material.normalTexture.getPath();
        if (!stored.empty()) {
            const fs::path png = absoluteOf(project, mcp::McpValues::assetPathToJson(project, stored));
            std::error_code ec;
            if (lower(jasperengine::editor::FileUtils::pathToUtf8(png.extension())) == ".png" && fs::is_regular_file(png, ec)) {
                if (const std::optional<nlohmann::json> recipe = sn::readRecipeHead(png)) {
                    sn::Spec fromRecipe;
                    std::string recipeError;
                    if (sn::specFromRecipe(*recipe, png, root, fromRecipe, notes, recipeError)) {
                        spec.settings = fromRecipe.settings;
                        spec.output = fromRecipe.output;
                        spec.output.framesJson = false;
                        recipePng = png;
                        for (const std::string& path : {fromRecipe.output.normal, fromRecipe.output.atlas, fromRecipe.output.height}) {
                            if (!path.empty()) recipeOutputs.push_back(path);
                        }
                    }
                }
            }
        }
    }

    if (now) {
        // a second Shift-click while the first one runs: nothing more to do
        for (const QuickJob& job : quickJobs) {
            if (job.target.sceneId == sceneId && job.target.entity == entity && job.target.submesh == submesh) return true;
        }
        // a generated map of this texture's (its recipe names it as the source), read without hashing
        auto generatedFrom = [&](const fs::path& png, nlohmann::json* recipeOut) {
            const std::optional<nlohmann::json> recipe = sn::readRecipeHead(png);
            std::string invalid;
            if (!recipe || !sn::validateRecipe(*recipe, invalid)) return false;
            const fs::path recorded = fs::u8path((*recipe)["source"]["path"].get<std::string>());
            if (recorded.lexically_normal() != fs::u8path(texture).lexically_normal()) return false;
            if (recipeOut) *recipeOut = *recipe;
            return true;
        };
        sn::Spec planning = spec;
        planning.source = texture;
        sn::SourceInfo info;
        std::vector<sn::Problem> problems;
        sn::probeSource(absoluteOf(project, texture), info, problems);
        // no recipe on the Normal Texture, but a generated map of this texture at the default path:
        // regenerated with its own settings (not the last ones)
        if (recipePng.empty()) {
            for (const sn::PlannedOutput& out : sn::plannedOutputs(root, planning, info.kind)) {
                if (out.role != "normal") continue;
                const fs::path abs = absoluteOf(project, out.path);
                nlohmann::json recipe;
                std::error_code ec;
                if (!fs::is_regular_file(abs, ec) || !generatedFrom(abs, &recipe)) break;
                sn::Spec fromRecipe;
                std::string recipeError;
                std::vector<std::string> ignored;
                if (sn::specFromRecipe(recipe, abs, root, fromRecipe, ignored, recipeError)) {
                    spec.settings = fromRecipe.settings;
                    spec.output = fromRecipe.output;
                    spec.output.framesJson = false;
                    recipePng = abs;
                    for (const std::string& path : {fromRecipe.output.normal, fromRecipe.output.atlas, fromRecipe.output.height}) {
                        if (!path.empty()) recipeOutputs.push_back(path);
                    }
                    planning = spec;
                    planning.source = texture;
                }
            }
        }
        // overwrite only the files the recipe names (or another generated map of this source):
        // an existing file of someone else opens the window, whose Save asks first
        bool blocked = false;
        std::string blocker;
        for (const sn::PlannedOutput& out : sn::plannedOutputs(root, planning, info.kind)) {
            std::error_code ec;
            const fs::path abs = absoluteOf(project, out.path);
            if (out.path.empty() || !fs::exists(abs, ec)) continue;
            if (std::find(recipeOutputs.begin(), recipeOutputs.end(), out.path) != recipeOutputs.end()) continue;
            if (generatedFrom(abs, nullptr)) continue;
            blocked = true;
            blocker = out.path;
            break;
        }
        if (!blocked) {
            sn::Spec running = spec;
            running.overwrite = true;
            // every check here (the project, the scene); the decode and generation on the job's worker
            auto work = std::make_shared<QuickWork>();
            if (!sn::preparePlan(root, running, sp->scene, work->prepared, problems)) {
                error = problems.empty() ? "the source can't be generated from" : problems.front().message;
                return false;
            }
            work->threads = workerThreads();
            QuickJob job;
            job.target = {sceneId, entity, submesh, sp->scene->getEntityName(entity)};
            job.overwrite = true;
            for (const sn::PlannedOutput& out : sn::plannedOutputs(root, planning, info.kind)) {
                if (out.role == "normal") job.label = fileName(out.path);
            }
            job.work = work;
            startWork(work);
            quickJobs.push_back(std::move(job));
            return true;
        }
        if (!paintDiscardOk([this, sceneId, entity, submesh, now]() {
                std::string e;
                if (!openForEntity(sceneId, entity, submesh, now, e)) setStatus(e, true);
            })) {
            return true;
        }
        notices.clear();
        notices.push_back(fileName(blocker) + " exists and was not generated by the Sprite Normal Map: Save asks before replacing it.");
    } else {
        if (!paintDiscardOk([this, sceneId, entity, submesh]() {
                std::string e;
                if (!openForEntity(sceneId, entity, submesh, false, e)) setStatus(e, true);
            })) {
            return true;
        }
        notices.clear();
    }

    stopRun();
    saveAll = SaveAllJob();
    items = {makeItem(spec)};
    current = 0;
    settings = spec.settings;
    for (const std::string& note : notes) notices.push_back(note);
    if (!recipePng.empty()) {
        const sn::Staleness s = sn::checkStaleness(root, recipePng);
        if (s.state == sn::Staleness::State::Changed) notices.push_back(fileName(s.sourcePath) + " changed since this normal map was generated.");
        else if (s.state == sn::Staleness::State::Missing) notices.push_back("The source " + fileName(s.sourcePath) + " is missing.");
        else notices.push_back("Restored from " + sn::projectRelative(root, recipePng) + "; its source is unchanged, so Regenerate reproduces it.");
    }
    runErrors.clear();
    lastWritten.clear();
    setStatus("", false);
    probe(items[0], true);
    refreshProblems(items[0]);
    setAssignTarget(sceneId, entity, submesh);
    assign = true;
    resetPreviewFor(false);
    loadPaintForTarget();
    markChanged();
    open();
    return true;
}

bool SpriteNormalsWindow::writeAndNotify(Project* project, const std::vector<sn::OutputFile>& files, std::string& error) {
    // the Project window (fresh thumbnails, the new files listed) and the watcher notice them
    // through the write's refresh (ProjectFileWrites::refresh)
    return sn::writeOutputsUndoable(project, files, error);
}

bool SpriteNormalsWindow::regenerateFromRecipe(Project* project, const fs::path& generatedPng, std::vector<std::string>& written, std::string& error) {
    written.clear();
    const fs::path root = project->getProjectPath();
    const fs::path png = generatedPng.is_absolute() ? generatedPng.lexically_normal() : (root / generatedPng).lexically_normal();
    std::string readError;
    const std::optional<nlohmann::json> recipe = sn::readRecipeHead(png, &readError);
    if (!recipe) {
        error = readError.empty() ? "it carries no Sprite Normal Map recipe" : readError;
        return false;
    }
    sn::Spec spec;
    std::vector<std::string> notes;
    if (!sn::specFromRecipe(*recipe, png, root, spec, notes, error)) return false;
    const sn::Staleness s = sn::checkStaleness(root, png);
    if (s.state == sn::Staleness::State::Missing) {
        error = "the source " + s.sourcePath + " is missing";
        return false;
    }
    spec.overwrite = true;
    sn::Prepared prepared;
    std::vector<sn::Problem> problems;
    if (!sn::prepare(root, spec, nullptr, prepared, problems)) {
        error = problems.empty() ? "the recipe can't be used" : problems.front().message;
        return false;
    }
    // the map's paint (kept from it) composited again
    const sn::Result result = sn::generatePrepared(prepared);
    if (result.status != sn::Status::Ok) {
        error = result.errors.empty() ? std::string("the generation failed") : result.errors.front().message;
        return false;
    }
    std::vector<sn::OutputFile> files;
    if (!sn::encodeOutputs(prepared, result, files, error)) return false;
    if (!writeAndNotify(project, files, error)) return false;
    for (const sn::OutputFile& f : files) written.push_back(f.path);
    return true;
}

// ====================================================================== settings

void SpriteNormalsWindow::setSettings(const sn::Settings& s) {
    loadStored();   // the project's stored values first, then this change over them
    settings = s;
    markChanged();
    storeSettings();
}

void SpriteNormalsWindow::setPreset(sn::Preset p) {
    loadStored();   // the project's stored values first, then this change over them
    const sn::Settings base = sn::presetSettings(p);
    const sn::Preset previous = settings.preset;
    settings.preset = p;
    settings.strength = base.strength;
    settings.detail = base.detail;
    settings.smoothing = base.smoothing;
    settings.volume = base.volume;
    settings.softness = base.softness;
    settings.radius = base.radius;
    settings.quantize = base.quantize;   // Toon steps: the toon preset's, off for the others
    settings.profile = base.profile;     // auto, or the painted / props / stones preset's
    settings.gaps = base.gaps;           // Dark Gaps Are Edges: the stones preset's, off for the others
    // tile and stones wrap; leaving them for another preset goes back to the preset's edges
    auto wraps = [](sn::Preset q) { return q == sn::Preset::Tile || q == sn::Preset::Stones; };
    if (wraps(p) || wraps(previous)) settings.edges = base.edges;
    settings.version = sn::kSettingsVersion;   // a preset is a fresh start with the current generator
    markChanged();
    storeSettings();
}

void SpriteNormalsWindow::setFrames(const sn::FramesSpec& f) {
    if (current >= items.size()) return;
    items[current].spec.frames = f;
    frame = -1;
    markChanged();
}

void SpriteNormalsWindow::setOutput(const std::string& projectRelative) {
    if (current >= items.size()) return;
    const bool changed = items[current].spec.output.normal != projectRelative;
    if (!changed) return;
    // another target file. With a paint layer of its own: that one is loaded (unsaved strokes asked
    // about first); without one (no file, or no paint chunk): Save As, the strokes stay, unsaved
    // for the new file
    const fs::path next = plannedNormalFor(items[current], projectRelative);
    std::string problem;
    std::error_code ec;
    const bool ownPaint = !isBatch() && !next.empty() && fs::is_regular_file(next, ec) &&
                          (sn::readPaintSummary(next, &problem).has_value() || !problem.empty());
    if (ownPaint && !paintDiscardOk([this, projectRelative]() { setOutput(projectRelative); })) return;
    items[current].spec.output.normal = projectRelative;
    // where it goes is not part of the result: no regeneration, only the checks
    refreshProblems(items[current]);
    warningsDirty = true;
    if (ownPaint || isBatch()) {
        loadPaintForTarget();
        return;
    }
    endStroke();
    paintTarget = jasperengine::editor::FileUtils::pathToGenericUtf8(next);
    paintSaved = sn::PaintOverlay();
    paintProblem.clear();
    recordPaintStamp();
    paintChanged();
}

void SpriteNormalsWindow::setHeightMap(bool on) {
    loadStored();   // the project's stored values first, then this change over them
    settings.height = on;
    markChanged();
    storeSettings();
}

const sn::Spec& SpriteNormalsWindow::currentSpec() const {
    specCache = current < items.size() ? specOf(items[current]) : sn::Spec();
    if (current >= items.size()) specCache.settings = settings;
    return specCache;
}

void SpriteNormalsWindow::setAssignTarget(uint32_t sceneId, Entity entity, unsigned submesh) {
    const SceneProject* sp = project->getScene(sceneId);
    target = AssignTarget();
    if (!sp || !sp->scene || entity == NULL_ENTITY || !sp->scene->isEntityCreated(entity)) return;
    target = {sceneId, entity, submesh, sp->scene->getEntityName(entity)};
    warningsDirty = true;
}

void SpriteNormalsWindow::clearAssignTarget() {
    target = AssignTarget();
    assign = false;
    warningsDirty = true;
}

void SpriteNormalsWindow::setAssign(bool on) {
    assign = on;
    warningsDirty = true;
}

// ====================================================================== batch

void SpriteNormalsWindow::selectItem(size_t i) {
    if (i >= items.size() || i == current) return;
    stopRun();
    // the item left keeps no decoded image when it is written already or cheap to make again
    // (live sizes); a large item's manual Generate is kept. Save All keeps them until its write.
    Item& left = items[current];
    if (!saveAll.active && left.result && (left.savedKey == left.resultKey || liveAllowed(left))) left.result.reset();
    current = i;
    probe(items[current], false);
    refreshProblems(items[current]);
    resetPreviewFor(false);
    warningsDirty = true;
    // a result of its own, still valid: shown at once; else a live run
    if (!shown() || isStale()) markChanged();
}

void SpriteNormalsWindow::setItemChecked(size_t i, bool checked) {
    if (i < items.size()) items[i].checked = checked;
}

bool SpriteNormalsWindow::isItemChecked(size_t i) const {
    return i < items.size() && items[i].checked;
}

// ====================================================================== running

bool SpriteNormalsWindow::startRun(size_t index, std::string& error) {
    if (index >= items.size()) {
        error = "pick a source first";
        return false;
    }
    Item& item = items[index];
    probe(item, false);
    sn::Spec spec = specOf(item);
    spec.overwrite = true;   // whether outputs exist is Save's business
    sn::Prepared prepared;
    std::vector<sn::Problem> problems;
    if (!sn::prepare(project->getProjectPath(), spec, sceneFor(spec), prepared, problems)) {
        item.problems = problems;
        runErrors = problems;
        error.clear();
        for (const sn::Problem& p : problems) error += (error.empty() ? "" : "; ") + p.message;
        warningsDirty = true;
        return false;
    }
    stopRun();
    run = sn::SpriteNormalsRun::start(std::move(prepared), workerThreads());
    runItem = index;
    runKey = keyOf(item);
    runStartedAt = now();
    runErrors.clear();
    if (index == current) changedAt = -1.0;
    return true;
}

void SpriteNormalsWindow::stopRun() {
    if (run && !run->finished()) run->cancel();
}

bool SpriteNormalsWindow::regenerate(std::string& error) {
    return startRun(current, error);
}

void SpriteNormalsWindow::cancel() {
    const bool wasBusy = isRunning() || saveAll.active;
    stopRun();
    saveAll = SaveAllJob();
    changedAt = -1.0;
    if (wasBusy) setStatus("Cancelled.", false);
}

bool SpriteNormalsWindow::isRunning() const {
    return run && !run->finished();
}

bool SpriteNormalsWindow::hasResult() const {
    return shown() != nullptr;
}

bool SpriteNormalsWindow::isStale() const {
    if (!hasResult()) return false;
    return keyOf(items[current]) != items[current].resultKey;
}

void SpriteNormalsWindow::finishRun() {
    if (!run || !run->finished()) return;
    const sn::Result& result = run->result();
    if (runItem < items.size()) {
        Item& item = items[runItem];
        if (result.status == sn::Status::Ok) {
            item.result = run;
            item.resultId = ++resultIds;
            item.resultKey = runKey;
            if (runItem == current) {
                resultSerial++;
                litDirty = true;
                colorLoadedKey.clear();
                warningsDirty = true;
            }
        } else if (result.status != sn::Status::Cancelled) {
            runErrors = result.errors;
            if (saveAll.active) {
                setStatus("Can't generate " + item.label + ": " + (result.errors.empty() ? std::string("the generation failed") : result.errors.front().message) +
                              " Nothing was written.",
                          true);
                saveAll = SaveAllJob();
            }
        }
    }
    run.reset();
}

// ---------------------------------------------------------------------- save

std::vector<std::string> SpriteNormalsWindow::existingOutputs(const std::vector<size_t>& itemIndices) const {
    std::vector<std::string> existing;
    const fs::path root = project->getProjectPath();
    for (size_t i : itemIndices) {
        const Item& item = items[i];
        sn::Spec spec = specOf(item);
        if (spec.hasSprite()) spec.source = item.texturePath;
        for (const sn::PlannedOutput& out : sn::plannedOutputs(root, spec, item.info.kind)) {
            std::error_code ec;
            if (!out.path.empty() && fs::exists(absoluteOf(project, out.path), ec)) existing.push_back(out.path);
        }
    }
    return existing;
}

std::vector<sn::PlannedOutput> SpriteNormalsWindow::plannedOutputs() const {
    if (current >= items.size()) return {};
    const Item& item = items[current];
    sn::Spec spec = specOf(item);
    if (spec.hasSprite()) spec.source = item.texturePath;
    if (spec.source.empty()) return {};
    return sn::plannedOutputs(project->getProjectPath(), spec, item.info.kind);
}

bool SpriteNormalsWindow::writeResults(const std::vector<size_t>& itemIndices, bool overwrite, std::vector<std::string>& written, std::string& error) {
    written.clear();
    const fs::path root = project->getProjectPath();
    std::vector<sn::OutputFile> all;
    size_t normals = 0;
    for (size_t i : itemIndices) {
        const Item& item = items[i];
        if (!item.result || !item.result->finished() || item.result->result().status != sn::Status::Ok) {
            error = item.label + " has no result yet";
            return false;
        }
        // the shown result, written where the window says now (the output path may have been
        // edited after the run: it is not part of the result)
        sn::Prepared prepared = item.result->prepared();
        prepared.spec.output = item.spec.output;
        prepared.spec.overwrite = overwrite;
        for (const sn::Problem& p : sn::checkSpec(root, prepared.spec, sceneFor(prepared.spec))) {
            if (p.field.rfind("output", 0) == 0) {
                error = p.kind == "output_exists" ? p.message + " (Save replaces it after asking)" : p.message;
                return false;
            }
        }
        sn::Spec planning = prepared.spec;
        planning.source = prepared.sourcePath;
        prepared.outputs = sn::plannedOutputs(root, planning, prepared.source.kind);
        std::vector<sn::OutputFile> files;
        // single mode: the window's paint layer goes into the map, over the generation shown
        sn::Result result = item.result->result();
        if (!isBatch() && i == current) {
            prepared.paint = sn::PaintLayer();
            if (paint.sized(result.normal.width, result.normal.height)) {
                prepared.paint.overlay = paint;
                result.normal = composedNormal();
            }
        }
        sn::Problem problem;
        if (!sn::encodeOutputs(prepared, result, files, error, &problem)) return false;
        for (sn::OutputFile& f : files) {
            if (f.role == "normal") normals++;
            all.push_back(std::move(f));
        }
    }
    if (all.empty()) {
        error = "nothing to write";
        return false;
    }
    if (!writeAndNotify(project, all, error)) return false;
    recordWrite();
    for (const sn::OutputFile& f : all) written.push_back(f.path);
    lastWritten = written;
    for (size_t i : itemIndices) items[i].savedKey = items[i].resultKey;
    // what is saved now (the stroke stack stays: Ctrl+Z still undoes strokes)
    if (!isBatch()) {
        for (const sn::OutputFile& f : all) {
            if (f.role == "normal") paintTarget = jasperengine::editor::FileUtils::pathToGenericUtf8(f.absolute.lexically_normal());
        }
        paintSaved = paint;
        paintProblem.clear();
        recordPaintStamp();
        unsavedFor = 0;
        warningsDirty = true;
    }
    // batch: the decoded sources and results of the written items not shown are not kept
    if (isBatch()) {
        for (size_t i : itemIndices) {
            if (i != current) items[i].result.reset();
        }
    }
    if (editorUi()) {
        if (ProjectWindow* projectWindow = Backend::getApp().getProjectWindow()) {
            for (const sn::OutputFile& f : all) {
                if (f.role == "normal") {
                    projectWindow->revealAsset(f.absolute);
                    break;
                }
            }
        }
    }
    const std::string first = fileName(written.front());
    if (itemIndices.size() > 1 || normals > 1) {
        setStatus("Saved " + std::to_string(written.size()) + " files as one undo step.", false);
    } else if (written.size() > 1) {
        setStatus("Saved " + std::to_string(written.size()) + " files as one undo step.", false);
    } else {
        setStatus("Saved " + first + " (Project undo).", false);
    }
    return true;
}

bool SpriteNormalsWindow::save(bool overwrite, std::vector<std::string>& written, std::string& error) {
    written.clear();
    if (items.empty()) {
        error = "Nothing to save: pick a source first.";
        return false;
    }
    if (isBatch()) {
        std::vector<size_t> checked;
        for (size_t i = 0; i < items.size(); i++) {
            if (items[i].checked) checked.push_back(i);
        }
        if (checked.empty()) {
            error = "No image is checked.";
            return false;
        }
        if (!overwrite) {
            const std::vector<std::string> existing = existingOutputs(checked);
            if (!existing.empty()) {
                error = joined(existing) + (existing.size() > 1 ? " exist" : " exists") + " (overwrite to replace)";
                return false;
            }
        }
        // the checked items without a fresh result run first (one after another), then one write
        saveAll = SaveAllJob();
        saveAll.overwrite = overwrite;
        for (size_t i : checked) {
            const Item& item = items[i];
            const bool fresh = item.result && item.result->finished() && item.result->result().status == sn::Status::Ok && item.resultKey == keyOf(item);
            if (!fresh) saveAll.queue.push_back(i);
        }
        if (saveAll.queue.empty()) return writeResults(checked, overwrite, written, error);
        saveAll.active = true;
        setStatus("", false);
        tickSaveAll();
        return true;
    }
    if (!hasResult()) {
        error = "Nothing to save: wait for the preview.";
        return false;
    }
    if (isStale() || (isRunning() && runItem == current)) {
        error = "Updating: wait for the preview";
        return false;
    }
    if (paintSizeMismatch()) {
        error = paintDisabledReason();
        return false;
    }
    Item& item = items[current];
    if (item.result->generation() != texsynth::projectGeneration()) {
        setStatus("This result was generated in another project: it regenerates.", false);
        error = statusMessage;
        std::string ignored;
        startRun(current, ignored);
        return false;
    }
    const bool assigning = getAssign();
    if (assigning && project->isAnyScenePlaying()) {
        error = "Stop the scene to assign the normal map";
        return false;
    }
    if (!writeResults({current}, overwrite, written, error)) return false;
    if (!assigning) return true;

    // Save & Assign: one Scene undo step on the target
    const sn::Prepared& prepared = item.result->prepared();
    sn::ApplyRequest request;
    request.sceneId = target.sceneId;
    request.entity = target.entity;
    request.colorSourcePath = prepared.sourcePath;
    sn::Spec planning = prepared.spec;
    planning.output = item.spec.output;
    planning.source = prepared.sourcePath;
    for (const sn::PlannedOutput& out : sn::plannedOutputs(project->getProjectPath(), planning, prepared.source.kind)) {
        if (out.role == "normal") request.normalPath = out.path;
        if (out.role == "atlas") request.atlasPath = out.path;
    }
    if (isAnimated(prepared.source.kind)) {
        request.frames = prepared.source.cells;
        for (const sn::Delay& d : prepared.source.delays) request.frameMs.push_back(d.playbackMs());
        request.loopCount = prepared.source.loopCount;
    }
    const sn::ApplyResult applied = sn::applyToSprite(project, request);
    if (!applied.ok) {
        error = "Saved " + fileName(request.normalPath) + " (Project undo), but it could not be assigned: " + applied.error.message;
        setStatus(error, true);
        return false;
    }
    recordAssign(target.sceneId);
    std::string status = "Saved " + fileName(request.normalPath) + " (Project undo) and assigned to " + target.name + " (Scene undo).";
    if (written.size() > 1) status = "Saved " + std::to_string(written.size()) + " files (one Project undo step) and assigned them to " + target.name + " (Scene undo).";
    for (const sn::Problem& w : applied.warnings) status += " " + w.message;
    setStatus(status, false);
    warningsDirty = true;
    return true;
}

void SpriteNormalsWindow::confirm(const std::string& title, const std::string& message, std::function<void()> onYes) {
    lastConfirm = onYes;
    if (holdConfirms) return;
    getEditorHost().registerConfirmAlert(title, message, std::move(onYes));
}

void SpriteNormalsWindow::requestSave() {
    std::vector<size_t> indices;
    if (isBatch()) {
        for (size_t i = 0; i < items.size(); i++) {
            if (items[i].checked) indices.push_back(i);
        }
    } else if (current < items.size()) {
        indices.push_back(current);
    }
    auto doSave = [this](bool overwrite) {
        std::vector<std::string> written;
        std::string error;
        if (!save(overwrite, written, error)) setStatus(error, true);
    };
    const std::vector<std::string> existing = existingOutputs(indices);
    if (existing.empty()) {
        doSave(false);
        return;
    }
    std::string list;
    for (const std::string& path : existing) list += "\n  " + path;
    confirm("Replace Files?", "These files exist and would be replaced:" + list + "\n\nReplace them? (Undo in the Project window puts them back.)",
            [doSave]() { doSave(true); });
}

void SpriteNormalsWindow::tickSaveAll() {
    if (!saveAll.active) return;
    if (isRunning()) return;
    while (!saveAll.queue.empty()) {
        const size_t next = saveAll.queue.front();
        Item& item = items[next];
        const bool fresh = item.result && item.result->finished() && item.result->result().status == sn::Status::Ok && item.resultKey == keyOf(item);
        if (fresh) {
            saveAll.queue.erase(saveAll.queue.begin());
            saveAll.done++;
            continue;
        }
        std::string error;
        if (!startRun(next, error)) {
            setStatus("Can't generate " + item.label + ": " + error + " Nothing was written.", true);
            saveAll = SaveAllJob();
        }
        return;
    }
    // every checked item has its result; a setting changed meanwhile makes some stale again:
    // those run again first (a write is always of the settings shown)
    std::vector<size_t> checked;
    for (size_t i = 0; i < items.size(); i++) {
        if (!items[i].checked) continue;
        checked.push_back(i);
        const Item& item = items[i];
        const bool fresh = item.result && item.result->finished() && item.result->result().status == sn::Status::Ok && item.resultKey == keyOf(item);
        if (!fresh) saveAll.queue.push_back(i);
    }
    if (!saveAll.queue.empty()) return;
    const bool overwrite = saveAll.overwrite;
    saveAll = SaveAllJob();
    std::vector<std::string> written;
    std::string error;
    if (!writeResults(checked, overwrite, written, error)) setStatus("Save failed: " + error, true);
}

void SpriteNormalsWindow::recordWrite() {
    CommandHistory* history = project->getProjectCommandHistory();
    writePosition = history->position();
    writeSerial = history->undoSerial();
    writeRecorded = true;
}

void SpriteNormalsWindow::recordAssign(uint32_t sceneId) {
    CommandHistory* history = CommandHandle::get(sceneId);
    assignSceneId = sceneId;
    assignPosition = history->position();
    assignSerial = history->undoSerial();
    assignRecorded = true;
}

bool SpriteNormalsWindow::startRegenerate(const fs::path& generatedPng, std::string& error) {
    const fs::path root = project->getProjectPath();
    const fs::path png = (generatedPng.is_absolute() ? generatedPng : root / generatedPng).lexically_normal();
    for (const QuickJob& job : quickJobs) {
        // already regenerating this very file: one is enough
        if (job.target.entity == NULL_ENTITY && job.png == png) return true;
    }
    if (project->isAnyScenePlaying()) {
        error = "stop the scene first: regenerating reloads the texture the game draws";
        return false;
    }
    std::string readError;
    const std::optional<nlohmann::json> recipe = sn::readRecipeHead(png, &readError);
    if (!recipe) {
        error = readError.empty() ? "it carries no Sprite Normal Map recipe" : readError;
        return false;
    }
    sn::Spec spec;
    std::vector<std::string> notes;
    if (!sn::specFromRecipe(*recipe, png, root, spec, notes, error)) return false;
    std::error_code ec;
    if (!fs::is_regular_file(absoluteOf(project, spec.source), ec)) {
        error = "the source " + spec.source + " is missing";
        return false;
    }
    spec.overwrite = true;
    spec.output.framesJson = false;
    // the whole check here, as a Shift-click's; the worker only decodes, generates and encodes
    auto work = std::make_shared<QuickWork>();
    std::vector<sn::Problem> problems;
    if (!sn::preparePlan(root, spec, nullptr, work->prepared, problems)) {
        error = problems.empty() ? "the recipe can't be used" : problems.front().message;
        return false;
    }
    work->threads = workerThreads();
    QuickJob job;
    job.overwrite = true;
    job.label = jasperengine::editor::FileUtils::pathToUtf8(png.filename());
    job.png = png;
    job.work = work;
    // the in-progress outcome before the job moves into the list
    regenerateOutcomes[jasperengine::editor::FileUtils::pathToGenericUtf8(png)] = {true, false, false, "Regenerating " + job.label + "..."};
    startWork(work);
    quickJobs.push_back(std::move(job));
    return true;
}

void SpriteNormalsWindow::tickQuickJobs() {
    for (size_t j = 0; j < quickJobs.size();) {
        QuickJob& job = quickJobs[j];
        if (!job.work || !job.work->done) {
            j++;
            continue;
        }
        QuickWork& work = *job.work;
        const bool assigning = job.target.entity != NULL_ENTITY;
        // a Regenerate written while a scene plays would reload what the game draws: it waits
        if (!assigning && project->isAnyScenePlaying()) {
            regenerateOutcomes[jasperengine::editor::FileUtils::pathToGenericUtf8(job.png)] = {true, false, false, "Regenerating " + job.label + " once the scene stops..."};
            j++;
            continue;
        }
        const sn::Prepared& prepared = work.prepared;
        const std::string normalName = job.label.empty() ? fileName(prepared.sourcePath) : job.label;
        std::string outcome;
        bool ok = false;
        auto fail = [&](const std::string& why) {
            outcome = (assigning ? "Can't generate " : "Can't regenerate ") + normalName + ": " + why;
            getEditorHost().registerAlert("Sprite Normal Map", outcome);
        };
        // the target as it is now: still there, still drawing the source, the scene stopped
        std::string targetProblem;
        if (assigning) {
            const SceneProject* sp = project->getScene(job.target.sceneId);
            if (!sp || !sp->scene || !sp->scene->isEntityCreated(job.target.entity)) {
                targetProblem = "\"" + job.target.name + "\" is gone";
            } else if (project->isAnyScenePlaying()) {
                targetProblem = "a scene is playing";
            } else if (baseTextureOf(project, sp->scene, job.target.entity, job.target.submesh) !=
                       jasperengine::editor::FileUtils::pathToGenericUtf8(fs::u8path(prepared.sourcePath).lexically_normal())) {
                targetProblem = "\"" + job.target.name + "\" no longer draws " + fileName(prepared.sourcePath);
            }
        }
        if (work.cancel || work.result.status == sn::Status::Cancelled) {
            outcome = "Cancelled.";
        } else if (!work.error.empty()) {
            fail(work.error);
        } else if (work.generation != texsynth::projectGeneration()) {
            fail(sn::kProjectChangedMessage);
        } else if (!targetProblem.empty()) {
            fail(targetProblem + "; nothing was written.");
        } else {
            std::string error;
            if (!writeAndNotify(project, work.files, error)) {
                fail(error);
            } else if (!assigning) {
                ok = true;
                outcome = "Regenerated " + normalName + ".";
                Out::info("Regenerated %s", normalName.c_str());
            } else {
                recordWrite();
                sn::ApplyRequest request;
                request.sceneId = job.target.sceneId;
                request.entity = job.target.entity;
                request.colorSourcePath = prepared.sourcePath;
                for (const sn::OutputFile& f : work.files) {
                    if (f.role == "normal") request.normalPath = f.path;
                    if (f.role == "atlas") request.atlasPath = f.path;
                }
                if (isAnimated(prepared.source.kind)) {
                    request.frames = prepared.source.cells;
                    for (const sn::Delay& d : prepared.source.delays) request.frameMs.push_back(d.playbackMs());
                    request.loopCount = prepared.source.loopCount;
                }
                const sn::ApplyResult applied = sn::applyToSprite(project, request);
                if (!applied.ok) {
                    fail("it was written (Project undo removes it) but not assigned: " + applied.error.message);
                } else {
                    ok = true;
                    recordAssign(job.target.sceneId);
                    Out::info("Generated %s and assigned it to %s", normalName.c_str(), job.target.name.c_str());
                }
            }
        }
        if (!assigning) regenerateOutcomes[jasperengine::editor::FileUtils::pathToGenericUtf8(job.png)] = {true, true, ok, outcome};
        quickJobs.erase(quickJobs.begin() + static_cast<std::ptrdiff_t>(j));
    }
}

// ====================================================================== the tick

void SpriteNormalsWindow::checkTarget() {
    if (!hasAssignTarget()) return;
    const SceneProject* sp = project->getScene(target.sceneId);
    if (!sp || !sp->scene || !sp->scene->isEntityCreated(target.entity)) {
        target = AssignTarget();
        assign = false;
        warningsDirty = true;
        setStatus("The assign target was removed.", false);
    }
}

void SpriteNormalsWindow::checkSourceOnDisk() {
    if (current >= items.size()) return;
    Item& item = items[current];
    if (!item.probed || item.texturePath.empty() || item.changedOnDisk) return;
    const double t = now();
    if (sourceCheckedAt >= 0.0 && t - sourceCheckedAt < 1.0 && t >= sourceCheckedAt) return;
    sourceCheckedAt = t;
    std::error_code ec;
    const fs::path abs = absoluteOf(project, item.texturePath);
    const fs::file_time_type time = fs::last_write_time(abs, ec);
    const uintmax_t size = ec ? 0 : fs::file_size(abs, ec);
    if (time != item.stampTime || size != item.stampSize) {
        item.changedOnDisk = true;
        warningsDirty = true;
    }
}

void SpriteNormalsWindow::tick() {
    loadStored();
    const double t = now();
    const double dt = lastTick < 0.0 ? 0.0 : std::clamp(t - lastTick, 0.0, 1.0);
    lastTick = t;

    finishRun();
    tickQuickJobs();
    tickSaveAll();
    checkTarget();
    checkSourceOnDisk();
    checkPaintTarget();

    // debounce: the live run 150 ms after the last change
    if (changedAt >= 0.0 && t - changedAt >= DEBOUNCE_SECONDS - 1e-9 && !saveAll.active) {
        if (current < items.size() && liveAllowed(items[current])) {
            std::string error;
            if (!startRun(current, error)) changedAt = -1.0;
        } else {
            changedAt = -1.0;
        }
    }

    // Play: the cells with the result's delays (sheets: 100 ms); Orbit: one turn per 4 s
    const std::vector<sn::Cell> cells = shownCells();
    if (windowOpen && playing && cells.size() > 1) {
        if (frame < 0) frame = 0;
        playElapsed += dt;
        std::vector<sn::Delay> delays;
        if (const std::shared_ptr<sn::SpriteNormalsRun> r = shown()) delays = r->prepared().source.delays;
        for (int guard = 0; guard < 1000; guard++) {
            const int ms = static_cast<size_t>(frame) < delays.size() ? delays[static_cast<size_t>(frame)].playbackMs() : 100;
            if (playElapsed * 1000.0 + 1e-6 < ms) break;
            playElapsed -= ms / 1000.0;
            frame = (frame + 1) % static_cast<int>(cells.size());
            litDirty = true;
        }
    }
    if (windowOpen && orbit && shown()) {
        orbitTime += dt;
        int x, y, w, h;
        cellRect(frame, x, y, w, h);
        if (w > 0 && h > 0) {
            const float larger = static_cast<float>(std::max(w, h));
            const double angle = orbitTime / 4.0 * 2.0 * M_PI;
            ensureLight();
            light.x = w * 0.5f + 0.6f * larger * static_cast<float>(std::cos(angle));
            light.y = h * 0.5f - 0.6f * larger * static_cast<float>(std::sin(angle));
            light.placed = true;
            litDirty = true;
        }
    }
    // frames while something moves on screen or work waits for a tick (a closed window animates nothing)
    const bool animating = windowOpen && shown() && (playing || orbit);
    const bool working = (windowOpen && (isRunning() || changedAt >= 0.0 || saveAll.active)) || !quickJobs.empty();
    if ((animating || working) && editorUi()) Backend::getApp().requestRedraw();
    refreshWarnings(false);
}

// ====================================================================== preview model

std::vector<sn::Cell> SpriteNormalsWindow::shownCells() const {
    const std::shared_ptr<sn::SpriteNormalsRun> r = shown();
    if (!r) return {};
    return r->prepared().request.cells;
}

void SpriteNormalsWindow::cellRect(int index, int& x, int& y, int& w, int& h) const {
    x = y = w = h = 0;
    const std::shared_ptr<sn::SpriteNormalsRun> r = shown();
    if (!r) return;
    const std::vector<sn::Cell>& cells = r->prepared().request.cells;
    const sn::Image& normal = r->result().normal;
    if (index >= 0 && static_cast<size_t>(index) < cells.size()) {
        const sn::Cell& c = cells[static_cast<size_t>(index)];
        x = c.x;
        y = c.y;
        w = c.width;
        h = c.height;
        return;
    }
    w = normal.width;
    h = normal.height;
}

void SpriteNormalsWindow::defaultLight() {
    int x, y, w, h;
    cellRect(frame, x, y, w, h);
    if (w <= 0 || h <= 0) return;
    const float larger = static_cast<float>(std::max(w, h));
    light.x = w * 0.5f - 0.5f * larger;
    light.y = h * 0.5f - 0.5f * larger;
    const float stored = project->getSpriteNormalsSettings().lightHeight;
    light.userHeight = stored > 0.0f;
    light.height = stored > 0.0f ? stored : std::clamp(0.6f * larger, 4.0f, 512.0f);
    light.range = 3.0f * larger;
    light.cellWidth = w;
    light.cellHeight = h;
    light.placed = true;
    litDirty = true;
}

void SpriteNormalsWindow::ensureLight() {
    int x, y, w, h;
    cellRect(frame, x, y, w, h);
    if (!light.placed || light.cellWidth != w || light.cellHeight != h) defaultLight();
}

sn::LightSetup SpriteNormalsWindow::lightSetup() const {
    sn::LightSetup setup;
    sn::PreviewLight l;
    l.x = light.x;
    l.y = light.y;
    l.height = std::max(1.0f, light.height);
    l.range = std::max(1.0f, light.range);
    l.falloff = 1.0f;
    l.red = light.colour.x;
    l.green = light.colour.y;
    l.blue = light.colour.z;
    l.intensity = 1.0f;
    setup.lights.push_back(l);
    setup.ambientIntensity = light.ambient;
    setup.lightSteps = effectiveLightSteps();
    if (const std::shared_ptr<sn::SpriteNormalsRun> r = shown()) {
        setup.directx = r->prepared().request.settings.directx;
        setup.alphaThreshold = r->prepared().request.settings.alphaThreshold;
    }
    return setup;
}

void SpriteNormalsWindow::renderLit() {
    litDirty = false;
    litRenderedAt = now();
    litDirtyX0 = litDirtyY0 = litDirtyX1 = litDirtyY1 = 0;
    const std::shared_ptr<sn::SpriteNormalsRun> r = shown();
    if (!r) {
        litRgba = sn::Image();
        retire(litGpu);
        return;
    }
    ensureLight();
    litSteps = effectiveLightSteps();
    int x, y, w, h;
    cellRect(frame, x, y, w, h);
    const sn::Image& normal = displayNormal();
    const sn::Image& color = r->prepared().source.color;
    sn::LightSetup setup = lightSetup();
    // All view of a large image: only what is on screen (the light moves with the crop)
    int cx = x, cy = y, cw = w, ch = h;
    if (frame < 0 && static_cast<int64_t>(w) * h > LIVE_PIXELS && visibleX1 > visibleX0 && visibleY1 > visibleY0) {
        cx = visibleX0;
        cy = visibleY0;
        cw = visibleX1 - visibleX0;
        ch = visibleY1 - visibleY0;
        setup.lights[0].x -= static_cast<float>(cx - x);
        setup.lights[0].y -= static_cast<float>(cy - y);
    }
    litOriginX = cx - x;
    litOriginY = cy - y;
    const sn::Image n = crop(normal, cx, cy, cw, ch);
    const sn::Image c = crop(color, cx, cy, cw, ch);
    if (n.empty() || c.empty()) {
        litRgba = sn::Image();
        retire(litGpu);
        return;
    }
    litRgba = sn::renderLit(n, c, setup);
    litSerial++;
    if (windowOpen) updateGpuImage(litGpu, litRgba, litSerial);
}

// ====================================================================== preview controls

void SpriteNormalsWindow::setView(View v) {
    loadStored();   // the project's stored values first, then this change over them
    view = v;
    litDirty = true;
    storeSettings();
}

void SpriteNormalsWindow::setLight(float x, float y, float height) {
    loadStored();   // the project's stored values first, then this change over them
    ensureLight();
    light.x = x;
    light.y = y;
    if (height > 0.0f) {
        light.height = std::clamp(height, 1.0f, 4096.0f);
        light.userHeight = true;
    }
    light.placed = true;
    orbit = false;
    litDirty = true;
    storeSettings();
}

void SpriteNormalsWindow::setLightColour(const Vector3& c) {
    loadStored();   // the project's stored values first, then this change over them
    light.colour = c;
    litDirty = true;
    storeSettings();
}

void SpriteNormalsWindow::setAmbient(float a) {
    loadStored();   // the project's stored values first, then this change over them
    light.ambient = std::clamp(a, 0.0f, 1.0f);
    litDirty = true;
    storeSettings();
}

void SpriteNormalsWindow::setLightSteps(int steps) {
    lightSteps = std::clamp(steps, kSceneLightSteps, sn::kMaxLightSteps);
    litDirty = true;
}

int SpriteNormalsWindow::effectiveLightSteps() const {
    if (lightSteps != kSceneLightSteps) return lightSteps;
    const SceneProject* sp = project->getScene(hasAssignTarget() ? target.sceneId : project->getSelectedSceneId());
    return sp && sp->scene ? std::clamp(sp->scene->getLight2DSteps(), 0, sn::kMaxLightSteps) : 0;
}

void SpriteNormalsWindow::setFrame(int index) {
    const int count = static_cast<int>(shownCells().size());
    frame = count > 1 && index >= 0 ? std::min(index, count - 1) : -1;
    playElapsed = 0.0;
    litDirty = true;
}

void SpriteNormalsWindow::setPlaying(bool on) {
    playing = on && shownCells().size() > 1;
    playElapsed = 0.0;
    if (playing && frame < 0) {
        frame = 0;
        litDirty = true;
    }
    lastTick = now();
}

void SpriteNormalsWindow::setOrbit(bool on) {
    loadStored();   // the project's stored values first, then this change over them
    orbit = on;
    orbitTime = 0.0;
    lastTick = now();
    litDirty = true;
    storeSettings();
}

bool SpriteNormalsWindow::pickRegionColour(int region, int px, int py, std::string& error) {
    loadStored();   // the project's stored values first, then this change over them
    if (current >= items.size()) {
        error = "Pick a source first.";
        return false;
    }
    // the source's pixels: the shown result's colour, else the file
    sn::Image colour;
    if (const std::shared_ptr<sn::SpriteNormalsRun> r = shown()) {
        colour = r->prepared().source.color;
    } else {
        probe(items[current], false);
        sn::Source source;
        std::vector<sn::Problem> problems;
        if (items[current].texturePath.empty() || !sn::loadSource(absoluteOf(project, items[current].texturePath), source, problems)) {
            error = problems.empty() ? "The source can't be read." : problems.front().message;
            return false;
        }
        colour = source.color;
    }
    if (px < 0 || py < 0 || px >= colour.width || py >= colour.height) {
        error = "That pixel is outside the image.";
        return false;
    }
    const uint8_t* p = colour.pixel(px, py);
    if (p[3] / 255.0f <= settings.alphaThreshold) {
        error = "That pixel is empty.";
        setStatus(error, false);
        return false;
    }
    // Alt-click (region -1): the most recently edited region, else the last one, else a new region 1
    if (region < 0) region = lastEditedRegion >= 0 ? lastEditedRegion : static_cast<int>(settings.regions.size()) - 1;
    if (region < 0) {
        settings.regions.push_back(sn::Region());
        region = 0;
    }
    if (region >= static_cast<int>(settings.regions.size())) {
        error = "There is no region " + std::to_string(region + 1) + ".";
        return false;
    }
    sn::Region& r = settings.regions[static_cast<size_t>(region)];
    const uint32_t rgb = (static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[2];
    lastEditedRegion = region;
    if (std::find(r.colors.begin(), r.colors.end(), rgb) != r.colors.end()) return true;
    if (static_cast<int>(r.colors.size()) >= MAX_REGION_COLOURS) {
        error = "At most 16 colours per region.";
        setStatus(error, false);
        return false;
    }
    r.colors.push_back(rgb);
    markChanged();
    storeSettings();
    return true;
}

// ====================================================================== warnings

void SpriteNormalsWindow::refreshWarnings(bool force) {
    const double t = now();
    if (!force && !warningsDirty && warningsAt >= 0.0 && t - warningsAt < 0.5 && t >= warningsAt) return;
    warningsDirty = false;
    warningsAt = t;
    warnings.clear();
    if (current >= items.size()) return;
    Item& item = items[current];

    // the session's problems: Save waits for them
    for (const sn::Problem& p : item.problems) warnings.push_back({p.kind, p.message, true, {}});
    if (item.problems.empty() && !item.probeError.empty()) warnings.push_back({"invalid_source", item.probeError, true, {}});
    if (item.changedOnDisk) warnings.push_back({"source_changed", "The source changed on disk.", false, {"Reload"}});
    if (!isBatch()) {
        const std::vector<std::string> existing = existingOutputs({current});
        if (!existing.empty()) {
            std::vector<std::string> names;
            for (const std::string& path : existing) names.push_back(fileName(path));
            warnings.push_back({"output_exists",
                                existing.size() == 1 ? names[0] + " exists: Save replaces it (undo puts it back)."
                                                     : joined(names) + " exist: Save replaces them (undo puts them back).",
                                false,
                                {}});
        }
    }

    // the paint layer loaded from the target
    if (paintSizeMismatch()) {
        const std::shared_ptr<sn::SpriteNormalsRun> shownRun = shown();
        const sn::Image& n = shownRun->result().normal;
        warnings.push_back({"paint_size_mismatch",
                            fileName(paintTarget) + " has " + std::to_string(paint.paintedCount()) + " painted pixels for a " + std::to_string(paint.width) +
                                "x" + std::to_string(paint.height) + " image; the map is " + std::to_string(n.width) + "x" + std::to_string(n.height) +
                                " now. Clear the paint to save (or restore the source size).",
                            true,
                            {"Clear Paint"}});
    }
    if (!paintProblem.empty()) warnings.push_back({"paint_unreadable", paintProblem, false, {}});

    const std::shared_ptr<sn::SpriteNormalsRun> r = shown();
    // what the shown result says about the shape, with the window's fixes
    if (r) {
        const sn::Result& res = r->result();
        const sn::Settings& used = r->prepared().request.settings;
        if (used.edges == sn::Edges::Transparent && res.boxyIslands > 0) {
            const bool inflate = (res.profilesUsed & (1u << static_cast<int>(sn::Profile::Inflate))) != 0;
            warnings.push_back({"boxy",
                                std::string(res.boxyIslands == 1 ? "The sprite fills its box" : "The sprites fill their boxes") + " like a crate or panel: the " +
                                    (inflate ? "Inflate" : "Rounded") + " profile bulges a box into a pillow.",
                                false,
                                {"Use Bevel (radius " + std::to_string(sn::boxyBevelRadius(res.boxySide)) + ")", "Use Props"}});
        }
        for (const sn::Problem& p : res.warnings) {
            if (p.kind != "gaps_unmatched") continue;
            warnings.push_back({"gaps_unmatched",
                                "Dark Gaps Are Edges found no dark gaps: the darkest " + std::to_string(static_cast<int>(std::lround(used.gaps.share * 100.0f))) +
                                    "% of the image is about as light as the rest (flat or evenly lit art), so nothing became an edge.",
                                false,
                                {"Lower Gap Amount", "Turn Off Dark Gaps"}});
        }
    }

    // what the target's scene says (Phase 2's sceneWarnings, verbatim)
    int width = r ? r->result().normal.width : item.info.width;
    int height = r ? r->result().normal.height : item.info.height;
    std::vector<sn::Delay> delays = r ? r->prepared().source.delays : std::vector<sn::Delay>();
    const bool unknownSize = !r && isAnimated(item.info.kind);
    const Scene* scene = nullptr;
    if (hasAssignTarget()) {
        const SceneProject* sp = project->getScene(target.sceneId);
        scene = sp ? sp->scene : nullptr;
    }
    for (const sn::Problem& p : sn::sceneWarnings(scene, scene ? target.entity : NULL_ENTITY, settings, width, height, delays, getAssign(),
                                                      scene ? target.submesh : 0u, sn::framePixels(shownCells(), width, height))) {
        if (p.kind == "size_mismatch" && unknownSize) continue;
        WarningRow row{p.kind, p.message, false, {}};
        if (p.kind == "no_light") row.fixes = {"Create Light 2D"};
        else if (p.kind == "no_light_height") row.fixes = {"Set Height 64", "Select Lights"};
        else if (p.kind == "light_low") row.fixes = {"Raise Light(s)"};
        else if (p.kind == "filter_mismatch") row.fixes = {"Match Base Texture"};
        else if (p.kind == "size_mismatch") {
            row.error = true;
            row.fixes = {"Use Base Texture"};
        } else if (p.kind == "directx_in_engine") {
            row.fixes = {"Switch to OpenGL"};
        }
        warnings.push_back(row);
    }
}

bool SpriteNormalsWindow::applyFix(const std::string& kind, const std::string& fix, std::string& error) {
    auto needTarget = [&]() -> SceneProject* {
        if (!hasAssignTarget()) {
            error = "There is no assign target.";
            return nullptr;
        }
        SceneProject* sp = project->getScene(target.sceneId);
        if (!sp || !sp->scene) {
            error = "The target's scene is not open.";
            return nullptr;
        }
        if (sp->playState != ScenePlayState::STOPPED) {
            error = "Stop the scene first.";
            return nullptr;
        }
        return sp;
    };
    auto enabledLights = [](SceneProject* sp) {
        std::vector<Entity> out;
        auto lights = sp->scene->getComponentArray<Light2DComponent>();
        for (size_t i = 0; lights && i < lights->size(); i++) {
            const Entity e = lights->getEntity(i);
            const Light2DComponent* l = lights->findComponentFromIndex(i);
            if (!l || !l->enabled || !sp->scene->isEntityCreated(e)) continue;
            if (std::find(sp->entities.begin(), sp->entities.end(), e) == sp->entities.end()) continue;
            out.push_back(e);
        }
        return out;
    };
    bool done = false;
    if (kind == "no_light" && fix == "Create Light 2D") {
        SceneProject* sp = needTarget();
        if (!sp) return false;
        Scene* scene = sp->scene;
        Vector3 position(0, 0, 0);
        float w = 64.0f, h = 64.0f;
        if (Transform* t = scene->findComponent<Transform>(target.entity)) position = t->worldPosition;
        if (SpriteComponent* sprite = scene->findComponent<SpriteComponent>(target.entity)) {
            // world units: the pixel size over its Pixels Per Unit (S41)
            const Vector2 worldSize = spriteWorldSize(*sprite);
            if (sprite->width > 0) w = worldSize.x;
            if (sprite->height > 0) h = worldSize.y;
        }
        auto* create = new CreateEntityCmd(project, sp->id, "Light 2D", EntityCreationType::POINT_LIGHT_2D);
        create->addProperty<Vector3>(ComponentType::Transform, "position", Vector3(position.x - 0.5f * w, position.y + 0.5f * h, position.z));
        create->addProperty<float>(ComponentType::Light2DComponent, "height", Light2DComponent::NEW_HEIGHT);
        create->setNoMerge();
        CommandHandle::get(sp->id)->addCommand(create);
        project->setSelectedEntity(sp->id, create->getEntity());
        done = true;
    } else if (kind == "no_light_height" && fix == "Set Height 64") {
        SceneProject* sp = needTarget();
        if (!sp) return false;
        auto* multi = new MultiPropertyCmd();
        int count = 0;
        for (Entity e : enabledLights(sp)) {
            if (sp->scene->getComponent<Light2DComponent>(e).height > 0.0f) continue;
            multi->addPropertyCmd<float>(project, sp->id, e, ComponentType::Light2DComponent, "height", Light2DComponent::NEW_HEIGHT);
            count++;
        }
        if (count == 0) {
            delete multi;
            error = "No Light 2D has height 0.";
            return false;
        }
        multi->setNoMerge();
        CommandHandle::get(sp->id)->addCommand(multi);
        done = true;
    } else if (kind == "light_low" && fix == "Raise Light(s)") {
        SceneProject* sp = needTarget();
        if (!sp) return false;
        // the size the warning measured: the shown result's frames, else the source image
        const std::shared_ptr<sn::SpriteNormalsRun> r = shown();
        const int width = r ? r->result().normal.width : (current < items.size() ? items[current].info.width : 0);
        const int height = r ? r->result().normal.height : (current < items.size() ? items[current].info.height : 0);
        const sn::LowLights low = sn::lowLights(sp->scene, target.entity, sn::framePixels(shownCells(), width, height));
        if (low.lights.empty()) {
            error = "No Light 2D sits low for this sprite.";
            return false;
        }
        auto* multi = new MultiPropertyCmd();
        for (Entity e : low.lights) multi->addPropertyCmd<float>(project, sp->id, e, ComponentType::Light2DComponent, "height", low.raiseTo);
        multi->setNoMerge();
        CommandHandle::get(sp->id)->addCommand(multi);
        done = true;
    } else if (kind == "no_light_height" && fix == "Select Lights") {
        SceneProject* sp = needTarget();
        if (!sp) return false;
        project->replaceSelectedEntities(sp->id, enabledLights(sp));
        done = true;
    } else if (kind == "filter_mismatch" && fix == "Match Base Texture") {
        SceneProject* sp = needTarget();
        if (!sp) return false;
        MeshComponent* mesh = sp->scene->findComponent<MeshComponent>(target.entity);
        if (!mesh) {
            error = "The target has no mesh.";
            return false;
        }
        const unsigned s = target.submesh;
        const Texture& base = mesh->submeshes[s].material.baseColorTexture;
        Texture normal = mesh->submeshes[s].material.normalTexture;
        normal.setMinFilter(base.getMinFilter());
        normal.setMagFilter(base.getMagFilter());
        normal.setWrapU(base.getWrapU());
        normal.setWrapV(base.getWrapV());
        auto* cmd = new PropertyCmd<Texture>(project, sp->id, target.entity, ComponentType::MeshComponent,
                                             "submeshes[" + std::to_string(s) + "].material.normalTexture", normal);
        cmd->setNoMerge();
        CommandHandle::get(sp->id)->addCommand(cmd);
        done = true;
    } else if (kind == "size_mismatch" && fix == "Use Base Texture") {
        if (!hasAssignTarget() || current >= items.size()) {
            error = "There is no assign target.";
            return false;
        }
        // another source: unsaved strokes asked about first
        if (!paintDiscardOk([this, kind, fix]() {
                std::string e;
                if (!applyFix(kind, fix, e)) setStatus(e, true);
            })) {
            return true;
        }
        // the GameObject itself as the source: its texture, its frames
        Item& item = items[current];
        item.spec.source.clear();
        item.spec.sceneId = target.sceneId;
        item.spec.entity = target.entity;
        item.spec.submesh = target.submesh;
        item.spec.frames = sn::FramesSpec();
        item.spec.output = sn::OutputSpec();
        item.label = target.name;
        item.result.reset();
        probe(item, true);
        resetPreviewFor(false);
        loadPaintForTarget();
        markChanged();
        done = true;
    } else if (kind == "paint_size_mismatch" && fix == "Clear Paint") {
        clearPaint();
        done = true;
    } else if (kind == "directx_in_engine" && fix == "Switch to OpenGL") {
        settings.directx = false;
        markChanged();
        storeSettings();
        done = true;
    } else if (kind == "boxy" && fix.rfind("Use Bevel", 0) == 0) {
        // the radius the row offered: 5% of the boxy island's shorter side
        const std::shared_ptr<sn::SpriteNormalsRun> r = shown();
        if (!r || r->result().boxyIslands == 0) {
            error = "There is no boxy sprite in the preview.";
            return false;
        }
        settings.profile = sn::Profile::Bevel;
        settings.radius = static_cast<float>(sn::boxyBevelRadius(r->result().boxySide));
        markChanged();
        storeSettings();
        done = true;
    } else if (kind == "boxy" && fix == "Use Props") {
        setPreset(sn::Preset::Props);
        done = true;
    } else if (kind == "gaps_unmatched" && fix == "Lower Gap Amount") {
        // half of it, in whole percent, at least 1%
        settings.gaps.share = std::max(0.01f, std::round(settings.gaps.share * 50.0f) / 100.0f);
        markChanged();
        storeSettings();
        done = true;
    } else if (kind == "gaps_unmatched" && fix == "Turn Off Dark Gaps") {
        settings.gaps.share = 0.0f;
        markChanged();
        storeSettings();
        done = true;
    } else if (kind == "source_changed" && fix == "Reload") {
        if (current >= items.size()) return false;
        probe(items[current], true);
        colorLoadedKey.clear();
        markChanged();
        done = true;
    }
    if (!done) {
        error = "No fix \"" + fix + "\" for " + kind + ".";
        return false;
    }
    refreshWarnings(true);
    return true;
}

// ====================================================================== undo routing

bool SpriteNormalsWindow::undoGoesToScene(uint32_t& sceneId) const {
    if (!assignRecorded || !project->getScene(assignSceneId)) return false;
    // the assign itself is the scene's newest step (its position and its command: a new edit
    // made after undoing it, at the same position, is not it)
    const CommandHistory* history = CommandHandle::get(assignSceneId);
    if (history->position() == assignPosition && history->undoSerial() == assignSerial && history->canUndo()) {
        sceneId = assignSceneId;
        return true;
    }
    return false;
}

bool SpriteNormalsWindow::redoGoesToScene(uint32_t& sceneId) const {
    if (!assignRecorded || !project->getScene(assignSceneId)) return false;
    // undone after the assign, the write is the newest redoable: it comes back first
    const CommandHistory* projectHistory = const_cast<Project*>(project)->getProjectCommandHistory();
    if (writeRecorded && projectHistory->position() + 1 == writePosition && projectHistory->redoSerial() == writeSerial && projectHistory->canRedo()) {
        return false;
    }
    const CommandHistory* history = CommandHandle::get(assignSceneId);
    if (assignPosition > 0 && history->position() + 1 == assignPosition && history->redoSerial() == assignSerial && history->canRedo()) {
        sceneId = assignSceneId;
        return true;
    }
    return false;
}

// ====================================================================== GPU images

void SpriteNormalsWindow::retire(GpuImage& gpu) {
    if (!gpu.texture.empty()) retired.push_back(gpu.texture);
    gpu = GpuImage();
}

void SpriteNormalsWindow::releaseRetired() {
    for (Texture& texture : retired) texture.destroy();
    retired.clear();
}

void SpriteNormalsWindow::updateGpuImage(GpuImage& gpu, const sn::Image& rgba, uint64_t serial) {
    retire(gpu);
    // no GPU objects without the editor's renderer (the headless editor, tests)
    if (!editorUi() || rgba.empty() || rgba.channels != 4 || rgba.bitDepth != 8) return;
    static uint64_t counter = 0;
    // TextureData frees what it is given (stbi_image_free = free)
    const size_t bytes = rgba.data.size();
    void* pixels = std::malloc(bytes);
    if (!pixels) return;
    std::memcpy(pixels, rgba.data.data(), bytes);
    TextureData data(rgba.width, rgba.height, static_cast<unsigned int>(bytes), ColorFormat::RGBA, 4, pixels);
    gpu.texture.setData("editor:sprite-normals:" + std::to_string(++counter), data);
    gpu.texture.setMinFilter(TextureFilter::LINEAR);
    gpu.texture.setMagFilter(TextureFilter::NEAREST);
    TextureRender* render = gpu.texture.getRender();
    gpu.texture.releaseData();   // the GPU has it now (frees `pixels`)
    if (!render || !render->isCreated()) {
        retire(gpu);
        return;
    }
    gpu.width = rgba.width;
    gpu.height = rgba.height;
    gpu.serial = serial;
}

void SpriteNormalsWindow::updatePreviewImages() {
    const std::shared_ptr<sn::SpriteNormalsRun> r = shown();
    // the colour: the result's source (the atlas for an animation), else the file as it is
    std::string key;
    if (r) {
        key = "result:" + std::to_string(shownId());
    } else if (current < items.size() && !items[current].texturePath.empty()) {
        key = "file:" + items[current].texturePath;
    }
    if (key != colorLoadedKey) {
        colorLoadedKey = key;
        colorRgba = sn::Image();
        retire(colorGpu);
        retire(normalGpu);
        normalGpuFor = 0;
        if (r) {
            colorRgba = r->prepared().source.color;
        } else if (!key.empty() && current < items.size() && items[current].probeOk && liveAllowed(items[current])) {
            sn::Source source;
            std::vector<sn::Problem> problems;
            if (sn::loadSource(absoluteOf(project, items[current].texturePath), source, problems)) colorRgba = source.color;
        }
        updateGpuImage(colorGpu, colorRgba, resultSerial);
    }
    // the Normal view: the generation with the paint (a stroke changes it; uploaded at most 30
    // times a second while stroking)
    const bool uploadDue = !stroking || normalUploadedAt < 0.0 || now() - normalUploadedAt >= 1.0 / 30.0 || now() < normalUploadedAt;
    if (r && uploadDue && (normalGpuFor != paintSerial * 2 + (showPaint ? 1 : 0) || normalGpuResult != shownId() || (editorUi() && !normalGpu.valid()))) {
        updateGpuImage(normalGpu, displayNormal(), resultSerial);
        normalGpuFor = paintSerial * 2 + (showPaint ? 1 : 0);
        normalGpuResult = shownId();
        normalUploadedAt = now();
    }
    const bool litDue = litRenderedAt < 0.0 || now() - litRenderedAt >= 1.0 / 30.0 || now() < litRenderedAt;
    // following the scene's Light Steps: an edit of the scene redraws the preview
    if (lightSteps == kSceneLightSteps && effectiveLightSteps() != litSteps) litDirty = true;
    if (litDirty && litDue) renderLit();
    else if (litDirtyX0 < litDirtyX1 && litDirtyY0 < litDirtyY1 && litDue) renderLitRect();
}

void SpriteNormalsWindow::renderLitRect() {
    // a stroke: only its pixels of the lit preview again (the light did not move)
    litRenderedAt = now();
    const int rx0 = litDirtyX0, ry0 = litDirtyY0, rx1 = litDirtyX1, ry1 = litDirtyY1;
    litDirtyX0 = litDirtyY0 = litDirtyX1 = litDirtyY1 = 0;
    const std::shared_ptr<sn::SpriteNormalsRun> r = shown();
    if (!r || litRgba.empty()) {
        renderLit();
        return;
    }
    int x, y, w, h;
    cellRect(frame, x, y, w, h);
    // litRgba covers [x + litOriginX, ...) of the image
    const int lx0 = x + litOriginX, ly0 = y + litOriginY;
    const int cx0 = std::max(rx0, lx0), cy0 = std::max(ry0, ly0);
    const int cx1 = std::min(rx1, lx0 + litRgba.width), cy1 = std::min(ry1, ly0 + litRgba.height);
    if (cx0 >= cx1 || cy0 >= cy1) return;
    ensureLight();
    sn::LightSetup setup = lightSetup();
    // the light is relative to the shown cell: moved to the crop's origin
    setup.lights[0].x -= static_cast<float>(cx0 - x);
    setup.lights[0].y -= static_cast<float>(cy0 - y);
    const sn::Image part = sn::renderLit(crop(displayNormal(), cx0, cy0, cx1 - cx0, cy1 - cy0), crop(r->prepared().source.color, cx0, cy0, cx1 - cx0, cy1 - cy0), setup);
    if (part.empty()) return;
    for (int row = 0; row < part.height; row++) {
        std::memcpy(litRgba.pixel(cx0 - lx0, cy0 - ly0 + row), part.pixel(0, row), static_cast<size_t>(part.width) * 4);
    }
    litSerial++;
    if (windowOpen) updateGpuImage(litGpu, litRgba, litSerial);
}

// ====================================================================== sources

bool SpriteNormalsWindow::setSourcePath(const std::string& text, std::string& error) {
    if (text.empty()) {
        error = "Give an image of the project.";
        return false;
    }
    const fs::path abs = absoluteOf(project, text);
    std::error_code ec;
    const std::string ext = lower(jasperengine::editor::FileUtils::pathToUtf8(abs.extension()));
    if (!fs::is_regular_file(abs, ec)) {
        error = text + " does not exist.";
        return false;
    }
    if (!Util::isImageFile(ext) || ext == ".svg") {
        error = fileName(text) + " is not a PNG, GIF, APNG, JPEG, TGA or BMP image.";
        return false;
    }
    if (!paintDiscardOk([this, text]() {
            std::string e;
            if (!setSourcePath(text, e)) setStatus(e, true);
        })) {
        return true;
    }
    sn::Spec spec;
    spec.source = sn::projectRelative(project->getProjectPath(), abs);
    stopRun();
    if (items.empty()) items.push_back(Item());
    items[current] = makeItem(spec);
    notices.clear();
    runErrors.clear();
    probe(items[current], true);
    // the target stays only while it draws the new image
    keepTargetFor(items[current]);
    if (!hasAssignTarget()) autoTargetFor(items[current].texturePath);
    resetPreviewFor(false);
    loadPaintForTarget();
    markChanged();
    return true;
}

bool SpriteNormalsWindow::targetDraws(const Item& item) const {
    if (!hasAssignTarget()) return false;
    const SceneProject* sp = project->getScene(target.sceneId);
    if (!sp || !sp->scene) return false;
    const std::string base = baseTextureOf(project, sp->scene, target.entity, target.submesh);
    if (base.empty()) return false;
    if (base == jasperengine::editor::FileUtils::pathToGenericUtf8(fs::u8path(item.texturePath).lexically_normal())) return true;
    // an animation's atlas (a GIF assigned before draws its sheet)
    sn::Spec spec = specOf(item);
    if (spec.hasSprite()) spec.source = item.texturePath;
    for (const sn::PlannedOutput& out : sn::plannedOutputs(project->getProjectPath(), spec, item.info.kind)) {
        if (out.role == "atlas" && jasperengine::editor::FileUtils::pathToGenericUtf8(fs::u8path(out.path).lexically_normal()) == base) return true;
    }
    return false;
}

void SpriteNormalsWindow::keepTargetFor(const Item& item) {
    if (hasAssignTarget() && !targetDraws(item)) clearAssignTarget();
}

bool SpriteNormalsWindow::setSourceEntity(uint32_t sceneId, Entity entity, std::string& error) {
    const SceneProject* sp = project->getScene(sceneId);
    std::string texture;
    std::vector<sn::Cell> cells;
    if (!sp || !sp->scene || !sn::framesFromSprite(sp->scene, entity, 0, texture, cells, error)) {
        if (error.empty()) error = "The scene is not open.";
        return false;
    }
    if (!paintDiscardOk([this, sceneId, entity]() {
            std::string e;
            if (!setSourceEntity(sceneId, entity, e)) setStatus(e, true);
        })) {
        return true;
    }
    sn::Spec spec;
    spec.sceneId = sceneId;
    spec.entity = entity;
    stopRun();
    if (items.empty()) items.push_back(Item());
    items[current] = makeItem(spec);
    notices.clear();
    runErrors.clear();
    probe(items[current], true);
    setAssignTarget(sceneId, entity, 0);
    assign = true;
    resetPreviewFor(false);
    loadPaintForTarget();
    markChanged();
    return true;
}

std::string SpriteNormalsWindow::sourceSummary() const {
    if (current >= items.size()) return std::string();
    const Item& item = items[current];
    if (!item.probeOk) return item.probeError;
    const sn::SourceInfo& info = item.info;
    const std::string size = std::to_string(info.width) + " x " + std::to_string(info.height);
    if (isAnimated(info.kind)) {
        std::string text = std::to_string(info.frameCount) + "-frame " + (info.kind == sn::SourceKind::Gif ? "GIF" : "APNG") + ", " + size;
        if (const std::shared_ptr<sn::SpriteNormalsRun> r = shown()) {
            const int loops = r->prepared().source.loopCount;
            text += loops == 0 ? ", loops" : loops == 1 ? ", plays once" : ", plays " + std::to_string(loops) + " times";
        }
        return text;
    }
    std::string ext = lower(jasperengine::editor::FileUtils::pathToUtf8(fs::u8path(item.texturePath).extension()));
    if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    std::string text = size + " " + ext;
    if (item.spec.hasSprite()) {
        if (!item.spriteCells.empty()) {
            text += ", " + std::to_string(item.spriteCells.size()) + " frames " + std::to_string(item.spriteCells[0].width) + " x " +
                    std::to_string(item.spriteCells[0].height) + " (from Sprite \"" + item.label + "\")";
        } else {
            text = "Sprite \"" + item.label + "\" (" + fileName(item.texturePath) + "), " + text;
        }
    }
    return text;
}

std::string SpriteNormalsWindow::framesSummary() const {
    if (current >= items.size()) return std::string();
    const Item& item = items[current];
    if (!item.probeOk) return std::string();
    if (isAnimated(item.info.kind)) return std::to_string(item.info.frameCount) + " frames from the " + (item.info.kind == sn::SourceKind::Gif ? "GIF" : "APNG");
    const sn::FramesSpec& f = item.spec.frames;
    if (f.mode == sn::FramesSpec::Mode::Auto && !item.spriteCells.empty()) {
        return std::to_string(item.spriteCells.size()) + " frames " + std::to_string(item.spriteCells[0].width) + " x " +
               std::to_string(item.spriteCells[0].height) + " from Sprite \"" + item.label + "\"";
    }
    if (f.mode == sn::FramesSpec::Mode::Cells) return std::to_string(f.cells.size()) + " frames given by the recipe";
    if (f.mode == sn::FramesSpec::Mode::Grid) {
        std::vector<sn::Cell> cells;
        std::string error;
        if (sn::sliceGrid(f.grid, item.info.width, item.info.height, cells, error)) {
            return std::to_string(cells.size()) + " cells " + std::to_string(cells[0].width) + " x " + std::to_string(cells[0].height);
        }
        return error;
    }
    return "One image (no frames)";
}

// ====================================================================== UI helpers

namespace {

    // A text field over a std::string; enterOnly: Return or leaving the field applies it
    bool inputString(const char* id, const char* hint, std::string& value, float width, bool enterOnly = false) {
        char buffer[1024];
        std::snprintf(buffer, sizeof(buffer), "%s", value.c_str());
        ImGui::SetNextItemWidth(width);
        const ImGuiInputTextFlags flags = enterOnly ? ImGuiInputTextFlags_EnterReturnsTrue : ImGuiInputTextFlags_None;
        const bool changed = ImGui::InputTextWithHint(id, hint, buffer, sizeof(buffer), flags);
        if (enterOnly) {
            value = buffer;
            return changed || ImGui::IsItemDeactivatedAfterEdit();
        }
        if (changed) value = buffer;
        return changed;
    }

    // A tooltip above the hovered item (never over the rows under it, the controls a user
    // goes to next); wrapped at 30 font-sizes
    void tooltipAbove(const std::string& text, ImGuiHoveredFlags flags) {
        if (text.empty() || !ImGui::IsItemHovered(flags)) return;
        const ImVec2 min = ImGui::GetItemRectMin();
        ImGui::SetNextWindowPos(ImVec2(min.x, min.y - Theme::dpi(4.0f)), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
        if (ImGui::BeginTooltip()) {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
            ImGui::TextUnformatted(text.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }

    void tooltip(const std::string& text) {
        tooltipAbove(text, ImGuiHoveredFlags_ForTooltip);
    }

    // a tooltip past the right edge of the panel holding the hovered item, at its height: the
    // controls beside and under it stay visible
    void tooltipBeside(const std::string& text) {
        if (text.empty() || !ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) return;
        const ImVec2 min = ImGui::GetItemRectMin();
        const float right = ImGui::GetWindowPos().x + ImGui::GetWindowSize().x;
        ImGui::SetNextWindowPos(ImVec2(right + Theme::dpi(8.0f), min.y), ImGuiCond_Always);
        if (ImGui::BeginTooltip()) {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22.0f);
            ImGui::TextUnformatted(text.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }

    // a tooltip that also shows on a disabled item (the reason it is disabled)
    void reasonTooltip(const std::string& text) {
        tooltipAbove(text, ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_ForTooltip);
    }

    void subtle(const std::string& text) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::SubtleText);
        ImGui::TextWrapped("%s", text.c_str());
        ImGui::PopStyleColor();
    }

    void errorText(const std::string& text) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::ErrorText);
        ImGui::TextWrapped(ICON_FA_CIRCLE_XMARK " %s", text.c_str());
        ImGui::PopStyleColor();
    }

    void warningText(const std::string& text) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::WarningText);
        ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION " %s", text.c_str());
        ImGui::PopStyleColor();
    }

    // the label column of the settings rows
    float labelWidth() {
        return ImGui::GetFontSize() * 6.5f;
    }

    void rowLabel(const char* label, const char* help) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        if (help) tooltip(help);
        ImGui::SameLine(labelWidth());
    }

    // the toon preset's tooltip (its values: presetSettings(Preset::Toon))
    constexpr const char* kToonPresetTip = "Cel-shaded pixel art: Toon steps 8 (one tilt ring, flat below 15 deg), light in flat bands (strength 2, detail 0.5, smoothing 2, volume 1.3)";
    // the high-resolution presets' tooltips (their values: presetSettings)
    constexpr const char* kPaintedPresetTip = "Painted, vector and organic art at any size: profile inflate (blown-up shapes, no crease down the middle; strength 1.5, detail 3, smoothing 1)";
    constexpr const char* kPropsPresetTip = "Crates, panels, coins and buttons: profile dome, a round rim and a flat face (strength 1.5, volume 0.7, detail 3, smoothing 0.5, softness 1)";
    constexpr const char* kTilePresetTip = "Seamless textures: edges wrap, more painted relief (strength 2, detail 3, smoothing 1)";
    constexpr const char* kStonesPresetTip =
        "Seamless floors of stones or bricks between dark gaps: Dark Gaps Are Edges (Gap Amount 30%), profile dome, edges wrap: every stone "
        "a slab with a round rim (strength 2, detail 3, smoothing 1, softness 1.5)";

    // Physics Placement's segmented buttons (one selected, the others plain), equal widths; in a
    // dock too narrow for every label on one row they wrap to rows of equal columns (6: 3 + 3,
    // then 2 + 2 + 2), each row starting where the first did. With `probe`, each button is the UI
    // probe's "<probe>.<label>" ("<probe>Selected.<label>" when selected)
    bool segmentedButtons(const char* id, int& value, const char* const* labels, const char* const* tooltips, int count, const char* probe = nullptr) {
        bool changed = false;
        const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
        const float avail = ImGui::GetContentRegionAvail().x;
        float needed = 0.0f;
        for (int i = 0; i < count; i++) needed = std::max(needed, ImGui::CalcTextSize(labels[i]).x + ImGui::GetStyle().FramePadding.x * 2.0f);
        int columns = count;
        auto widthOf = [&](int n) { return std::max(1.0f, (avail - spacing * (float)(n - 1)) / (float)n); };
        while (columns > 1 && widthOf(columns) < needed) {
            const int rows = (count + columns - 1) / columns + 1;
            columns = std::min(columns - 1, (count + rows - 1) / rows);
        }
        const float width = widthOf(columns);
        const float startX = ImGui::GetCursorPosX();
        ImGui::PushID(id);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing, ImGui::GetStyle().ItemSpacing.y));
        for (int i = 0; i < count; i++) {
            if (i % columns != 0) ImGui::SameLine();
            else if (i > 0) ImGui::SetCursorPosX(startX);
            const bool selected = value == i;
            if (selected) {
                ImGui::PushStyleColor(ImGuiCol_Button, Theme::Colors::ButtonActivated);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::Colors::ButtonActivated);
            }
            if (ImGui::Button(labels[i], ImVec2(width, 0.0f)) && !selected) {
                value = i;
                changed = true;
            }
            if (selected) ImGui::PopStyleColor(2);
            if (probe) UIProbe::item(std::string(probe) + (selected ? "Selected." : ".") + labels[i]);
            if (tooltips && tooltips[i]) tooltip(tooltips[i]);
        }
        ImGui::PopStyleVar();
        ImGui::PopID();
        return changed;
    }

    // A small arrow before a value that overrides its preset: clicked, the preset's is back
    bool resetToPresetButton(const char* id) {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Theme::dpi(2.0f), ImGui::GetStyle().FramePadding.y));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        const bool clicked = ImGui::Button((std::string(ICON_FA_ROTATE_LEFT) + "##" + id).c_str());
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
        tooltip("Back to the preset");
        return clicked;
    }

    ImVec4 colourOf(uint32_t rgb) {
        return ImVec4(((rgb >> 16) & 0xff) / 255.0f, ((rgb >> 8) & 0xff) / 255.0f, (rgb & 0xff) / 255.0f, 1.0f);
    }

    void accentButtonColours() {
        const ImVec4 accent = Theme::Colors::Selection;
        ImGui::PushStyleColor(ImGuiCol_Button, accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(accent.x * 1.25f, accent.y * 1.25f, accent.z * 1.25f, accent.w));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(accent.x * 0.85f, accent.y * 0.85f, accent.z * 0.85f, accent.w));
    }

    // a GameObject the Selection button takes: a mesh whose base texture is an image file
    bool usableSelection(Project* project, uint32_t& sceneId, Entity& entity) {
        sceneId = project->getSelectedSceneId();
        SceneProject* sp = project->getScene(sceneId);
        const std::vector<Entity> selection = project->getSelectedEntities(sceneId);
        if (!sp || !sp->scene || selection.size() != 1) return false;
        entity = selection[0];
        return !baseTextureOf(project, sp->scene, entity, 0).empty();
    }

}

void SpriteNormalsWindow::scanProjectImages(bool force) {
    if (!force && projectImagesScannedAt >= 0.0 && now() - projectImagesScannedAt < 5.0) return;
    projectImagesScannedAt = now();
    projectImages.clear();
    const fs::path root = project->getProjectPath();
    for (const fs::path& file : ProjectChecks::findProjectFiles(root, {".png", ".jpg", ".jpeg", ".tga", ".bmp", ".gif", ".apng"})) {
        projectImages.push_back(sn::projectRelative(root, file));
        if (projectImages.size() >= 5000) break;
    }
}

bool SpriteNormalsWindow::imagePickerPopup(const char* popupId, std::string& path) {
    bool changed = false;
    ImGui::SetNextWindowSizeConstraints(ImVec2(Theme::dpi(320.0f), 0), ImVec2(Theme::dpi(560.0f), Theme::dpi(420.0f)));
    if (ImGui::BeginPopup(popupId)) {
        if (ImGui::IsWindowAppearing()) {
            scanProjectImages(true);
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##filter", ICON_FA_MAGNIFYING_GLASS " Filter project images", pickerFilter, sizeof(pickerFilter));
        const std::string filter = lower(pickerFilter);
        ImGui::BeginChild("##list", ImVec2(0, Theme::dpi(300.0f)));
        int listed = 0;
        for (const std::string& image : projectImages) {
            if (!filter.empty() && lower(image).find(filter) == std::string::npos) continue;
            if (++listed > 400) break;
            if (ImGui::Selectable(image.c_str(), image == path)) {
                path = image;
                changed = true;
                ImGui::CloseCurrentPopup();
            }
        }
        if (listed == 0) subtle("No image matches.");
        ImGui::EndChild();
        ImGui::EndPopup();
    }
    return changed;
}

// ====================================================================== left column

void SpriteNormalsWindow::drawNotices() {
    for (const std::string& notice : notices) {
        const bool fine = notice.find("unchanged") != std::string::npos;
        ImGui::PushStyleColor(ImGuiCol_Text, fine ? Theme::Colors::SubtleText : Theme::Colors::WarningText);
        ImGui::TextWrapped(ICON_FA_CIRCLE_INFO " %s", notice.c_str());
        ImGui::PopStyleColor();
    }
}

void SpriteNormalsWindow::drawSource() {
    ImGui::SeparatorText("Source");
    const float button = ImGui::GetFrameHeight();
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    std::string error;
    if (inputString("##source", "Image, GIF or a Sprite GameObject", sourceText, ImGui::GetContentRegionAvail().x - (button + spacing) * 2.0f, true)) {
        const std::string shownText = current < items.size() ? (items[current].spec.hasSprite() ? items[current].label : items[current].spec.source) : std::string();
        if (sourceText != shownText && !setSourcePath(sourceText, error)) setStatus(error, true);
    }
    UIProbe::item("spriteNormals.source");
    tooltip("A PNG, sprite sheet, GIF or APNG in the project, or a Sprite GameObject (its texture and its frames)");
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")) {
            const char* data = static_cast<const char*>(payload->Data);
            size_t offset = 0;
            while (offset < static_cast<size_t>(payload->DataSize)) {
                const std::string file(data + offset, strnlen(data + offset, static_cast<size_t>(payload->DataSize) - offset));
                offset += file.size() + 1;
                if (file.empty() || !Util::isImageFile(file) || lower(jasperengine::editor::FileUtils::pathToUtf8(fs::u8path(file).extension())) == ".svg") continue;
                if (!setSourcePath(file, error)) setStatus(error, true);
                break;
            }
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("entity")) {
            if (payload->DataSize >= static_cast<int>(sizeof(EntityPayload))) {
                const EntityPayload* entityPayload = static_cast<const EntityPayload*>(payload->Data);
                const uint32_t sceneId = payloadSceneId(*entityPayload, project->getSelectedSceneId());
                if (!setSourceEntity(sceneId, entityPayload->entity, error)) setStatus(error, true);
            }
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::SameLine();
    if (ImGui::Button("...##sourcePick", ImVec2(button, 0))) {
        pickerFilter[0] = 0;
        ImGui::OpenPopup("##sourcePicker");
    }
    UIProbe::item("spriteNormals.sourcePick");
    tooltip("Pick an image of the project");
    std::string picked = current < items.size() ? items[current].spec.source : std::string();
    if (imagePickerPopup("##sourcePicker", picked) && !setSourcePath(picked, error)) setStatus(error, true);
    ImGui::SameLine();
    uint32_t selScene = 0;
    Entity selEntity = NULL_ENTITY;
    const bool usable = usableSelection(project, selScene, selEntity);
    ImGui::BeginDisabled(!usable);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, ImGui::GetStyle().FramePadding.y));
    const bool selectionClicked = ImGui::Button(ICON_FA_ARROW_POINTER "##sourceSel", ImVec2(button, 0));
    ImGui::PopStyleVar();
    if (selectionClicked && usable) {
        if (!setSourceEntity(selScene, selEntity, error)) setStatus(error, true);
    }
    ImGui::EndDisabled();
    UIProbe::item("spriteNormals.sourceSel");
    reasonTooltip(usable ? "Use the selected GameObject: its texture, its frames, and assign the normal map to it" : "Select one Sprite GameObject first");
    const std::string summary = sourceSummary();
    if (!summary.empty()) {
        if (current < items.size() && !items[current].probeOk) errorText(summary);
        else subtle(summary);
    }
}

void SpriteNormalsWindow::drawBatch() {
    ImGui::SeparatorText("Sources");
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
    const float rows = std::min<float>(static_cast<float>(items.size()), 8.0f) + 0.5f;
    if (ImGui::BeginTable("##batch", 5, flags, ImVec2(0, ImGui::GetFrameHeightWithSpacing() * rows))) {
        ImGui::TableSetupColumn("##on", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Frames", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("##state", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 1.2f);
        for (size_t i = 0; i < items.size(); i++) {
            Item& item = items[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Checkbox("##checked", &item.checked);
            UIProbe::item("spriteNormals.batch." + item.label);
            tooltip("Save All writes this one");
            ImGui::TableSetColumnIndex(1);
            if (ImGui::Selectable(item.label.c_str(), i == current, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) selectItem(i);
            tooltip(item.texturePath.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextDisabled("%s", item.probeOk ? (std::to_string(item.info.width) + " x " + std::to_string(item.info.height)).c_str() : "-");
            ImGui::TableSetColumnIndex(3);
            // where its frames come from (each item's own Auto)
            const char* from = !item.probeOk                                      ? "-"
                               : item.info.kind == sn::SourceKind::Gif            ? "GIF"
                               : item.info.kind == sn::SourceKind::Apng           ? "APNG"
                               : item.spec.frames.mode == sn::FramesSpec::Mode::Grid  ? "grid"
                               : item.spec.frames.mode == sn::FramesSpec::Mode::Cells ? "given"
                               : !item.spriteCells.empty()                        ? "Sprite"
                                                                                  : "single";
            ImGui::TextDisabled("%s", from);
            tooltip(std::string("Frames from: ") + from);
            ImGui::TableSetColumnIndex(4);
            std::string problem = item.probeError;
            for (const sn::Problem& p : item.problems) {
                if (problem.empty()) problem = p.message;
            }
            const std::vector<std::string> existing = existingOutputs({i});
            if (!problem.empty()) {
                ImGui::TextColored(Theme::Colors::ErrorText, ICON_FA_CIRCLE_XMARK);
                tooltip(problem.c_str());
            } else if (!existing.empty()) {
                ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION);
                tooltip(fileName(existing[0]) + " exists: Save All replaces it (undo puts it back)");
            } else {
                ImGui::TextColored(ImVec4(0.5f, 0.85f, 0.55f, 1.0f), ICON_FA_CHECK);
                tooltip("Ready");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::SmallButton("All")) {
        for (Item& item : items) item.checked = true;
    }
    tooltip("Check every image");
    ImGui::SameLine();
    if (ImGui::SmallButton("None")) {
        for (Item& item : items) item.checked = false;
    }
    tooltip("Uncheck every image");
    subtle("Click a row to preview it. The settings are shared; each image keeps its own frames.");
}

void SpriteNormalsWindow::drawFrames() {
    if (current >= items.size()) return;
    Item& item = items[current];
    ImGui::SeparatorText("Frames");
    sn::FramesSpec f = item.spec.frames;
    const bool animated = item.probeOk && isAnimated(item.info.kind);
    bool changed = false;
    if (ImGui::RadioButton("Auto", f.mode == sn::FramesSpec::Mode::Auto) && f.mode != sn::FramesSpec::Mode::Auto) {
        f.mode = sn::FramesSpec::Mode::Auto;
        changed = true;
    }
    UIProbe::item("spriteNormals.frames.Auto");
    tooltip("The Sprite's frames, an animation's frames, else the whole image");
    ImGui::SameLine();
    ImGui::BeginDisabled(animated || isBatch());
    if (ImGui::RadioButton("None", f.mode == sn::FramesSpec::Mode::None) && f.mode != sn::FramesSpec::Mode::None) {
        f.mode = sn::FramesSpec::Mode::None;
        changed = true;
    }
    reasonTooltip(animated ? "An animation's frames come from the file" : "The whole image as one picture (no frames)");
    ImGui::SameLine();
    if (ImGui::RadioButton("Grid", f.mode == sn::FramesSpec::Mode::Grid) && f.mode != sn::FramesSpec::Mode::Grid) {
        f.mode = sn::FramesSpec::Mode::Grid;
        if (f.grid.tileWidth <= 0 && f.grid.columns <= 0) {
            f.grid.tileWidth = item.probeOk ? std::min(32, std::max(1, item.info.width)) : 32;
            f.grid.tileHeight = item.probeOk ? std::min(32, std::max(1, item.info.height)) : 32;
        }
        changed = true;
    }
    reasonTooltip(animated ? "An animation's frames come from the file" : "Cut the sheet into cells, each processed on its own");
    ImGui::EndDisabled();

    if (f.mode == sn::FramesSpec::Mode::Grid) {
        sn::Grid& g = f.grid;
        auto int2 = [&](const char* label, const char* id, int& a, int& b, int lo, const char* help) {
            rowLabel(label, help);
            int v[2] = {a, b};
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputInt2(id, v)) {
                a = std::clamp(v[0], lo, 4096);
                b = std::clamp(v[1], lo, 4096);
                changed = true;
            }
        };
        auto int1 = [&](const char* label, const char* id, int& a, const char* help) {
            rowLabel(label, help);
            ImGui::SetNextItemWidth(-FLT_MIN);
            int v = a;
            if (ImGui::InputInt(id, &v)) {
                a = std::clamp(v, 0, 4096);
                changed = true;
            }
        };
        int2("Tile size", "##tile", g.tileWidth, g.tileHeight, 1, "Width and height of one cell, in pixels");
        int1("Columns", "##cols", g.columns, "0 = as many as fit");
        int1("Rows", "##rows", g.rows, "0 = as many as fit");
        int1("Count", "##count", g.count, "0 = every cell");
        int2("Offset", "##offset", g.offsetX, g.offsetY, 0, "Where the first cell starts, in pixels");
        int2("Padding", "##padding", g.paddingX, g.paddingY, 0, "Space between cells, in pixels");
        const SceneProject* sp = hasAssignTarget() ? project->getScene(target.sceneId) : nullptr;
        const SpriteComponent* sprite = sp && sp->scene ? sp->scene->findComponent<SpriteComponent>(target.entity) : nullptr;
        ImGui::BeginDisabled(!sprite || sprite->numFramesRect == 0);
        std::vector<sn::Cell> c;
        if (ImGui::Button("From Sprite Frames##fromSprite") && sprite) {
            std::string texture, error;
            sn::framesFromSprite(sp->scene, target.entity, 0, texture, c, error);
        }
        if (!c.empty()) {
            // the Slicer's uniform grid back into the fields
            g = sn::Grid();
            g.tileWidth = c[0].width;
            g.tileHeight = c[0].height;
            g.offsetX = c[0].x;
            g.offsetY = c[0].y;
            int columns = 0;
            for (const sn::Cell& cell : c) {
                if (cell.y == c[0].y) columns++;
            }
            if (c.size() > 1 && columns > 1) g.paddingX = std::max(0, c[1].x - c[0].x - c[0].width);
            for (const sn::Cell& cell : c) {
                if (cell.y != c[0].y) {
                    g.paddingY = std::max(0, cell.y - c[0].y - c[0].height);
                    break;
                }
            }
            g.count = static_cast<int>(c.size());
            changed = true;
        }
        ImGui::EndDisabled();
        reasonTooltip(sprite && sprite->numFramesRect > 0 ? "Use the frames the Slicer Tool made for this sprite" : "The assign target has no frames from the Slicer Tool");
    } else if (f.mode == sn::FramesSpec::Mode::Cells) {
        subtle(framesSummary());
    } else {
        subtle(framesSummary());
    }
    for (const sn::Problem& p : item.problems) {
        if (p.field.rfind("frames", 0) == 0) errorText(p.message);
    }
    if (changed) setFrames(f);
}

void SpriteNormalsWindow::drawPresetAndSliders() {
    ImGui::SeparatorText("Settings");
    const char* labels[] = {"Soft", "Default", "Crisp", "Detailed", "Chunky", "Toon", "Painted", "Props", "Tile", "Stones"};
    const char* tips[] = {"Gentle relief for painterly art (strength 1, detail 2, smoothing 1)",
                          "Best measured accuracy on 16-64 px sprites (strength 1.5, detail 3, smoothing 1); above 128 px it inflates the shape",
                          "Small characters: keeps 1 px features (smoothing 0.6)",
                          "The Sprite Fusion defaults (radius 4 for an exact match): the most painted detail; highlights become bumps",
                          "Stylised, strong volume (strength 2.5, detail 4, volume 2)",
                          kToonPresetTip,
                          kPaintedPresetTip,
                          kPropsPresetTip,
                          kTilePresetTip,
                          kStonesPresetTip};
    int preset = static_cast<int>(settings.preset);
    if (segmentedButtons("##preset", preset, labels, tips, 10, "spriteNormals.presets")) setPreset(static_cast<sn::Preset>(preset));
    UIProbe::item("spriteNormals.preset");
    const sn::Settings base = sn::presetSettings(settings.preset);
    auto slider = [&](const char* label, const char* id, float& value, float presetValue, float lo, float hi, const char* format, const char* help) {
        ImGui::PushID(id);
        rowLabel(label, help);
        if (std::fabs(value - presetValue) > 1e-4f) {
            if (resetToPresetButton(id)) {
                value = presetValue;
                markChanged();
                storeSettings();
            }
            ImGui::SameLine();
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        float v = value;
        if (UIUtils::sliderFloatInput(id, &v, lo, hi, format)) {
            value = v;
            markChanged();
            storeSettings();
        }
        UIProbe::item(std::string("spriteNormals.") + (id + 2));
        tooltip(help);
        ImGui::PopID();
    };
    slider("Strength", "##strength", settings.strength, base.strength, 0.0f, 8.0f, "%.2f", "How strongly every slope tilts the normals");
    slider("Detail", "##detail", settings.detail, base.detail, 0.0f, 8.0f, "%.2f",
           "Relief from the painted shading: brighter reads as higher (for a 128 px sprite: larger sprites scale it, see Resolution scale)");
    slider("Smoothing", "##smoothing", settings.smoothing, base.smoothing, 0.0f, 32.0f, "%.2f px",
           "Blur of the shading before it becomes relief: higher removes banding steps and brush texture, lower keeps 1 px detail "
           "(px of a 128 px sprite: larger sprites scale it, see Resolution scale)");
    slider("Volume", "##volume", settings.volume, base.volume, 0.0f, 8.0f, "%.2f", "Rounding from the silhouette: how domed the sprite is");
    drawToonSteps(base);
    drawGaps(base);
    for (const sn::Problem& p : current < items.size() ? items[current].problems : std::vector<sn::Problem>()) {
        if (p.kind == "invalid_settings") errorText(p.message);
    }
}

void SpriteNormalsWindow::drawToonSteps(const sn::Settings& base) {
    static const char* const help =
        "Snap every normal to a few directions so a moving light falls in flat, stepped bands (cel-shaded pixel art) "
        "instead of smooth gradients. Off: smooth normals. 4, 8, 16: directions around the circle; 6 and 12, the tilt rings, "
        "the flat cone and the rotation are under Advanced. Paint is kept exactly as painted";
    ImGui::PushID("##toonSteps");
    rowLabel("Toon steps", help);
    if (settings.quantize.directions != base.quantize.directions) {
        if (resetToPresetButton("toonSteps")) {
            settings.quantize = base.quantize;
            markChanged();
            storeSettings();
        }
        ImGui::SameLine();
    }
    // Off / 4 / 8 / 16, and 6 or 12 (set in Advanced) shown in its place among them while selected
    std::vector<int> values = {0, 4, 8, 16};
    std::vector<std::string> names = {"Off", "4", "8", "16"};
    std::vector<std::string> tipTexts = {"Smooth normals (no steps)", "4 directions: chunky, blocky light", "8 directions: classic cel shading",
                                         "16 directions: finer steps, still banded"};
    const int d = settings.quantize.directions;
    if (d == 6 || d == 12) {
        const size_t at = d == 6 ? 2 : 3;
        values.insert(values.begin() + static_cast<std::ptrdiff_t>(at), d);
        names.insert(names.begin() + static_cast<std::ptrdiff_t>(at), std::to_string(d));
        tipTexts.insert(tipTexts.begin() + static_cast<std::ptrdiff_t>(at), std::to_string(d) + " directions (set in Advanced)");
    }
    std::vector<const char*> labels, tips;
    for (size_t i = 0; i < values.size(); i++) {
        labels.push_back(names[i].c_str());
        tips.push_back(tipTexts[i].c_str());
    }
    int v = -1;
    for (size_t i = 0; i < values.size(); i++) {
        if (d == values[i]) v = static_cast<int>(i);
    }
    if (segmentedButtons("##toonStepsButtons", v, labels.data(), tips.data(), static_cast<int>(labels.size()), "spriteNormals.toonSteps")) {
        settings.quantize.directions = values[static_cast<size_t>(v)];
        markChanged();
        storeSettings();
    }
    ImGui::PopID();
}

void SpriteNormalsWindow::drawGaps(const sn::Settings& base) {
    static const char* const help =
        "Floor tiles and walls: the darkest part of the image (the gaps between stones or bricks) is not part of the shape, so "
        "every stone gets its own rounded rim. Gap Amount: how much of the image is gap. Works with every profile and edges mode";
    ImGui::PushID("##gaps");
    rowLabel("Gaps", help);
    if (settings.gaps != base.gaps) {
        if (resetToPresetButton("gaps")) {
            settings.gaps = base.gaps;
            markChanged();
            storeSettings();
        }
        ImGui::SameLine();
    }
    bool on = settings.gaps.on();
    if (ImGui::Checkbox("Dark Gaps Are Edges", &on)) {
        settings.gaps.share = on ? (base.gaps.on() ? base.gaps.share : 0.3f) : 0.0f;
        markChanged();
        storeSettings();
    }
    UIProbe::item("spriteNormals.gaps");
    tooltip(help);
    static const char* const amountHelp =
        "How much of the image is gap: the darkest share of its pixels (30%: about the grout of a stone floor). Raise it when stones "
        "merge, lower it when stones lose their dark edges into the gaps";
    rowLabel("Gap Amount", amountHelp);
    ImGui::BeginDisabled(!on);
    ImGui::SetNextItemWidth(-FLT_MIN);
    float percent = (on ? settings.gaps.share : (base.gaps.on() ? base.gaps.share : 0.3f)) * 100.0f;
    if (UIUtils::sliderFloatInput("##gapAmount", &percent, 1.0f, 90.0f, "%.0f%%") && on) {
        settings.gaps.share = std::clamp(percent, 1.0f, 90.0f) / 100.0f;
        markChanged();
        storeSettings();
    }
    ImGui::EndDisabled();
    UIProbe::item("spriteNormals.gapAmount");
    if (on) tooltip(amountHelp);
    else reasonTooltip("Turn Dark Gaps Are Edges on to use this");
    ImGui::PopID();
}

void SpriteNormalsWindow::drawToonAdvanced(const sn::Settings& base, bool& changed) {
    const bool on = settings.quantize.on();
    ImGui::SeparatorText("Toon steps");
    rowLabel("Directions", "Toon steps: how many directions around the circle (Off, 4, 6, 8, 12, 16)");
    {
        static const int values[] = {0, 4, 6, 8, 12, 16};
        const char* names[] = {"Off", "4", "6", "8", "12", "16"};
        int v = 0;
        for (int i = 0; i < 6; i++) {
            if (settings.quantize.directions == values[i]) v = i;
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##toonDirections", names[v])) {
            for (int i = 0; i < 6; i++) {
                if (ImGui::Selectable(names[i], i == v) && i != v) {
                    settings.quantize.directions = values[i];
                    changed = true;
                }
                UIProbe::item(std::string("spriteNormals.toonDirections.") + names[i]);
            }
            ImGui::EndCombo();
        }
        UIProbe::item("spriteNormals.toonDirections");
        tooltip("Toon steps: how many directions around the circle (Off, 4, 6, 8, 12, 16)");
    }
    ImGui::BeginDisabled(!on);
    const char* offReason = "Turn Toon steps on (Settings) to use this";
    auto quantSlider = [&](const char* label, const char* id, float& value, float presetValue, float lo, float hi, const char* format,
                           const char* help) {
        ImGui::PushID(id);
        rowLabel(label, help);
        if (on && std::fabs(value - presetValue) > 1e-4f) {
            if (resetToPresetButton(id)) {
                value = presetValue;
                changed = true;
            }
            ImGui::SameLine();
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        float v = value;
        if (UIUtils::sliderFloatInput(id, &v, lo, hi, format)) {
            value = v;
            changed = true;
        }
        if (on) tooltip(help);
        else reasonTooltip(offReason);
        ImGui::PopID();
    };
    {
        ImGui::PushID("##toonTilts");
        const char* help = "Toon steps: how many tilt bands between flat and edge-on (1 = one ring of slopes, 4 = finer)";
        rowLabel("Tilt rings", help);
        if (on && settings.quantize.tilts != base.quantize.tilts) {
            if (resetToPresetButton("toonTilts")) {
                settings.quantize.tilts = base.quantize.tilts;
                changed = true;
            }
            ImGui::SameLine();
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        int tilts = settings.quantize.tilts;
        if (ImGui::SliderInt("##tilts", &tilts, 1, 4, "%d", ImGuiSliderFlags_AlwaysClamp)) {
            settings.quantize.tilts = std::clamp(tilts, 1, 4);
            changed = true;
        }
        UIProbe::item("spriteNormals.toonTilts");
        if (on) tooltip(help);
        else reasonTooltip(offReason);
        ImGui::PopID();
    }
    quantSlider("Flat below", "##toonFlat", settings.quantize.flatDeg, base.quantize.flatDeg, 0.0f, 45.0f, "%.0f deg",
                "Toon steps: normals tilted less than this face the viewer exactly (flat tops, no speckle)");
    const float rotationBefore = settings.quantize.rotationDeg;
    quantSlider("Rotation", "##toonRotation", settings.quantize.rotationDeg, base.quantize.rotationDeg, 0.0f, 360.0f, "%.0f deg",
                "Toon steps: turns the directions (0 = one points right). Keep 0 for straight-sided art: a step boundary on an axis "
                "(22.5 with 8) makes those sides speckle");
    if (settings.quantize.rotationDeg != rotationBefore) {
        // 360 is 0 (a whole turn; recipes record 0)
        float r = std::fmod(settings.quantize.rotationDeg, 360.0f);
        settings.quantize.rotationDeg = r < 0.0f ? r + 360.0f : r;
    }
    ImGui::EndDisabled();
}

void SpriteNormalsWindow::drawAdvanced() {
    ImGui::SetNextItemOpen(advancedOpen, ImGuiCond_Always);
    const bool openNow = Theme::foldoutHeader("Advanced");
    UIProbe::item("spriteNormals.advanced");
    if (openNow != advancedOpen) {
        advancedOpen = openNow;
        storeSettings();
    }
    if (!advancedOpen) return;
    const sn::Settings base = sn::presetSettings(settings.preset);
    bool changed = false;

    // Radius: auto, or a value
    rowLabel("Radius", "How far in from the edge the rounding reaches. Auto: 30% of each island's thickness (the widest circle inside it; Dome: the thickness, at most 15% of the shorter side), at least 4 px");
    bool autoRadius = !settings.radius.has_value();
    if (ImGui::Checkbox("Auto##radiusAuto", &autoRadius)) {
        if (autoRadius) settings.radius.reset();
        else settings.radius = 4.0f;
        changed = true;
    }
    tooltip("How far in from the edge the rounding reaches. Auto: 30% of each island's thickness (the widest circle inside it; Dome: the thickness, at most 15% of the shorter side), at least 4 px");
    ImGui::SameLine();
    ImGui::BeginDisabled(autoRadius);
    float radius = settings.radius.value_or(4.0f);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderFloat("##radius", &radius, 0.5f, 512.0f, "%.1f px", ImGuiSliderFlags_Logarithmic) && !autoRadius) {
        settings.radius = radius;
        changed = true;
    }
    ImGui::EndDisabled();
    if (autoRadius) {
        if (const std::shared_ptr<sn::SpriteNormalsRun> r = shown()) {
            const sn::Result& res = r->result();
            ImGui::Indent(labelWidth());
            if (res.radiusPerIsland && res.radiusMax > res.radiusMin + 1e-3f) {
                subtle("Auto: per island (" + trimmed(res.radiusMin, 1) + "-" + trimmed(res.radiusMax, 1) + " px)");
            } else {
                subtle("Auto: " + trimmed(res.radiusMin > 0 ? res.radiusMin : res.radiusMax, 1) + " px");
            }
            ImGui::Unindent(labelWidth());
        }
    }

    auto slider = [&](const char* label, const char* id, float& value, float presetValue, float lo, float hi, const char* format, const char* help) {
        ImGui::PushID(id);
        rowLabel(label, help);
        if (std::fabs(value - presetValue) > 1e-4f) {
            if (resetToPresetButton(id)) {
                value = presetValue;
                changed = true;
            }
            ImGui::SameLine();
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        float v = value;
        if (UIUtils::sliderFloatInput(id, &v, lo, hi, format)) {
            value = v;
            changed = true;
        }
        tooltip(help);
        ImGui::PopID();
    };
    slider("Softness", "##softness", settings.softness, base.softness, 0.0f, 32.0f, "%.2f px",
           "Blur of the silhouette rounding: removes the crease along the middle and fans from a wobbly outline (px of a 128 px sprite)");
    slider("Alpha cutoff", "##alpha", settings.alphaThreshold, base.alphaThreshold, 0.0f, 0.99f, "%.2f",
           "Pixels this transparent or more are empty (soft glows below it get no relief)");

    static constexpr const char* kProfileTip =
        "Auto: Rounded up to 128 px, Inflate above. Rounded: keeps rising toward the middle (Sprite Fusion). Dome: round edge, flat top "
        "(props, panels). Bevel: straight edge, flat top. Inflate: blown up like a balloon, follows every part's width with no crease "
        "(painted and vector art)";
    rowLabel("Profile", kProfileTip);
    {
        const char* names[] = {"Auto", "Rounded", "Dome", "Bevel", "Inflate"};
        const sn::Profile values[] = {sn::Profile::Auto, sn::Profile::Rounded, sn::Profile::Dome, sn::Profile::Bevel, sn::Profile::Inflate};
        int v = 0;
        for (int i = 0; i < 5; i++) {
            if (values[i] == settings.profile) v = i;
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo("##profile", &v, names, 5)) {
            settings.profile = values[v];
            changed = true;
        }
        UIProbe::item("spriteNormals.profile");
        tooltip(kProfileTip);
        if (settings.profile == sn::Profile::Auto) {
            if (const std::shared_ptr<sn::SpriteNormalsRun> r = shown()) {
                std::string used;
                for (sn::Profile p : {sn::Profile::Rounded, sn::Profile::Dome, sn::Profile::Bevel, sn::Profile::Inflate}) {
                    if (r->result().profilesUsed & (1u << static_cast<int>(p))) used += (used.empty() ? "" : ", ") + std::string(sn::toString(p));
                }
                if (!used.empty()) {
                    ImGui::Indent(labelWidth());
                    subtle("Auto: " + used);
                    ImGui::Unindent(labelWidth());
                }
            }
        }
    }
    static constexpr const char* kScaleTip =
        "Smoothing, Softness and Detail are set for a 128 px sprite; on: they grow with the sprite (x its widest part / 128, so "
        "the same art at 4x the size looks the same), off: they are taken as pixels. Sprites of 128 px or less are the same either way";
    rowLabel("Resolution", kScaleTip);
    {
        bool scale = settings.resolutionScale == sn::ResolutionScale::Auto;
        if (ImGui::Checkbox("Scale with size##resolutionScale", &scale)) {
            settings.resolutionScale = scale ? sn::ResolutionScale::Auto : sn::ResolutionScale::Off;
            changed = true;
        }
        UIProbe::item("spriteNormals.resolutionScale");
        tooltip(kScaleTip);
        if (const std::shared_ptr<sn::SpriteNormalsRun> r = shown()) {
            const sn::Result& res = r->result();
            if (res.scaleMax > 1.0f + 1e-4f) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", (res.scaleMax > res.scaleMin + 1e-3f ? "x" + trimmed(res.scaleMin, 2) + "-" + trimmed(res.scaleMax, 2)
                                                                                : "x" + trimmed(res.scaleMax, 2))
                                              .c_str());
            }
        }
    }
    static constexpr const char* kSoftEdgeTip =
        "Above 128 px: Outside (auto) = soft pixels (alpha under 0.5) that reach the transparent outside are not part of the "
        "shape (glows, feathered rims don't inflate the sprite); Solid = every visible pixel is shape (translucent art: ghosts, "
        "smoke, glass with a thin rim)";
    rowLabel("Soft edges", kSoftEdgeTip);
    {
        const char* names[] = {"Outside (auto)", "Solid"};
        int v = settings.softEdge == sn::SoftEdge::Solid ? 1 : 0;
        if (segmentedButtons("##softEdge", v, names, nullptr, 2, "spriteNormals.softEdge")) {
            settings.softEdge = v == 1 ? sn::SoftEdge::Solid : sn::SoftEdge::Auto;
            changed = true;
        }
        tooltip(kSoftEdgeTip);
    }
    if (settings.version < sn::kSettingsVersion) {
        ImGui::Indent(labelWidth());
        subtle("Made before high-resolution support: regenerating keeps its old look (no resolution scaling, the old auto radius).");
        if (ImGui::SmallButton("Use the current generator##settingsVersion")) {
            settings.version = sn::kSettingsVersion;
            changed = true;
        }
        UIProbe::item("spriteNormals.upgradeVersion");
        ImGui::Unindent(labelWidth());
    }
    rowLabel("Edges", nullptr);
    {
        const char* names[] = {"Transparent", "Wrap (seamless)", "Extend (tiles)"};
        int v = static_cast<int>(settings.edges);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo("##edges", &v, names, 3)) {
            settings.edges = static_cast<sn::Edges>(v);
            changed = true;
        }
        tooltip("What lies past the image (or each frame): Transparent = empty (the border bevels), Wrap = the opposite side (seamless "
                "textures), Extend = the border continues (tileset tiles)");
    }
    rowLabel("Outline", nullptr);
    if (ImGui::Checkbox("Ignore outline##ignoreOutline", &settings.ignoreOutline)) changed = true;
    tooltip("Find the dark outline colour and keep it out of the relief (no trench inside the rim)");
    if (settings.ignoreOutline) {
        if (const std::shared_ptr<sn::SpriteNormalsRun> r = shown()) {
            ImGui::SameLine();
            if (r->result().outlineColor) {
                const uint32_t c = *r->result().outlineColor;
                ImGui::ColorButton("##outlineColour", colourOf(c), ImGuiColorEditFlags_NoTooltip, ImVec2(ImGui::GetFontSize(), ImGui::GetFontSize()));
                ImGui::SameLine();
                ImGui::TextDisabled("%s", sn::colorText(c).c_str());
            } else {
                ImGui::TextDisabled("No outline found");
            }
        }
    }
    rowLabel("Green", nullptr);
    {
        const char* names[] = {"OpenGL", "DirectX"};
        const char* tips[] = {"Green points up the image: what JasperEngine, Unity, Godot and Blender read",
                              "Green points down: Unreal. Not for this engine's sprites"};
        int v = settings.directx ? 1 : 0;
        if (segmentedButtons("##green", v, names, tips, 2)) {
            settings.directx = v == 1;
            changed = true;
        }
    }
    drawToonAdvanced(base, changed);
    if (changed) {
        markChanged();
        storeSettings();
    }
    drawRegions();
}

void SpriteNormalsWindow::drawRegions() {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Regions");
    tooltip("Per-colour overrides: keep highlights or outlines out of the relief, raise or sink a material, or keep it flat");
    ImGui::SameLine();
    const float addWidth = ImGui::CalcTextSize("+ Region").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - addWidth));
    if (ImGui::Button("+ Region##addRegion")) {
        if (static_cast<int>(settings.regions.size()) >= MAX_REGIONS) {
            setStatus("At most 16 regions.", false);
        } else {
            settings.regions.push_back(sn::Region());
            lastEditedRegion = static_cast<int>(settings.regions.size()) - 1;
            pickingRegion = lastEditedRegion;
            markChanged();
            storeSettings();
        }
    }
    UIProbe::item("spriteNormals.addRegion");
    tooltip("Per-colour overrides: keep highlights or outlines out of the relief, raise or sink a material, or keep it flat");
    if (settings.regions.empty()) subtle("None. Add one, then click colours in the preview (or Alt+click anywhere).");
    int removeAt = -1;
    bool changed = false;
    for (size_t i = 0; i < settings.regions.size(); i++) {
        sn::Region& region = settings.regions[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::Separator();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Region %zu", i + 1);
        ImGui::SameLine();
        // colour chips: a click removes the colour
        const float chip = ImGui::GetFontSize();
        int removeColour = -1;
        for (size_t c = 0; c < region.colors.size(); c++) {
            ImGui::PushID(static_cast<int>(c));
            if (ImGui::GetContentRegionAvail().x < chip + ImGui::GetStyle().ItemSpacing.x) ImGui::NewLine();
            if (ImGui::ColorButton("##chip", colourOf(region.colors[c]), ImGuiColorEditFlags_NoTooltip, ImVec2(chip, chip))) removeColour = static_cast<int>(c);
            tooltip(sn::colorText(region.colors[c]) + ": click to remove");
            ImGui::SameLine();
            ImGui::PopID();
        }
        if (removeColour >= 0) {
            region.colors.erase(region.colors.begin() + removeColour);
            lastEditedRegion = static_cast<int>(i);
            changed = true;
        }
        // eyedropper toggle and a typed colour
        const bool picking = pickingRegion == static_cast<int>(i);
        if (picking) ImGui::PushStyleColor(ImGuiCol_Button, Theme::Colors::ButtonActivated);
        if (ImGui::Button(ICON_FA_EYE_DROPPER "##pick")) {
            pickingRegion = picking ? -1 : static_cast<int>(i);
            lastEditedRegion = static_cast<int>(i);
        }
        if (picking) ImGui::PopStyleColor();
        UIProbe::item("spriteNormals.region." + std::to_string(i) + ".pick");
        tooltip("Pick colours from the preview: click pixels to add their colour; Esc or a second click ends picking");
        ImGui::SameLine();
        // the colour "+" adds, chosen in the Color window (S27: every colour field is ui::colorField;
        // sRGB as the region stores it). A swatch beside the button, not a popup: the Color window
        // taking focus would close a popup and lose the choice
        {
            Vector3 rgb(((addColourValue >> 16) & 0xff) / 255.0f, ((addColourValue >> 8) & 0xff) / 255.0f, (addColourValue & 0xff) / 255.0f);
            ui::ColorFieldOptions addOptions;
            addOptions.alpha = false;
            addOptions.linearStorage = false;
            addOptions.label = "Region Colour";
            addOptions.width = ImGui::GetFrameHeight() * 1.5f;
            const ui::EditPhase phase = ui::colorField("##addColour", rgb, addOptions);
            if (phase == ui::EditPhase::Changing || phase == ui::EditPhase::Cancelled || phase == ui::EditPhase::Applied) {
                auto byte = [](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
                addColourValue = (byte(rgb.x) << 16) | (byte(rgb.y) << 8) | byte(rgb.z);
            }
            tooltip("The colour + adds (click to choose it)");
        }
        ImGui::SameLine();
        if (ImGui::Button("+##add")) {
            if (static_cast<int>(region.colors.size()) >= MAX_REGION_COLOURS) {
                setStatus("At most 16 colours per region.", false);
            } else if (std::find(region.colors.begin(), region.colors.end(), addColourValue) == region.colors.end()) {
                region.colors.push_back(addColourValue);
                lastEditedRegion = static_cast<int>(i);
                changed = true;
            }
        }
        tooltip("Add the colour beside it by value");
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight()));
        if (Widgets::iconButton("##remove", ICON_FA_TRASH, ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) removeAt = static_cast<int>(i);
        tooltip("Remove this region");
        auto regionSlider = [&](const char* label, const char* id, float& value, float lo, float hi, const char* format, const char* help) {
            rowLabel(label, help);
            ImGui::SetNextItemWidth(-FLT_MIN);
            float v = value;
            if (UIUtils::sliderFloatInput(id, &v, lo, hi, format)) {
                value = v;
                lastEditedRegion = static_cast<int>(i);
                changed = true;
            }
            tooltip(help);
        };
        regionSlider("Tolerance", "##tolerance", region.tolerance, 0.0f, 0.5f, "%.3f", "How close a colour must be (Oklab distance)");
        regionSlider("Detail", "##detailx", region.detail, 0.0f, 8.0f, "x %.2f", "0 keeps these colours out of the relief");
        regionSlider("Lift", "##lift", region.lift, -16.0f, 16.0f, "%.1f px", "Raise (+) or sink (-) these pixels");
        rowLabel("Flat", nullptr);
        if (ImGui::Checkbox("##flat", &region.flat)) {
            lastEditedRegion = static_cast<int>(i);
            changed = true;
        }
        tooltip("These pixels face the viewer");
        if (region.colors.empty()) subtle("No colour yet: it matches nothing.");
        ImGui::PopID();
    }
    if (removeAt >= 0) {
        settings.regions.erase(settings.regions.begin() + removeAt);
        if (pickingRegion == removeAt) pickingRegion = -1;
        else if (pickingRegion > removeAt) pickingRegion--;
        lastEditedRegion = settings.regions.empty() ? -1 : std::min(lastEditedRegion, static_cast<int>(settings.regions.size()) - 1);
        changed = true;
    }
    if (changed) {
        markChanged();
        storeSettings();
    }
}

void SpriteNormalsWindow::drawOutput() {
    if (current >= items.size()) return;
    Item& item = items[current];
    ImGui::SeparatorText("Output");
    const fs::path root = project->getProjectPath();
    if (isBatch()) {
        size_t maps = 0, exist = 0;
        std::vector<size_t> checked;
        for (size_t i = 0; i < items.size(); i++) {
            if (!items[i].checked) continue;
            maps++;
            checked.push_back(i);
        }
        for (size_t i : checked) {
            if (!existingOutputs({i}).empty()) exist++;
        }
        subtle(std::to_string(maps) + " normal map" + (maps == 1 ? "" : "s") + (exist ? " (" + std::to_string(exist) + " exist)" : std::string()) +
               ", each next to its image");
    } else {
        // the placeholder is the default name; empty means the default
        std::string defaultNormal;
        for (const sn::PlannedOutput& out : sn::defaultOutputs(item.texturePath, item.info.kind, false, false)) {
            if (out.role == "normal") defaultNormal = out.path;
        }
        rowLabel("Normal map", "Where the normal map is written (project-relative PNG)");
        outputText = item.spec.output.normal;
        if (inputString("##output", defaultNormal.c_str(), outputText, -FLT_MIN, true)) setOutput(outputText);
        UIProbe::item("spriteNormals.output");
        tooltip("Where the normal map is written (project-relative PNG)");
        ImGui::TextDisabled("Writes:");
        for (const sn::PlannedOutput& out : plannedOutputs()) {
            std::error_code ec;
            const bool exists = !out.path.empty() && fs::exists(absoluteOf(project, out.path), ec);
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::SubtleText);
            ImGui::Bullet();
            ImGui::TextUnformatted(out.path.c_str());
            ImGui::PopStyleColor();
            if (exists) {
                ImGui::SameLine();
                ImGui::TextColored(Theme::Colors::WarningText, "(exists)");
            }
        }
    }
    bool height = settings.height;
    if (ImGui::Checkbox("Height map##height", &height)) setHeightMap(height);
    tooltip("Also write a 16-bit grey height map (for other tools; the engine's 2D lights use the normal map)");
    if (!isBatch() && hasAssignTarget()) {
        bool on = assign;
        const std::string label = "Assign to \"" + target.name + "\"##assign";
        if (ImGui::Checkbox(label.c_str(), &on)) setAssign(on);
        UIProbe::item("spriteNormals.assign");
        tooltip("After saving, set it as this GameObject's Normal Texture (and copy the Base Texture's filter and wrap); for an animation "
                "also its texture, frames and Sprite Animation. One scene undo step.");
        if (target.submesh != 0) {
            const SceneProject* sp = project->getScene(target.sceneId);
            const bool tilemap = sp && sp->scene && sp->scene->findComponent<TilemapComponent>(target.entity);
            subtle("Material submesh " + std::to_string(target.submesh) + " of " + (tilemap ? "Tilemap" : "GameObject") + " \"" + target.name + "\"");
        }
    }
    for (const sn::Problem& p : item.problems) {
        if (p.field.rfind("output", 0) == 0) errorText(p.message);
    }
}

void SpriteNormalsWindow::drawWarnings() {
    // the rows not shown in their own section already (frames, settings, outputs)
    std::vector<size_t> rows;
    for (size_t i = 0; i < warnings.size(); i++) {
        const WarningRow& w = warnings[i];
        if (w.kind == "frames_mismatch" || w.kind == "invalid_settings" || w.kind == "output_invalid") continue;
        rows.push_back(i);
    }
    if (rows.empty()) return;
    ImGui::SeparatorText("Warnings");
    for (size_t i : rows) {
        const WarningRow& w = warnings[i];
        ImGui::PushID(static_cast<int>(i));
        if (w.error) errorText(w.text);
        else warningText(w.text);
        UIProbe::item("spriteNormals.warning." + w.kind);
        // the fixes on the next line, side by side (each one undoable)
        for (size_t f = 0; f < w.fixes.size(); f++) {
            const float width = ImGui::CalcTextSize(w.fixes[f].c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
            if (f > 0) {
                ImGui::SameLine();
                if (ImGui::GetContentRegionAvail().x < width) ImGui::NewLine();
            } else {
                ImGui::Indent(ImGui::GetFontSize());
            }
            if (ImGui::SmallButton((w.fixes[f] + "##fix" + std::to_string(f)).c_str())) {
                std::string error;
                const std::string kind = w.kind, fix = w.fixes[f];
                if (!applyFix(kind, fix, error)) setStatus(error, true);
                ImGui::Unindent(ImGui::GetFontSize());
                ImGui::PopID();
                return;   // the rows changed
            }
            UIProbe::item("spriteNormals.fix." + w.kind + "." + std::to_string(f));
        }
        if (!w.fixes.empty()) ImGui::Unindent(ImGui::GetFontSize());
        ImGui::PopID();
    }
}

void SpriteNormalsWindow::drawSettings() {
    drawNotices();
    if (items.empty()) {
        drawSource();
        subtle("Drop an image or a Sprite GameObject here, pick one with ..., or select a Sprite and use the arrow.");
        return;
    }
    if (isBatch()) drawBatch();
    else drawSource();
    drawFrames();
    drawPresetAndSliders();
    drawAdvanced();
    drawPaint();
    drawOutput();
    drawWarnings();
}

std::string SpriteNormalsWindow::primaryDisabledReason() const {
    if (items.empty()) return "Pick a source first";
    if (isBatch()) {
        if (saveAll.active) return "Saving: wait for the items to generate";
        bool any = false;
        for (const Item& item : items) {
            if (!item.checked) continue;
            any = true;
            if (!item.problems.empty() || !item.probeError.empty()) return "Fix the problems listed above first";
        }
        return any ? std::string() : "Check at least one image";
    }
    const Item& item = items[current];
    if (!item.problems.empty() || !item.probeError.empty()) return "Fix the problems listed above first";
    if (getAssign() && project->isAnyScenePlaying()) return "Stop the scene to assign the normal map";
    if (!hasResult()) return "Wait for the preview";
    if (isStale() || (isRunning() && runItem == current)) return "Updating: wait for the preview";
    if (paintSizeMismatch()) return paintDisabledReason();
    return std::string();
}

void SpriteNormalsWindow::primaryAction() {
    if (!primaryDisabledReason().empty()) return;
    requestSave();
}

void SpriteNormalsWindow::drawFooter() {
    ImGui::Separator();
    ImGui::Spacing();
    const float width = ImGui::GetContentRegionAvail().x;
    const float half = (width - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    const float height = ImGui::GetFrameHeight() * 1.4f;

    // primary: Save / Save & Assign / Save All (N)
    std::string label = ICON_FA_FLOPPY_DISK " Save";
    if (isBatch()) {
        size_t checked = 0;
        for (const Item& item : items) checked += item.checked ? 1 : 0;
        label = ICON_FA_FLOPPY_DISK " Save All (" + std::to_string(checked) + ")";
    } else if (getAssign()) {
        label = ICON_FA_FLOPPY_DISK " Save & Assign";
    }
    const std::string reason = primaryDisabledReason();
    ImGui::BeginDisabled(!reason.empty());
    // the accent only when it can be pressed: disabled, it looks like any disabled button
    if (reason.empty()) accentButtonColours();
    if (ImGui::Button((label + "##primary").c_str(), ImVec2(half, height))) primaryAction();
    if (reason.empty()) ImGui::PopStyleColor(3);
    ImGui::EndDisabled();
    UIProbe::item("spriteNormals.primary");
    reasonTooltip(!reason.empty() ? reason
                  : isBatch()     ? "Write one normal map per checked image, with their recipes, as one undo step (Ctrl+Enter)"
                  : getAssign()   ? "Write the normal map (one Project undo step) and set it on the GameObject (one Scene undo step) (Ctrl+Enter)"
                                  : "Write the normal map and its recipe (one Project undo step) (Ctrl+Enter)");
    ImGui::SameLine();
    // secondary: Cancel / Generate / Regenerate
    const bool busy = isRunning() || saveAll.active;
    const bool live = current < items.size() && liveAllowed(items[current]);
    if (busy) {
        if (ImGui::Button(ICON_FA_STOP " Cancel##secondary", ImVec2(half, height))) cancel();
        tooltip(saveAll.active ? "Stop Save All (nothing is written)" : "Stop this run (the preview keeps the last result)");
    } else {
        ImGui::BeginDisabled(items.empty());
        if (ImGui::Button(live ? ICON_FA_ROTATE " Regenerate##secondary" : ICON_FA_PLAY " Generate##secondary", ImVec2(half, height))) {
            std::string error;
            if (!regenerate(error)) setStatus(error, true);
        }
        ImGui::EndDisabled();
        tooltip(live ? "Run the generation again now (it runs by itself after every change)"
                     : "Generate the preview (this source is too large to regenerate on every change)");
    }
    UIProbe::item("spriteNormals.secondary");

    // strokes not in the file yet (Save writes them; closing asks)
    if (!isBatch() && hasUnsavedPaint()) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::WarningText);
        ImGui::TextWrapped(ICON_FA_PAINTBRUSH " Unsaved paint: Save writes it into the normal map.");
        ImGui::PopStyleColor();
        UIProbe::item("spriteNormals.unsavedPaint");
    }
    // progress, errors, status
    if (saveAll.active) {
        size_t total = saveAll.done + saveAll.queue.size();
        const size_t index = std::min(total, saveAll.done + 1);
        const float fraction = total ? static_cast<float>(saveAll.done) / static_cast<float>(total) : 0.0f;
        const std::string overlay = "Item " + std::to_string(index) + " of " + std::to_string(total);
        ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, 0), overlay.c_str());
    } else if (isRunning() && now() - runStartedAt > 0.3) {
        const sn::Progress p = run->progress();
        char overlay[32];
        std::snprintf(overlay, sizeof(overlay), "%.0f%%", p.fraction * 100.0);
        ImGui::ProgressBar(static_cast<float>(p.fraction), ImVec2(-FLT_MIN, 0), overlay);
    }
    for (const sn::Problem& p : runErrors) errorText(p.message);
    if (!statusMessage.empty()) {
        if (statusError) errorText(statusMessage);
        else subtle(statusMessage);
    }
}

// ====================================================================== preview

void SpriteNormalsWindow::drawToolbar() {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    auto next = [&](float w) {
        ImGui::SameLine();
        if (ImGui::GetCursorScreenPos().x + w > right) ImGui::NewLine();
    };
    auto radioWidth = [&](const char* label) { return ImGui::GetFrameHeight() + style.ItemInnerSpacing.x + ImGui::CalcTextSize(label).x; };
    auto buttonWidth = [&](const char* label) { return ImGui::CalcTextSize(label, nullptr, true).x + style.FramePadding.x * 2.0f; };
    const char* views[] = {"Color", "Normal", "Lit", "Split"};
    const char* viewTips[] = {"The source image (1)", "The normal map: red = right, green = up, blue = toward you (2)",
                              "The sprite lit by the light you drag, as the engine lights it (3)", "Color left, Lit right; drag the divider (4)"};
    for (int v = 0; v < 4; v++) {
        if (v) next(radioWidth(views[v]));
        if (ImGui::RadioButton(views[v], static_cast<int>(view) == v)) setView(static_cast<View>(v));
        UIProbe::item(std::string("spriteNormals.view.") + views[v]);
        // UI tests: which view is selected
        if (static_cast<int>(view) == v) UIProbe::item(std::string("spriteNormals.viewSelected.") + views[v]);
        tooltip(viewTips[v]);
    }
    const std::vector<sn::Cell> cells = shownCells();
    if (cells.size() > 1) {
        next(ImGui::GetFontSize() * 6.0f + ImGui::GetFrameHeight() * 2.0f + style.ItemSpacing.x * 4.0f);
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();
        const std::string preview = frame < 0 ? std::string("All") : "Frame " + std::to_string(frame + 1);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
        if (ImGui::BeginCombo("##frame", preview.c_str())) {
            if (ImGui::Selectable("All", frame < 0)) setFrame(-1);
            for (size_t i = 0; i < cells.size(); i++) {
                if (ImGui::Selectable(("Frame " + std::to_string(i + 1)).c_str(), frame == static_cast<int>(i))) setFrame(static_cast<int>(i));
            }
            ImGui::EndCombo();
        }
        UIProbe::item("spriteNormals.frame");
        tooltip("Show one frame, or the whole sheet (All)");
        ImGui::SameLine(0, style.ItemInnerSpacing.x);
        if (ImGui::ArrowButton("##prevFrame", ImGuiDir_Left)) setFrame(frame <= 0 ? static_cast<int>(cells.size()) - 1 : frame - 1);
        tooltip("Previous frame (,)");
        ImGui::SameLine(0, style.ItemInnerSpacing.x);
        if (ImGui::ArrowButton("##nextFrame", ImGuiDir_Right)) setFrame((frame + 1) % static_cast<int>(cells.size()));
        tooltip("Next frame (.)");
        next(ImGui::GetFrameHeight() * 1.3f);
        const bool wasPlaying = playing;
        if (wasPlaying) ImGui::PushStyleColor(ImGuiCol_Button, Theme::Colors::ButtonActivated);
        if (Widgets::iconButton("##play", playing ? ICON_FA_PAUSE : ICON_FA_PLAY, ImVec2(ImGui::GetFrameHeight() * 1.3f, ImGui::GetFrameHeight()))) setPlaying(!playing);
        if (wasPlaying) ImGui::PopStyleColor();
        UIProbe::item("spriteNormals.play");
        tooltip(playing ? "Pause (Space)" : "Play the frames with their delays (Space)");
    }
    // nothing to show yet: the views only
    const std::shared_ptr<sn::SpriteNormalsRun> result = shown();
    if (!result && colorRgba.empty()) return;
    next(buttonWidth("Orbit") + style.ItemSpacing.x);
    const bool wasOrbit = orbit;
    if (wasOrbit) ImGui::PushStyleColor(ImGuiCol_Button, Theme::Colors::ButtonActivated);
    if (ImGui::Button("Orbit")) setOrbit(!orbit);
    if (wasOrbit) ImGui::PopStyleColor();
    UIProbe::item("spriteNormals.orbit");
    tooltip("Circle the light around the sprite, one turn every 4 s (O)");
    if (!cells.empty()) {
        next(radioWidth("Grid"));
        if (ImGui::Checkbox("Grid", &grid)) storeSettings();
        UIProbe::item("spriteNormals.grid");
        tooltip("Outline the frames in the All view (G)");
    }
    char zoomText[32];
    std::snprintf(zoomText, sizeof(zoomText), "%.0f%%", zoom * 100.0f);
    next(buttonWidth("Fit") + style.ItemSpacing.x + ImGui::CalcTextSize("6400%").x);
    if (ImGui::Button("Fit")) fitRequested = true;
    UIProbe::item("spriteNormals.fit");
    tooltip("Fit the view (F, or a double click); 0: 100%. Mouse wheel zooms, right or middle drag pans");
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", zoomText);

    // the light (Lit and Split, once there is a result to light)
    if ((view == View::Lit || view == View::Split) && result) {
        ensureLight();
        const float field = ImGui::GetFontSize() * 5.0f;
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled(ICON_FA_LIGHTBULB);
        ImGui::SameLine();
        ImGui::TextUnformatted("Height");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(field);
        float h = light.height;
        if (ImGui::DragFloat("##lightHeight", &h, 0.5f, 1.0f, 4096.0f, "%.0f px", ImGuiSliderFlags_AlwaysClamp)) {
            light.height = h;
            light.userHeight = true;
            litDirty = true;
            storeSettings();
        }
        UIProbe::item("spriteNormals.lightHeight");
        tooltip("Height of the light above the sprite, in pixels (the Light 2D's Height). Mouse wheel over the canvas with Shift changes it");
        next(ImGui::GetFrameHeight() + ImGui::CalcTextSize("Colour").x + style.ItemSpacing.x);
        ImGui::TextUnformatted("Colour");
        ImGui::SameLine();
        {
            // the preview light's colour as shown (ui::colorField: the Color window)
            Vector3 c = light.colour;
            ui::ColorFieldOptions lightOptions;
            lightOptions.alpha = false;
            lightOptions.linearStorage = false;
            lightOptions.label = "Light Colour";
            lightOptions.width = ImGui::GetFrameHeight();
            const ui::EditPhase phase = ui::colorField("##lightColour", c, lightOptions);
            if (phase == ui::EditPhase::Changing || phase == ui::EditPhase::Cancelled || phase == ui::EditPhase::Applied) setLightColour(c);
        }
        tooltip("The light's colour");
        next(field + ImGui::CalcTextSize("Ambient").x + style.ItemSpacing.x);
        ImGui::TextUnformatted("Ambient");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(field);
        float a = light.ambient;
        if (ImGui::SliderFloat("##ambient", &a, 0.0f, 1.0f, "%.2f")) setAmbient(a);
        tooltip("Light everywhere, like the scene's 2D ambient");
        next(field + ImGui::CalcTextSize("Range").x + style.ItemSpacing.x);
        ImGui::TextUnformatted("Range");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(field);
        float range = light.range;
        if (ImGui::DragFloat("##range", &range, 1.0f, 1.0f, 8192.0f, "%.0f px", ImGuiSliderFlags_AlwaysClamp)) {
            light.range = range;
            litDirty = true;
        }
        tooltip("How far the light reaches, in pixels (the Light 2D's Range)");
        const float stepsField = ImGui::GetFontSize() * 6.0f;   // "Scene (16)"
        next(stepsField + ImGui::CalcTextSize("Light Steps").x + style.ItemSpacing.x);
        ImGui::TextUnformatted("Light Steps");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(stepsField);
        const int sceneSteps = effectiveLightSteps();
        auto stepsName = [](int n) { return n > 0 ? std::to_string(n) : std::string("Off"); };
        const std::string stepsText = lightSteps == kSceneLightSteps ? "Scene (" + stepsName(sceneSteps) + ")" : stepsName(lightSteps);
        if (ImGui::BeginCombo("##lightSteps", stepsText.c_str())) {
            if (ImGui::Selectable(("Scene (" + stepsName(sceneSteps) + ")").c_str(), lightSteps == kSceneLightSteps) && lightSteps != kSceneLightSteps) {
                setLightSteps(kSceneLightSteps);
            }
            UIProbe::item("spriteNormals.lightSteps.Scene");
            for (int n = 0; n <= sn::kMaxLightSteps; n++) {
                if (ImGui::Selectable(stepsName(n).c_str(), n == lightSteps) && n != lightSteps) setLightSteps(n);
                UIProbe::item("spriteNormals.lightSteps." + stepsName(n));
            }
            ImGui::EndCombo();
        }
        UIProbe::item("spriteNormals.lightSteps");
        tooltip("Light in flat bands, as the scene's Light Steps draw it: each light's brightness rounded up to N steps (Scene: the scene's own setting; Off: smooth)");
        next(buttonWidth("Reset light"));
        if (ImGui::SmallButton("Reset light")) {
            project->getSpriteNormalsSettings().lightHeight = 0.0f;
            light.placed = false;
            orbit = false;
            defaultLight();
            storeSettings();
        }
        UIProbe::item("spriteNormals.resetLight");
        tooltip("The light back up-left of the sprite at its default height");
    }
}

void SpriteNormalsWindow::drawCanvas(const ImVec2& size) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 area(std::max(size.x, 1.0f), std::max(size.y, 1.0f));
    ImGui::InvisibleButton("##canvas", area, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    UIProbe::item("spriteNormals.canvas");
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const ImVec2 end(origin.x + area.x, origin.y + area.y);
    canvasSize = area;
    draw->AddRectFilled(origin, end, IM_COL32(28, 28, 28, 255));
    draw->PushClipRect(origin, end, true);

    const std::shared_ptr<sn::SpriteNormalsRun> r = shown();
    int cx = 0, cy = 0, cw = 0, ch = 0;
    int imageW = 0, imageH = 0;
    if (r) {
        cellRect(frame, cx, cy, cw, ch);
        imageW = r->result().normal.width;
        imageH = r->result().normal.height;
    } else if (!colorRgba.empty()) {
        cw = imageW = colorRgba.width;
        ch = imageH = colorRgba.height;
    }

    const ImU32 labelColour = IM_COL32(220, 220, 220, 210);
    const float pad = Theme::dpi(6.0f);
    if (cw <= 0 || ch <= 0) {
        const char* text = items.empty() ? "Pick a source image (or right-click an image in the Project window > Generate Normal Map...)"
                         : isRunning() || changedAt >= 0.0 ? "Updating..."
                         : (current < items.size() && !liveAllowed(items[current])) ? "Generate to see the preview"
                                                                                     : "Nothing to show yet";
        const float wrap = std::max(Theme::dpi(100.0f), area.x - Theme::dpi(40.0f));
        const ImVec2 t = ImGui::CalcTextSize(text, nullptr, false, wrap);
        draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(origin.x + (area.x - t.x) * 0.5f, origin.y + (area.y - t.y) * 0.5f),
                      IM_COL32(150, 150, 150, 255), text, nullptr, wrap);
        draw->PopClipRect();
        return;
    }

    // zoom: screen pixels per image pixel; Fit = the largest whole number that fits
    auto fitZoom = [&]() {
        // a margin wide enough for the light (it starts up-left of the sprite, at its corner)
        const float margin = Theme::dpi(56.0f);
        const float fit = std::min((area.x - margin) / cw, (area.y - margin) / ch);
        return fit >= 1.0f ? std::floor(fit) : std::max(0.05f, fit);
    };
    if (fitRequested || zoom <= 0.0f) {
        zoom = fitZoom();
        pan = ImVec2(0, 0);
        fitRequested = false;
    }
    const ImVec2 imgMin(std::floor(origin.x + (area.x - cw * zoom) * 0.5f + pan.x), std::floor(origin.y + (area.y - ch * zoom) * 0.5f + pan.y));
    const ImVec2 imgMax(imgMin.x + cw * zoom, imgMin.y + ch * zoom);
    // what of the region is on screen (All view of a large image renders only that)
    visibleX0 = cx + std::clamp(static_cast<int>(std::floor((origin.x - imgMin.x) / zoom)), 0, cw);
    visibleY0 = cy + std::clamp(static_cast<int>(std::floor((origin.y - imgMin.y) / zoom)), 0, ch);
    visibleX1 = cx + std::clamp(static_cast<int>(std::ceil((end.x - imgMin.x) / zoom)), 0, cw);
    visibleY1 = cy + std::clamp(static_cast<int>(std::ceil((end.y - imgMin.y) / zoom)), 0, ch);

    // the checkerboard under the image (Image Viewer pattern), so alpha shows
    {
        const ImVec2 vMin(std::max(imgMin.x, origin.x), std::max(imgMin.y, origin.y));
        const ImVec2 vMax(std::min(imgMax.x, end.x), std::min(imgMax.y, end.y));
        if (vMin.x < vMax.x && vMin.y < vMax.y) {
            const float checker = Theme::dpi(8.0f);
            const int x0 = std::max(0, static_cast<int>(std::floor((vMin.x - imgMin.x) / checker)));
            const int y0 = std::max(0, static_cast<int>(std::floor((vMin.y - imgMin.y) / checker)));
            const int x1 = static_cast<int>(std::ceil((vMax.x - imgMin.x) / checker));
            const int y1 = static_cast<int>(std::ceil((vMax.y - imgMin.y) / checker));
            draw->AddRectFilled(vMin, vMax, IM_COL32(105, 105, 105, 255));
            for (int y = y0; y < y1; y++) {
                for (int x = x0; x < x1; x++) {
                    if ((x + y) % 2 != 0) continue;
                    const ImVec2 a(imgMin.x + x * checker, imgMin.y + y * checker);
                    draw->AddRectFilled(ImVec2(std::max(a.x, vMin.x), std::max(a.y, vMin.y)),
                                        ImVec2(std::min(a.x + checker, vMax.x), std::min(a.y + checker, vMax.y)), IM_COL32(135, 135, 135, 255));
                }
            }
        }
    }

    // the views (UVs crop the shown cell out of the whole images)
    const ImVec2 uv0(imageW ? static_cast<float>(cx) / imageW : 0.0f, imageH ? static_cast<float>(cy) / imageH : 0.0f);
    const ImVec2 uv1(imageW ? static_cast<float>(cx + cw) / imageW : 1.0f, imageH ? static_cast<float>(cy + ch) / imageH : 1.0f);
    auto drawColour = [&]() {
        if (colorGpu.valid()) draw->AddImage(Backend::getImGuiTexture(colorGpu.texture.getRender()), imgMin, imgMax, uv0, uv1);
    };
    auto drawNormal = [&]() {
        if (normalGpu.valid()) draw->AddImage(Backend::getImGuiTexture(normalGpu.texture.getRender()), imgMin, imgMax, uv0, uv1);
        else drawColour();
    };
    auto drawLit = [&]() {
        if (litGpu.valid()) {
            const ImVec2 a(imgMin.x + litOriginX * zoom, imgMin.y + litOriginY * zoom);
            draw->AddImage(Backend::getImGuiTexture(litGpu.texture.getRender()), a, ImVec2(a.x + litGpu.width * zoom, a.y + litGpu.height * zoom));
        } else {
            drawColour();
        }
    };
    const float dividerX = origin.x + area.x * splitPosition;
    switch (view) {
        case View::Color: drawColour(); break;
        case View::Normal: drawNormal(); break;
        case View::Lit: drawLit(); break;
        case View::Split:
            draw->PushClipRect(origin, ImVec2(dividerX, end.y), true);
            drawColour();
            draw->PopClipRect();
            draw->PushClipRect(ImVec2(dividerX, origin.y), end, true);
            drawLit();
            draw->PopClipRect();
            break;
    }
    draw->AddRect(ImVec2(imgMin.x - 1, imgMin.y - 1), ImVec2(imgMax.x + 1, imgMax.y + 1), IM_COL32(90, 90, 90, 255));

    // the frame grid (All view): the Texture Slicer's green rects and indices
    const std::vector<sn::Cell> cells = shownCells();
    if (grid && frame < 0 && cells.size() > 1) {
        for (size_t i = 0; i < cells.size(); i++) {
            const sn::Cell& c = cells[i];
            const ImVec2 a(imgMin.x + (c.x - cx) * zoom, imgMin.y + (c.y - cy) * zoom);
            const ImVec2 b(a.x + c.width * zoom, a.y + c.height * zoom);
            draw->AddRect(a, b, IM_COL32(100, 200, 100, 220));
            if (b.x - a.x > Theme::dpi(16.0f) && b.y - a.y > Theme::dpi(12.0f)) {
                const std::string index = std::to_string(i);
                draw->AddText(ImVec2(a.x + Theme::dpi(3.0f), a.y + Theme::dpi(2.0f)), IM_COL32(255, 255, 255, 200), index.c_str());
            }
        }
    }

    ImGuiIO& io = ImGui::GetIO();
    const bool lit = view == View::Lit || view == View::Split;
    // a paint tool owns left-drag, Alt+click and Shift+wheel (Navigate keeps the light and the eyedropper)
    const bool paintInput = painting() && r;
    const bool picking = !paintInput && (pickingRegion >= 0 || io.KeyAlt);
    const ImVec2 pixel((io.MousePos.x - imgMin.x) / zoom, (io.MousePos.y - imgMin.y) / zoom);
    const int imagePx = cx + static_cast<int>(std::floor(pixel.x)), imagePy = cy + static_cast<int>(std::floor(pixel.y));
    const bool overImage = pixel.x >= 0 && pixel.y >= 0 && pixel.x < cw && pixel.y < ch;
    drawPaintOverlays(draw, imgMin, zoom, cx, cy, cw, ch, pixel, hovered);
    // the light: a disc with a soft halo
    if (lit && r) {
        ensureLight();
        const float radius = Theme::dpi(5.0f);
        // kept in view (dragged or zoomed past the edge it stays grabbable at the border)
        const float inset = radius + Theme::dpi(3.0f);
        const ImVec2 p(std::clamp(imgMin.x + light.x * zoom, origin.x + inset, end.x - inset), std::clamp(imgMin.y + light.y * zoom, origin.y + inset, end.y - inset));
        for (int ring = 4; ring >= 1; ring--) draw->AddCircleFilled(p, radius + ring * Theme::dpi(3.0f), IM_COL32(255, 230, 150, 18 * (5 - ring)), 32);
        const ImU32 fill = ImGui::ColorConvertFloat4ToU32(ImVec4(std::min(1.0f, light.colour.x), std::min(1.0f, light.colour.y), std::min(1.0f, light.colour.z), 1.0f));
        draw->AddCircleFilled(p, radius, fill, 24);
        draw->AddCircle(p, radius, IM_COL32(40, 40, 40, 230), 24, 1.5f);
        UIProbe::rect("spriteNormals.light", p.x - radius, p.y - radius, p.x + radius, p.y + radius);
    }
    // the Split divider (2 px, accent; its handle 8 px wide)
    bool overDivider = false;
    if (view == View::Split) {
        const ImVec4 accent = Theme::Colors::Selection;
        draw->AddRectFilled(ImVec2(dividerX - Theme::dpi(1.0f), origin.y), ImVec2(dividerX + Theme::dpi(1.0f), end.y),
                            ImGui::ColorConvertFloat4ToU32(ImVec4(accent.x * 1.6f, accent.y * 1.6f, accent.z * 1.6f, 1.0f)));
        overDivider = hovered && std::fabs(io.MousePos.x - dividerX) <= Theme::dpi(4.0f);
        UIProbe::rect("spriteNormals.divider", dividerX - Theme::dpi(4.0f), origin.y, dividerX + Theme::dpi(4.0f), end.y);
        if (overDivider || draggingSplit) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }

    // ---- input
    float wheel = io.MouseWheel;
    if (wheel == 0.0f && io.KeyShift) wheel = io.MouseWheelH;
    if (hovered && wheel != 0.0f) {
        if (io.KeyShift && paintInput) {
            // the brush size: 1 px a notch, 4 with Ctrl
            sn::PaintBrush b = brush;
            b.size += (wheel > 0 ? 1 : -1) * (io.KeyCtrl ? 4 : 1);
            setPaintBrush(b);
        } else if (io.KeyShift && lit) {
            light.height = std::clamp(light.height * (wheel > 0 ? 1.1f : 1.0f / 1.1f), 1.0f, 4096.0f);
            light.userHeight = true;
            litDirty = true;
            storeSettings();
        } else {
            // the next step about the cursor
            const int steps = static_cast<int>(sizeof(ZOOM_STEPS) / sizeof(ZOOM_STEPS[0]));
            float nextZoom = zoom;
            if (wheel > 0) {
                for (int i = 0; i < steps; i++) {
                    if (ZOOM_STEPS[i] > zoom + 1e-4f) {
                        nextZoom = ZOOM_STEPS[i];
                        break;
                    }
                }
            } else {
                for (int i = steps - 1; i >= 0; i--) {
                    if (ZOOM_STEPS[i] < zoom - 1e-4f) {
                        nextZoom = ZOOM_STEPS[i];
                        break;
                    }
                }
            }
            if (nextZoom != zoom) {
                const ImVec2 centre(origin.x + area.x * 0.5f + pan.x, origin.y + area.y * 0.5f + pan.y);
                const ImVec2 rel(io.MousePos.x - centre.x, io.MousePos.y - centre.y);
                const float applied = nextZoom / zoom;
                pan.x -= rel.x * (applied - 1.0f);
                pan.y -= rel.y * (applied - 1.0f);
                zoom = nextZoom;
            }
        }
    }
    if (active && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) || ImGui::IsMouseDragging(ImGuiMouseButton_Right))) {
        pan.x += io.MouseDelta.x;
        pan.y += io.MouseDelta.y;
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !picking && !paintInput) {
        fitRequested = true;
    } else if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (paintInput && !overDivider) {
            if (!overImage) {
                // outside the image: nothing to paint (a stroke may still start there and enter it)
                if (!io.KeyAlt && brush.fill == sn::PaintBrush::Fill::Brush) {
                    beginStroke();
                    strokeTo(imagePx, imagePy);
                }
            } else if (io.KeyAlt) {
                // sample the composited normal into the direction (a transparent pixel: nothing)
                if (samplePaintDirection(imagePx, imagePy)) setStatus("Direction sampled from the pixel " + std::to_string(imagePx) + ", " + std::to_string(imagePy) + ".", false);
            } else if (brush.fill == sn::PaintBrush::Fill::Wand) {
                wandFill(imagePx, imagePy);
            } else {
                beginStroke();
                strokeTo(imagePx, imagePy);
            }
        } else if (picking) {
            // the eyedropper: the clicked source pixel (the shown cell's offset added)
            const int px = cx + static_cast<int>(std::floor(pixel.x)), py = cy + static_cast<int>(std::floor(pixel.y));
            std::string error;
            if (pixel.x < 0 || pixel.y < 0 || pixel.x >= cw || pixel.y >= ch) {
                setStatus("Click inside the image to pick a colour.", false);
            } else if (!pickRegionColour(pickingRegion >= 0 ? pickingRegion : -1, px, py, error)) {
                setStatus(error, false);
            } else {
                setStatus("Added " + sn::colorText(settings.regions[static_cast<size_t>(lastEditedRegion)].colors.back()) + " to region " +
                              std::to_string(lastEditedRegion + 1) + ".",
                          false);
            }
        } else if (overDivider) {
            draggingSplit = true;
        } else if (lit && r && !paintInput) {
            draggingLight = true;
            orbit = false;
        }
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        draggingLight = false;
        draggingSplit = false;
        endStroke();
    } else if (stroking) {
        strokeTo(imagePx, imagePy);
    }
    if (draggingSplit) {
        splitPosition = std::clamp((io.MousePos.x - origin.x) / area.x, 0.02f, 0.98f);
    } else if (draggingLight) {
        light.x = pixel.x;
        light.y = pixel.y;
        light.placed = true;
        litDirty = true;
    } else if (active && !picking && !paintInput && !lit && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        pan.x += io.MouseDelta.x;
        pan.y += io.MouseDelta.y;
    }
    if (hovered && picking && pixel.x >= 0 && pixel.y >= 0 && pixel.x < cw && pixel.y < ch) {
        // a crosshair over the image
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
        const ImVec2 m = io.MousePos;
        const float arm = Theme::dpi(8.0f);
        draw->AddLine(ImVec2(m.x - arm, m.y), ImVec2(m.x + arm, m.y), IM_COL32(0, 0, 0, 255), 3.0f);
        draw->AddLine(ImVec2(m.x, m.y - arm), ImVec2(m.x, m.y + arm), IM_COL32(0, 0, 0, 255), 3.0f);
        draw->AddLine(ImVec2(m.x - arm, m.y), ImVec2(m.x + arm, m.y), IM_COL32(255, 255, 255, 255), 1.0f);
        draw->AddLine(ImVec2(m.x, m.y - arm), ImVec2(m.x, m.y + arm), IM_COL32(255, 255, 255, 255), 1.0f);
    } else if (hovered && paintInput && overImage) {
        // the tool and its size beside the cursor
        static const char* names[] = {"", "Angle", "Flatten", "Smooth", "Erase"};
        const std::string hint = std::string(names[static_cast<int>(paintTool)]) +
                                 (brush.fill == sn::PaintBrush::Fill::Wand ? " wand: click" : " " + std::to_string(brush.size) + " px") + (brush.mirrorX ? ", mirror X" : "");
        draw->AddText(ImVec2(io.MousePos.x + Theme::dpi(14.0f), io.MousePos.y + Theme::dpi(14.0f)), IM_COL32(230, 230, 230, stroking ? 120 : 200), hint.c_str());
    } else if (hovered && lit && r && !overDivider && !draggingSplit && !paintInput) {
        const char* hint = ICON_FA_LIGHTBULB " Drag: move the light";
        draw->AddText(ImVec2(io.MousePos.x + Theme::dpi(14.0f), io.MousePos.y + Theme::dpi(14.0f)), IM_COL32(230, 230, 230, draggingLight ? 120 : 190), hint);
    }

    // ---- labels in the corner
    std::string label = viewName(view);
    if (view == View::Split) label = "Color | Lit";
    if (frame >= 0) label += "   Frame " + std::to_string(frame + 1) + " / " + std::to_string(cells.size());
    draw->AddRectFilled(ImVec2(origin.x + pad - 3, origin.y + pad - 2), ImVec2(origin.x + pad + ImGui::CalcTextSize(label.c_str()).x + 3, origin.y + pad + ImGui::GetFontSize() + 2),
                        IM_COL32(0, 0, 0, 120), 3.0f);
    draw->AddText(ImVec2(origin.x + pad, origin.y + pad), labelColour, label.c_str());
    std::string state;
    ImU32 stateColour = IM_COL32(255, 210, 90, 230);
    if (isRunning() || (changedAt >= 0.0 && current < items.size() && liveAllowed(items[current]))) {
        state = "Updating...";
    } else if (isStale()) {
        state = current < items.size() && !liveAllowed(items[current]) ? "Settings changed: Generate to update" : "Settings changed";
    } else if (!r) {
        state = current < items.size() && !liveAllowed(items[current]) ? "Generate to see the result" : "";
        stateColour = IM_COL32(200, 200, 200, 220);
    }
    if (!state.empty()) {
        const ImVec2 t = ImGui::CalcTextSize(state.c_str());
        const ImVec2 at(end.x - pad - t.x, origin.y + pad);
        draw->AddRectFilled(ImVec2(at.x - 3, at.y - 2), ImVec2(at.x + t.x + 3, at.y + t.y + 2), IM_COL32(0, 0, 0, 140), 3.0f);
        draw->AddText(at, stateColour, state.c_str());
    }
    if (pickingRegion >= 0) {
        const std::string text = ICON_FA_EYE_DROPPER " Picking colours for region " + std::to_string(pickingRegion + 1) + ": click pixels, Esc ends";
        draw->AddText(ImVec2(origin.x + pad, end.y - pad - ImGui::GetFontSize()), IM_COL32(255, 230, 150, 230), text.c_str());
    }
    draw->PopClipRect();
}

void SpriteNormalsWindow::drawMetrics() {
    const std::shared_ptr<sn::SpriteNormalsRun> r = shown();
    if (!r) {
        if (!items.empty()) subtle("Nothing is written until Save.");
        return;
    }
    // (while a stroke is drawn the line waits for its end: measuring a large atlas per mouse sample is too slow)
    if (metricsSerial != resultSerial || metricsResult != shownId() || (!stroking && metricsPaintSerial != paintSerial * 2 + (showPaint ? 1 : 0))) {
        metricsSerial = resultSerial;
        metricsResult = shownId();
        metricsPaintSerial = paintSerial * 2 + (showPaint ? 1 : 0);
        const sn::Result& res = r->result();
        const sn::Settings& used = r->prepared().request.settings;
        const std::vector<sn::Cell>& cells = r->prepared().request.cells;
        // the metrics of what is shown (with the paint unless Show paint is off)
        const bool painted = showPaint && paint.sized(res.normal.width, res.normal.height);
        const sn::Metrics m = sn::measure(displayNormal(), cells, used.directx, used.alphaThreshold, painted ? &paint : nullptr,
                                          res.shape.empty() ? nullptr : &res.shape, workerThreads());
        metricsAssessment = sn::assess(used, m, static_cast<int>(std::max<size_t>(1, cells.size())), &res);
        std::string radius = res.radiusPerIsland && res.radiusMax > res.radiusMin + 1e-3f
                                 ? "radius " + trimmed(res.radiusMin, 1) + "-" + trimmed(res.radiusMax, 1) + " px"
                                 : "radius " + trimmed(std::max(res.radiusMin, res.radiusMax), 1) + " px";
        metricsLine = std::to_string(res.normal.width) + " x " + std::to_string(res.normal.height) + ", " +
                      std::to_string(std::max<size_t>(1, cells.size())) + (cells.size() > 1 ? " frames, " : " frame, ") + radius;
        for (const std::string& line : metricsAssessment.lines) metricsLine += "  |  " + line;
    }
    ImGui::TextWrapped("%s", metricsLine.c_str());
    // (boxy and gaps_unmatched are rows of the Warnings section, with their fixes)
    for (const sn::Problem& w : metricsAssessment.warnings) {
        if (w.kind == "boxy") continue;
        warningText(w.fix);
        break;
    }
    for (const sn::Problem& w : r->result().warnings) {
        if (w.kind != "gaps_unmatched") warningText(w.message);
    }
    if (!lastWritten.empty()) {
        subtle("Saved: " + lastWritten.front() + (lastWritten.size() > 1 ? " (+" + std::to_string(lastWritten.size() - 1) + " more)" : std::string()));
    }
}

void SpriteNormalsWindow::drawPreview() {
    drawToolbar();
    const float metricsHeight = ImGui::GetTextLineHeightWithSpacing() * 3.2f;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    avail.y = std::max(avail.y - metricsHeight, Theme::dpi(80.0f));
    drawCanvas(avail);
    drawMetrics();
}

// ====================================================================== paint

namespace {

    constexpr size_t PAINT_UNDO_STEPS = 256;

    sn::PaintOpKind kindOf(sn::PaintTool tool) {
        switch (tool) {
            case sn::PaintTool::Angle: return sn::PaintOpKind::Angle;
            case sn::PaintTool::Smooth: return sn::PaintOpKind::Smooth;
            case sn::PaintTool::Erase: return sn::PaintOpKind::Erase;
            default: return sn::PaintOpKind::Flatten;
        }
    }

    std::array<uint8_t, 4> overlayPixel(const sn::PaintOverlay& o, int i) {
        const uint8_t* p = &o.rgba[static_cast<size_t>(i) * 4];
        return {p[0], p[1], p[2], p[3]};
    }

}

void SpriteNormalsWindow::paintChanged() {
    paintSerial++;
    litDirty = true;
    warningsDirty = true;
    dirtyAll = true;
}

void SpriteNormalsWindow::paintChangedRect(int x0, int y0, int x1, int y1) {
    if (x0 >= x1 || y0 >= y1) return;
    paintSerial++;
    warningsDirty = true;
    auto grow = [&](int& ax0, int& ay0, int& ax1, int& ay1) {
        if (ax0 >= ax1 || ay0 >= ay1) {
            ax0 = x0, ay0 = y0, ax1 = x1, ay1 = y1;
            return;
        }
        ax0 = std::min(ax0, x0), ay0 = std::min(ay0, y0), ax1 = std::max(ax1, x1), ay1 = std::max(ay1, y1);
    };
    // the composite shown and the lit preview: only these pixels again
    grow(dirtyX0, dirtyY0, dirtyX1, dirtyY1);
    grow(litDirtyX0, litDirtyY0, litDirtyX1, litDirtyY1);
}

void SpriteNormalsWindow::paintChangedPixels(const std::vector<int>& pixels) {
    if (pixels.empty() || paint.width <= 0) return;
    int x0 = paint.width, y0 = paint.height, x1 = 0, y1 = 0;
    for (int i : pixels) {
        x0 = std::min(x0, i % paint.width);
        y0 = std::min(y0, i / paint.width);
        x1 = std::max(x1, i % paint.width + 1);
        y1 = std::max(y1, i / paint.width + 1);
    }
    paintChangedRect(x0, y0, x1, y1);
}

bool SpriteNormalsWindow::paintDiscardOk(const std::function<void()>& retry) {
    if (!hasUnsavedPaint()) return true;
    if (!confirmPaintDiscard) {
        discardPaint();
        return true;
    }
    confirm("Discard Paint Changes?",
            "The paint strokes made since the last Save are not in " + (paintTarget.empty() ? std::string("the normal map") : fileName(paintTarget)) +
                " yet.\n\nDiscard them and open the other one?",
            [this, retry]() {
                discardPaint();
                retry();
            });
    return false;
}

fs::path SpriteNormalsWindow::plannedNormalFor(const Item& item, const std::string& output) const {
    sn::Spec spec = specOf(item);
    spec.output.normal = output;
    if (spec.hasSprite()) spec.source = item.texturePath;
    if (spec.source.empty()) return fs::path();
    for (const sn::PlannedOutput& out : sn::plannedOutputs(project->getProjectPath(), spec, item.info.kind)) {
        if (out.role == "normal" && !out.path.empty()) return absoluteOf(project, out.path);
    }
    return fs::path();
}

void SpriteNormalsWindow::recordPaintStamp() {
    std::error_code ec;
    const fs::path target = fs::u8path(paintTarget);
    paintStampExists = !paintTarget.empty() && fs::is_regular_file(target, ec);
    paintStampTime = paintStampExists ? fs::last_write_time(target, ec) : fs::file_time_type{};
    paintStampSize = paintStampExists ? fs::file_size(target, ec) : 0;
}

void SpriteNormalsWindow::checkPaintTarget() {
    if (paintTarget.empty() || isBatch()) return;
    const double t = now();
    if (paintStampCheckedAt >= 0.0 && t - paintStampCheckedAt < 0.5 && t >= paintStampCheckedAt) return;
    paintStampCheckedAt = t;
    std::error_code ec;
    const fs::path target = fs::u8path(paintTarget);
    const bool exists = fs::is_regular_file(target, ec);
    const fs::file_time_type time = exists ? fs::last_write_time(target, ec) : fs::file_time_type{};
    const uintmax_t size = exists ? fs::file_size(target, ec) : 0;
    if (exists == paintStampExists && time == paintStampTime && size == paintStampSize) return;
    // the file changed under the window (a Save undone, a regeneration, another tool): what it
    // holds now is the saved state. Without strokes in the window (none since the target was
    // loaded, or all undone) the window takes it as its overlay too; with strokes (an undone Save
    // included, and strokes whose undo patches the cap dropped) the overlay stays, unsaved
    const bool hadUnsaved = hasUnsavedPaint();
    std::string why;
    std::optional<sn::PaintOverlay> onDisk = exists ? sn::readPaint(target, &why) : std::nullopt;
    paintSaved = onDisk ? std::move(*onDisk) : sn::PaintOverlay();
    recordPaintStamp();
    unsavedFor = 0;
    warningsDirty = true;
    if (paintStrokes == 0 && !stroking) {
        paint = paintSaved;
        paintRedo.clear();
        paintProblem = why.empty() ? std::string() : fileName(paintTarget) + "'s paint layer can't be read (" + why + "); Save writes the map without it.";
        paintChanged();
        return;
    }
    if (hasUnsavedPaint() || hadUnsaved) {
        setStatus(fileName(paintTarget) + (exists ? " changed on disk (regenerated, or its Save undone)" : " is gone (its Save undone)") +
                      " while this window has unsaved paint: Save writes this window's result and paint" + (exists ? " over it." : "."),
                  false);
    }
}

uint64_t SpriteNormalsWindow::shownId() const {
    return shown() ? items[current].resultId : 0;
}

const sn::PaintTopology* SpriteNormalsWindow::topologyFor(bool needed) {
    const std::shared_ptr<sn::SpriteNormalsRun> r = shown();
    if (!r || !needed) return nullptr;
    if (topologyResult != shownId()) {
        topologyResult = shownId();
        topology = sn::paintTopology(r->prepared().request.color, r->prepared().request.cells, r->prepared().request.settings.alphaThreshold);
    }
    return &topology;
}

void SpriteNormalsWindow::loadPaintForTarget() {
    endStroke();
    paintUndo.clear();
    paintRedo.clear();
    paintUndoBytes = 0;
    paintStrokes = 0;
    paint = sn::PaintOverlay();
    paintSaved = sn::PaintOverlay();
    paintTarget.clear();
    paintProblem.clear();
    paintOpenSet = false;
    if (!isBatch() && current < items.size()) {
        for (const sn::PlannedOutput& out : plannedOutputs()) {
            if (out.role != "normal" || out.path.empty()) continue;
            const fs::path abs = absoluteOf(project, out.path);
            paintTarget = jasperengine::editor::FileUtils::pathToGenericUtf8(abs);
            std::error_code ec;
            if (!fs::is_regular_file(abs, ec)) break;
            std::string why;
            if (std::optional<sn::PaintOverlay> loaded = sn::readPaint(abs, &why)) {
                paint = std::move(*loaded);
            } else if (!why.empty()) {
                paintProblem = fileName(out.path) + "'s paint layer can't be read (" + why + "); Save writes the map without it.";
            }
            break;
        }
    }
    paintSaved = paint;
    recordPaintStamp();
    if (!paint.empty()) {
        paintOpen = true;
        paintOpenSet = true;
    }
    paintChanged();
}

bool SpriteNormalsWindow::paintSizeMismatch() const {
    const std::shared_ptr<sn::SpriteNormalsRun> r = shown();
    if (!r || isBatch() || paint.empty()) return false;
    return !paint.sized(r->result().normal.width, r->result().normal.height);
}

std::string SpriteNormalsWindow::paintDisabledReason() const {
    if (items.empty()) return "Pick a source first";
    if (isBatch()) return "Paint one image at a time (batch mode generates only)";
    if (!shown()) return "Generate first";
    if (paintSizeMismatch()) {
        const sn::Image& n = shown()->result().normal;
        return "The loaded paint is " + std::to_string(paint.width) + "x" + std::to_string(paint.height) + " but the map is " + std::to_string(n.width) +
               "x" + std::to_string(n.height) + ": Clear Paint first";
    }
    return std::string();
}

bool SpriteNormalsWindow::hasUnsavedPaint() const {
    if (unsavedFor != paintSerial) {
        unsavedFor = paintSerial;
        unsavedCached = !(paint.empty() && paintSaved.empty()) && paint != paintSaved;
    }
    return unsavedCached;
}

void SpriteNormalsWindow::discardPaint() {
    endStroke();
    paint = paintSaved;
    paintUndo.clear();
    paintRedo.clear();
    paintUndoBytes = 0;
    paintStrokes = 0;
    paintChanged();
}

const sn::Image& SpriteNormalsWindow::generatedGL() {
    static const sn::Image none;
    const std::shared_ptr<sn::SpriteNormalsRun> r = shown();
    if (!r) return none;
    if (generatedGLResult != shownId()) {
        generatedGLResult = shownId();
        generatedGLImage = r->result().normal;
        const sn::Settings& used = r->prepared().request.settings;
        // a DirectX generation in OpenGL green (the paint is OpenGL whatever the setting)
        if (used.directx) sn::flipGreen(generatedGLImage, r->prepared().request.color, used.alphaThreshold);
    }
    return generatedGLImage;
}

sn::Image SpriteNormalsWindow::composedNormal() {
    const std::shared_ptr<sn::SpriteNormalsRun> r = shown();
    if (!r) return sn::Image();
    const sn::Image& normal = r->result().normal;
    if (paint.empty() || !paint.sized(normal.width, normal.height)) return normal;
    const sn::Settings& used = r->prepared().request.settings;
    return sn::composite(generatedGL(), paint, r->prepared().request.color, used.alphaThreshold, used.directx);
}

const sn::Image& SpriteNormalsWindow::displayNormal() {
    static const sn::Image none;
    const std::shared_ptr<sn::SpriteNormalsRun> r = shown();
    if (!r) return none;
    const sn::Image& normal = r->result().normal;
    // Show paint off previews the generation alone (Save still writes the paint)
    if (!showPaint || paint.empty() || !paint.sized(normal.width, normal.height)) {
        displayedValid = false;
        return normal;
    }
    if (!displayedValid || displayedResult != shownId() || dirtyAll) {
        displayedResult = shownId();
        displayed = composedNormal();
        displayedValid = true;
        dirtyAll = false;
        dirtyX0 = dirtyY0 = dirtyX1 = dirtyY1 = 0;
    } else if (dirtyX0 < dirtyX1 && dirtyY0 < dirtyY1) {
        // a stroke: only its pixels composited again
        const sn::Settings& used = r->prepared().request.settings;
        sn::compositeRect(displayed, generatedGL(), paint, r->prepared().request.color, used.alphaThreshold, used.directx, dirtyX0, dirtyY0, dirtyX1, dirtyY1);
        dirtyX0 = dirtyY0 = dirtyX1 = dirtyY1 = 0;
    }
    return displayed;
}

void SpriteNormalsWindow::setPaintTool(sn::PaintTool tool) {
    endStroke();
    paintTool = tool;
    // a brush and the eyedropper don't share the left button
    if (tool != sn::PaintTool::Navigate) pickingRegion = -1;
    draggingLight = false;
}

void SpriteNormalsWindow::setPaintBrush(const sn::PaintBrush& b) {
    brush = b;
    brush.size = std::clamp(brush.size, 1, 64);
    brush.strength = std::clamp(std::isfinite(brush.strength) ? brush.strength : 1.0f, 0.0f, 1.0f);
    brush.smoothRadius = std::clamp(brush.smoothRadius, 1, 4);
    brush.wandTolerance = std::clamp(std::isfinite(brush.wandTolerance) ? brush.wandTolerance : 0.0f, 0.0f, 0.5f);
    std::string error;
    std::array<float, 3> d;
    brush.direction = sn::normaliseDirection(brush.direction, d, error) ? d : std::array<float, 3>{0.0f, 0.0f, 1.0f};
    // at most 85 degrees from the viewer (what the direction picker allows)
    float az = 0, tilt = 0;
    sn::anglesOf(brush.direction, az, tilt);
    if (tilt > sn::kMaxPaintTilt) brush.direction = sn::directionFromAngles(az, sn::kMaxPaintTilt);
}

void SpriteNormalsWindow::setShowPaint(bool on) {
    showPaint = on;
    litDirty = true;
}

void SpriteNormalsWindow::pushPaintPatch(PaintPatch patch) {
    if (patch.pixels.empty()) return;
    paintUndoBytes += patch.bytes();
    paintUndo.push_back(std::move(patch));
    paintRedo.clear();
    paintStrokes++;
    // the cap: 256 steps or 64 MiB, the oldest dropped first
    while (!paintUndo.empty() && (paintUndo.size() > PAINT_UNDO_STEPS || paintUndoBytes > paintUndoByteLimit)) {
        paintUndoBytes -= paintUndo.front().bytes();
        paintUndo.erase(paintUndo.begin());
    }
}

void SpriteNormalsWindow::recordPaint(const std::function<void()>& change) {
    const sn::PaintOverlay before = paint;
    change();
    PaintPatch patch;
    patch.width = paint.width;
    patch.height = paint.height;
    const bool sized = before.sized(paint.width, paint.height);
    for (int i = 0; i < paint.width * paint.height; i++) {
        const std::array<uint8_t, 4> was = sized ? overlayPixel(before, i) : std::array<uint8_t, 4>{128, 128, 255, 0};
        const std::array<uint8_t, 4> now = overlayPixel(paint, i);
        if (was == now) continue;
        patch.pixels.push_back(i);
        patch.before.push_back(was);
        patch.after.push_back(now);
    }
    if (!patch.pixels.empty()) {
        if (sized) paintChangedPixels(patch.pixels);
        else paintChanged();
        pushPaintPatch(std::move(patch));
    }
}

void SpriteNormalsWindow::beginStroke() {
    endStroke();
    const std::shared_ptr<sn::SpriteNormalsRun> r = shown();
    if (!r || !painting()) return;
    const sn::Image& normal = r->result().normal;
    if (!paint.sized(normal.width, normal.height)) paint = sn::PaintOverlay::blank(normal.width, normal.height);
    stroking = true;
    strokeStarted = false;
    strokeMask.assign(static_cast<size_t>(normal.width) * normal.height, 0);
    strokePatch = PaintPatch();
    strokePatch.width = paint.width;
    strokePatch.height = paint.height;
}

void SpriteNormalsWindow::strokeTo(int x, int y) {
    const std::shared_ptr<sn::SpriteNormalsRun> r = shown();
    if (!stroking || !r) return;
    const sn::Image& color = r->prepared().request.color;
    const std::vector<sn::Cell>& cells = r->prepared().request.cells;
    const int width = color.width, height = color.height;
    // what the canvas shows (one frame, or all): a stroke stays in it
    int clipX, clipY, clipW, clipH;
    cellRect(frame, clipX, clipY, clipW, clipH);
    const std::vector<std::array<int, 2>> line = strokeStarted ? sn::linePixels(strokeLast, {x, y}) : std::vector<std::array<int, 2>>{{x, y}};
    strokeStarted = true;
    strokeLast = {x, y};
    std::vector<int> pixels;
    std::vector<std::array<float, 3>> directions;
    const std::array<float, 3> mirrored = {-brush.direction[0], brush.direction[1], brush.direction[2]};
    auto add = [&](int px, int py, const std::array<float, 3>& d) {
        if (px < clipX || py < clipY || px >= clipX + clipW || py >= clipY + clipH || px < 0 || py < 0 || px >= width || py >= height) return;
        const size_t i = static_cast<size_t>(py) * width + px;
        if (strokeMask[i]) return;   // each pixel once per stroke: strength never accumulates
        strokeMask[i] = 1;
        pixels.push_back(static_cast<int>(i));
        directions.push_back(d);
    };
    for (const std::array<int, 2>& p : line) {
        // Mirror X: within the cursor's frame cell (the whole image without cells)
        const int cell = sn::cellAt(cells, width, height, p[0], p[1]);
        int cellX = 0, cellW = width;
        if (cell >= 0 && !cells.empty()) {
            cellX = cells[static_cast<size_t>(cell)].x;
            cellW = cells[static_cast<size_t>(cell)].width;
        }
        for (const std::array<int, 2>& f : sn::brushFootprint(brush.shape == sn::PaintBrush::Shape::Square ? sn::PaintBrush::Shape::Square : sn::PaintBrush::Shape::Round,
                                                              brush.size, p[0], p[1])) {
            add(f[0], f[1], brush.direction);
            if (brush.mirrorX) add(cellX + cellW - 1 - (f[0] - cellX), f[1], mirrored);
        }
    }
    if (pixels.empty()) return;
    std::vector<std::array<uint8_t, 4>> before;
    for (int i : pixels) before.push_back(overlayPixel(paint, i));
    const sn::PaintContext ctx{color, generatedGL(), cells, r->prepared().request.settings.alphaThreshold, topologyFor(paintTool == sn::PaintTool::Smooth)};
    sn::applyToPixels(paint, kindOf(paintTool), pixels, brush.direction, brush.strength, brush.smoothRadius, ctx, &directions);
    std::vector<int> changed;
    for (size_t k = 0; k < pixels.size(); k++) {
        const std::array<uint8_t, 4> now = overlayPixel(paint, pixels[k]);
        if (now == before[k]) continue;
        strokePatch.pixels.push_back(pixels[k]);
        strokePatch.before.push_back(before[k]);
        strokePatch.after.push_back(now);
        changed.push_back(pixels[k]);
    }
    // only the changed pixels are composited and lit again
    paintChangedPixels(changed);
}

void SpriteNormalsWindow::endStroke() {
    if (!stroking) return;
    stroking = false;
    strokeMask.clear();
    pushPaintPatch(std::move(strokePatch));
    strokePatch = PaintPatch();
}

bool SpriteNormalsWindow::paintStroke(const std::vector<std::array<int, 2>>& path) {
    if (!painting() || path.empty()) return false;
    beginStroke();
    if (!stroking) return false;
    for (const std::array<int, 2>& p : path) strokeTo(p[0], p[1]);
    endStroke();
    return true;
}

bool SpriteNormalsWindow::wandFill(int x, int y) {
    const std::shared_ptr<sn::SpriteNormalsRun> r = shown();
    if (!painting() || !r) return false;
    endStroke();
    const sn::Image& color = r->prepared().request.color;
    const sn::PaintContext ctx{color, generatedGL(), r->prepared().request.cells, r->prepared().request.settings.alphaThreshold,
                               topologyFor(paintTool == sn::PaintTool::Smooth)};
    const std::vector<int> pixels = sn::wandSelect(ctx, x, y, brush.wandTolerance, brush.wandContiguous, brush.wandAllFrames);
    if (pixels.empty()) {
        setStatus("The wand found nothing to paint there (an empty pixel).", false);
        return false;
    }
    recordPaint([&]() { sn::applyToPixels(paint, kindOf(paintTool), pixels, brush.direction, brush.strength, brush.smoothRadius, ctx); });
    return true;
}

bool SpriteNormalsWindow::applyPaintOps(const std::vector<sn::PaintOp>& ops, std::vector<sn::Problem>* problems) {
    const std::shared_ptr<sn::SpriteNormalsRun> r = shown();
    if (problems) problems->clear();
    const std::string reason = paintDisabledReason();
    if (!reason.empty() || !r) {
        if (problems) problems->push_back({"invalid_paint", "paint", reason, ""});
        return false;
    }
    endStroke();
    bool smooths = false;
    for (const sn::PaintOp& op : ops) smooths = smooths || op.kind == sn::PaintOpKind::Smooth;
    const sn::PaintContext ctx{r->prepared().request.color, generatedGL(), r->prepared().request.cells, r->prepared().request.settings.alphaThreshold,
                               topologyFor(smooths)};
    std::vector<sn::Problem> found = sn::validatePaintOps(ops, static_cast<int>(ctx.cells.size()));
    if (found.empty()) recordPaint([&]() { found = sn::applyPaintOps(paint, ops, ctx); });
    bool ok = true;
    for (const sn::Problem& p : found) ok = ok && p.kind != "invalid_paint" && p.kind != "paint_size_mismatch";
    if (problems) *problems = found;
    return ok;
}

bool SpriteNormalsWindow::undoPaint() {
    endStroke();
    if (paintUndo.empty()) return false;
    PaintPatch patch = std::move(paintUndo.back());
    paintUndo.pop_back();
    paintUndoBytes -= patch.bytes();
    if (paintStrokes > 0) paintStrokes--;
    const bool resized = !paint.sized(patch.width, patch.height);
    if (resized) paint = sn::PaintOverlay::blank(patch.width, patch.height);
    for (size_t k = 0; k < patch.pixels.size(); k++) {
        if (static_cast<size_t>(patch.pixels[k]) * 4 < paint.rgba.size()) std::memcpy(&paint.rgba[static_cast<size_t>(patch.pixels[k]) * 4], patch.before[k].data(), 4);
    }
    if (resized) paintChanged();
    else paintChangedPixels(patch.pixels);
    paintRedo.push_back(std::move(patch));
    return true;
}

bool SpriteNormalsWindow::redoPaint() {
    endStroke();
    if (paintRedo.empty()) return false;
    PaintPatch patch = std::move(paintRedo.back());
    paintRedo.pop_back();
    const bool resized = !paint.sized(patch.width, patch.height);
    if (resized) paint = sn::PaintOverlay::blank(patch.width, patch.height);
    for (size_t k = 0; k < patch.pixels.size(); k++) {
        if (static_cast<size_t>(patch.pixels[k]) * 4 < paint.rgba.size()) std::memcpy(&paint.rgba[static_cast<size_t>(patch.pixels[k]) * 4], patch.after[k].data(), 4);
    }
    if (resized) paintChanged();
    else paintChangedPixels(patch.pixels);
    paintUndoBytes += patch.bytes();
    paintUndo.push_back(std::move(patch));
    paintStrokes++;
    return true;
}

void SpriteNormalsWindow::clearPaint() {
    endStroke();
    // one undo step; an overlay of another size than the map (paint_size_mismatch) is empty after
    // it, so Save works again (undo brings the old one back)
    recordPaint([&]() {
        for (size_t i = 0; i + 3 < paint.rgba.size(); i += 4) {
            paint.rgba[i] = 128;
            paint.rgba[i + 1] = 128;
            paint.rgba[i + 2] = 255;
            paint.rgba[i + 3] = 0;
        }
    });
}

bool SpriteNormalsWindow::samplePaintDirection(int x, int y) {
    const std::shared_ptr<sn::SpriteNormalsRun> r = shown();
    if (!r) return false;
    const sn::Image& color = r->prepared().request.color;
    const sn::Settings& used = r->prepared().request.settings;
    if (x < 0 || y < 0 || x >= color.width || y >= color.height || color.pixel(x, y)[3] / 255.0f <= used.alphaThreshold) return false;
    // the composite (with the paint, as Save writes it), back in OpenGL green
    const sn::Image composed = composedNormal();
    std::array<float, 3> n = sn::decodeDirection(composed.pixel(x, y));
    if (used.directx) n[1] = -n[1];
    std::string error;
    std::array<float, 3> d;
    if (!sn::normaliseDirection(n, d, error)) return false;
    sn::PaintBrush b = brush;
    b.direction = d;
    setPaintBrush(b);
    setPaintTool(sn::PaintTool::Angle);
    return true;
}

// ---------------------------------------------------------------------- paint UI

bool SpriteNormalsWindow::directionDisk(const char* id, float size) {
    // the colour wheel: each point drawn with the colour of the normal it stands for (CPU, once)
    if (!hemisphereGpu.valid() && editorUi()) {
        const int n = 96;
        sn::Image wheel = sn::Image::make(n, n, 4, 8);
        for (int y = 0; y < n; y++) {
            for (int x = 0; x < n; x++) {
                const float u = (x + 0.5f) / n * 2.0f - 1.0f, v = 1.0f - (y + 0.5f) / n * 2.0f;
                const float rr = std::sqrt(u * u + v * v);
                uint8_t* p = wheel.pixel(x, y);
                if (rr > 1.0f) {
                    p[3] = 0;
                    continue;
                }
                const std::array<uint8_t, 3> c =
                    sn::encodeDirection(sn::directionFromAngles(static_cast<float>(std::atan2(v, u) * 180.0 / M_PI), rr * sn::kMaxPaintTilt));
                p[0] = c[0];
                p[1] = c[1];
                p[2] = c[2];
                p[3] = 255;
            }
        }
        updateGpuImage(hemisphereGpu, wheel, 1);
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(size, size));
    const ImVec2 centre(origin.x + size * 0.5f, origin.y + size * 0.5f);
    const float radius = size * 0.5f;
    if (hemisphereGpu.valid()) draw->AddImage(Backend::getImGuiTexture(hemisphereGpu.texture.getRender()), origin, ImVec2(origin.x + size, origin.y + size));
    else draw->AddCircleFilled(centre, radius, IM_COL32(128, 128, 255, 255), 48);
    draw->AddCircle(centre, radius, IM_COL32(20, 20, 20, 255), 48, 1.0f);
    draw->AddCircle(centre, radius * 0.5f, IM_COL32(0, 0, 0, 60), 32, 1.0f);
    bool changed = false;
    if (ImGui::IsItemActive()) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        float u = (m.x - centre.x) / radius, v = (centre.y - m.y) / radius;
        const float rr = std::min(1.0f, std::sqrt(u * u + v * v));
        float az = static_cast<float>(std::atan2(v, u) * 180.0 / M_PI), tilt = rr * sn::kMaxPaintTilt;
        if (az < 0.0f) az += 360.0f;
        if (ImGui::GetIO().KeyShift) {
            az = std::fmod(std::round(az / 45.0f) * 45.0f, 360.0f);
            tilt = std::min(sn::kMaxPaintTilt, std::round(tilt / 15.0f) * 15.0f);
        }
        brush.direction = sn::directionFromAngles(az, tilt);
        changed = true;
    }
    // the dot at the current direction
    float az = 0, tilt = 0;
    sn::anglesOf(brush.direction, az, tilt);
    const float rr = std::min(1.0f, tilt / sn::kMaxPaintTilt) * radius;
    const ImVec2 dot(centre.x + rr * std::cos(az * static_cast<float>(M_PI) / 180.0f), centre.y - rr * std::sin(az * static_cast<float>(M_PI) / 180.0f));
    draw->AddCircleFilled(dot, Theme::dpi(3.5f), IM_COL32(255, 255, 255, 255), 16);
    draw->AddCircle(dot, Theme::dpi(3.5f), IM_COL32(0, 0, 0, 255), 16, 1.5f);
    return changed;
}

void SpriteNormalsWindow::drawPaint() {
    if (!paintOpenSet && !paint.empty()) {
        paintOpen = true;
        paintOpenSet = true;
    }
    ImGui::SetNextItemOpen(paintOpen, ImGuiCond_Always);
    const bool openNow = Theme::foldoutHeader("Paint##paintFoldout", ImGuiTreeNodeFlags_AllowOverlap);
    UIProbe::item("spriteNormals.paint");
    paintOpen = openNow;
    // on the header's line: what is painted, and Clear
    {
        const size_t painted = paint.paintedCount();
        const std::string count = "Painted: " + std::to_string(painted) + " px" + (hasUnsavedPaint() ? "*" : "");
        const float clearWidth = ImGui::CalcTextSize("Clear").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float textWidth = ImGui::CalcTextSize(count.c_str()).x;
        const float right = ImGui::GetItemRectMax().x - ImGui::GetStyle().FramePadding.x;
        ImGui::SameLine();
        ImGui::SetCursorScreenPos(ImVec2(right - clearWidth - textWidth - ImGui::GetStyle().ItemSpacing.x, ImGui::GetItemRectMin().y));
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", count.c_str());
        if (hasUnsavedPaint()) tooltip("Unsaved paint: Save writes it into the normal map");
        ImGui::SameLine();
        ImGui::BeginDisabled(painted == 0);
        if (ImGui::SmallButton("Clear##clearPaint")) {
            if (painted > 0) confirm("Clear Paint?", "Remove all " + std::to_string(painted) + " painted pixels? (Ctrl+Z puts them back.)", [this]() { clearPaint(); });
        }
        ImGui::EndDisabled();
        UIProbe::item("spriteNormals.clearPaint");
        reasonTooltip(painted == 0 ? "Nothing is painted" : "Remove all the paint (one undo step)");
    }
    if (!paintOpen) return;

    const std::string reason = paintDisabledReason();
    ImGui::BeginDisabled(!reason.empty());
    // the tools: icon and key; the selected one in the accent colour
    {
        const char* labels[] = {ICON_FA_HAND " V##paintNavigate", ICON_FA_LOCATION_ARROW " B##paintAngle", ICON_FA_SQUARE " N##paintFlatten",
                                ICON_FA_DROPLET " M##paintSmooth", ICON_FA_ERASER " E##paintErase"};
        const char* tips[] = {"Navigate: no brush; drag moves the light, Alt+click picks region colours (V)",
                              "Angle: paint a direction (the disk under the tools) (B)", "Flatten: face the viewer, no relief (N)",
                              "Smooth: blur the result under the brush. Smooth paints the current result; re-smooth after big setting changes (M)",
                              "Erase: remove paint, back to the generation (E)"};
        const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
        const float width = std::max(1.0f, (ImGui::GetContentRegionAvail().x - spacing * 4.0f) / 5.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing, ImGui::GetStyle().ItemSpacing.y));
        for (int t = 0; t < 5; t++) {
            if (t > 0) ImGui::SameLine();
            const bool selected = static_cast<int>(paintTool) == t;
            if (selected) accentButtonColours();
            if (ImGui::Button(labels[t], ImVec2(width, 0.0f)) && !selected) setPaintTool(static_cast<sn::PaintTool>(t));
            if (selected) ImGui::PopStyleColor(3);
            UIProbe::item(std::string("spriteNormals.paintTool.") + std::to_string(t));
            tooltip(tips[t]);
        }
        ImGui::PopStyleVar();
        UIProbe::item("spriteNormals.paintTool");
    }
    ImGui::EndDisabled();
    if (!reason.empty()) {
        reasonTooltip(reason);
        subtle(reason + ".");
    }
    sn::PaintBrush b = brush;
    bool changed = false;
    // Angle: its direction first (what it paints), right under the tools
    if (paintTool == sn::PaintTool::Angle) {
        rowLabel("Direction", nullptr);
        const float disk = ImGui::GetFrameHeight() * 4.0f;
        if (directionDisk("##direction", disk)) changed = true;
        UIProbe::item("spriteNormals.direction");
        // beside the disk: the fields it describes stay visible
        tooltipBeside("Where the painted pixels face: the centre faces you, the rim is 85 degrees; Shift snaps. Alt+click on the canvas "
                      "samples a direction.");
        ImGui::SameLine();
        ImGui::BeginGroup();
        float az = 0, tilt = 0;
        sn::anglesOf(b.direction, az, tilt);
        const float field = std::max(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 4.0f);
        ImGui::TextDisabled("Azimuth");
        ImGui::SetNextItemWidth(field);
        bool angles = UIUtils::sliderFloatInput("##azimuth", &az, 0.0f, 360.0f, "%.0f deg");
        tooltip("0 = right, 90 = up, 180 = left, 270 = down");
        ImGui::TextDisabled("Tilt");
        ImGui::SetNextItemWidth(field);
        angles = UIUtils::sliderFloatInput("##tilt", &tilt, 0.0f, sn::kMaxPaintTilt, "%.0f deg") || angles;
        tooltip("0 = facing you, 85 = nearly sideways");
        ImGui::EndGroup();
        if (angles) {
            b.direction = sn::directionFromAngles(az, tilt);
            changed = true;
        } else if (changed) {
            b.direction = brush.direction;   // the disk set it
        }
    }
    rowLabel("Fill", "Brush: paint where you drag. Wand: a click paints every pixel of the clicked colour");
    {
        int fill = b.fill == sn::PaintBrush::Fill::Wand ? 1 : 0;
        if (ImGui::RadioButton("Brush##fillBrush", fill == 0)) fill = 0;
        tooltip("Paint where you drag (W toggles)");
        ImGui::SameLine();
        if (ImGui::RadioButton("Wand##fillWand", fill == 1)) fill = 1;
        tooltip("A click paints every pixel of the clicked colour (W toggles)");
        if ((fill == 1) != (b.fill == sn::PaintBrush::Fill::Wand)) {
            b.fill = fill == 1 ? sn::PaintBrush::Fill::Wand : sn::PaintBrush::Fill::Brush;
            changed = true;
        }
    }
    if (b.fill == sn::PaintBrush::Fill::Brush) {
        rowLabel("Shape", nullptr);
        int shape = b.shape == sn::PaintBrush::Shape::Square ? 1 : 0;
        if (ImGui::RadioButton("Round##shapeRound", shape == 0)) shape = 0;
        ImGui::SameLine();
        if (ImGui::RadioButton("Square##shapeSquare", shape == 1)) shape = 1;
        if ((shape == 1) != (b.shape == sn::PaintBrush::Shape::Square)) {
            b.shape = shape == 1 ? sn::PaintBrush::Shape::Square : sn::PaintBrush::Shape::Round;
            changed = true;
        }
        rowLabel("Size", "Brush size in pixels (1 = pencil). Shift+wheel over the canvas, or - and =");
        ImGui::SetNextItemWidth(-FLT_MIN);
        float size = static_cast<float>(b.size);
        if (UIUtils::sliderFloatInput("##brushSize", &size, 1.0f, 64.0f, size <= 1.0f ? "1 px (pencil)" : "%.0f px")) {
            b.size = static_cast<int>(std::lround(size));
            changed = true;
        }
        tooltip("Brush size in pixels (1 = pencil). Shift+wheel over the canvas, or - and =");
    }
    rowLabel("Strength", "How much of the brush's normal replaces the result (coverage); a stroke never adds up over itself");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (UIUtils::sliderFloatInput("##brushStrength", &b.strength, 0.0f, 1.0f, "%.2f")) changed = true;
    tooltip("How much of the brush's normal replaces the result (coverage); a stroke never adds up over itself");
    if (paintTool == sn::PaintTool::Smooth) {
        rowLabel("Radius", "Smooth: the box averaged around each pixel (1 = 3x3, 4 = 9x9), within its island and frame");
        ImGui::SetNextItemWidth(-FLT_MIN);
        float radius = static_cast<float>(b.smoothRadius);
        if (UIUtils::sliderFloatInput("##smoothRadius", &radius, 1.0f, 4.0f, "%.0f")) {
            b.smoothRadius = static_cast<int>(std::lround(radius));
            changed = true;
        }
        tooltip("Smooth paints the current result; re-smooth after big setting changes");
    }
    if (b.fill == sn::PaintBrush::Fill::Wand) {
        rowLabel("Tolerance", "0 = the exact colour; higher takes similar colours too (Oklab distance)");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (UIUtils::sliderFloatInput("##wandTolerance", &b.wandTolerance, 0.0f, 0.5f, b.wandTolerance <= 0.0f ? "0 (exact)" : "%.3f")) changed = true;
        tooltip("0 = the exact colour; higher takes similar colours too (Oklab distance)");
        rowLabel("Wand", nullptr);
        if (ImGui::Checkbox("Contiguous##wandContiguous", &b.wandContiguous)) changed = true;
        tooltip("Only the pixels connected to the clicked one");
        ImGui::SameLine();
        bool all = b.wandAllFrames;
        if (ImGui::Checkbox("All frames##wandAllFrames", &all)) {
            b.wandAllFrames = all;
            changed = true;
        }
        tooltip("Every frame of the sheet, not only the clicked pixel's");
    }
    rowLabel("View", nullptr);
    if (ImGui::Checkbox("Mirror X##mirrorX", &b.mirrorX)) changed = true;
    tooltip("Paint the mirrored pixels too, across the middle of the frame; Angle mirrors its direction (X)");
    ImGui::SameLine();
    bool show = showPaint;
    if (ImGui::Checkbox("Show##showPaint", &show)) setShowPaint(show);
    tooltip("Off: preview the generation alone (Save still writes the paint) (P)");
    ImGui::SameLine();
    ImGui::Checkbox("Highlight##highlightPainted", &highlightPainted);
    UIProbe::item("spriteNormals.highlightPainted");
    tooltip("Outline the painted pixels in magenta in every view (H)");
    if (changed) setPaintBrush(b);
}

void SpriteNormalsWindow::drawPaintOverlays(ImDrawList* draw, const ImVec2& imgMin, float z, int cx, int cy, int cw, int ch, const ImVec2& pixel, bool hovered) {
    const std::shared_ptr<sn::SpriteNormalsRun> r = shown();
    if (!r) return;
    const sn::Image& color = r->prepared().request.color;
    const float threshold = r->prepared().request.settings.alphaThreshold;
    const int width = color.width, height = color.height;
    // the outline of a pixel set: an edge wherever a member's neighbour is not one
    auto outline = [&](const std::function<bool(int, int)>& in, int x0, int y0, int x1, int y1, ImU32 colour) {
        for (int y = std::max(y0, cy); y < std::min(y1, cy + ch); y++) {
            for (int x = std::max(x0, cx); x < std::min(x1, cx + cw); x++) {
                if (!in(x, y)) continue;
                const ImVec2 a(imgMin.x + (x - cx) * z, imgMin.y + (y - cy) * z);
                const ImVec2 b(a.x + z, a.y + z);
                if (!in(x, y - 1)) draw->AddLine(ImVec2(a.x, a.y), ImVec2(b.x, a.y), colour);
                if (!in(x, y + 1)) draw->AddLine(ImVec2(a.x, b.y - 1.0f), ImVec2(b.x, b.y - 1.0f), colour);
                if (!in(x - 1, y)) draw->AddLine(ImVec2(a.x, a.y), ImVec2(a.x, b.y), colour);
                if (!in(x + 1, y)) draw->AddLine(ImVec2(b.x - 1.0f, a.y), ImVec2(b.x - 1.0f, b.y), colour);
            }
        }
    };
    // Highlight painted: the painted visible pixels, magenta (reads on every view)
    if (highlightPainted && paint.sized(width, height) && !paint.empty()) {
        auto painted = [&](int x, int y) {
            if (x < 0 || y < 0 || x >= width || y >= height) return false;
            const size_t i = static_cast<size_t>(y) * width + x;
            return paint.rgba[i * 4 + 3] > 0 && color.data[i * 4 + 3] / 255.0f > threshold;
        };
        outline(painted, visibleX0, visibleY0, visibleX1, visibleY1, IM_COL32(255, 0, 255, 255));
    }
    if (!hovered || !painting()) return;
    const int px = cx + static_cast<int>(std::floor(pixel.x)), py = cy + static_cast<int>(std::floor(pixel.y));
    if (pixel.x < 0 || pixel.y < 0 || pixel.x >= cw || pixel.y >= ch) return;
    if (brush.fill == sn::PaintBrush::Fill::Wand) {
        // the would-be selection, cyan, before the click
        if (wandHoverX != px || wandHoverY != py || wandHoverSerial != paintSerial) {
            wandHoverX = px;
            wandHoverY = py;
            wandHoverSerial = paintSerial;
            const sn::PaintContext ctx{color, generatedGL(), r->prepared().request.cells, threshold};
            wandHover = sn::wandSelect(ctx, px, py, brush.wandTolerance, brush.wandContiguous, brush.wandAllFrames);
            std::sort(wandHover.begin(), wandHover.end());
        }
        auto selected = [&](int x, int y) {
            if (x < 0 || y < 0 || x >= width || y >= height) return false;
            return std::binary_search(wandHover.begin(), wandHover.end(), y * width + x);
        };
        outline(selected, visibleX0, visibleY0, visibleX1, visibleY1, IM_COL32(0, 255, 255, 255));
        return;
    }
    // the brush's footprint snapped to the pixel grid (and its mirror)
    std::vector<std::array<int, 2>> footprint = sn::brushFootprint(brush.shape, brush.size, px, py);
    if (brush.mirrorX) {
        const std::vector<sn::Cell>& cells = r->prepared().request.cells;
        const int cell = sn::cellAt(cells, width, height, px, py);
        const int cellX = cell >= 0 && !cells.empty() ? cells[static_cast<size_t>(cell)].x : 0;
        const int cellW = cell >= 0 && !cells.empty() ? cells[static_cast<size_t>(cell)].width : width;
        const size_t n = footprint.size();
        for (size_t k = 0; k < n; k++) footprint.push_back({cellX + cellW - 1 - (footprint[k][0] - cellX), footprint[k][1]});
    }
    std::sort(footprint.begin(), footprint.end());
    int x0 = px, y0 = py, x1 = px + 1, y1 = py + 1;
    for (const std::array<int, 2>& f : footprint) {
        x0 = std::min(x0, f[0]);
        y0 = std::min(y0, f[1]);
        x1 = std::max(x1, f[0] + 1);
        y1 = std::max(y1, f[1] + 1);
    }
    auto inBrush = [&](int x, int y) { return std::binary_search(footprint.begin(), footprint.end(), std::array<int, 2>{x, y}); };
    outline(inBrush, x0, y0, x1, y1, IM_COL32(255, 255, 255, 230));
}

// ====================================================================== shortcuts

void SpriteNormalsWindow::handleShortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) return;
    // Ctrl+Enter: the primary button
    if (io.KeyCtrl && !io.KeyAlt && !io.KeySuper && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))) {
        primaryAction();
        return;
    }
    if (io.KeyCtrl || io.KeyAlt || io.KeySuper) return;
    if (ImGui::IsKeyPressed(ImGuiKey_1, false)) setView(View::Color);  // shortcut-lint: allow the tool window's own keys while it is focused (handleShortcuts)
    if (ImGui::IsKeyPressed(ImGuiKey_2, false)) setView(View::Normal);  // shortcut-lint: allow the tool window's own keys while it is focused (handleShortcuts)
    if (ImGui::IsKeyPressed(ImGuiKey_3, false)) setView(View::Lit);  // shortcut-lint: allow the tool window's own keys while it is focused (handleShortcuts)
    if (ImGui::IsKeyPressed(ImGuiKey_4, false)) setView(View::Split);  // shortcut-lint: allow the tool window's own keys while it is focused (handleShortcuts)
    if (ImGui::IsKeyPressed(ImGuiKey_G, false)) {  // shortcut-lint: allow the tool window's own keys while it is focused (handleShortcuts)
        grid = !grid;
        storeSettings();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) setPlaying(!playing);  // shortcut-lint: allow the tool window's own keys while it is focused (handleShortcuts)
    const int count = static_cast<int>(shownCells().size());
    if (count > 1 && ImGui::IsKeyPressed(ImGuiKey_Comma, true)) setFrame(frame <= 0 ? count - 1 : frame - 1);  // shortcut-lint: allow the tool window's own keys while it is focused (handleShortcuts)
    if (count > 1 && ImGui::IsKeyPressed(ImGuiKey_Period, true)) setFrame((frame + 1) % count);  // shortcut-lint: allow the tool window's own keys while it is focused (handleShortcuts)
    if (ImGui::IsKeyPressed(ImGuiKey_O, false)) setOrbit(!orbit);  // shortcut-lint: allow the tool window's own keys while it is focused (handleShortcuts)
    if (ImGui::IsKeyPressed(ImGuiKey_0, false)) {  // shortcut-lint: allow the tool window's own keys while it is focused (handleShortcuts)
        zoom = 1.0f;
        pan = ImVec2(0, 0);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) pickingRegion = -1;
    // paint: the tools, the fill, mirror, highlight, show, the brush size
    if (ImGui::IsKeyPressed(ImGuiKey_V, false)) setPaintTool(sn::PaintTool::Navigate);  // shortcut-lint: allow the tool window's own keys while it is focused (handleShortcuts)
    if (ImGui::IsKeyPressed(ImGuiKey_B, false)) setPaintTool(sn::PaintTool::Angle);  // shortcut-lint: allow the tool window's own keys while it is focused (handleShortcuts)
    if (ImGui::IsKeyPressed(ImGuiKey_N, false)) setPaintTool(sn::PaintTool::Flatten);  // shortcut-lint: allow the tool window's own keys while it is focused (handleShortcuts)
    if (ImGui::IsKeyPressed(ImGuiKey_M, false)) setPaintTool(sn::PaintTool::Smooth);  // shortcut-lint: allow the tool window's own keys while it is focused (handleShortcuts)
    if (ImGui::IsKeyPressed(ImGuiKey_E, false)) setPaintTool(sn::PaintTool::Erase);  // shortcut-lint: allow the tool window's own keys while it is focused (handleShortcuts)
    sn::PaintBrush b = brush;
    bool brushChanged = false;
    if (ImGui::IsKeyPressed(ImGuiKey_W, false)) {  // shortcut-lint: allow the tool window's own keys while it is focused (handleShortcuts)
        b.fill = b.fill == sn::PaintBrush::Fill::Wand ? sn::PaintBrush::Fill::Brush : sn::PaintBrush::Fill::Wand;
        brushChanged = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_X, false)) {  // shortcut-lint: allow the tool window's own keys while it is focused (handleShortcuts)
        b.mirrorX = !b.mirrorX;
        brushChanged = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Minus, true)) {  // shortcut-lint: allow the tool window's own keys while it is focused (handleShortcuts)
        b.size--;
        brushChanged = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Equal, true)) {  // shortcut-lint: allow the tool window's own keys while it is focused (handleShortcuts)
        b.size++;
        brushChanged = true;
    }
    if (brushChanged) setPaintBrush(b);
    if (ImGui::IsKeyPressed(ImGuiKey_H, false)) highlightPainted = !highlightPainted;  // shortcut-lint: allow the tool window's own keys while it is focused (handleShortcuts)
    if (ImGui::IsKeyPressed(ImGuiKey_P, false)) setShowPaint(!showPaint);  // shortcut-lint: allow the tool window's own keys while it is focused (handleShortcuts)
}

// ====================================================================== show

void SpriteNormalsWindow::show() {
    releaseRetired();
    tick();
    if (!windowOpen) return;

    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 64.0f, ImGui::GetFontSize() * 40.0f), ImGuiCond_FirstUseEver);
    if (focusRequested) {
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }
    const bool visible = ImGui::Begin(WINDOW_NAME, &windowOpen);
    focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    // closing with unsaved paint asks first (the strokes would be lost)
    if (!windowOpen && hasUnsavedPaint()) {
        windowOpen = true;
        confirm("Discard Paint Changes?", "The paint strokes made since the last Save are not in the normal map yet.\n\nClose and discard them?", [this]() {
            discardPaint();
            setOpen(false);
        });
    }
    if (!visible) {
        ImGui::End();
        if (!windowOpen) setOpen(false);
        return;
    }
    updatePreviewImages();
    if (focused) handleShortcuts();

    if (ImGui::BeginTable("##layout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("settings", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 23.0f);
        ImGui::TableSetupColumn("preview", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        // the settings scroll; the buttons, progress and status stay in view under them
        // at least the buttons and one status line
        const float minimum = ImGui::GetFrameHeight() * 1.4f + ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 3.0f;
        ImGui::BeginChild("##settings", ImVec2(0, -std::max(footerHeight, minimum)), ImGuiChildFlags_None);
        {
            // UI tests: the settings' scrolling area
            const ImRect inner = ImGui::GetCurrentWindow()->InnerClipRect;
            UIProbe::rect("spriteNormals.settings", inner.Min.x + 1, inner.Min.y + 1, inner.Max.x - 1, inner.Max.y - 1);
        }
        drawSettings();
        ImGui::EndChild();
        const float footerTop = ImGui::GetCursorPosY();
        drawFooter();
        footerHeight = ImGui::GetCursorPosY() - footerTop + ImGui::GetStyle().ItemSpacing.y;
        ImGui::TableSetColumnIndex(1);
        ImGui::BeginChild("##preview", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        drawPreview();
        ImGui::EndChild();
        ImGui::EndTable();
    }
    ImGui::End();
    if (!windowOpen) setOpen(false);
}
