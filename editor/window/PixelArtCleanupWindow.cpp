// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PixelArtCleanupWindow.h"

#include "App.h"
#include "Backend.h"
#include "EditorHost.h"
#include "Out.h"
#include "Project.h"
#include "Theme.h"
#include "command/CommandHistory.h"
#include "command/type/SetAssetMetaCmd.h"
#include "external/IconsFontAwesome6.h"
#include "pixelart/PaletteAssets.h"
#include "pixelart/aseprite/AsepriteEditorHooks.h"
#include "pixelart/cleanup/CleanupImportCook.h"
#include "pixelart/cleanup/CleanupRecipe.h"
#include "pixelart/cleanup/CleanupSample.h"
#include "pixelart/cleanup/CleanupSynthetic.h"
#include "shortcuts/CommandCatalog.h"
#include "texture/TextureData.h"
#include "util/FileDialogs.h"
#include "util/FileUtils.h"
#include "util/UIProbe.h"
#include "util/Util.h"
#include "window/PaletteAssetField.h"

#include "imgui_internal.h"
#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;
namespace cu = jasperengine::editor::pixelart::cleanup;
using jasperengine::editor::pixelart::PixelImage;
using jasperengine::editor::pixelart::PixelProblem;
using jasperengine::editor::pixelart::Verdict;

namespace {

    const float CLEANUP_ZOOM_STEPS[] = {0.25f, 0.5f, 1.0f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f, 12.0f, 16.0f, 24.0f, 32.0f, 48.0f, 64.0f};

    // Whether the ImGui frame belongs to the real editor (headless tests draw without a platform)
    bool cleanupEditorUi() {
        return ImGui::GetCurrentContext() && ImGui::GetIO().BackendPlatformName != nullptr;
    }

    // ImGui's clock, or 0 in a test without a context
    double cleanupNow() {
        return ImGui::GetCurrentContext() ? ImGui::GetTime() : 0.0;
    }

    std::string lowerOf(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    bool isCleanupAsepritePath(const fs::path& path) {
        const std::string ext = lowerOf(FileUtils::pathToUtf8(path.extension()));
        return ext == ".aseprite" || ext == ".ase";
    }

    bool isCleanupSourceFile(const std::string& file) {
        const fs::path path = fs::u8path(file);
        return isCleanupAsepritePath(path) || (Util::isImageFile(file) && lowerOf(FileUtils::pathToUtf8(path.extension())) != ".svg");
    }

    std::string problemText(const std::vector<PixelProblem>& problems) {
        if (problems.empty()) return "the source could not be read";
        const PixelProblem& p = problems.front();
        return p.fix.empty() ? p.message : p.message + " " + p.fix;
    }

    ImVec4 verdictColor(Verdict verdict) {
        switch (verdict) {
            case Verdict::Clean: return ImVec4(0.30f, 0.72f, 0.36f, 1.0f);
            case Verdict::Fixable: return ImVec4(0.27f, 0.55f, 0.90f, 1.0f);
            case Verdict::NeedsPerson: default: return ImVec4(0.90f, 0.64f, 0.20f, 1.0f);
        }
    }

    const char* verdictLabel(Verdict verdict) {
        switch (verdict) {
            case Verdict::Clean: return "Clean";
            case Verdict::Fixable: return "Fixable";
            case Verdict::NeedsPerson: default: return "Needs a Person";
        }
    }

    // the output pixels of each palette colour, in one picture's art
    void countColors(const cu::PixelArt& art, std::map<uint32_t, int>& counts, int& opaque) {
        for (int32_t index : art.indices) {
            if (index < 0 || index >= static_cast<int32_t>(art.palette.size())) continue;
            counts[jasperengine::pixelart::packRgb(art.palette[static_cast<size_t>(index)])]++;
            opaque++;
        }
    }

    // Fills each art pixel onto the cell of `canvas` it covers: the Compare view's "result upscaled
    // onto the source's cells". `xs`/`ys` are the cuts of a grid with art.width x art.height cells; if
    // the art has another size (Shrink) it is stretched over the picture instead.
    void paintArtOntoCells(PixelImage& canvas, const cu::PixelArt& art, const std::vector<double>& xs, const std::vector<double>& ys, int offsetX, int offsetY) {
        if (art.width <= 0 || art.height <= 0) return;
        const bool cells = static_cast<int>(xs.size()) == art.width + 1 && static_cast<int>(ys.size()) == art.height + 1;
        const double fullW = xs.empty() ? canvas.width() : xs.back();
        const double fullH = ys.empty() ? canvas.height() : ys.back();
        std::span<uint8_t> pixels = canvas.editPixels();
        const int width = canvas.width(), height = canvas.height();
        for (int ay = 0; ay < art.height; ay++) {
            for (int ax = 0; ax < art.width; ax++) {
                const int32_t index = art.indices[static_cast<size_t>(ay) * art.width + ax];
                if (index < 0 || index >= static_cast<int32_t>(art.palette.size())) continue;
                const jasperengine::pixelart::Rgba8 color = art.palette[static_cast<size_t>(index)];
                double x0, x1, y0, y1;
                if (cells) {
                    x0 = xs[ax], x1 = xs[ax + 1], y0 = ys[ay], y1 = ys[ay + 1];
                } else {
                    x0 = fullW * ax / art.width, x1 = fullW * (ax + 1) / art.width;
                    y0 = fullH * ay / art.height, y1 = fullH * (ay + 1) / art.height;
                }
                const int left = std::clamp(offsetX + static_cast<int>(std::floor(x0)), 0, width);
                const int right = std::clamp(offsetX + static_cast<int>(std::ceil(x1)), 0, width);
                const int top = std::clamp(offsetY + static_cast<int>(std::floor(y0)), 0, height);
                const int bottom = std::clamp(offsetY + static_cast<int>(std::ceil(y1)), 0, height);
                for (int y = top; y < bottom; y++) {
                    for (int x = left; x < right; x++) {
                        uint8_t* p = pixels.data() + (static_cast<size_t>(y) * width + x) * 4;
                        p[0] = color.r;
                        p[1] = color.g;
                        p[2] = color.b;
                        p[3] = 255;
                    }
                }
            }
        }
    }

    // The top row's leftmost opaque pixel: a frame's art and its placed copy share it, shifted
    bool firstOpaqueCell(const cu::PixelArt& art, int& x, int& y) {
        for (int i = 0; i < static_cast<int>(art.indices.size()); i++) {
            if (art.indices[static_cast<size_t>(i)] != cu::TRANSPARENT) {
                x = i % art.width;
                y = i / art.width;
                return true;
            }
        }
        return false;
    }

    // What the Recipe's grid-related choices are: when they change, every cached step is redone (R39)
    std::string fullRunKey(const cu::Recipe& recipe) {
        char text[192];
        std::snprintf(text, sizeof(text), "%d %.3f %.3f %.3f %.3f %d %d %d %d %.3f %.3f", static_cast<int>(recipe.grid.kind), recipe.grid.spec.cellWidth,
                      recipe.grid.spec.cellHeight, recipe.grid.spec.offsetX, recipe.grid.spec.offsetY, static_cast<int>(recipe.follow),
                      static_cast<int>(recipe.outlines), static_cast<int>(recipe.moved.size()), static_cast<int>(recipe.sheetMode), recipe.sample.inset,
                      recipe.sample.alphaThreshold);
        return text;
    }

    // The sheet the window edits: the recipe's own once it is Given, else the one the last run found
    cu::Sheet& givenSheet(cu::Recipe& recipe, const cu::Result* result) {
        if (recipe.sheetMode != cu::SheetMode::Given || !recipe.sheet) {
            recipe.sheet = (result && result->animation) ? result->animation->sheet : cu::Sheet();
            recipe.sheetMode = cu::SheetMode::Given;
        }
        return *recipe.sheet;
    }

}

struct PixelArtCleanupWindow::Job {
    std::atomic<bool> cancel{false};
    jobs::Task<void> task;
    std::shared_ptr<Bundle> out;
    bool full = false;
};

struct PixelArtCleanupWindow::RerunJob {
    std::string output;
    cu::Prepared prepared;
    jobs::Task<void> task;
    std::vector<cu::OutputFile> files;
    PixelProblem problem;
    bool encoded = false;
};

PixelArtCleanupWindow::PixelArtCleanupWindow(Project* owner) : project(owner) {}

PixelArtCleanupWindow::~PixelArtCleanupWindow() {
    shutdownJobs(2.0);
}

void PixelArtCleanupWindow::open() {
    windowOpen = true;
    focusRequested = true;
}

void PixelArtCleanupWindow::setOpen(bool value) {
    if (value) {
        if (!windowOpen) focusRequested = true;
        windowOpen = true;
        return;
    }
    windowOpen = false;
    // a closed window holds no keys, no undo routing (App: lastFocusedWindow) and nothing heavy:
    // the worker stops, the cache and the GPU copies go (reopening runs and uploads again)
    focused = false;
    focusRequested = false;
    painting = false;
    draggingAxis = 0;
    draggingLine = -1;
    draggingBox = -1;
    addingBox = false;
    playing = false;
    commitEdit();
    cancelJob();
    cache = std::make_shared<cu::RunCache>();
    lastFullKey.clear();
    // a run stopped here is run again when the window opens
    if (prepared && (!shown || shown->recipeText != cu::recipeText(recipe))) dirty = true;
    retire(sourceGpu);
    retire(resultGpu);
    retire(compareGpu);
    retire(playerGpu);
    playerKey.clear();
    uploadedFrame = -1;
    uploadedResult.reset();
}

void PixelArtCleanupWindow::bindCommands(CommandCatalog& catalog) {
    auto bind = [&catalog, this](const char* id, std::function<void()> action, std::function<bool()> enabled) {
        const CommandDef* existing = catalog.find(id);
        if (!existing) return;
        CommandDef def = *existing;
        def.run = [action](const CommandArgs&) { action(); };
        def.enabled = std::move(enabled);
        def.keyGate = [this]() { return takesKeys(); };
        catalog.add(std::move(def));
    };
    bind("pixelArtCleanup.write", [this]() {
        std::string error;
        if (!write(error)) setStatus(error, true);
    }, [this]() { return hasResult() && !isDemo; });
    const auto stepping = [this]() { return tab == Tab::Lints && lintSelected >= 0 && shown != nullptr; };
    bind("pixelArtCleanup.nextSpot", [this]() { stepLintSpot(1); }, stepping);
    bind("pixelArtCleanup.previousSpot", [this]() { stepLintSpot(-1); }, stepping);
    const auto nudging = [this]() { return tab == Tab::Frames && shown && shown->result.animation.has_value(); };
    bind("pixelArtCleanup.nudgeLeft", [this]() { nudgeCurrentFrame(-1, 0); }, nudging);
    bind("pixelArtCleanup.nudgeRight", [this]() { nudgeCurrentFrame(1, 0); }, nudging);
    bind("pixelArtCleanup.nudgeUp", [this]() { nudgeCurrentFrame(0, -1); }, nudging);
    bind("pixelArtCleanup.nudgeDown", [this]() { nudgeCurrentFrame(0, 1); }, nudging);
}

void PixelArtCleanupWindow::shutdownJobs(double timeoutSeconds) {
    cancelJob();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeoutSeconds);
    for (const std::shared_ptr<RerunJob>& rerun : reruns) {
        while (rerun->task.isValid() && !rerun->task.isReady() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    reruns.clear();
}

void PixelArtCleanupWindow::resetForProjectSwitch() {
    cancelJob();
    reruns.clear();
    prepared.reset();
    shown.reset();
    uploadedResult.reset();
    uploadedFrame = -1;
    clearAll();
    sourceName.clear();
    isDemo = false;
    windowOpen = false;
}

void PixelArtCleanupWindow::clearAll() {
    cache = std::make_shared<cu::RunCache>();
    undoStack.clear();
    redoStack.clear();
    editBefore.reset();
    written.clear();
    statusMessage.clear();
    statusError = false;
    outputPath.clear();
    outputFormatPicked = false;
    dirty = false;
    frame = 0;
    animationSelected = 0;
    lintSelected = spotSelected = -1;
    hoverLostShown = hoverLintShown = -1;
    playing = false;
    painting = false;
    draggingAxis = 0;
    draggingLine = -1;
    draggingBox = -1;
    addingBox = false;
    fitRequested = true;
    zoom = 0.0f;
    pan = ImVec2(0, 0);
    flashUntil = 0.0;
    retire(sourceGpu);
    retire(resultGpu);
    retire(compareGpu);
    retire(playerGpu);
}

void PixelArtCleanupWindow::setStatus(const std::string& text, bool error) {
    statusMessage = text;
    statusError = error;
}

void PixelArtCleanupWindow::confirm(const std::string& title, const std::string& message, std::function<void()> yes) {
    getEditorHost().registerConfirmAlert(title, message, std::move(yes));
}

// ---- entry points

void PixelArtCleanupWindow::adopt(cu::Prepared&& value, const std::string& name, bool demo) {
    cancelJob();
    clearAll();
    prepared = std::make_unique<cu::Prepared>(std::move(value));
    shown.reset();
    uploadedResult.reset();
    uploadedFrame = -1;
    sourceName = name;
    isDemo = demo;
    recipe = prepared->recipe;
    dirty = true;
    open();
}

bool PixelArtCleanupWindow::openSource(const fs::path& image, std::string& error) {
    cu::Spec spec;
    spec.source = FileUtils::pathToGenericUtf8(project ? project->normalizeToProjectRelative(image) : image);
    spec.person = true;
    std::vector<PixelProblem> problems;
    std::optional<cu::Prepared> ready = cu::prepare(project, spec, problems);
    if (!ready) {
        error = problemText(problems);
        return false;
    }
    const std::string name = ready->sourcePath;
    const fs::path absolute = ready->sourceAbsolute;
    adopt(std::move(*ready), name, false);
    // a texture with an import step opens on the recipe it cooks with (the Inspector's Open in Cleanup Window)
    const std::string stepText = cu::importStepRecipeText(absolute);
    if (!stepText.empty()) {
        cu::Recipe decoded;
        std::vector<PixelProblem> ignored;
        if (cu::decodeRecipeText(stepText, decoded, ignored)) {
            decoded.source = recipe.source;
            recipe = decoded;
            lastFullKey.clear();
        }
    }
    return true;
}

bool PixelArtCleanupWindow::openOutput(const fs::path& output, std::string& error) {
    cu::Spec spec;
    spec.source = FileUtils::pathToGenericUtf8(project ? project->normalizeToProjectRelative(output) : output);
    spec.rerun = true;
    spec.person = true;
    std::vector<PixelProblem> problems;
    std::optional<cu::Prepared> ready = cu::prepare(project, spec, problems);
    if (!ready) {
        error = problemText(problems);
        return false;
    }
    const std::string name = ready->sourcePath;
    const std::string writtenAt = ready->rerunOutput;
    const bool aseprite = isCleanupAsepritePath(fs::u8path(writtenAt));
    adopt(std::move(*ready), name, false);
    outputPath = writtenAt;
    outputFormat = aseprite ? cu::OutputFormat::Aseprite : cu::OutputFormat::Png;
    outputFormatPicked = true;
    return true;
}

bool PixelArtCleanupWindow::openDemo() {
    cu::Prepared demo;
    demo.frames = {cu::demoImage()};
    demo.spec.person = true;
    demo.spec.format = cu::OutputFormat::Png;
    adopt(std::move(demo), "(demo)", true);
    return true;
}

bool PixelArtCleanupWindow::open(const fs::path& image, std::string& error) {
    PixelProblem problem;
    fs::path absolute = image;
    if (project && image.is_relative()) absolute = project->getProjectPath() / image;
    if (cu::readOutputRecipe(absolute, problem)) return openOutput(image, error);
    return openSource(image, error);
}

bool PixelArtCleanupWindow::startRerun(const fs::path& output, std::string& error) {
    cu::Spec spec;
    spec.source = FileUtils::pathToGenericUtf8(project ? project->normalizeToProjectRelative(output) : output);
    spec.rerun = true;
    spec.person = true;
    spec.output.write = true;
    std::vector<PixelProblem> problems;
    std::optional<cu::Prepared> ready = cu::prepare(project, spec, problems);
    if (!ready) {
        error = problemText(problems);
        return false;
    }
    auto rerun = std::make_shared<RerunJob>();
    rerun->output = ready->rerunOutput;
    rerun->prepared = std::move(*ready);
    jobs::SubmitOptions options;
    options.lane = jobs::Lane::Background;
    options.name = "pixel-art-cleanup-rerun";
    rerun->task = jobs::submit([rerun]() {
        const cu::Result result = cu::run(rerun->prepared);
        if (!result.problem.empty()) {
            rerun->problem = result.problem;
            return;
        }
        rerun->encoded = cu::encodeOutputs(rerun->prepared, result, rerun->files, rerun->problem);
    }, options);
    reruns.push_back(std::move(rerun));
    return true;
}

void PixelArtCleanupWindow::tickReruns() {
    for (auto it = reruns.begin(); it != reruns.end();) {
        const std::shared_ptr<RerunJob>& rerun = *it;
        if (!rerun->task.isValid() || !rerun->task.isReady()) {
            ++it;
            continue;
        }
        rerun->task.get();
        const std::string name = FileUtils::pathToUtf8(fs::u8path(rerun->output).filename());
        PixelProblem problem = rerun->problem;
        if (rerun->encoded && cu::writeUndoable(project, rerun->files, problem)) {
            Out::info("Pixel Art Cleanup: re-ran %s.", name.c_str());
        } else {
            getEditorHost().registerAlert("Pixel Art Cleanup", "Can't re-run " + name + ": " + problem.message + (problem.fix.empty() ? std::string() : " " + problem.fix));
        }
        it = reruns.erase(it);
    }
}

// ---- recipe edits

void PixelArtCleanupWindow::edited(bool) {
    dirty = true;
}

void PixelArtCleanupWindow::applyEdit(const std::function<void(cu::Recipe&)>& edit, bool gridAffecting) {
    cu::Recipe before = recipe;
    edit(recipe);
    if (recipe == before) return;
    undoStack.push_back(std::move(before));
    if (undoStack.size() > 256) undoStack.erase(undoStack.begin());
    redoStack.clear();
    edited(gridAffecting);
}

void PixelArtCleanupWindow::beginEdit() {
    if (!editBefore) editBefore = recipe;
}

void PixelArtCleanupWindow::commitEdit() {
    if (!editBefore) return;
    if (recipe != *editBefore) {
        undoStack.push_back(std::move(*editBefore));
        if (undoStack.size() > 256) undoStack.erase(undoStack.begin());
        redoStack.clear();
    }
    editBefore.reset();
}

void PixelArtCleanupWindow::setRecipe(const cu::Recipe& value, bool undoStep) {
    if (undoStep) {
        applyEdit([&](cu::Recipe& r) { r = value; }, true);
    } else {
        recipe = value;
        edited(true);
    }
}

bool PixelArtCleanupWindow::undoEdit() {
    commitEdit();
    if (undoStack.empty()) return false;
    redoStack.push_back(recipe);
    recipe = std::move(undoStack.back());
    undoStack.pop_back();
    edited(true);
    return true;
}

bool PixelArtCleanupWindow::redoEdit() {
    if (redoStack.empty()) return false;
    undoStack.push_back(recipe);
    recipe = std::move(redoStack.back());
    redoStack.pop_back();
    edited(true);
    return true;
}

void PixelArtCleanupWindow::moveGridLine(char axis, int index, double position) {
    beginEdit();
    setGridLine(axis, index, position);
    commitEdit();
}

void PixelArtCleanupWindow::setGridLine(char axis, int index, double position) {
    const std::string id = std::string(1, axis) + ":" + std::to_string(index);
    position = std::round(position * 100.0) / 100.0;
    bool found = false;
    for (auto& entry : recipe.moved) {
        if (entry.first == id) {
            entry.second = position;
            found = true;
        }
    }
    if (!found) recipe.moved.emplace_back(id, position);
    std::sort(recipe.moved.begin(), recipe.moved.end());
    edited(true);
}

// ---- running

bool PixelArtCleanupWindow::isStale() const {
    if (!shown) return prepared != nullptr;
    return dirty || shown->recipeText != cu::recipeText(recipe) || (outputFormatPicked && shown->result.format != outputFormat);
}

std::shared_ptr<PixelArtCleanupWindow::Bundle> PixelArtCleanupWindow::makeBundle(const std::atomic<bool>* cancel, const std::string& text, double* outMs) {
    const auto start = std::chrono::steady_clock::now();
    auto bundle = std::make_shared<Bundle>();
    bundle->recipeText = text;
    bundle->result = cu::run(*prepared, cache.get(), cancel);
    const cu::Result& result = bundle->result;
    if (!result.cancelled && result.problem.empty()) {
        if (result.animation) {
            const cu::Atlas atlas = cu::pngSheet(cu::atlasClips(*result.animation));
            bundle->image = cu::toRgba(atlas.art);
            bundle->atlasRects = atlas.rects;
            for (const cu::FixedFrame& fixed : result.animation->frames) countColors(fixed.art, bundle->colorCounts, bundle->opaqueCount);
        } else if (result.outcome) {
            bundle->image = cu::toRgba(result.outcome->exported);
            const cu::PixelArt& art = result.outcome->art;
            countColors(result.outcome->exported, bundle->colorCounts, bundle->opaqueCount);
            if (art.width != result.outcome->exported.width || art.height != result.outcome->exported.height) {
                // the crop is the box around the opaque pixels of `art`
                int x0 = art.width, y0 = art.height;
                for (int y = 0; y < art.height; y++) {
                    for (int x = 0; x < art.width; x++) {
                        if (art.indices[static_cast<size_t>(y) * art.width + x] != cu::TRANSPARENT) {
                            x0 = std::min(x0, x);
                            y0 = std::min(y0, y);
                        }
                    }
                }
                bundle->cropX = x0 == art.width ? 0 : x0;
                bundle->cropY = y0 == art.height ? 0 : y0;
            }
        }
        bundle->compare = compareForFrame(*bundle, 0);
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    bundle->ms = ms;
    if (outMs) *outMs = ms;
    return bundle;
}

PixelImage PixelArtCleanupWindow::compareForFrame(const Bundle& bundle, int frameIndex) const {
    PixelImage canvas;
    if (!prepared || prepared->frames.empty()) return canvas;
    const cu::Result& result = bundle.result;
    if (result.animation) {
        const bool animated = prepared->frames.size() > 1;
        const int sourceIndex = animated ? std::clamp(frameIndex, 0, static_cast<int>(prepared->frames.size()) - 1) : 0;
        canvas = PixelImage(prepared->frames[static_cast<size_t>(sourceIndex)].width(), prepared->frames[static_cast<size_t>(sourceIndex)].height());
        for (const cu::FixedFrame& fixed : result.animation->frames) {
            if (animated && fixed.index != sourceIndex) continue;
            paintArtOntoCells(canvas, fixed.art, fixed.outcome.grid.xs, fixed.outcome.grid.ys, fixed.box.x, fixed.box.y);
        }
        return canvas;
    }
    if (result.outcome) {
        canvas = PixelImage(prepared->frames[0].width(), prepared->frames[0].height());
        paintArtOntoCells(canvas, result.outcome->art, result.outcome->grid.xs, result.outcome->grid.ys, 0, 0);
    }
    return canvas;
}

void PixelArtCleanupWindow::applyBundle(std::shared_ptr<Bundle> bundle, bool full) {
    // a cancelled run stopped part way: its time predicts nothing (R39)
    if (bundle->result.cancelled) return;
    lastRunMs = bundle->ms;
    if (full) lastFullRunMs = bundle->ms;
    if (!bundle->result.problem.empty()) {
        const PixelProblem& p = bundle->result.problem;
        setStatus(p.message + (p.fix.empty() ? std::string() : " " + p.fix), true);
        return;
    }
    shown = std::move(bundle);
    if (statusError) {
        statusMessage.clear();
        statusError = false;
    }
    // first run: the output and format follow what the source would write
    if (!outputFormatPicked) outputFormat = shown->result.format;
    if (outputPath.empty()) outputPath = shown->result.defaultOutput;
}

void PixelArtCleanupWindow::startRun(bool allowSync) {
    if (!prepared || job) return;
    dirty = false;
    prepared->recipe = recipe;
    if (outputFormatPicked) prepared->spec.format = outputFormat;
    else prepared->spec.format.reset();
    const std::string key = fullRunKey(recipe);
    const bool full = key != lastFullKey;
    lastFullKey = key;
    const std::string text = cu::recipeText(recipe);
    const double predicted = (!shown || full) ? std::max(lastFullRunMs, lastRunMs) : lastRunMs;
    if (allowSync && shown && predicted <= 33.0) {
        applyBundle(makeBundle(nullptr, text, nullptr), full);
        return;
    }
    auto running = std::make_shared<Job>();
    running->full = full;
    jobs::SubmitOptions options;
    options.lane = jobs::Lane::Background;
    options.name = "pixel-art-cleanup";
    job = running;
    running->task = jobs::submit([this, running, text]() { running->out = makeBundle(&running->cancel, text, nullptr); }, options);
}

void PixelArtCleanupWindow::collectJob() {
    if (!job || !job->task.isValid() || !job->task.isReady()) return;
    const std::shared_ptr<Job> done = job;
    job.reset();
    try {
        done->task.get();
    } catch (const std::exception& e) {
        setStatus(std::string("Pixel Art Cleanup failed: ") + e.what(), true);
        return;
    }
    if (done->out) applyBundle(done->out, done->full);
}

void PixelArtCleanupWindow::cancelJob() {
    if (!job) return;
    job->cancel = true;
    if (job->task.isValid()) job->task.wait();
    job.reset();
}

void PixelArtCleanupWindow::waitForRunningJob() {
    if (job && job->task.isValid()) job->task.wait();
}

void PixelArtCleanupWindow::runNow() {
    if (!prepared) return;
    if (job) {
        job->task.wait();
        collectJob();
    }
    if (!dirty && shown) return;
    dirty = false;
    prepared->recipe = recipe;
    if (outputFormatPicked) prepared->spec.format = outputFormat;
    else prepared->spec.format.reset();
    const std::string key = fullRunKey(recipe);
    const bool full = key != lastFullKey;
    lastFullKey = key;
    applyBundle(makeBundle(nullptr, cu::recipeText(recipe), nullptr), full);
}

void PixelArtCleanupWindow::tick() {
    collectJob();
    tickReruns();
    // a settled edit needs no stale run; a drag lets the last run finish, so it updates as it moves
    if (job && dirty && !editBefore) job->cancel = true;
    if (dirty && !job && windowOpen) startRun(true);
    if ((job || playing || !reruns.empty() || flashUntil > cleanupNow()) && cleanupEditorUi()) Backend::getApp().requestRedraw();
}

// ---- writing

bool PixelArtCleanupWindow::write(std::string& error, bool confirmed) {
    if (!prepared || isDemo) {
        error = "Open an image first: the demo cannot be written.";
        return false;
    }
    runNow();
    if (!shown) {
        error = statusMessage.empty() ? "There is no result to write yet." : statusMessage;
        return false;
    }
    if (shown->result.report.verdict == Verdict::NeedsPerson && !confirmed) {
        std::string guards;
        for (const cu::Guard& guard : shown->result.report.guards) guards += (guards.empty() ? "" : ", ") + guard.id;
        confirm("Write Result Anyway?", "This cleanup needs a person: " + (guards.empty() ? shown->result.report.summary : guards) + ".\n\nWrite it as it is?", [this]() {
            std::string message;
            if (!write(message, true) && !message.empty()) setStatus(message, true);
        });
        return true;
    }
    prepared->spec.output = cu::OutputSpec();
    prepared->spec.output.write = true;
    if (outputFormat == cu::OutputFormat::Aseprite) prepared->spec.output.aseprite = outputPath;
    else prepared->spec.output.png = outputPath;
    prepared->spec.person = true;
    std::vector<cu::OutputFile> files;
    PixelProblem problem;
    if (!cu::encodeOutputs(*prepared, shown->result, files, problem)) {
        if (problem.kind == "output_exists" || problem.kind == "output_edited") {
            const std::string name = FileUtils::pathToUtf8(fs::u8path(outputPath).filename());
            const bool editedSince = problem.kind == "output_edited";
            confirm(editedSince ? "Replace Edited Output?" : "Replace Existing File?",
                    editedSince ? name + " was edited after this cleanup. Replace it and lose those edits?\n\nUndo in the Project window brings it back."
                                : name + " already exists. Replace it?\n\nUndo in the Project window brings it back.",
                    [this]() {
                        std::string message;
                        if (!replaceOutput(message) && !message.empty()) setStatus(message, true);
                    });
            return true;
        }
        error = problem.message + (problem.fix.empty() ? std::string() : " " + problem.fix);
        return false;
    }
    if (!cu::writeUndoable(project, files, problem)) {
        error = problem.message;
        return false;
    }
    written.clear();
    for (const cu::OutputFile& file : files) {
        if (file.role == "png" || file.role == "aseprite") written.push_back(file.path);
    }
    prepared->spec.overwrite = false;
    // the local stack ends at Write (R43): what was written is now the project's history
    undoStack.clear();
    redoStack.clear();
    setStatus("Wrote " + (written.empty() ? outputPath : written.front()) + ".", false);
    return true;
}

bool PixelArtCleanupWindow::replaceOutput(std::string& error) {
    if (!prepared) return false;
    // no longer a re-run over its own file: an explicit replace, for this write only
    prepared->rerunOutput.clear();
    prepared->rerunOutputAbsolute.clear();
    prepared->spec.overwrite = true;
    const bool ok = write(error, true);
    prepared->spec.overwrite = false;
    return ok;
}

bool PixelArtCleanupWindow::saveAsImportStep(std::string& error, bool confirmed) {
    if (!prepared || isDemo || prepared->sourceAbsolute.empty() || isCleanupAsepritePath(prepared->sourceAbsolute)) {
        error = "Save as Import Step needs a raster image in the project (.aseprite sources never take this step).";
        return false;
    }
    runNow();
    if (!shown) {
        error = "There is no result to save yet.";
        return false;
    }
    if (shown->result.report.verdict == Verdict::NeedsPerson && !confirmed) {
        confirm("Save Import Step Anyway?", "This cleanup needs a person.\n\nCook it as an import step anyway?", [this]() {
            std::string message;
            if (!saveAsImportStep(message, true) && !message.empty()) setStatus(message, true);
        });
        return true;
    }
    YAML::Node node(YAML::NodeType::Map);
    node["enabled"] = true;
    node["recipe"] = cu::recipeText(shown->result.resolved);
    auto* command = new SetAssetMetaCmd(project, project->normalizeToProjectRelative(prepared->sourceAbsolute), "import.pixelCleanup", node);
    command->setAction("Save as Import Step");
    command->setNoMerge();
    if (!project->getProjectCommandHistory()->addCommand(command)) {
        error = "The import step could not be saved.";
        return false;
    }
    setStatus("Saved as the import step of " + sourceName + ".", false);
    return true;
}

std::string PixelArtCleanupWindow::recipeJson() const {
    return cu::encodeRecipe(shown && !isStale() ? shown->result.resolved : recipe).dump(2);
}

// ---- GPU

void PixelArtCleanupWindow::retire(Gpu& gpu) {
    if (!gpu.texture.empty()) retired.push_back(gpu.texture);
    gpu = Gpu();
}

void PixelArtCleanupWindow::releaseRetired() {
    for (Texture& texture : retired) texture.destroy();
    retired.clear();
}

void PixelArtCleanupWindow::updateGpu(Gpu& gpu, const PixelImage& image) {
    retire(gpu);
    if (!cleanupEditorUi() || image.empty()) return;
    static uint64_t counter = 0;
    const size_t bytes = image.pixels().size();
    void* pixels = std::malloc(bytes);
    if (!pixels) return;
    std::memcpy(pixels, image.pixels().data(), bytes);
    TextureData data(image.width(), image.height(), static_cast<unsigned int>(bytes), ColorFormat::RGBA, 4, pixels);
    gpu.texture.setData("editor:pixel-art-cleanup:" + std::to_string(++counter), data);
    gpu.texture.setMinFilter(TextureFilter::NEAREST);
    gpu.texture.setMagFilter(TextureFilter::NEAREST);
    TextureRender* render = gpu.texture.getRender();
    gpu.texture.releaseData();
    if (!render || !render->isCreated()) {
        retire(gpu);
        return;
    }
    gpu.width = image.width();
    gpu.height = image.height();
}

int PixelArtCleanupWindow::sourceFrameIndex() const {
    if (!prepared || prepared->frames.size() <= 1) return 0;
    if (shown && shown->result.animation) {
        const auto& clips = shown->result.animation->clips;
        if (animationSelected >= 0 && animationSelected < static_cast<int>(clips.size())) {
            const auto& placed = clips[static_cast<size_t>(animationSelected)].placed;
            if (frame >= 0 && frame < static_cast<int>(placed.size())) return std::clamp(placed[static_cast<size_t>(frame)].index, 0, static_cast<int>(prepared->frames.size()) - 1);
        }
    }
    return std::clamp(frame, 0, static_cast<int>(prepared->frames.size()) - 1);
}

void PixelArtCleanupWindow::uploadImages() {
    if (!prepared) return;
    const int sourceFrame = sourceFrameIndex();
    if (sourceFrame != uploadedFrame) {
        updateGpu(sourceGpu, prepared->frames[static_cast<size_t>(sourceFrame)]);
        uploadedFrame = sourceFrame;
        uploadedResult.reset();
    }
    if (shown && shown != uploadedResult) {
        updateGpu(resultGpu, shown->image);
        updateGpu(compareGpu, prepared->frames.size() > 1 ? compareForFrame(*shown, sourceFrame) : shown->compare);
        uploadedResult = shown;
    }
}

void PixelArtCleanupWindow::setView(View value) {
    if (resultSpace() != (value == View::Result)) fitRequested = true;
    view = value;
}

void PixelArtCleanupWindow::setFrame(int index) {
    frame = std::max(0, index);
}

PixelArtCleanupWindow::UiState PixelArtCleanupWindow::uiState() const {
    UiState state;
    state.open = windowOpen;
    state.focused = focused;
    state.source = sourceName;
    state.view = view == View::Source ? "source" : view == View::Result ? "result" : "compare";
    state.running = isRunning();
    state.recipeMoved = static_cast<int>(recipe.moved.size());
    state.undoDepth = undoStack.size();
    state.zoom = zoom;
    state.imageMinX = imageMin.x;
    state.imageMinY = imageMin.y;
    if (zoom > 0.0f) {
        const ImVec2 centre = screenToView(ImVec2(canvasOrigin.x + canvasSize.x * 0.5f, canvasOrigin.y + canvasSize.y * 0.5f));
        state.centerX = centre.x;
        state.centerY = centre.y;
    }
    state.copiedRecipe = copiedRecipe;
    state.highlight = highlightText;
    switch (tab) {
        case Tab::Palette: state.tab = "palette"; break;
        case Tab::LostColors: state.tab = "lost_colors"; break;
        case Tab::Cleanup: state.tab = "cleanup"; break;
        case Tab::Lints: state.tab = "lints"; break;
        case Tab::Frames: state.tab = "frames"; break;
    }
    if (shown) {
        state.verdict = pixelart::verdictName(shown->result.report.verdict);
        state.confidence = shown->result.report.confidence;
        if (shown->result.outcome) {
            state.xs = shown->result.outcome->grid.xs;
            state.ys = shown->result.outcome->grid.ys;
        }
        const auto& results = shown->result.report.lints.results;
        for (int i = 0; i < static_cast<int>(results.size()) && state.spots.size() < 32; i++) {
            for (int s = 0; s < static_cast<int>(results[static_cast<size_t>(i)].at.size()) && state.spots.size() < 32; s++) {
                double x0, y0, x1, y1;
                if (lintSpotRect(results[static_cast<size_t>(i)].at[static_cast<size_t>(s)], x0, y0, x1, y1)) {
                    state.spots.push_back({i, s, (x0 + x1) * 0.5, (y0 + y1) * 0.5});
                }
            }
        }
    }
    return state;
}

// ---- coordinate maps

ImVec2 PixelArtCleanupWindow::viewImageSize() const {
    if (resultSpace() && shown && !shown->image.empty()) return ImVec2(static_cast<float>(shown->image.width()), static_cast<float>(shown->image.height()));
    if (prepared && !prepared->frames.empty()) {
        const PixelImage& image = prepared->frames[static_cast<size_t>(sourceFrameIndex())];
        return ImVec2(static_cast<float>(image.width()), static_cast<float>(image.height()));
    }
    return ImVec2(1, 1);
}

ImVec2 PixelArtCleanupWindow::viewToScreen(double x, double y) const {
    return ImVec2(imageMin.x + static_cast<float>(x) * zoom, imageMin.y + static_cast<float>(y) * zoom);
}

ImVec2 PixelArtCleanupWindow::screenToView(const ImVec2& p) const {
    return ImVec2((p.x - imageMin.x) / zoom, (p.y - imageMin.y) / zoom);
}

bool PixelArtCleanupWindow::cellRect(int frameIndex, int ax, int ay, double& x0, double& y0, double& x1, double& y1) const {
    if (!shown) return false;
    const cu::Result& result = shown->result;
    if (result.animation && frameIndex >= 0 && frameIndex < static_cast<int>(result.animation->frames.size())) {
        const cu::FixedFrame& fixed = result.animation->frames[static_cast<size_t>(frameIndex)];
        const std::vector<double>& xs = fixed.outcome.grid.xs;
        const std::vector<double>& ys = fixed.outcome.grid.ys;
        if (ax >= 0 && ay >= 0 && ax + 1 < static_cast<int>(xs.size()) && ay + 1 < static_cast<int>(ys.size())) {
            x0 = fixed.box.x + xs[ax], x1 = fixed.box.x + xs[ax + 1];
            y0 = fixed.box.y + ys[ay], y1 = fixed.box.y + ys[ay + 1];
            return true;
        }
        x0 = fixed.box.x, y0 = fixed.box.y;
        x1 = x0 + fixed.box.width, y1 = y0 + fixed.box.height;
        return true;
    }
    if (result.outcome) {
        const cu::Grid& grid = result.outcome->grid;
        const cu::PixelArt& art = result.outcome->art;
        if (art.width + 1 == static_cast<int>(grid.xs.size()) && art.height + 1 == static_cast<int>(grid.ys.size()) && ax >= 0 && ay >= 0 && ax < art.width && ay < art.height) {
            x0 = grid.xs[ax], x1 = grid.xs[ax + 1];
            y0 = grid.ys[ay], y1 = grid.ys[ay + 1];
            return true;
        }
        if (art.width > 0 && art.height > 0 && !grid.xs.empty() && !grid.ys.empty()) {
            const double w = grid.xs.back(), h = grid.ys.back();
            x0 = w * ax / art.width, x1 = w * (ax + 1) / art.width;
            y0 = h * ay / art.height, y1 = h * (ay + 1) / art.height;
            return true;
        }
    }
    return false;
}

bool PixelArtCleanupWindow::cellRectInView(int frameIndex, int ax, int ay, double& x0, double& y0, double& x1, double& y1) const {
    if (!shown) return false;
    if (!resultSpace()) return cellRect(frameIndex, ax, ay, x0, y0, x1, y1);
    const cu::Result& result = shown->result;
    if (!result.animation) {
        x0 = ax - shown->cropX;
        y0 = ay - shown->cropY;
        x1 = x0 + 1;
        y1 = y0 + 1;
        return true;
    }
    // a sheet's Result is its atlas: the frame's cell, on its placed canvas, on its rectangle
    int clipIndex = 0;
    for (const cu::ClipOutcome* clip : cu::exportedClips(*result.animation)) {
        for (int position = 0; position < static_cast<int>(clip->placed.size()); position++) {
            const cu::ClipFrame& placed = clip->placed[static_cast<size_t>(position)];
            if (placed.index != frameIndex) continue;
            int placedX = 0, placedY = 0, fixedX = 0, fixedY = 0;
            if (frameIndex < 0 || frameIndex >= static_cast<int>(result.animation->frames.size()) || !firstOpaqueCell(placed.art, placedX, placedY) ||
                !firstOpaqueCell(result.animation->frames[static_cast<size_t>(frameIndex)].art, fixedX, fixedY)) {
                return false;
            }
            for (const cu::AtlasRect& rect : shown->atlasRects) {
                if (rect.clip != clipIndex || rect.frame != position) continue;
                x0 = rect.x + ax + placedX - fixedX;
                y0 = rect.y + ay + placedY - fixedY;
                x1 = x0 + 1;
                y1 = y0 + 1;
                return true;
            }
            return false;
        }
        clipIndex++;
    }
    return false;
}

bool PixelArtCleanupWindow::lintSpotRect(const pixelart::lint::LintSpot& spot, double& x0, double& y0, double& x1, double& y1) const {
    if (!shown) return false;
    const cu::Result& result = shown->result;
    if (result.animation) {
        // an exported frame: numbered from 1 across the exported clips, in order
        int remaining = spot.frame - 1;
        int clipIndex = 0;
        for (const cu::ClipOutcome* clip : cu::exportedClips(*result.animation)) {
            if (remaining >= static_cast<int>(clip->placed.size())) {
                remaining -= static_cast<int>(clip->placed.size());
                clipIndex++;
                continue;
            }
            const cu::ClipFrame& placed = clip->placed[static_cast<size_t>(remaining)];
            if (resultSpace()) {
                for (const cu::AtlasRect& rect : shown->atlasRects) {
                    if (rect.clip == clipIndex && rect.frame == remaining) {
                        x0 = rect.x + spot.x, y0 = rect.y + spot.y;
                        x1 = x0 + 1, y1 = y0 + 1;
                        return true;
                    }
                }
                return false;
            }
            // a spot is on the clip's canvas: back to the frame's own cells (anchor, padding, nudge)
            int ax = spot.x, ay = spot.y;
            int placedX, placedY, fixedX, fixedY;
            if (placed.index >= 0 && placed.index < static_cast<int>(result.animation->frames.size()) && firstOpaqueCell(placed.art, placedX, placedY) &&
                firstOpaqueCell(result.animation->frames[static_cast<size_t>(placed.index)].art, fixedX, fixedY)) {
                ax += fixedX - placedX;
                ay += fixedY - placedY;
            }
            return cellRect(placed.index, ax, ay, x0, y0, x1, y1);
        }
        return false;
    }
    if (resultSpace()) {
        x0 = spot.x, y0 = spot.y;
        x1 = x0 + 1, y1 = y0 + 1;
        return true;
    }
    return cellRect(-1, spot.x + shown->cropX, spot.y + shown->cropY, x0, y0, x1, y1);
}

void PixelArtCleanupWindow::focusOn(double x0, double y0, double x1, double y1, bool flash) {
    const ImVec2 size = viewImageSize();
    const double x = (x0 + x1) * 0.5, y = (y0 + y1) * 0.5;
    // a cell at least 16 screen pixels wide, never zooming out further than the fit
    const double cell = std::max(1.0, x1 - x0);
    float target = zoom;
    for (float step : CLEANUP_ZOOM_STEPS) {
        if (step * cell >= 16.0) {
            target = step;
            break;
        }
    }
    zoom = std::max(zoom, target);
    pan.x = (size.x * 0.5f - static_cast<float>(x)) * zoom;
    pan.y = (size.y * 0.5f - static_cast<float>(y)) * zoom;
    fitRequested = false;
    if (flash) {
        flashSourceX = x;
        flashSourceY = y;
        flashCellW = std::max(1.0, x1 - x0);
        flashCellH = std::max(1.0, y1 - y0);
        flashUntil = cleanupNow() + 1.0;
    }
}

void PixelArtCleanupWindow::selectLintSpot(int lintIndex, int spotIndex) {
    if (!shown) return;
    const auto& results = shown->result.report.lints.results;
    if (lintIndex < 0 || lintIndex >= static_cast<int>(results.size())) return;
    const auto& at = results[static_cast<size_t>(lintIndex)].at;
    if (at.empty()) return;
    spotIndex = std::clamp(spotIndex, 0, static_cast<int>(at.size()) - 1);
    lintSelected = lintIndex;
    spotSelected = spotIndex;
    double x0, y0, x1, y1;
    if (!lintSpotRect(at[static_cast<size_t>(spotIndex)], x0, y0, x1, y1)) return;
    focusOn(x0, y0, x1, y1, true);
}

void PixelArtCleanupWindow::nudgeCurrentFrame(int dx, int dy) {
    if (!shown || !shown->result.animation) return;
    const auto& clips = shown->result.animation->clips;
    if (animationSelected < 0 || animationSelected >= static_cast<int>(clips.size())) return;
    const auto& placed = clips[static_cast<size_t>(animationSelected)].placed;
    if (frame < 0 || frame >= static_cast<int>(placed.size())) return;
    const int sheetIndex = placed[static_cast<size_t>(frame)].index;
    const cu::Result* result = this->result();
    applyEdit([&](cu::Recipe& r) {
        cu::Sheet& sheet = givenSheet(r, result);
        if (sheetIndex >= 0 && sheetIndex < static_cast<int>(sheet.frames.size())) {
            sheet.frames[static_cast<size_t>(sheetIndex)].nudge.x += dx;
            sheet.frames[static_cast<size_t>(sheetIndex)].nudge.y += dy;
        }
    });
}

const cu::Sheet* PixelArtCleanupWindow::currentSheet() const {
    if (recipe.sheetMode == cu::SheetMode::Given && recipe.sheet) return &*recipe.sheet;
    return shown && shown->result.animation ? &shown->result.animation->sheet : nullptr;
}

// ---- hand tools

void PixelArtCleanupWindow::beginStroke(int ax, int ay, bool area) {
    if (!shown || !shown->result.outcome) return;
    const cu::PixelArt& art = shown->result.outcome->art;
    if (ax < 0 || ay < 0 || ax >= art.width || ay >= art.height) return;
    beginEdit();
    painting = true;
    const std::string size = std::to_string(art.width) + "x" + std::to_string(art.height);
    if (recipe.edits.size != size) {
        recipe.edits.size = size;
        recipe.edits.strokes.clear();
    }
    recipe.edits.strokes.emplace_back();
    if (area) {
        cu::Edit edit;
        edit.x = ax;
        edit.y = ay;
        edit.area = true;
        edit.tool = tool == HandTool::Erase ? cu::EditTool::Erase : tool == HandTool::Restore ? cu::EditTool::Restore : cu::EditTool::Paint;
        edit.color = paintColor;
        recipe.edits.strokes.back().push_back(edit);
        edited();
    } else {
        addStroke(ax, ay);
    }
}

void PixelArtCleanupWindow::addStroke(int ax, int ay) {
    if (recipe.edits.strokes.empty() || !shown || !shown->result.outcome) return;
    const cu::PixelArt& art = shown->result.outcome->art;
    if (ax < 0 || ay < 0 || ax >= art.width || ay >= art.height) return;
    std::vector<cu::Edit>& stroke = recipe.edits.strokes.back();
    if (!stroke.empty() && stroke.back().x == ax && stroke.back().y == ay) return;
    cu::Edit edit;
    edit.x = ax;
    edit.y = ay;
    edit.tool = tool == HandTool::Erase ? cu::EditTool::Erase : tool == HandTool::Restore ? cu::EditTool::Restore : cu::EditTool::Paint;
    edit.color = paintColor;
    stroke.push_back(edit);
    edited();
}

// ---- shortcuts

void PixelArtCleanupWindow::stepLintSpot(int step) {
    if (tab == Tab::Lints && lintSelected >= 0 && shown) {
        // the next or previous spot, across the lints that have some
        const auto& results = shown->result.report.lints.results;
        if (results.empty()) return;
        int lint = lintSelected, spot = spotSelected + step;
        for (size_t guard = 0; guard <= results.size(); guard++) {
            const int count = static_cast<int>(results[static_cast<size_t>(lint)].at.size());
            if (spot >= 0 && spot < count) {
                selectLintSpot(lint, spot);
                return;
            }
            lint = (lint + step + static_cast<int>(results.size())) % static_cast<int>(results.size());
            const int nextCount = static_cast<int>(results[static_cast<size_t>(lint)].at.size());
            spot = step > 0 ? 0 : nextCount - 1;
        }
    }
}

// ====================================================================== UI

void PixelArtCleanupWindow::acceptDrop() {
    const ImRect rect = ImGui::GetCurrentWindow()->InnerRect;
    if (!ImGui::BeginDragDropTargetCustom(rect, ImGui::GetID("##cleanup_drop"))) return;
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")) {
        const char* data = static_cast<const char*>(payload->Data);
        size_t offset = 0;
        while (offset < static_cast<size_t>(payload->DataSize)) {
            const std::string file(data + offset, strnlen(data + offset, static_cast<size_t>(payload->DataSize) - offset));
            offset += file.size() + 1;
            if (file.empty() || !isCleanupSourceFile(file)) continue;
            std::string error;
            if (!open(fs::u8path(file), error)) setStatus(error, true);
            break;
        }
    }
    ImGui::EndDragDropTarget();
}

void PixelArtCleanupWindow::drawEmptyState(const ImVec2& area) {
    const char* title = "Clean up AI or loose pixel art";
    const char* hint = "Drop an image here from the Project window, or:";
    const float buttonWidth = Theme::dpi(150.0f);
    const float totalHeight = ImGui::GetTextLineHeightWithSpacing() * 2.0f + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y * 2.0f;
    const ImVec2 start = ImGui::GetCursorPos();
    ImGui::SetCursorPos(ImVec2(start.x + (area.x - ImGui::CalcTextSize(title).x) * 0.5f, start.y + (area.y - totalHeight) * 0.45f));
    ImGui::TextUnformatted(title);
    ImGui::SetCursorPosX(start.x + (area.x - ImGui::CalcTextSize(hint).x) * 0.5f);
    ImGui::TextColored(Theme::Colors::SubtleText, "%s", hint);
    ImGui::SetCursorPosX(start.x + (area.x - buttonWidth * 2.0f - ImGui::GetStyle().ItemSpacing.x) * 0.5f);
    if (ImGui::Button("Open Image...", ImVec2(buttonWidth, 0))) {
        const std::string chosen = FileDialogs::openFileDialogFiltered(project ? FileUtils::pathToUtf8(project->getAssetsPath()) : std::string(), "Images and Aseprite",
                                                                       "png,jpg,jpeg,bmp,tga,psd,gif,aseprite,ase");
        if (!chosen.empty()) {
            std::string error;
            if (!open(fs::u8path(chosen), error)) setStatus(error, true);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Try a Demo", ImVec2(buttonWidth, 0))) openDemo();
    ImGui::SetItemTooltip("A fake AI sprite (CleanupSynthetic): see what the cleanup does in one click");
    if (!statusMessage.empty()) ImGui::TextColored(statusError ? Theme::Colors::ErrorText : Theme::Colors::SubtleText, "%s", statusMessage.c_str());
}

void PixelArtCleanupWindow::drawToolbar() {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(sourceName.c_str());
    ImGui::SetItemTooltip("The source image. Drop another one on the window to replace it.");
    ImGui::SameLine();
    const float comboWidth = Theme::dpi(80.0f);
    static const char* gridNames[] = {"Auto", "Cells", "Size"};
    int gridKind = static_cast<int>(recipe.grid.kind);
    ImGui::TextUnformatted("Grid");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(comboWidth);
    if (ImGui::Combo("##cleanup_grid", &gridKind, gridNames, 3)) {
        const cu::GridKind next = static_cast<cu::GridKind>(gridKind);
        applyEdit([&](cu::Recipe& r) {
            r.grid.kind = next;
            if (next == cu::GridKind::Cells && r.grid.spec.cellWidth <= 0.0 && shown && shown->result.report.grid) {
                const cu::ReportGrid& found = *shown->result.report.grid;
                r.grid.spec = {found.cellWidth, found.cellHeight, found.offsetX, found.offsetY};
            }
            if (next == cu::GridKind::Size && r.grid.width <= 0.0) r.grid.width = shown ? std::max(1, shown->result.report.outputWidth) : 32;
        }, true);
    }
    auto numberField = [this](const char* id, double& field, double lo, double hi, const char* tip) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(Theme::dpi(56.0f));
        double value = field;
        if (ImGui::DragScalar(id, ImGuiDataType_Double, &value, 0.05f, &lo, &hi, "%.2f")) {
            beginEdit();
            field = std::clamp(value, lo, hi);
            edited(true);
        }
        if (ImGui::IsItemDeactivated()) commitEdit();
        ImGui::SetItemTooltip("%s", tip);
    };
    if (recipe.grid.kind == cu::GridKind::Cells) {
        numberField("##cleanup_cell_w", recipe.grid.spec.cellWidth, 1.0, 128.0, "Cell Width, source pixels");
        numberField("##cleanup_cell_h", recipe.grid.spec.cellHeight, 1.0, 128.0, "Cell Height, source pixels");
        numberField("##cleanup_offset_x", recipe.grid.spec.offsetX, -128.0, 128.0, "Offset X, source pixels");
        numberField("##cleanup_offset_y", recipe.grid.spec.offsetY, -128.0, 128.0, "Offset Y, source pixels");
    } else if (recipe.grid.kind == cu::GridKind::Size) {
        numberField("##cleanup_size_w", recipe.grid.width, 1.0, 4096.0, "Width of the result in pixels");
    }
    static const char* followNames[] = {"Off", "Straight", "Bend"};
    int follow = static_cast<int>(recipe.follow);
    ImGui::SameLine();
    ImGui::TextUnformatted("Follow");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(comboWidth);
    if (ImGui::Combo("##cleanup_follow", &follow, followNames, 3)) {
        applyEdit([&](cu::Recipe& r) { r.follow = static_cast<cu::Follow>(follow); }, true);
    }
    static const char* outlineNames[] = {"Auto", "On", "Off"};
    int outlines = static_cast<int>(recipe.outlines);
    ImGui::SameLine();
    ImGui::TextUnformatted("Outlines");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(comboWidth);
    if (ImGui::Combo("##cleanup_outlines", &outlines, outlineNames, 3)) {
        applyEdit([&](cu::Recipe& r) { r.outlines = static_cast<cu::Outlines>(outlines); }, true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Detect Again")) {
        applyEdit([](cu::Recipe& r) {
            r.grid = cu::GridChoice();
            r.moved.clear();
        }, true);
    }
    ImGui::SetItemTooltip("Find the grid again and forget the lines dragged by hand");

    // the verdict badge, right-aligned
    const cu::Report* report = shown ? &shown->result.report : nullptr;
    char badge[64];
    if (report) std::snprintf(badge, sizeof(badge), "%s %d%%###cleanup_verdict", verdictLabel(report->verdict), static_cast<int>(std::round(report->confidence * 100.0)));
    else std::snprintf(badge, sizeof(badge), "...###cleanup_verdict");
    const ImVec2 badgeSize = ImGui::CalcTextSize(badge, nullptr, true);
    const float badgeWidth = badgeSize.x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - badgeWidth));
    const ImVec4 color = report ? verdictColor(report->verdict) : ImVec4(0.4f, 0.4f, 0.4f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, color);
    ImGui::Button(badge);
    ImGui::PopStyleColor(3);
    if (report && ImGui::IsItemHovered()) {
        std::string tip = report->summary;
        for (const cu::Guard& guard : report->guards) {
            char line[160];
            std::snprintf(line, sizeof(line), "\n%s: %.3g (limit %.3g)", guard.id.c_str(), guard.value, guard.threshold);
            tip += line;
        }
        ImGui::SetTooltip("%s", tip.c_str());
    }
}

void PixelArtCleanupWindow::drawFooter() {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Output");
    ImGui::SameLine();
    char buffer[512];
    std::snprintf(buffer, sizeof(buffer), "%s", outputPath.c_str());
    ImGui::SetNextItemWidth(Theme::dpi(260.0f));
    ImGui::BeginDisabled(isDemo);
    if (ImGui::InputText("##cleanup_output", buffer, sizeof(buffer))) outputPath = buffer;
    ImGui::SameLine();
    ImGui::TextUnformatted("Format");
    ImGui::SameLine();
    static const char* formatNames[] = {"PNG", "Aseprite"};
    int formatIndex = outputFormat == cu::OutputFormat::Aseprite ? 1 : 0;
    ImGui::SetNextItemWidth(Theme::dpi(90.0f));
    if (ImGui::Combo("##cleanup_format", &formatIndex, formatNames, 2)) {
        outputFormat = formatIndex == 1 ? cu::OutputFormat::Aseprite : cu::OutputFormat::Png;
        outputFormatPicked = true;
        // the default output name follows the format
        outputPath.clear();
        dirty = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!shown || isDemo);
    if (ImGui::Button("Write")) {
        std::string error;
        if (!write(error)) setStatus(error, true);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Write the result (%s). One undo step in the Project window.", CommandCatalog::editor().menuShortcutText("pixelArtCleanup.write").c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(!shown || isDemo || (prepared && isCleanupAsepritePath(prepared->sourceAbsolute)));
    if (ImGui::Button("Save as Import Step")) {
        std::string error;
        if (!saveAsImportStep(error)) setStatus(error, true);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Cook this cleanup instead of the source when the texture is imported. The source stays as it is.");
    ImGui::SameLine();
    if (ImGui::Button("Copy Recipe")) {
        copiedRecipe = recipeJson();
        ImGui::SetClipboardText(copiedRecipe.c_str());
        setStatus("The recipe is on the clipboard.", false);
    }
    ImGui::SetItemTooltip("The recipe as JSON, for fix_pixel_art or fix-pixel-art");
    ImGui::SameLine();
    ImGui::BeginDisabled(written.empty());
    if (ImGui::Button("Open in Aseprite")) {
        pixelart::aseprite::openInAsepriteOrFallback(project, {written.front()}, false);
    }
    ImGui::EndDisabled();
    if (!statusMessage.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(statusError ? Theme::Colors::ErrorText : Theme::Colors::SubtleText, "%s", statusMessage.c_str());
    }
}

void PixelArtCleanupWindow::outlineHighlight(ImDrawList* draw, double x0, double y0, double x1, double y1, ImU32 color) {
    draw->AddRect(viewToScreen(x0, y0), viewToScreen(x1, y1), color, 0.0f, 0, Theme::dpi(1.5f));
}

void PixelArtCleanupWindow::drawHighlights(ImDrawList* draw, bool canvasHovered) {
    highlightText.clear();
    if (!shown) return;
    const ImU32 magenta = IM_COL32(255, 60, 220, 255);
    const cu::Report& report = shown->result.report;
    bool any = false;
    double minX = 0, minY = 0, maxX = 0, maxY = 0;
    auto include = [&](double x0, double y0, double x1, double y1) {
        if (!any) {
            minX = x0;
            minY = y0;
            maxX = x1;
            maxY = y1;
            any = true;
        }
        minX = std::min(minX, x0);
        minY = std::min(minY, y0);
        maxX = std::max(maxX, x1);
        maxY = std::max(maxY, y1);
    };
    if (hoverLostShown >= 0 && hoverLostShown < static_cast<int>(report.lost.size())) {
        const cu::ReportLost& lost = report.lost[static_cast<size_t>(hoverLostShown)];
        highlightText = "lost:" + std::to_string(hoverLostShown);
        for (const std::array<int, 2>& cell : lost.at) {
            double x0, y0, x1, y1;
            if (!cellRectInView(lost.frame < 0 ? -1 : lost.frame - 1, cell[0], cell[1], x0, y0, x1, y1)) continue;
            outlineHighlight(draw, x0, y0, x1, y1, magenta);
            include(x0, y0, x1, y1);
        }
    }
    if (hoverLintShown >= 0 && hoverLintShown < static_cast<int>(report.lints.results.size())) {
        highlightText = "lint:" + std::to_string(hoverLintShown);
        for (const pixelart::lint::LintSpot& spot : report.lints.results[static_cast<size_t>(hoverLintShown)].at) {
            double x0, y0, x1, y1;
            if (!lintSpotRect(spot, x0, y0, x1, y1)) continue;
            outlineHighlight(draw, x0, y0, x1, y1, magenta);
            include(x0, y0, x1, y1);
        }
    }
    if (flashUntil > cleanupNow()) {
        const double hw = flashCellW * 0.5, hh = flashCellH * 0.5;
        const bool beat = std::fmod((flashUntil - cleanupNow()) * 6.0, 2.0) > 1.0;
        outlineHighlight(draw, flashSourceX - hw, flashSourceY - hh, flashSourceX + hw, flashSourceY + hh, beat ? magenta : IM_COL32(255, 255, 255, 255));
        include(flashSourceX - hw, flashSourceY - hh, flashSourceX + hw, flashSourceY + hh);
        highlightText = "spot:" + std::to_string(lintSelected) + ":" + std::to_string(spotSelected);
    }
    if (any && !canvasHovered) {
        // find_ui_item's handle on the outlines (the mouse is over the list that lit them)
        const ImVec2 a = viewToScreen(minX, minY), b = viewToScreen(maxX, maxY);
        const ImRect rect(a, ImVec2(std::max(a.x + 1.0f, b.x), std::max(a.y + 1.0f, b.y)));
        const ImGuiID id = ImGui::GetID("##cleanup_highlight");
        ImGuiContext& g = *GImGui;   // the macro's
        if (ImGui::ItemAdd(rect, id)) {
            IMGUI_TEST_ENGINE_ITEM_INFO(id, "##cleanup_highlight", ImGuiItemStatusFlags_None);
        }
        UIProbe::rect("pixelArtCleanup.highlight", rect.Min.x, rect.Min.y, rect.Max.x, rect.Max.y);
    }
}

void PixelArtCleanupWindow::drawCanvasGrid(ImDrawList* draw, const ImVec2& origin, const ImVec2& area) {
    hoveredAxis = 0;
    hoveredLine = -1;
    if (!shown || !shown->result.outcome || resultSpace() || !showGrid) return;
    const cu::Grid& grid = shown->result.outcome->grid;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const ImVec2 mv = screenToView(mouse);
    const float hit = Theme::dpi(4.0f);
    const float clipRight = view == View::Compare ? origin.x + area.x * splitPosition : origin.x + area.x;
    const bool bent = grid.bend.has_value();
    const ImU32 line = IM_COL32(120, 200, 255, 110);
    const ImU32 strong = IM_COL32(255, 220, 90, 255);
    const bool overCanvas = mouse.x >= origin.x && mouse.y >= origin.y && mouse.x < origin.x + area.x && mouse.y < origin.y + area.y;
    draw->PushClipRect(origin, ImVec2(clipRight, origin.y + area.y), true);
    for (int i = 0; i < static_cast<int>(grid.xs.size()); i++) {
        const bool hovered = showLines && overCanvas && mouse.x <= clipRight && std::fabs(viewToScreen(grid.xs[i], 0).x - mouse.x) <= hit && !bent;
        if (bent && i < static_cast<int>(grid.bend->xs.size())) {
            const std::vector<double>& bends = grid.bend->xs[static_cast<size_t>(i)];
            bool barHovered = false;
            for (int j = 0; j + 1 < static_cast<int>(grid.ys.size()) && j < static_cast<int>(bends.size()); j++) {
                const ImVec2 a = viewToScreen(bends[j], grid.ys[j]), b = viewToScreen(j + 1 < static_cast<int>(bends.size()) ? bends[j + 1] : bends[j], grid.ys[j + 1]);
                draw->AddLine(a, b, line);
                if (showLines && overCanvas && mv.y >= grid.ys[j] && mv.y <= grid.ys[j + 1] && std::fabs(a.x - mouse.x) <= hit) barHovered = true;
            }
            if (barHovered) {
                hoveredAxis = 'x';
                hoveredLine = i;
            }
        } else {
            const float x = viewToScreen(grid.xs[i], 0).x;
            const bool active = draggingAxis == 'x' && draggingLine == i;
            draw->AddLine(ImVec2(x, std::max(origin.y, viewToScreen(0, 0).y)), ImVec2(x, std::min(origin.y + area.y, viewToScreen(0, grid.ys.back()).y)), hovered || active ? strong : line, hovered || active ? 2.0f : 1.0f);
            if (hovered) {
                hoveredAxis = 'x';
                hoveredLine = i;
            }
        }
    }
    for (int i = 0; i < static_cast<int>(grid.ys.size()); i++) {
        const float y = viewToScreen(0, grid.ys[i]).y;
        const bool hovered = showLines && overCanvas && mouse.x <= clipRight && std::fabs(y - mouse.y) <= hit && hoveredLine < 0 && !bent;
        const bool active = draggingAxis == 'y' && draggingLine == i;
        if (bent && i < static_cast<int>(grid.bend->ys.size())) {
            const std::vector<double>& bends = grid.bend->ys[static_cast<size_t>(i)];
            for (int j = 0; j + 1 < static_cast<int>(grid.xs.size()) && j < static_cast<int>(bends.size()); j++) {
                draw->AddLine(viewToScreen(grid.xs[j], bends[j]), viewToScreen(grid.xs[j + 1], j + 1 < static_cast<int>(bends.size()) ? bends[j + 1] : bends[j]), line);
            }
            if (showLines && overCanvas && hoveredLine < 0 && std::fabs(y - mouse.y) <= hit) {
                hoveredAxis = 'y';
                hoveredLine = i;
            }
        } else {
            draw->AddLine(ImVec2(viewToScreen(0, 0).x, y), ImVec2(std::min(clipRight, viewToScreen(grid.xs.back(), 0).x), y), hovered || active ? strong : line, hovered || active ? 2.0f : 1.0f);
            if (hovered) {
                hoveredAxis = 'y';
                hoveredLine = i;
            }
        }
    }
    draw->PopClipRect();
    // a drag holds the line even as the mouse outruns it
    if (draggingAxis != 0) {
        hoveredAxis = draggingAxis;
        hoveredLine = draggingLine;
    }
    if (showLines && hoveredLine >= 0) ImGui::SetMouseCursor(hoveredAxis == 'x' ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
}

void PixelArtCleanupWindow::drawFrameBoxes(ImDrawList* draw) {
    hoveredBox = -1;
    hoveredHandle = 0;
    if (tab != Tab::Frames || resultSpace() || !prepared || prepared->frames.size() != 1) return;
    const cu::Sheet* sheet = currentSheet();
    if (!sheet) return;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float handle = Theme::dpi(5.0f);
    for (int i = 0; i < static_cast<int>(sheet->frames.size()); i++) {
        const cu::SheetFrame& entry = sheet->frames[static_cast<size_t>(i)];
        const cu::FrameBox& box = entry.box;
        const ImVec2 a = viewToScreen(box.x, box.y), b = viewToScreen(box.x + box.width, box.y + box.height);
        const ImU32 color = !entry.include ? IM_COL32(150, 150, 150, 130) : i == draggingBox ? IM_COL32(255, 220, 90, 255) : IM_COL32(120, 255, 170, 220);
        draw->AddRect(a, b, color, 0.0f, 0, 1.5f);
        char label[16];
        std::snprintf(label, sizeof(label), "%d", i + 1);
        draw->AddText(ImVec2(a.x + 3, a.y + 2), color, label);
        if (!entry.include) {
            draw->AddLine(a, b, color);
            draw->AddLine(ImVec2(b.x, a.y), ImVec2(a.x, b.y), color);
        }
        const bool left = std::fabs(mouse.x - a.x) <= handle, right = std::fabs(mouse.x - b.x) <= handle;
        const bool top = std::fabs(mouse.y - a.y) <= handle, bottom = std::fabs(mouse.y - b.y) <= handle;
        const bool insideX = mouse.x >= a.x - handle && mouse.x <= b.x + handle, insideY = mouse.y >= a.y - handle && mouse.y <= b.y + handle;
        if (!(insideX && insideY)) continue;
        hoveredBox = i;
        hoveredHandle = (left && top) ? 1 : (right && top) ? 2 : (left && bottom) ? 3 : (right && bottom) ? 4 : left ? 5 : right ? 6 : top ? 7 : bottom ? 8 : 0;
    }
    if (draggingBox >= 0) {
        hoveredBox = draggingBox;
        hoveredHandle = boxHandle;
    }
    if (hoveredBox >= 0) {
        ImGui::SetMouseCursor(hoveredHandle == 0 ? ImGuiMouseCursor_ResizeAll : hoveredHandle == 5 || hoveredHandle == 6 ? ImGuiMouseCursor_ResizeEW :
                              hoveredHandle == 7 || hoveredHandle == 8 ? ImGuiMouseCursor_ResizeNS : (hoveredHandle == 1 || hoveredHandle == 4) ? ImGuiMouseCursor_ResizeNWSE : ImGuiMouseCursor_ResizeNESW);
    }
    if (addingBox) {
        const ImVec2 current = ImGui::GetIO().MousePos;
        draw->AddRect(addStart, current, IM_COL32(255, 220, 90, 255));
    }
}

void PixelArtCleanupWindow::handleFrameBoxInput(bool hovered) {
    const ImGuiIO& io = ImGui::GetIO();
    if (tab != Tab::Frames || resultSpace() || !prepared || prepared->frames.size() != 1) return;
    const cu::Result* currentResult = result();
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hoveredBox >= 0) {
        const int index = hoveredBox;
        applyEdit([&](cu::Recipe& r) {
            cu::Sheet& sheet = givenSheet(r, currentResult);
            if (index < static_cast<int>(sheet.frames.size())) sheet.frames[static_cast<size_t>(index)].include = !sheet.frames[static_cast<size_t>(index)].include;
        });
        draggingBox = -1;
        return;
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt) {
        if (hoveredBox >= 0 && draggingBox < 0) {
            const cu::Sheet* sheet = currentSheet();
            if (sheet && hoveredBox < static_cast<int>(sheet->frames.size())) {
                beginEdit();
                draggingBox = hoveredBox;
                boxHandle = hoveredHandle;
                dragStartMouse = io.MousePos;
                dragStartBox = sheet->frames[static_cast<size_t>(hoveredBox)].box;
            }
        } else if (hoveredBox < 0 && !ImGui::IsKeyDown(ImGuiKey_Space)) {
            addingBox = true;
            addStart = io.MousePos;
        }
    }
    if (draggingBox >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const int dx = static_cast<int>(std::round((io.MousePos.x - dragStartMouse.x) / zoom));
        const int dy = static_cast<int>(std::round((io.MousePos.y - dragStartMouse.y) / zoom));
        if (dx != 0 || dy != 0 || recipe.sheetMode == cu::SheetMode::Given) {
            cu::Sheet& sheet = givenSheet(recipe, currentResult);
            if (draggingBox < static_cast<int>(sheet.frames.size())) {
                cu::FrameBox box = dragStartBox;
                const int w = dragStartBox.width, h = dragStartBox.height;
                const bool moveLeft = boxHandle == 1 || boxHandle == 3 || boxHandle == 5;
                const bool moveRight = boxHandle == 2 || boxHandle == 4 || boxHandle == 6;
                const bool moveTop = boxHandle == 1 || boxHandle == 2 || boxHandle == 7;
                const bool moveBottom = boxHandle == 3 || boxHandle == 4 || boxHandle == 8;
                if (boxHandle == 0) {
                    box.x += dx;
                    box.y += dy;
                } else {
                    if (moveLeft) {
                        box.x = std::min(dragStartBox.x + dx, dragStartBox.x + w - 1);
                        box.width = dragStartBox.x + w - box.x;
                    }
                    if (moveRight) box.width = std::max(1, w + dx);
                    if (moveTop) {
                        box.y = std::min(dragStartBox.y + dy, dragStartBox.y + h - 1);
                        box.height = dragStartBox.y + h - box.y;
                    }
                    if (moveBottom) box.height = std::max(1, h + dy);
                }
                if (!(sheet.frames[static_cast<size_t>(draggingBox)].box == box)) {
                    sheet.frames[static_cast<size_t>(draggingBox)].box = box;
                    edited(false);
                }
            }
        }
    }
    if (addingBox && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        addingBox = false;
        const ImVec2 a = screenToView(addStart), b = screenToView(io.MousePos);
        const int x = static_cast<int>(std::floor(std::min(a.x, b.x))), y = static_cast<int>(std::floor(std::min(a.y, b.y)));
        const int w = static_cast<int>(std::ceil(std::fabs(b.x - a.x))), h = static_cast<int>(std::ceil(std::fabs(b.y - a.y)));
        if (w >= 4 && h >= 4) {
            const int selected = animationSelected;
            applyEdit([&](cu::Recipe& r) {
                cu::Sheet& sheet = givenSheet(r, currentResult);
                cu::SheetFrame added;
                added.box = {x, y, w, h};
                const int index = static_cast<int>(sheet.frames.size());
                sheet.frames.push_back(added);
                if (sheet.animations.empty()) {
                    cu::SheetAnimation animation;
                    animation.name = "animation-1";
                    sheet.animations.push_back(animation);
                }
                sheet.animations[static_cast<size_t>(std::clamp(selected, 0, static_cast<int>(sheet.animations.size()) - 1))].frames.push_back(index);
            });
        }
    }
    if (draggingBox >= 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        draggingBox = -1;
        commitEdit();
    }
}

void PixelArtCleanupWindow::handleHandTool(bool hovered) {
    if (tool == HandTool::None || !resultSpace() || !shown || !shown->result.outcome) return;
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 at = screenToView(io.MousePos);
    const int px = static_cast<int>(std::floor(at.x)), py = static_cast<int>(std::floor(at.y));
    const int ax = px + shown->cropX, ay = py + shown->cropY;
    if (tool == HandTool::Pick) {
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const pixelart::PixelRgba color = shown->image.at(px, py);
            if (color[3] > 0) {
                paintColor = {color[0], color[1], color[2], 255};
                setStatus("Picked a colour for Paint.", false);
            }
        }
        return;
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) beginStroke(ax, ay, io.KeyShift);
    else if (painting && ImGui::IsMouseDown(ImGuiMouseButton_Left) && !io.KeyShift) addStroke(ax, ay);
    if (painting && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        painting = false;
        commitEdit();
    }
}

void PixelArtCleanupWindow::handleCanvasInput(bool hovered, bool active, const ImVec2& origin, const ImVec2& area) {
    const ImGuiIO& io = ImGui::GetIO();
    if (hovered && io.MouseWheel != 0.0f) {
        const int steps = static_cast<int>(sizeof(CLEANUP_ZOOM_STEPS) / sizeof(CLEANUP_ZOOM_STEPS[0]));
        float next = zoom;
        if (io.MouseWheel > 0) {
            for (int i = 0; i < steps; i++) {
                if (CLEANUP_ZOOM_STEPS[i] > zoom + 1e-4f) {
                    next = CLEANUP_ZOOM_STEPS[i];
                    break;
                }
            }
        } else {
            for (int i = steps - 1; i >= 0; i--) {
                if (CLEANUP_ZOOM_STEPS[i] < zoom - 1e-4f) {
                    next = CLEANUP_ZOOM_STEPS[i];
                    break;
                }
            }
        }
        if (next != zoom) {
            const ImVec2 centre(origin.x + area.x * 0.5f + pan.x, origin.y + area.y * 0.5f + pan.y);
            const ImVec2 rel(io.MousePos.x - centre.x, io.MousePos.y - centre.y);
            const float applied = next / zoom;
            pan.x -= rel.x * (applied - 1.0f);
            pan.y -= rel.y * (applied - 1.0f);
            zoom = next;
        }
    }
    const bool spaceHeld = ImGui::IsKeyDown(ImGuiKey_Space);
    if (active && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) || (spaceHeld && ImGui::IsMouseDragging(ImGuiMouseButton_Left)))) {
        pan.x += io.MouseDelta.x;
        pan.y += io.MouseDelta.y;
    }
    if (spaceHeld) return;
    // the Compare divider first, then the grid lines, the boxes and the hand tools
    if (draggingSplit) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) draggingSplit = false;
        else splitPosition = std::clamp((io.MousePos.x - origin.x) / area.x, 0.02f, 0.98f);
        return;
    }
    if (view == View::Compare && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && std::fabs(io.MousePos.x - (origin.x + area.x * splitPosition)) <= Theme::dpi(4.0f)) {
        draggingSplit = true;
        return;
    }
    if (draggingAxis != 0) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            draggingAxis = 0;
            draggingLine = -1;
            commitEdit();
        } else if (shown && shown->result.outcome) {
            const cu::Grid& grid = shown->result.outcome->grid;
            const std::vector<double>& cuts = draggingAxis == 'x' ? grid.xs : grid.ys;
            const ImVec2 at = screenToView(io.MousePos);
            double position = draggingAxis == 'x' ? at.x : at.y;
            if (draggingLine > 0) position = std::max(position, cuts[static_cast<size_t>(draggingLine) - 1] + 0.5);
            if (draggingLine + 1 < static_cast<int>(cuts.size())) position = std::min(position, cuts[static_cast<size_t>(draggingLine) + 1] - 0.5);
            setGridLine(draggingAxis, draggingLine, position);
        }
        return;
    }
    if (showLines && hoveredLine >= 0 && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !resultSpace()) {
        beginEdit();
        draggingAxis = hoveredAxis;
        draggingLine = hoveredLine;
        return;
    }
    handleFrameBoxInput(hovered);
    if (tab != Tab::Frames) handleHandTool(hovered);
}

void PixelArtCleanupWindow::drawCanvas(const ImVec2& size) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 area(std::max(size.x, 1.0f), std::max(size.y, 1.0f));
    ImGui::InvisibleButton("##cleanup_canvas", area, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    UIProbe::item("pixelArtCleanup.canvas");
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const ImVec2 end(origin.x + area.x, origin.y + area.y);
    canvasOrigin = origin;
    canvasSize = area;
    draw->AddRectFilled(origin, end, IM_COL32(28, 28, 28, 255));
    draw->PushClipRect(origin, end, true);

    const ImVec2 imageSize = viewImageSize();
    if (fitRequested || zoom <= 0.0f) {
        const float margin = Theme::dpi(24.0f);
        const float fit = std::min((area.x - margin) / imageSize.x, (area.y - margin) / imageSize.y);
        zoom = fit >= 1.0f ? std::floor(fit) : std::max(0.05f, fit);
        pan = ImVec2(0, 0);
        fitRequested = false;
    }
    imageMin = ImVec2(std::floor(origin.x + (area.x - imageSize.x * zoom) * 0.5f + pan.x), std::floor(origin.y + (area.y - imageSize.y * zoom) * 0.5f + pan.y));
    const ImVec2 imageMax(imageMin.x + imageSize.x * zoom, imageMin.y + imageSize.y * zoom);

    auto image = [&](Gpu& gpu) {
        if (gpu.valid()) draw->AddImage(Backend::getImGuiTexture(gpu.texture.getRender()), imageMin, imageMax);
    };
    draw->AddRectFilled(imageMin, imageMax, IM_COL32(52, 52, 56, 255));
    std::string viewLabel;
    if (view == View::Result) {
        image(resultGpu);
        viewLabel = "Result";
    } else if (view == View::Source) {
        image(sourceGpu);
        viewLabel = "Source";
    } else {
        const float divider = origin.x + area.x * splitPosition;
        draw->PushClipRect(origin, ImVec2(divider, end.y), true);
        image(sourceGpu);
        draw->PopClipRect();
        draw->PushClipRect(ImVec2(divider, origin.y), end, true);
        image(compareGpu);
        draw->PopClipRect();
        draw->AddLine(ImVec2(divider, origin.y), ImVec2(divider, end.y), IM_COL32(230, 230, 230, 200), 2.0f);
        const char* label = "Source | Result";
        const float labelWidth = ImGui::CalcTextSize(label).x;
        draw->AddText(ImVec2(divider - labelWidth * 0.5f, origin.y + Theme::dpi(4.0f)), IM_COL32(230, 230, 230, 255), label);
        UIProbe::rect("pixelArtCleanup.divider", divider - Theme::dpi(4.0f), origin.y, divider + Theme::dpi(4.0f), end.y);
        if (std::fabs(ImGui::GetIO().MousePos.x - divider) <= Theme::dpi(4.0f) && hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        viewLabel = "Compare";
    }
    drawCanvasGrid(draw, origin, area);
    drawFrameBoxes(draw);
    drawHighlights(draw, hovered);

    char info[64];
    std::snprintf(info, sizeof(info), "%s  %d%%", viewLabel.c_str(), static_cast<int>(zoom * 100.0f));
    draw->AddText(ImVec2(origin.x + Theme::dpi(6.0f), origin.y + Theme::dpi(6.0f)), IM_COL32(220, 220, 220, 220), info);
    if (isRunning() || dirty) {
        const char* text = "Updating...";
        const ImVec2 size2 = ImGui::CalcTextSize(text);
        draw->AddText(ImVec2(end.x - size2.x - Theme::dpi(6.0f), origin.y + Theme::dpi(6.0f)), IM_COL32(255, 210, 90, 230), text);
    }
    draw->PopClipRect();
    handleCanvasInput(hovered, active, origin, area);
}

void PixelArtCleanupWindow::drawRightPanel() {
    hoverLost = -1;
    hoverLint = -1;
    if (ImGui::BeginTabBar("##cleanup_tabs")) {
        const ImGuiTabItemFlags want = ImGuiTabItemFlags_SetSelected;
        const auto tabFlags = [&](Tab which) { return tabRequested && tab == which ? want : ImGuiTabItemFlags_None; };
        const bool framesVisible = (shown && shown->result.animation) || (prepared && prepared->frames.size() > 1);
        auto beginTab = [&](const char* name, Tab which) {
            if (!ImGui::BeginTabItem(name, nullptr, tabFlags(which))) return false;
            tab = which;
            return true;
        };
        if (beginTab("Palette", Tab::Palette)) {
            drawPaletteTab();
            ImGui::EndTabItem();
        }
        if (beginTab("Lost Colors", Tab::LostColors)) {
            drawLostColorsTab();
            ImGui::EndTabItem();
        }
        if (beginTab("Cleanup", Tab::Cleanup)) {
            drawCleanupTab();
            ImGui::EndTabItem();
        }
        if (beginTab("Lints", Tab::Lints)) {
            drawLintsTab();
            ImGui::EndTabItem();
        }
        if (framesVisible && beginTab("Frames", Tab::Frames)) {
            drawFramesTab();
            ImGui::EndTabItem();
        }
        tabRequested = false;
        ImGui::EndTabBar();
    }
    hoverLostShown = hoverLost;
    hoverLintShown = hoverLint;
}

void PixelArtCleanupWindow::drawPaletteTab() {
    static const char* modes[] = {"Auto", "Palette Asset or Preset", "Own Colors"};
    const int modeIndex = recipe.paletteMode == cu::PaletteMode::None ? 2 : recipe.paletteMode == cu::PaletteMode::Fixed ? 1 : 0;
    int nextMode = modeIndex;
    ImGui::TextUnformatted("Palette");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##cleanup_palette_mode", &nextMode, modes, 3) && nextMode != modeIndex) {
        applyEdit([&](cu::Recipe& r) {
            if (nextMode == 0) {
                r.paletteMode = cu::PaletteMode::Auto;
                r.palette.reset();
                r.autoPalette = cu::AutoPalette();
            } else if (nextMode == 2) {
                r.paletteMode = cu::PaletteMode::None;
                r.palette.reset();
                r.autoPalette.reset();
            } else if (shown && shown->result.resolved.palette) {
                r.paletteMode = cu::PaletteMode::Fixed;
                r.palette = shown->result.resolved.palette;
                r.autoPalette.reset();
            }
        });
    }
    if (recipe.paletteMode == cu::PaletteMode::Auto || recipe.paletteMode == cu::PaletteMode::Default) {
        cu::AutoPalette current = recipe.autoPalette.value_or(cu::AutoPalette());
        static const char* methods[] = {"Dominant", "Balanced", "Distinct"};
        int method = static_cast<int>(current.method);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##cleanup_palette_method", &method, methods, 3)) {
            applyEdit([&](cu::Recipe& r) {
                r.paletteMode = cu::PaletteMode::Auto;
                r.palette.reset();
                cu::AutoPalette value = r.autoPalette.value_or(cu::AutoPalette());
                value.method = static_cast<cu::ProposeMethod>(method);
                r.autoPalette = value;
            });
        }
        int count = current.count;
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderInt("##cleanup_palette_count", &count, 2, 256, "Count %d", ImGuiSliderFlags_AlwaysClamp)) {
            beginEdit();
            recipe.paletteMode = cu::PaletteMode::Auto;
            recipe.palette.reset();
            current.count = count;
            recipe.autoPalette = current;
            edited();
        }
        if (ImGui::IsItemDeactivated()) commitEdit();
    } else if (recipe.paletteMode == cu::PaletteMode::Fixed && project) {
        std::string picked;
        const std::string currentPath = recipe.palette ? recipe.palette->assetPath : std::string();
        if (paletteAssetField("##cleanup_palette_asset", *project, currentPath, picked, false)) {
            const pixelart::ResolvedPalette resolved = pixelart::resolvePaletteName(*project, picked);
            if (!resolved.ok) {
                setStatus(resolved.error.message, true);
            } else {
                cu::RecipePalette palette;
                palette.name = resolved.palette.name;
                palette.assetPath = resolved.needsCopy ? std::string() : resolved.assetPath;
                for (jasperengine::pixelart::Rgba8 color : resolved.palette.colors) {
                    if (color.a == 0) continue;
                    color.a = 255;
                    palette.colors.push_back(color);
                }
                applyEdit([&](cu::Recipe& r) {
                    r.palette = palette;
                    r.paletteMode = cu::PaletteMode::Fixed;
                    r.autoPalette.reset();
                });
            }
        }
    }
    if (shown && shown->result.resolved.palette) {
        ImGui::Separator();
        const std::vector<jasperengine::pixelart::Rgba8>& colors = shown->result.resolved.palette->colors;
        const std::vector<int>& locked = recipe.palette ? recipe.palette->locked : shown->result.resolved.palette->locked;
        const float size = Theme::dpi(18.0f);
        for (int i = 0; i < static_cast<int>(colors.size()); i++) {
            const jasperengine::pixelart::Rgba8 color = colors[static_cast<size_t>(i)];
            ImGui::PushID(i);
            ImGui::ColorButton("##swatch", ImVec4(color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, 1.0f), ImGuiColorEditFlags_NoTooltip, ImVec2(size, size));
            ImGui::SameLine();
            const bool isLocked = std::find(locked.begin(), locked.end(), i) != locked.end();
            char lockLabel[48];
            std::snprintf(lockLabel, sizeof(lockLabel), "%s###cleanup_lock_%d", isLocked ? ICON_FA_LOCK : ICON_FA_LOCK_OPEN, i);
            if (ImGui::SmallButton(lockLabel)) {
                const std::optional<cu::RecipePalette> palette = shown->result.resolved.palette;
                applyEdit([&, i, isLocked](cu::Recipe& r) {
                    if (!r.palette) {
                        // a lock needs concrete colours: the auto palette as it was proposed
                        r.palette = palette;
                        r.paletteMode = cu::PaletteMode::Fixed;
                        r.autoPalette.reset();
                    }
                    std::vector<int>& list = r.palette->locked;
                    if (isLocked) list.erase(std::remove(list.begin(), list.end(), i), list.end());
                    else list.push_back(i);
                });
            }
            ImGui::SetItemTooltip("Locked colours stay exact and are never merged away.");
            ImGui::SameLine();
            const auto found = shown->colorCounts.find(jasperengine::pixelart::packRgb(color));
            const double share = shown->opaqueCount > 0 && found != shown->colorCounts.end() ? 100.0 * found->second / shown->opaqueCount : 0.0;
            ImGui::Text("#%02x%02x%02x  %.1f%%", color.r, color.g, color.b, share);
            ImGui::PopID();
        }
    }
    ImGui::Separator();
    static const char* metrics[] = {"OKLab", "CIEDE2000"};
    int metric = recipe.metric == jasperengine::pixelart::ColorMetric::Ciede2000 ? 1 : 0;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##cleanup_metric", &metric, metrics, 2)) {
        applyEdit([&](cu::Recipe& r) { r.metric = metric == 1 ? jasperengine::pixelart::ColorMetric::Ciede2000 : jasperengine::pixelart::ColorMetric::OkLab; });
    }
    bool dither = recipe.dither == cu::Dither::Ordered;
    if (ImGui::Checkbox("Dither", &dither)) {
        applyEdit([&](cu::Recipe& r) { r.dither = dither ? cu::Dither::Ordered : cu::Dither::None; });
    }
    if (dither) {
        double value = recipe.ditherStrength, lo = 0.0, hi = 1.0;
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderScalar("##cleanup_dither_strength", ImGuiDataType_Double, &value, &lo, &hi, "Strength %.2f")) {
            beginEdit();
            recipe.ditherStrength = value;
            edited();
        }
        if (ImGui::IsItemDeactivated()) commitEdit();
    }
}

void PixelArtCleanupWindow::drawLostColorsTab() {
    if (!shown || shown->result.report.lost.empty()) {
        ImGui::TextDisabled("No colors were lost.");
        return;
    }
    const std::vector<cu::ReportLost>& lost = shown->result.report.lost;
    const float size = Theme::dpi(18.0f);
    for (int i = 0; i < static_cast<int>(lost.size()); i++) {
        const cu::ReportLost& entry = lost[static_cast<size_t>(i)];
        ImGui::PushID(i);
        char label[96];
        std::snprintf(label, sizeof(label), "%d px###cleanup_lost_%d", entry.pixels, i);
        const float avail = ImGui::GetContentRegionAvail().x;
        if (ImGui::Selectable(label, false, ImGuiSelectableFlags_AllowOverlap, ImVec2(avail - Theme::dpi(160.0f), size)) && !entry.at.empty()) {
            double x0, y0, x1, y1;
            if (cellRectInView(entry.frame < 0 ? -1 : entry.frame - 1, entry.at[0][0], entry.at[0][1], x0, y0, x1, y1)) {
                focusOn(x0, y0, x1, y1, true);
            }
        }
        if (ImGui::IsItemHovered()) hoverLost = i;
        ImGui::SameLine();
        ImGui::ColorButton("##color", ImVec4(entry.color.r / 255.0f, entry.color.g / 255.0f, entry.color.b / 255.0f, 1.0f), ImGuiColorEditFlags_NoTooltip, ImVec2(size, size));
        ImGui::SameLine();
        ImGui::TextDisabled(ICON_FA_ARROW_RIGHT);
        ImGui::SameLine();
        ImGui::ColorButton("##became", ImVec4(entry.became.r / 255.0f, entry.became.g / 255.0f, entry.became.b / 255.0f, 1.0f), ImGuiColorEditFlags_NoTooltip, ImVec2(size, size));
        ImGui::SameLine();
        if (ImGui::SmallButton("Add to Palette")) {
            const jasperengine::pixelart::Rgba8 add = entry.color;
            const std::optional<cu::RecipePalette> palette = shown->result.resolved.palette;
            applyEdit([&](cu::Recipe& r) {
                if (!r.palette) {
                    r.palette = palette ? *palette : cu::RecipePalette();
                    r.paletteMode = cu::PaletteMode::Fixed;
                    r.autoPalette.reset();
                }
                jasperengine::pixelart::Rgba8 opaque = add;
                opaque.a = 255;
                r.palette->colors.push_back(opaque);
            });
        }
        ImGui::PopID();
    }
}

void PixelArtCleanupWindow::drawCleanupTab() {
    auto slide = [this](const char* label, double& field, double lo, double hi, const char* format) {
        double value = field;
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderScalar(label, ImGuiDataType_Double, &value, &lo, &hi, format, ImGuiSliderFlags_AlwaysClamp)) {
            beginEdit();
            field = value;
            edited();
        }
        if (ImGui::IsItemDeactivated()) commitEdit();
    };
    auto checkbox = [this](const char* label, bool value, const std::function<void(cu::Recipe&, bool)>& set) {
        bool shownValue = value;
        if (ImGui::Checkbox(label, &shownValue)) applyEdit([&, shownValue](cu::Recipe& r) { set(r, shownValue); });
    };

    ImGui::SeparatorText("Specks");
    slide("##cleanup_speck_size", recipe.cleanup.size, 0.0, 16.0, "Size %.0f");
    slide("##cleanup_speck_strength", recipe.cleanup.strength, 0.0, 1.0, "Strength %.2f");

    ImGui::SeparatorText("Remove Background");
    checkbox("Remove Background", recipe.removeBackground, [](cu::Recipe& r, bool v) { r.removeBackground = v; });
    if (recipe.removeBackground) {
        slide("##cleanup_bg_tolerance", recipe.background.tolerance, 0.0, 0.3, "Tolerance %.3f");
        checkbox("Enclosed", recipe.background.enclosed, [](cu::Recipe& r, bool v) { r.background.enclosed = v; });
        checkbox("Specks##bg", recipe.background.specks, [](cu::Recipe& r, bool v) { r.background.specks = v; });
        checkbox("Shadow", recipe.background.shadow, [](cu::Recipe& r, bool v) { r.background.shadow = v; });
    }

    ImGui::SeparatorText("Alpha");
    slide("##cleanup_alpha_cutoff", recipe.alphaCutoff, 0.0, 255.0, "Alpha Cutoff %.0f");

    ImGui::SeparatorText("Shrink");
    bool shrink = recipe.targetWidth.has_value();
    if (ImGui::Checkbox("Shrink To Width", &shrink)) {
        applyEdit([&](cu::Recipe& r) {
            if (shrink) r.targetWidth = shown ? std::max(1, shown->result.report.outputWidth) : 32;
            else r.targetWidth.reset();
        });
    }
    if (recipe.targetWidth) {
        double width = *recipe.targetWidth;
        double lo = 1.0, hi = 4096.0;
        ImGui::SetNextItemWidth(-1);
        if (ImGui::DragScalar("##cleanup_target_width", ImGuiDataType_Double, &width, 0.5f, &lo, &hi, "Target Width %.0f")) {
            beginEdit();
            recipe.targetWidth = width;
            edited();
        }
        if (ImGui::IsItemDeactivated()) commitEdit();
        int mode = recipe.reduceMode == cu::ReduceMode::Outlines ? 0 : 1;
        static const char* modes[] = {"Keep Outlines", "Most Common Color"};
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##cleanup_reduce_mode", &mode, modes, 2)) {
            applyEdit([&](cu::Recipe& r) { r.reduceMode = mode == 0 ? cu::ReduceMode::Outlines : cu::ReduceMode::Common; });
        }
    }
    checkbox("Crop", recipe.crop, [](cu::Recipe& r, bool v) { r.crop = v; });

    ImGui::SeparatorText("Hand Tools");
    const bool single = shown && !shown->result.animation;
    ImGui::BeginDisabled(!single);
    auto toolButton = [&](const char* label, HandTool which) {
        const bool on = tool == which;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, Theme::Colors::Selection);
        if (ImGui::Button(label)) {
            tool = on ? HandTool::None : which;
            if (tool != HandTool::None) setView(View::Result);
        }
        if (on) ImGui::PopStyleColor();
        ImGui::SameLine();
    };
    toolButton(ICON_FA_ERASER " Erase", HandTool::Erase);
    toolButton("Restore", HandTool::Restore);
    toolButton(ICON_FA_PAINTBRUSH " Paint", HandTool::Paint);
    toolButton(ICON_FA_EYE_DROPPER " Pick", HandTool::Pick);
    ImGui::NewLine();
    ImGui::EndDisabled();
    ImGui::ColorButton("##cleanup_paint_color", ImVec4(paintColor.r / 255.0f, paintColor.g / 255.0f, paintColor.b / 255.0f, 1.0f), ImGuiColorEditFlags_NoTooltip);
    ImGui::SameLine();
    ImGui::TextColored(Theme::Colors::SubtleText, "Shift: the whole area. Paint snaps to the palette.");
}

void PixelArtCleanupWindow::drawLintsTab() {
    if (!shown) {
        ImGui::TextDisabled("Running...");
        return;
    }
    const std::vector<pixelart::lint::LintResult>& results = shown->result.report.lints.results;
    using LS = pixelart::lint::LintStatus;
    for (int i = 0; i < static_cast<int>(results.size()); i++) {
        const pixelart::lint::LintResult& lint = results[static_cast<size_t>(i)];
        const ImVec4 color = lint.status == LS::Fail ? Theme::Colors::ErrorText : lint.status == LS::Warn ? Theme::Colors::WarningText :
                             lint.status == LS::Pass ? Theme::Colors::DisabledGreenText : Theme::Colors::SubtleText;
        const char* icon = lint.status == LS::Fail ? ICON_FA_CIRCLE_XMARK : lint.status == LS::Warn ? ICON_FA_TRIANGLE_EXCLAMATION : lint.status == LS::Pass ? ICON_FA_CIRCLE_CHECK : ICON_FA_MINUS;
        ImGui::PushID(i);
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextUnformatted(icon);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        char label[256];
        std::snprintf(label, sizeof(label), "%s  %.3g / %.3g###cleanup_lint_%d", lint.id.c_str(), lint.measured, lint.threshold, i);
        const bool selected = lintSelected == i;
        if (ImGui::Selectable(label, selected)) {
            if (lint.at.empty()) {
                lintSelected = i;
                spotSelected = -1;
            } else {
                selectLintSpot(i, 0);
            }
        }
        if (ImGui::IsItemHovered()) {
            hoverLint = i;
            if (!lint.message.empty()) ImGui::SetTooltip("%s", lint.message.c_str());
        }
        ImGui::PopID();
    }
}

void PixelArtCleanupWindow::uploadPlayer() {
    if (!shown || !shown->result.animation) {
        retire(playerGpu);
        return;
    }
    const auto& clips = shown->result.animation->clips;
    if (animationSelected < 0 || animationSelected >= static_cast<int>(clips.size())) {
        retire(playerGpu);
        return;
    }
    const cu::ClipOutcome& clip = clips[static_cast<size_t>(animationSelected)];
    if (clip.placed.empty()) {
        retire(playerGpu);
        return;
    }
    const int count = static_cast<int>(clip.placed.size());
    frame = (frame % count + count) % count;
    const std::string key = std::to_string(reinterpret_cast<uintptr_t>(shown.get())) + ":" + std::to_string(animationSelected) + ":" + std::to_string(frame) + ":" + std::to_string(onionSkin);
    if (key == playerKey && playerGpu.valid()) return;
    playerKey = key;
    PixelImage composite(std::max(1, clip.width), std::max(1, clip.height));
    const auto blendFrame = [&](int index, uint8_t opacity) {
        const cu::ClipFrame& placed = clip.placed[static_cast<size_t>((index % count + count) % count)];
        composite.blendOver(cu::toRgba(placed.art), 0, 0, opacity);
    };
    if (onionSkin && count > 1) {
        blendFrame(frame - 1, 77);   // 30 %
        blendFrame(frame + 1, 77);
    }
    blendFrame(frame, 255);
    updateGpu(playerGpu, composite);
}

void PixelArtCleanupWindow::drawPlayer() {
    if (!shown || !shown->result.animation) return;
    const auto& clips = shown->result.animation->clips;
    if (clips.empty() || animationSelected >= static_cast<int>(clips.size())) return;
    const cu::ClipOutcome& clip = clips[static_cast<size_t>(animationSelected)];
    const int count = static_cast<int>(clip.placed.size());
    ImGui::SeparatorText("Player");
    if (count == 0) {
        ImGui::TextDisabled("This animation has no included frames.");
        return;
    }
    frame = (frame % count + count) % count;
    const double now = ImGui::GetTime();
    if (playing && lastTime >= 0.0) {
        const auto& animations = shown->result.animation->sheet.animations;
        const int selected = clip.animation;
        const double fps = selected >= 0 && selected < static_cast<int>(animations.size()) ? std::max(0.1, animations[static_cast<size_t>(selected)].fps) : cu::DEFAULT_FPS;
        const cu::Loop loop = selected >= 0 && selected < static_cast<int>(animations.size()) ? animations[static_cast<size_t>(selected)].loop : cu::Loop::Forward;
        playClock += now - lastTime;
        while (playClock >= 1.0 / fps) {
            playClock -= 1.0 / fps;
            if (loop == cu::Loop::PingPong) {
                if (frame + playDirection >= count || frame + playDirection < 0) playDirection = -playDirection;
                frame += playDirection;
            } else {
                frame++;
            }
            frame = (frame % count + count) % count;
        }
    }
    lastTime = now;
    uploadPlayer();
    if (playerGpu.valid()) {
        const float scale = std::max(1.0f, std::floor(Theme::dpi(128.0f) / std::max(1, playerGpu.width)));
        ImGui::Image(Backend::getImGuiTexture(playerGpu.texture.getRender()), ImVec2(playerGpu.width * scale, playerGpu.height * scale));
        UIProbe::item("pixelArtCleanup.player");
    }
    if (ImGui::Button(playing ? ICON_FA_PAUSE "###cleanup_play" : ICON_FA_PLAY "###cleanup_play")) {
        playing = !playing;
        playDirection = 1;
    }
    ImGui::SameLine();
    int shownFrame = frame + 1;
    ImGui::SetNextItemWidth(-Theme::dpi(100.0f));
    if (ImGui::SliderInt("##cleanup_frame", &shownFrame, 1, count, "Frame %d")) {
        frame = shownFrame - 1;
        playing = false;
    }
    ImGui::SameLine();
    ImGui::Checkbox("Onion Skin", &onionSkin);
    ImGui::SetItemTooltip("The previous and next frame at 30 %%. Alt+Arrows nudges the frame.");
}

void PixelArtCleanupWindow::drawFramesTab() {
    const cu::Sheet* sheet = currentSheet();
    if (!sheet) {
        ImGui::TextDisabled("Not a sheet or an animation.");
        return;
    }
    const cu::Result* currentResult = result();
    ImGui::TextColored(Theme::Colors::SubtleText, "%d frames. Double-click a box to exclude it; drag on empty space to add one.", static_cast<int>(sheet->frames.size()));

    ImGui::SeparatorText("Animations");
    for (int i = 0; i < static_cast<int>(sheet->animations.size()); i++) {
        const cu::SheetAnimation& animation = sheet->animations[static_cast<size_t>(i)];
        ImGui::PushID(i);
        if (ImGui::RadioButton("##selected", animationSelected == i)) {
            animationSelected = i;
            frame = 0;
        }
        ImGui::SameLine();
        char name[128];
        std::snprintf(name, sizeof(name), "%s", animation.name.c_str());
        ImGui::SetNextItemWidth(Theme::dpi(110.0f));
        if (ImGui::InputText("##name", name, sizeof(name), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsNoBlank)) {
            const std::string value = name;
            applyEdit([&](cu::Recipe& r) { givenSheet(r, currentResult).animations[static_cast<size_t>(i)].name = value; });
        }
        ImGui::SameLine();
        double fps = animation.fps;
        double lo = 0.1, hi = 60.0;
        ImGui::SetNextItemWidth(Theme::dpi(56.0f));
        if (ImGui::DragScalar("##fps", ImGuiDataType_Double, &fps, 0.1f, &lo, &hi, "%.1f fps")) {
            beginEdit();
            givenSheet(recipe, currentResult).animations[static_cast<size_t>(i)].fps = fps;
            edited();
        }
        if (ImGui::IsItemDeactivated()) commitEdit();
        ImGui::SameLine();
        static const char* loops[] = {"Forward", "Ping-Pong"};
        int loop = static_cast<int>(animation.loop);
        ImGui::SetNextItemWidth(Theme::dpi(78.0f));
        if (ImGui::Combo("##loop", &loop, loops, 2)) {
            applyEdit([&](cu::Recipe& r) { givenSheet(r, currentResult).animations[static_cast<size_t>(i)].loop = static_cast<cu::Loop>(loop); });
        }
        ImGui::Indent(ImGui::GetFrameHeightWithSpacing());
        bool include = animation.include;
        if (ImGui::Checkbox("Include", &include)) {
            applyEdit([&](cu::Recipe& r) { givenSheet(r, currentResult).animations[static_cast<size_t>(i)].include = include; });
        }
        ImGui::SameLine();
        // the player's frame is in the selected animation: only its row can split there
        ImGui::BeginDisabled(i != animationSelected);
        const bool splitClicked = ImGui::SmallButton("Split Here");
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", i == animationSelected ? "Split at the player's frame" : "Select this animation and pick a frame in the player first");
        }
        if (splitClicked) {
            // the player counts the included frames: where its frame sits among all of them
            int at = frame;
            if (currentResult && currentResult->animation && i < static_cast<int>(currentResult->animation->clips.size())) {
                const auto& placed = currentResult->animation->clips[static_cast<size_t>(i)].placed;
                if (frame >= 0 && frame < static_cast<int>(placed.size())) {
                    const auto where = std::find(animation.frames.begin(), animation.frames.end(), placed[static_cast<size_t>(frame)].index);
                    if (where != animation.frames.end()) at = static_cast<int>(where - animation.frames.begin());
                }
            }
            applyEdit([&](cu::Recipe& r) {
                cu::Sheet& edit = givenSheet(r, currentResult);
                cu::SheetAnimation& original = edit.animations[static_cast<size_t>(i)];
                if (at <= 0 || at >= static_cast<int>(original.frames.size())) return;
                cu::SheetAnimation tail = original;
                tail.name = original.name + "-2";
                tail.frames.assign(original.frames.begin() + at, original.frames.end());
                original.frames.resize(static_cast<size_t>(at));
                edit.animations.insert(edit.animations.begin() + i + 1, std::move(tail));
            });
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(i + 1 >= static_cast<int>(sheet->animations.size()));
        if (ImGui::SmallButton("Join With Next")) {
            applyEdit([&](cu::Recipe& r) {
                cu::Sheet& edit = givenSheet(r, currentResult);
                if (i + 1 >= static_cast<int>(edit.animations.size())) return;
                cu::SheetAnimation& original = edit.animations[static_cast<size_t>(i)];
                const cu::SheetAnimation next = edit.animations[static_cast<size_t>(i) + 1];
                original.frames.insert(original.frames.end(), next.frames.begin(), next.frames.end());
                edit.animations.erase(edit.animations.begin() + i + 1);
            });
        }
        ImGui::EndDisabled();
        ImGui::Unindent(ImGui::GetFrameHeightWithSpacing());
        ImGui::PopID();
    }

    ImGui::SeparatorText("Layout");
    static const char* anchors[] = {"Ground", "Feet", "Center", "Box"};
    int anchor = static_cast<int>(sheet->anchor);
    ImGui::SetNextItemWidth(Theme::dpi(110.0f));
    if (ImGui::Combo("Anchor", &anchor, anchors, 4)) {
        applyEdit([&](cu::Recipe& r) { givenSheet(r, currentResult).anchor = static_cast<cu::Anchor>(anchor); });
    }
    int padding = sheet->padding;
    ImGui::SetNextItemWidth(Theme::dpi(110.0f));
    if (ImGui::DragInt("Padding", &padding, 0.2f, 0, 64)) {
        beginEdit();
        givenSheet(recipe, currentResult).padding = std::max(0, padding);
        edited();
    }
    if (ImGui::IsItemDeactivated()) commitEdit();

    drawPlayer();
}

void PixelArtCleanupWindow::show() {
    releaseRetired();
    tick();
    if (!windowOpen) return;

    ImGui::SetNextWindowSize(ImVec2(Theme::dpi(960.0f), Theme::dpi(600.0f)), ImGuiCond_FirstUseEver);
    if (focusRequested) {
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }
    const bool visible = ImGui::Begin(WINDOW_NAME, &windowOpen);
    focused = windowOpen && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (!visible) {
        ImGui::End();
        if (!windowOpen) setOpen(false);
        return;
    }
    acceptDrop();
    uploadImages();

    if (!prepared) {
        drawEmptyState(ImGui::GetContentRegionAvail());
    } else {
        drawToolbar();
        // the view buttons and grid toggles
        auto viewButton = [this](const char* label, View which) {
            const bool on = view == which;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, Theme::Colors::Selection);
            if (ImGui::Button(label)) setView(which);
            if (on) ImGui::PopStyleColor();
            ImGui::SameLine();
        };
        viewButton("Source", View::Source);
        viewButton("Result", View::Result);
        viewButton("Compare", View::Compare);
        ImGui::Checkbox("Grid", &showGrid);
        ImGui::SameLine();
        ImGui::Checkbox("Lines", &showLines);
        ImGui::SetItemTooltip("Drag a grid line to move it. A bent line moves as a whole.");
        ImGui::SameLine();
        if (ImGui::Button("Fit")) fitRequested = true;

        const float footerHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
        ImGui::BeginChild("##cleanup_body", ImVec2(0, -footerHeight), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        if (ImGui::BeginTable("##cleanup_layout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("canvas", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("panel", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 21.0f);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            drawCanvas(ImGui::GetContentRegionAvail());
            ImGui::TableSetColumnIndex(1);
            ImGui::BeginChild("##cleanup_panel", ImVec2(0, 0));
            drawRightPanel();
            ImGui::EndChild();
            ImGui::EndTable();
        }
        ImGui::EndChild();
        drawFooter();
    }
    ImGui::End();
    if (!windowOpen) setOpen(false);
}
