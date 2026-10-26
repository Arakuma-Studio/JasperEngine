// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TextureSynthesisWindow.h"
#include "util/FileUtils.h"

#include "App.h"
#include "Backend.h"
#include "EditorHost.h"
#include "Project.h"
#include "Theme.h"
#include "external/IconsFontAwesome6.h"
#include "texsynth/SynthesisRecipe.h"
#include "util/ProjectChecks.h"
#include "util/Util.h"
#include "window/ProjectWindow.h"
#include "window/Widgets.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;
namespace ts = jasperengine::editor::texsynth;

namespace {

    constexpr uint64_t FINAL_SERIAL = 0xFFFFFFFFull;

    uint64_t randomSeed() {
        std::random_device device;
        return std::uniform_int_distribution<uint64_t>(0, 2147483647ULL)(device);
    }

    double now() {
        return ImGui::GetTime();
    }

    // A text field over a std::string (no imgui_stdlib here)
    bool inputString(const char* id, const char* hint, std::string& value, float width = -FLT_MIN) {
        char buffer[1024];
        std::snprintf(buffer, sizeof(buffer), "%s", value.c_str());
        ImGui::SetNextItemWidth(width);
        if (ImGui::InputTextWithHint(id, hint, buffer, sizeof(buffer))) {
            value = buffer;
            return true;
        }
        return false;
    }

    void tooltip(const char* text) {
        ImGui::SetItemTooltip("%s", text);
    }

    void subtle(const std::string& text) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::SubtleText);
        ImGui::TextWrapped("%s", text.c_str());
        ImGui::PopStyleColor();
    }

    void errorText(const std::string& text) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::ErrorText);
        ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION " %s", text.c_str());
        ImGui::PopStyleColor();
    }

    void warningText(const std::string& text) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::WarningText);
        ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION " %s", text.c_str());
        ImGui::PopStyleColor();
    }

    std::string formatSeconds(double seconds) {
        char buffer[32];
        if (seconds < 60.0) std::snprintf(buffer, sizeof(buffer), "%.0f s", std::max(0.0, seconds));
        else std::snprintf(buffer, sizeof(buffer), "%d min %02d s", static_cast<int>(seconds) / 60, static_cast<int>(seconds) % 60);
        return buffer;
    }

    // The problems that belong to a field ("examples[0]..." for the examples' section)
    std::vector<std::string> problemsFor(const std::vector<ts::Problem>& problems, const std::string& prefix) {
        std::vector<std::string> out;
        for (const ts::Problem& p : problems) {
            if (p.field.rfind(prefix, 0) == 0) out.push_back(p.message);
        }
        return out;
    }

    ts::Image fitRgba8(const ts::Image& image, int maxSize) {
        ts::Image rgba = ts::toRgba8(image);
        if (rgba.width <= maxSize && rgba.height <= maxSize) return rgba;
        const double scale = static_cast<double>(maxSize) / std::max(rgba.width, rgba.height);
        return ts::resizeRgba8(rgba, std::max(1, static_cast<int>(std::lround(rgba.width * scale))),
                               std::max(1, static_cast<int>(std::lround(rgba.height * scale))));
    }

    // Image rect fitted into an area, scaled by zoom around the centre, moved by pan
    void placeImage(const ImVec2& areaMin, const ImVec2& areaSize, float imageW, float imageH, float zoom, const ImVec2& pan,
                    ImVec2& outMin, ImVec2& outMax) {
        const float fit = std::min(areaSize.x / std::max(1.0f, imageW), areaSize.y / std::max(1.0f, imageH));
        const float scale = fit * zoom;
        const ImVec2 size(imageW * scale, imageH * scale);
        outMin = ImVec2(areaMin.x + (areaSize.x - size.x) * 0.5f + pan.x, areaMin.y + (areaSize.y - size.y) * 0.5f + pan.y);
        outMax = ImVec2(outMin.x + size.x, outMin.y + size.y);
    }

}

// ====================================================================== lifecycle

TextureSynthesisWindow::TextureSynthesisWindow(Project* project)
    : project(project) {
    spec.seed = randomSeed();
}

TextureSynthesisWindow::~TextureSynthesisWindow() {
    if (run) run->cancel();
}

void TextureSynthesisWindow::open() {
    setOpen(true);
    focusRequested = true;
}

void TextureSynthesisWindow::setOpen(bool open) {
    if (open) {
        if (!windowOpen) focusRequested = true;
        windowOpen = true;
        return;
    }
    windowOpen = false;
    focusRequested = false;
    // closing frees the GPU copies and stops a running synthesis
    cancel();
    retire(exampleGpu);
    retire(resultGpu);
    retire(uncertaintyGpu);
    retire(maskGpu);
    exampleLoadedPath.clear();
    exampleRgba = ts::Image();
}

bool TextureSynthesisWindow::isOpen() const {
    return windowOpen;
}

void TextureSynthesisWindow::resetForProjectSwitch() {
    cancel();
    run.reset();
    spec = ts::SynthesisSpec();
    spec.seed = randomSeed();
    runSpec = ts::SynthesisSpec();
    runRoot.clear();
    runGeneration = 0;
    generatedKey.clear();
    resultSaved = false;
    lastWritten.clear();
    savedOutput.clear();
    statusMessage.clear();
    notices.clear();
    specWarnings.clear();
    problems.clear();
    problemsFingerprint.clear();
    problemsCheckedAt = -1.0;
    maskCanvas.reset();
    maskDirty = true;
    maskRevision++;
    projectImages.clear();
    projectImagesScannedAt = -1.0;
    exampleLoadedPath.clear();
    exampleRgba = ts::Image();
    retire(exampleGpu);
    retire(resultGpu);
    retire(uncertaintyGpu);
    retire(maskGpu);
    view = View::Result;
    zoom = 1.0f;
    pan = ImVec2(0, 0);
    syncSizePreset();
}

void TextureSynthesisWindow::setStatus(const std::string& message, bool error) {
    statusMessage = message;
    statusIsError = error;
}

// ====================================================================== entry points

void TextureSynthesisWindow::openWithExample(const fs::path& image) {
    const fs::path root = project->getProjectPath();
    ts::SynthesisSpec next;
    // the mode, quality and size the user works with stay
    next.mode = spec.mode;
    next.quality = spec.quality;
    next.width = spec.width;
    next.height = spec.height;
    next.seed = randomSeed();
    next.examples.push_back({ts::projectRelative(root, image), "", "", true});
    specWarnings.clear();
    ts::autoCompanions(root, next, specWarnings);
    next.output = ts::defaultOutputPath(next.examples[0].path, next.mode);
    spec = next;
    notices.clear();
    resetForNewSpec();
    open();
}

bool TextureSynthesisWindow::openRecipe(const fs::path& png, std::string& error) {
    const fs::path root = project->getProjectPath();
    std::string readError;
    const std::optional<nlohmann::json> recipe = ts::readRecipe(png, &readError);
    if (!recipe) {
        error = readError.empty() ? "it carries no synthesis recipe (it was not written by the texture synthesis)" : readError;
        return false;
    }
    ts::SynthesisSpec next;
    std::vector<std::string> notes;
    if (!ts::specFromRecipe(*recipe, png, root, next, notes, error)) return false;
    spec = next;
    notices = notes;
    specWarnings.clear();
    std::vector<std::string> changed, missing;
    for (const ts::SourceState& s : ts::checkRecipeSources(*recipe, png, root)) {
        const std::string shown = s.resolved.empty() ? s.path : ts::projectRelative(root, s.resolved);
        if (s.state == "changed") changed.push_back(shown);
        if (s.state == "missing") missing.push_back(shown);
    }
    auto join = [](const std::vector<std::string>& list) {
        std::string text;
        for (const std::string& item : list) text += (text.empty() ? "" : ", ") + item;
        return text;
    };
    if (!changed.empty()) notices.push_back("Changed since this texture was synthesized: " + join(changed) + ". Regenerate gives a different result.");
    if (!missing.empty()) notices.push_back("Missing: " + join(missing) + ". Pick replacements before regenerating.");
    if (changed.empty() && missing.empty()) notices.push_back("Restored from " + ts::projectRelative(root, png) + "; its sources are unchanged, so Regenerate reproduces it.");
    resetForNewSpec();
    open();
    return true;
}

void TextureSynthesisWindow::setSpec(const ts::SynthesisSpec& next) {
    spec = next;
    // a preview-only spec (MCP without output): a default path, so the human can Save it
    if (spec.output.empty() && !spec.examples.empty()) spec.output = ts::defaultOutputPath(spec.examples[0].path, spec.mode);
    notices.clear();
    specWarnings.clear();
    resetForNewSpec();
}

void TextureSynthesisWindow::resetForNewSpec() {
    cancel();
    run.reset();
    generatedKey.clear();
    resultSaved = false;
    lastWritten.clear();
    statusMessage.clear();
    retire(resultGpu);
    retire(uncertaintyGpu);
    maskCanvas.reset();
    if (spec.paintedMask) maskCanvas = std::make_shared<ts::Image>(*spec.paintedMask);
    maskDirty = true;
    maskRevision++;
    zoom = 1.0f;
    pan = ImVec2(0, 0);
    view = spec.mode == ts::Mode::Inpaint && spec.maskSource == ts::MaskSource::Painted ? View::Example : View::Result;
    syncSizePreset();
    refreshProblems(true);
}

void TextureSynthesisWindow::syncSizePreset() {
    if (spec.width != spec.height) sizePreset = SizePreset::Custom;
    else if (spec.width == 512) sizePreset = SizePreset::S512;
    else if (spec.width == 1024) sizePreset = SizePreset::S1024;
    else if (spec.width == 2048) sizePreset = SizePreset::S2048;
    else if (spec.width == 4096) sizePreset = SizePreset::S4096;
    else sizePreset = SizePreset::Custom;
}

// ====================================================================== model

std::shared_ptr<ts::SynthesisRun> TextureSynthesisWindow::generate(std::string& error, ts::SynthesisRun::Options options,
                                                                    ts::SynthesisRun::Finished onFinished) {
    const fs::path root = project->getProjectPath();
    if (spec.mode == ts::Mode::Inpaint && spec.maskSource == ts::MaskSource::Painted && maskCanvas) {
        spec.paintedMask = std::make_shared<const ts::Image>(*maskCanvas);
    }
    ts::Request request;
    std::vector<ts::Problem> errors;
    if (!ts::buildRequest(root, spec, request, errors)) {
        problems = errors;
        error.clear();
        for (const ts::Problem& p : errors) error += (error.empty() ? "" : "; ") + p.field + ": " + p.message;
        setStatus("Can't generate: " + error, true);
        return nullptr;
    }
    cancel();
    if (options.previewIntervalSeconds <= 0.0) options.previewIntervalSeconds = 0.2;
    options.previewMaxSize = 512;
    // one core left for the editor (the output does not depend on the thread count)
    if (options.threads <= 0) options.threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency() > 1 ? std::thread::hardware_concurrency() - 1 : 1u));
    run = ts::SynthesisRun::start(std::move(request), options, std::move(onFinished));
    runSpec = spec;
    runRoot = root;
    runGeneration = ts::projectGeneration();
    generatedKey = specKey();
    generatedMaskRevision = maskRevision;
    resultSaved = false;
    lastWritten.clear();
    statusMessage.clear();
    resultSerial++;
    // the result in view (beside the painted example when painting)
    if (view == View::Example) view = paintsMask() ? View::SideBySide : View::Result;
    return run;
}

void TextureSynthesisWindow::cancel() {
    if (run && !run->finished()) run->cancel();
}

bool TextureSynthesisWindow::isRunning() const {
    return run && !run->finished();
}

bool TextureSynthesisWindow::hasResult() const {
    return run && run->finished() && run->result().status == ts::Status::Ok;
}

std::string TextureSynthesisWindow::specKey() const {
    ts::SynthesisSpec key = spec;
    key.paintedMask.reset();
    return key.fingerprint();
}

bool TextureSynthesisWindow::paintsMask() const {
    return spec.mode == ts::Mode::Inpaint && spec.maskSource == ts::MaskSource::Painted;
}

bool TextureSynthesisWindow::isStale() const {
    if (!hasResult()) return false;
    return specKey() != generatedKey || (paintsMask() && maskRevision != generatedMaskRevision);
}

bool TextureSynthesisWindow::save(bool overwrite, std::vector<std::string>& written, std::string& error, const ts::SynthesisSpec* outputs) {
    written.clear();
    if (!hasResult()) {
        error = "nothing to save: Generate first";
        return false;
    }
    const fs::path root = project->getProjectPath();
    // the result belongs to the project it was generated in
    if (runGeneration != ts::projectGeneration() || runRoot != root) {
        error = "this result was generated in another project: Generate again";
        return false;
    }
    // where the files go: the caller's paths (MCP), else the window's
    const ts::SynthesisSpec& paths = outputs ? *outputs : spec;
    if (!outputs && isStale()) {
        error = "the settings changed since this result was generated: Generate again (or undo the change)";
        return false;
    }
    if (paths.output.empty()) {
        error = "choose an output path";
        return false;
    }
    // the files: the output names, the run's request and result
    ts::SynthesisSpec saving = runSpec;
    saving.output = paths.output;
    saving.uncertaintyOutput = paths.uncertaintyOutput;
    for (size_t i = 0; i < saving.companions.size() && i < paths.companions.size(); i++) saving.companions[i].output = paths.companions[i].output;
    for (const ts::Problem& p : ts::checkSpec(root, saving)) {
        if (p.field.rfind("output", 0) == 0) {
            error = p.message;
            return false;
        }
    }
    if (!overwrite) {
        std::error_code ec;
        for (const ts::PlannedOutput& out : ts::plannedOutputs(saving)) {
            if (fs::exists(root / fs::u8path(out.path), ec)) {
                error = out.path + " exists (overwrite to replace it)";
                return false;
            }
        }
    }
    std::vector<ts::OutputFile> files;
    if (!ts::encodeOutputs(root, saving, run->request(), run->result(), files, error)) return false;
    if (!ts::writeOutputsUndoable(files, error)) return false;
    for (const ts::OutputFile& f : files) written.push_back(f.path);
    resultSaved = true;
    savedOutput = paths.output;
    lastWritten = written;
    setStatus("Saved " + std::to_string(written.size()) + " file(s) as one undo step (Edit > Undo in the Project window removes them).", false);
    if (ProjectWindow* projectWindow = Backend::getApp().getProjectWindow()) {
        if (!files.empty()) projectWindow->revealAsset(files.front().absolute);
    }
    return true;
}

void TextureSynthesisWindow::requestSaveWithConfirm() {
    const fs::path root = project->getProjectPath();
    std::vector<std::string> existing;
    std::error_code ec;
    for (const ts::PlannedOutput& out : ts::plannedOutputs(spec)) {
        if (fs::exists(root / fs::u8path(out.path), ec)) existing.push_back(out.path);
    }
    auto doSave = [this](bool overwrite) {
        std::vector<std::string> written;
        std::string error;
        if (!save(overwrite, written, error)) setStatus("Save failed: " + error, true);
    };
    if (existing.empty()) {
        doSave(false);
        return;
    }
    std::string list;
    for (const std::string& path : existing) list += "\n  " + path;
    getEditorHost().registerConfirmAlert("Replace Files?", "These files exist and would be replaced:" + list +
                                         "\n\nReplace them? (Undo in the Project window puts them back.)",
                                         [doSave]() { doSave(true); });
}

void TextureSynthesisWindow::refreshProblems(bool force) {
    // stat-only checks, but not every frame
    const std::string fp = specKey() + "|" + std::to_string(maskRevision) + "|" + spec.output + "|" + spec.uncertaintyOutput;
    if (!force && problemsCheckedAt >= 0.0 && fp == problemsFingerprint && now() - problemsCheckedAt < 1.0) return;
    problemsFingerprint = fp;
    problemsCheckedAt = now();
    ts::SynthesisSpec checking = spec;
    if (checking.mode == ts::Mode::Inpaint && checking.maskSource == ts::MaskSource::Painted && maskCanvas) {
        checking.paintedMask = std::make_shared<const ts::Image>(*maskCanvas);
    }
    problems = ts::checkSpec(project->getProjectPath(), checking);
}

// ====================================================================== GPU images

void TextureSynthesisWindow::retire(GpuImage& gpu) {
    if (!gpu.texture.empty()) retired.push_back(gpu.texture);
    gpu = GpuImage();
}

void TextureSynthesisWindow::releaseRetired() {
    for (Texture& texture : retired) texture.destroy();
    retired.clear();
}

void TextureSynthesisWindow::updateGpuImage(GpuImage& gpu, const ts::Image& rgba, uint64_t serial) {
    retire(gpu);
    if (rgba.empty() || rgba.channels != 4 || rgba.bitDepth != 8) return;
    static uint64_t counter = 0;
    // TextureData frees what it is given (stbi_image_free = free)
    const size_t bytes = rgba.data.size();
    void* pixels = std::malloc(bytes);
    if (!pixels) return;
    std::memcpy(pixels, rgba.data.data(), bytes);
    TextureData data(rgba.width, rgba.height, static_cast<unsigned int>(bytes), ColorFormat::RGBA, 4, pixels);
    gpu.texture.setData("editor:texture-synthesis:" + std::to_string(++counter), data);
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

void TextureSynthesisWindow::loadExampleForDisplay() {
    const std::string path = spec.examples.empty() ? std::string() : spec.examples[0].path;
    if (path == exampleLoadedPath) return;
    exampleLoadedPath = path;
    exampleRgba = ts::Image();
    retire(exampleGpu);
    if (path.empty()) return;
    fs::path absolute;
    std::string error;
    if (!ts::resolveProjectFile(project->getProjectPath(), path, absolute, error)) return;
    ts::Image image;
    // an image the synthesis would refuse (over 8192 per side) is not decoded for display either
    if (!ts::loadImage(absolute, image, error, nullptr, ts::kMaxImageSide)) return;
    exampleRgba = ts::toRgba8(image);
    updateGpuImage(exampleGpu, fitRgba8(exampleRgba, 2048), 1);
    if (maskCanvas && (maskCanvas->width != exampleRgba.width || maskCanvas->height != exampleRgba.height)) {
        maskCanvas.reset();
        maskDirty = true;
    }
}

void TextureSynthesisWindow::ensureMaskCanvas() {
    if (exampleRgba.empty()) return;
    if (maskCanvas && maskCanvas->width == exampleRgba.width && maskCanvas->height == exampleRgba.height) return;
    maskCanvas = std::make_shared<ts::Image>(ts::Image::make(exampleRgba.width, exampleRgba.height, 1, 8));
    maskDirty = true;
    maskRevision++;
}

void TextureSynthesisWindow::paintAt(const ImVec2& p, bool erase) {
    ensureMaskCanvas();
    if (!maskCanvas) return;
    const float r = std::max(1.0f, brushSize);
    const int x0 = std::max(0, static_cast<int>(std::floor(p.x - r))), x1 = std::min(maskCanvas->width - 1, static_cast<int>(std::ceil(p.x + r)));
    const int y0 = std::max(0, static_cast<int>(std::floor(p.y - r))), y1 = std::min(maskCanvas->height - 1, static_cast<int>(std::ceil(p.y + r)));
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            const float dx = x + 0.5f - p.x, dy = y + 0.5f - p.y;
            if (dx * dx + dy * dy <= r * r) *maskCanvas->pixel(x, y) = erase ? 0 : 255;
        }
    }
    maskDirty = true;
    maskRevision++;
}

// ====================================================================== image slots

void TextureSynthesisWindow::scanProjectImages(bool force) {
    if (!force && projectImagesScannedAt >= 0.0 && now() - projectImagesScannedAt < 5.0) return;
    projectImagesScannedAt = now();
    projectImages.clear();
    const fs::path root = project->getProjectPath();
    for (const fs::path& file : ProjectChecks::findProjectFiles(root, {".png", ".jpg", ".jpeg", ".tga", ".bmp", ".psd", ".gif"})) {
        projectImages.push_back(ts::projectRelative(root, file));
        if (projectImages.size() >= 5000) break;
    }
}

bool TextureSynthesisWindow::imagePickerPopup(const char* popupId, std::string& path) {
    bool changed = false;
    ImGui::SetNextWindowSizeConstraints(ImVec2(Theme::dpi(320.0f), 0), ImVec2(Theme::dpi(560.0f), Theme::dpi(420.0f)));
    if (ImGui::BeginPopup(popupId)) {
        if (ImGui::IsWindowAppearing()) {
            scanProjectImages(true);
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##filter", ICON_FA_MAGNIFYING_GLASS " Filter project images", pickerFilter, sizeof(pickerFilter));
        std::string filter = pickerFilter;
        std::transform(filter.begin(), filter.end(), filter.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        ImGui::BeginChild("##list", ImVec2(0, Theme::dpi(300.0f)));
        int shown = 0;
        for (const std::string& image : projectImages) {
            std::string lowered = image;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (!filter.empty() && lowered.find(filter) == std::string::npos) continue;
            if (++shown > 400) break;
            if (ImGui::Selectable(image.c_str(), image == path)) {
                path = image;
                changed = true;
                ImGui::CloseCurrentPopup();
            }
        }
        if (shown == 0) subtle("No image matches.");
        ImGui::EndChild();
        ImGui::EndPopup();
    }
    return changed;
}

bool TextureSynthesisWindow::imageSlot(const char* id, std::string& path, const char* hint, bool allowClear) {
    ImGui::PushID(id);
    bool changed = false;
    const float buttons = ImGui::GetFrameHeight() * (allowClear && !path.empty() ? 2.0f : 1.0f) + ImGui::GetStyle().ItemSpacing.x * 2.0f;
    changed |= inputString("##path", hint, path, ImGui::GetContentRegionAvail().x - buttons);
    // drop images from the Project window
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")) {
            const char* data = static_cast<const char*>(payload->Data);
            const std::string first(data, strnlen(data, static_cast<size_t>(payload->DataSize)));
            if (Util::isImageFile(first)) {
                path = ts::projectRelative(project->getProjectPath(), fs::u8path(first));
                changed = true;
            }
        }
        ImGui::EndDragDropTarget();
    }
    tooltip("A project image: type its path, drop it from the Project window, or pick it with ...");
    ImGui::SameLine();
    if (ImGui::Button("...", ImVec2(ImGui::GetFrameHeight(), 0))) {
        pickerFilter[0] = 0;
        ImGui::OpenPopup("##picker");
    }
    tooltip("Pick a project image");
    changed |= imagePickerPopup("##picker", path);
    if (allowClear && !path.empty()) {
        ImGui::SameLine();
        if (Widgets::iconButton("##clear", ICON_FA_XMARK, ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) {
            path.clear();
            changed = true;
        }
        tooltip("Clear");
    }
    ImGui::PopID();
    return changed;
}

// ====================================================================== settings panel

void TextureSynthesisWindow::drawModeSection() {
    ImGui::SeparatorText("Mode");
    const ts::Mode modes[] = {ts::Mode::Extend, ts::Mode::Tileable, ts::Mode::MakeTileable, ts::Mode::Inpaint, ts::Mode::Guided};
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##mode", ts::modeTitle(spec.mode))) {
        for (ts::Mode mode : modes) {
            if (ImGui::Selectable(ts::modeTitle(mode), mode == spec.mode)) {
                const bool outputWasDefault = !spec.examples.empty() && spec.output == ts::defaultOutputPath(spec.examples[0].path, spec.mode);
                spec.mode = mode;
                if (outputWasDefault) spec.output = ts::defaultOutputPath(spec.examples[0].path, mode);
                if (mode == ts::Mode::Inpaint && spec.maskSource == ts::MaskSource::Painted) view = View::Example;
            }
            ImGui::SetItemTooltip("%s", ts::modeHelp(mode));
        }
        ImGui::EndCombo();
    }
    subtle(ts::modeHelp(spec.mode));
}

void TextureSynthesisWindow::drawExamplesSection() {
    ImGui::SeparatorText(spec.examples.size() > 1 ? "Examples" : "Example");
    if (spec.examples.empty()) spec.examples.push_back({});
    const fs::path root = project->getProjectPath();
    int removeAt = -1;
    for (size_t i = 0; i < spec.examples.size(); i++) {
        ts::ExampleSpec& ex = spec.examples[i];
        ImGui::PushID(static_cast<int>(i));
        if (spec.examples.size() > 1) {
            ImGui::AlignTextToFramePadding();
            ImGui::Text("Example %zu", i + 1);
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() + ImGui::GetCursorPosX() - ImGui::GetStyle().ItemSpacing.x);
            if (Widgets::iconButton("##remove", ICON_FA_TRASH, ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) removeAt = static_cast<int>(i);
            tooltip("Remove this example");
        }
        const std::string before = ex.path;
        if (imageSlot("image", ex.path, "Example image (albedo)", false) && ex.path != before) {
            // a new first example: its companions and the default output follow
            if (i == 0) {
                const bool outputWasDefault = spec.output.empty() || spec.output == ts::defaultOutputPath(before, spec.mode);
                specWarnings.clear();
                ts::autoCompanions(root, spec, specWarnings);
                if (outputWasDefault) spec.output = ts::defaultOutputPath(ex.path, spec.mode);
                maskCanvas.reset();
            } else {
                specWarnings.clear();
                ts::autoCompanions(root, spec, specWarnings);
            }
        }
        if (ImGui::TreeNodeEx("Options", ImGuiTreeNodeFlags_SpanAvailWidth | (ex.sampleMask.empty() && ex.guide.empty() && ex.sampleable ? 0 : ImGuiTreeNodeFlags_DefaultOpen))) {
            ImGui::Checkbox("Use for sampling", &ex.sampleable);
            tooltip("Off: never copied from, but still context (e.g. a second example whose look should not leak in)");
            ImGui::TextUnformatted("Sample mask");
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            tooltip("Optional, the example's size: white areas may be copied, black never (keep a logo or a crack out of the result)");
            imageSlot("samplemask", ex.sampleMask, "None", true);
            if (spec.mode == ts::Mode::Guided) {
                ImGui::TextUnformatted("Guide");
                ImGui::SameLine();
                ImGui::TextDisabled("(?)");
                tooltip("Optional: this example's guide map (e.g. a height or a material id map). Without it the target guide works as a style transfer");
                imageSlot("guide", ex.guide, "None (style transfer)", true);
            }
            ImGui::TreePop();
        }
        for (const std::string& message : problemsFor(problems, "examples[" + std::to_string(i) + "]")) errorText(message);
        ImGui::PopID();
    }
    if (removeAt >= 0) {
        spec.examples.erase(spec.examples.begin() + removeAt);
        specWarnings.clear();
        ts::autoCompanions(root, spec, specWarnings);
    }
    if (ImGui::Button(ICON_FA_PLUS " Add Example")) spec.examples.push_back({});
    tooltip("More examples give the synthesis more to draw from (every example must have the same companion maps)");
    for (const std::string& message : problemsFor(problems, "examples")) {
        if (message.find("at least") != std::string::npos) errorText(message);
    }
}

void TextureSynthesisWindow::drawCompanionsSection() {
    ImGui::SeparatorText("Companion Maps");
    const fs::path root = project->getProjectPath();
    if (spec.companions.empty()) subtle("None found next to the example (e.g. rock_normal.png for rock_albedo.png). They get the same arrangement, texel for texel.");
    const std::vector<ts::PlannedOutput> planned = ts::plannedOutputs(spec);
    int removeAt = -1;
    for (size_t c = 0; c < spec.companions.size(); c++) {
        ts::CompanionSpec& companion = spec.companions[c];
        ImGui::PushID(static_cast<int>(c));
        ImGui::Checkbox(("##on" + companion.name).c_str(), &companion.enabled);
        tooltip("Synthesize this map along (same arrangement, its own bit depth and channels)");
        ImGui::SameLine();
        ImGui::Text("%s", companion.name.c_str());
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::SubtleText);
        ImGui::TextUnformatted(companion.paths.empty() ? "" : FileUtils::pathToUtf8(fs::u8path(companion.paths[0]).filename()).c_str());
        ImGui::PopStyleColor();
        if (!companion.paths.empty()) tooltip(companion.paths[0].c_str());
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() + ImGui::GetCursorPosX() - ImGui::GetStyle().ItemSpacing.x);
        if (Widgets::iconButton("##remove", ICON_FA_XMARK, ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) removeAt = static_cast<int>(c);
        tooltip("Remove from the list");
        if (companion.enabled) {
            for (const ts::PlannedOutput& out : planned) {
                if (out.role == companion.name) {
                    ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::SubtleText);
                    ImGui::TextWrapped("    -> %s", out.path.c_str());
                    ImGui::PopStyleColor();
                }
            }
        }
        for (const std::string& message : problemsFor(problems, "companions[" + std::to_string(c) + "]")) errorText(message);
        ImGui::PopID();
    }
    if (removeAt >= 0) spec.companions.erase(spec.companions.begin() + removeAt);
    for (const std::string& warning : specWarnings) warningText(warning);

    // Add...: a map for the first example (the others: the file with the same suffix next to them)
    static std::string adding;
    if (ImGui::Button(ICON_FA_PLUS " Add...")) {
        pickerFilter[0] = 0;
        adding.clear();
        ImGui::OpenPopup("##addcompanion");
    }
    tooltip("Add a map that was not detected (it must have the example's exact size); or drop it here from the Project window");
    bool added = imagePickerPopup("##addcompanion", adding);
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")) {
            const char* data = static_cast<const char*>(payload->Data);
            const std::string first(data, strnlen(data, static_cast<size_t>(payload->DataSize)));
            if (Util::isImageFile(first)) {
                adding = ts::projectRelative(root, fs::u8path(first));
                added = true;
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (added && !adding.empty() && !spec.examples.empty()) {
        ts::CompanionSpec companion;
        char separator = '_';
        companion.name = ts::companionSuffix(FileUtils::pathToUtf8(fs::u8path(spec.examples[0].path).stem()), FileUtils::pathToUtf8(fs::u8path(adding).stem()), separator);
        if (companion.name.empty()) companion.name = "map" + std::to_string(spec.companions.size() + 1);
        companion.paths.push_back(adding);
        for (size_t e = 1; e < spec.examples.size(); e++) {
            std::string match;
            for (const ts::CompanionCandidate& candidate : ts::findCompanions(root, spec.examples[e].path)) {
                if (candidate.name == companion.name) match = candidate.path;
            }
            companion.paths.push_back(match);
        }
        spec.companions.push_back(companion);
        adding.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_ROTATE " Detect")) {
        specWarnings.clear();
        ts::autoCompanions(root, spec, specWarnings);
    }
    tooltip("Find the maps next to the example again");
}

void TextureSynthesisWindow::drawModeSpecificSection() {
    if (spec.mode == ts::Mode::Inpaint) {
        ImGui::SeparatorText("Region to Regenerate");
        int source = static_cast<int>(spec.maskSource);
        if (ImGui::RadioButton("Paint", source == static_cast<int>(ts::MaskSource::Painted))) {
            spec.maskSource = ts::MaskSource::Painted;
            view = View::Example;
        }
        tooltip("Paint the region on the example in the preview (left button)");
        ImGui::SameLine();
        if (ImGui::RadioButton("Mask image", source == static_cast<int>(ts::MaskSource::Image))) spec.maskSource = ts::MaskSource::Image;
        tooltip("A mask image of the example's size: white is regenerated");
        ImGui::SameLine();
        if (ImGui::RadioButton("Transparency", source == static_cast<int>(ts::MaskSource::Alpha))) spec.maskSource = ts::MaskSource::Alpha;
        tooltip("The example's transparent pixels are regenerated");
        if (spec.maskSource == ts::MaskSource::Painted) {
            ImGui::SetNextItemWidth(Theme::dpi(140.0f));
            ImGui::SliderFloat("Brush size", &brushSize, 1.0f, 256.0f, "%.0f px", ImGuiSliderFlags_Logarithmic);
            tooltip("Radius in example pixels (also the mouse wheel with Ctrl over the preview)");
            if (ImGui::RadioButton(ICON_FA_PAINTBRUSH " Paint", !brushErase)) brushErase = false;
            ImGui::SameLine();
            if (ImGui::RadioButton(ICON_FA_ERASER " Erase", brushErase)) brushErase = true;
            tooltip("Erase also with the right mouse button");
            ImGui::SameLine();
            if (ImGui::Button("Clear")) {
                if (maskCanvas) std::fill(maskCanvas->data.begin(), maskCanvas->data.end(), 0);
                maskDirty = true;
                maskRevision++;
            }
            ImGui::SameLine();
            if (ImGui::Button("Invert")) {
                ensureMaskCanvas();
                if (maskCanvas) {
                    for (uint8_t& v : maskCanvas->data) v = v >= 128 ? 0 : 255;
                }
                maskDirty = true;
                maskRevision++;
            }
            tooltip("Swap the painted and unpainted areas");
            subtle("Painted areas (red) are regenerated; the mask is saved with the result so it can be regenerated later.");
        } else if (spec.maskSource == ts::MaskSource::Image) {
            imageSlot("mask", spec.maskPath, "Mask image (white = regenerate)", true);
            const char* channels[] = {"Luma", "Red", "Green", "Blue", "Alpha"};
            int channel = static_cast<int>(spec.maskChannel);
            ImGui::SetNextItemWidth(Theme::dpi(120.0f));
            if (ImGui::Combo("Channel", &channel, channels, 5)) spec.maskChannel = static_cast<ts::MaskChannel>(channel);
            ImGui::SameLine();
            ImGui::Checkbox("Invert", &spec.maskInvert);
        } else {
            ImGui::Checkbox("Invert (regenerate the opaque pixels)", &spec.maskInvert);
        }
        for (const std::string& message : problemsFor(problems, "mask")) errorText(message);
    } else if (spec.mode == ts::Mode::MakeTileable) {
        ImGui::SeparatorText("Border");
        const ts::Params preset = ts::presetParams(spec.quality);
        float border = spec.overrides.borderFraction.value_or(preset.borderFraction);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderFloat("##border", &border, 0.02f, 0.5f, "%.3f of the size per edge")) spec.overrides.borderFraction = border;
        tooltip("Width of the band re-synthesized along each edge (shown in yellow on the example). Wider: smoother wrap, more of the image changes");
        if (ImGui::IsItemHovered() && view == View::Result && !hasResult()) view = View::Example;
    } else if (spec.mode == ts::Mode::Guided) {
        ImGui::SeparatorText("Target Guide");
        imageSlot("targetguide", spec.targetGuide, "Target guide image (the layout to follow)", true);
        for (const std::string& message : problemsFor(problems, "target_guide")) errorText(message);
    }
}

void TextureSynthesisWindow::drawOutputSection() {
    ImGui::SeparatorText("Output");
    // size
    if (ts::keepsExampleSize(spec.mode)) {
        if (!exampleRgba.empty()) subtle("Size: the example's, " + std::to_string(exampleRgba.width) + " x " + std::to_string(exampleRgba.height));
        else subtle("Size: the example's");
    } else {
        const char* presets[] = {"512 x 512", "1024 x 1024", "2048 x 2048", "4096 x 4096", "Same as example", "Custom"};
        int preset = static_cast<int>(sizePreset);
        ImGui::SetNextItemWidth(Theme::dpi(150.0f));
        if (ImGui::Combo("Size", &preset, presets, 6)) {
            sizePreset = static_cast<SizePreset>(preset);
            switch (sizePreset) {
                case SizePreset::S512: spec.width = spec.height = 512; break;
                case SizePreset::S1024: spec.width = spec.height = 1024; break;
                case SizePreset::S2048: spec.width = spec.height = 2048; break;
                case SizePreset::S4096: spec.width = spec.height = 4096; break;
                case SizePreset::SameAsExample:
                    if (!exampleRgba.empty()) {
                        spec.width = exampleRgba.width;
                        spec.height = exampleRgba.height;
                    }
                    break;
                case SizePreset::Custom: break;
            }
        }
        tooltip("About 1 s at 512, 4-5 s at 1024 and 20 s at 2048 on 8 cores (Balanced)");
        if (sizePreset == SizePreset::Custom || sizePreset == SizePreset::SameAsExample) {
            ImGui::SetNextItemWidth(Theme::dpi(150.0f));
            int size[2] = {spec.width, spec.height};
            if (ImGui::InputInt2("W x H", size)) {
                spec.width = std::clamp(size[0], 1, 8192);
                spec.height = std::clamp(size[1], 1, 8192);
                sizePreset = SizePreset::Custom;
            }
        }
        if (spec.mode == ts::Mode::Guided) subtle("Guided: the target guide is resized to this size.");
        for (const std::string& message : problemsFor(problems, "size")) errorText(message);
    }

    // quality
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Quality");
    ImGui::SameLine();
    const ts::Quality qualities[] = {ts::Quality::Draft, ts::Quality::Balanced, ts::Quality::High};
    const char* qualityNames[] = {"Draft", "Balanced", "High"};
    const char* qualityHelp[] = {"Fast previews (about 2x faster), rougher", "Upstream's defaults: the usual choice", "About 2x slower: fewer seams and repeats"};
    for (int q = 0; q < 3; q++) {
        if (q) ImGui::SameLine();
        if (ImGui::RadioButton(qualityNames[q], spec.quality == qualities[q])) spec.quality = qualities[q];
        tooltip(qualityHelp[q]);
    }

    // advanced
    if (ImGui::TreeNodeEx("Advanced", ImGuiTreeNodeFlags_SpanAvailWidth)) {
        const ts::Params p = ts::resolveParams(spec.quality, spec.overrides);
        ts::ParamOverrides& o = spec.overrides;
        int k = p.nearestNeighbors, m = p.randomSampleLocations, stages = p.backtrackStages;
        float cauchy = p.cauchyDispersion, backtrack = p.backtrackPercent, alpha = p.guideAlpha;
        bool decay = p.guideDecay;
        const float w = Theme::dpi(140.0f);
        ImGui::SetNextItemWidth(w);
        if (ImGui::SliderInt("Neighbours (k)", &k, 1, 256)) o.nearestNeighbors = k;
        tooltip("Resolved output pixels each pixel compares: larger keeps more of the global structure, slower");
        ImGui::SetNextItemWidth(w);
        if (ImGui::SliderInt("Random candidates (m)", &m, 1, 1024, "%d", ImGuiSliderFlags_Logarithmic)) o.randomSampleLocations = m;
        tooltip("Random source locations tried per pixel besides the coherent ones: more variety, slower");
        ImGui::SetNextItemWidth(w);
        if (ImGui::SliderFloat("Cauchy dispersion", &cauchy, 0.001f, 1.0f, "%.3f", ImGuiSliderFlags_Logarithmic)) o.cauchyDispersion = cauchy;
        tooltip("Near 0: hard seams between copied patches; 1: smooth blending of the cost");
        ImGui::SetNextItemWidth(w);
        if (ImGui::SliderFloat("Backtrack share", &backtrack, 0.0f, 0.95f, "%.2f")) o.backtrackPercent = backtrack;
        tooltip("Share of the pixels each backtracking stage redoes relative to the next");
        ImGui::SetNextItemWidth(w);
        if (ImGui::SliderInt("Backtrack stages", &stages, 0, 10)) o.backtrackStages = stages;
        tooltip("Coarse-to-fine passes: more fixes early mistakes, slower");
        if (spec.mode == ts::Mode::Guided) {
            ImGui::SetNextItemWidth(w);
            if (ImGui::SliderFloat("Guide weight", &alpha, 0.0f, 1.0f, "%.2f")) o.guideAlpha = alpha;
            tooltip("How strongly the guide steers the layout");
            if (ImGui::Checkbox("Guide decay", &decay)) o.guideDecay = decay;
            tooltip("On: the guide sets the layout early, the example the detail late (default)");
        }
        if (ImGui::Button("Reset to preset")) {
            const std::optional<float> border = o.borderFraction;
            o = ts::ParamOverrides();
            o.borderFraction = border;
        }
        for (const ts::Problem& problem : problems) {
            if (problem.field.rfind("params", 0) == 0) errorText(problem.message);
        }
        ImGui::TreePop();
    }

    // seed
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Seed");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(140.0f));
    ImGui::InputScalar("##seed", ImGuiDataType_U64, &spec.seed);
    tooltip("Same settings and seed: the same texture, bit for bit");
    ImGui::SameLine();
    if (Widgets::iconButton("##dice", ICON_FA_DICE, ImVec2(ImGui::GetFrameHeight() * 1.3f, ImGui::GetFrameHeight()))) spec.seed = randomSeed();
    tooltip("Random seed");

    // paths
    ImGui::TextUnformatted("Save as");
    inputString("##output", "assets/textures/name_tile.png", spec.output);
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")) {
            const char* data = static_cast<const char*>(payload->Data);
            const fs::path dropped = fs::u8path(std::string(data, strnlen(data, static_cast<size_t>(payload->DataSize))));
            std::error_code ec;
            const fs::path dir = fs::is_directory(dropped, ec) ? dropped : dropped.parent_path();
            const std::string name = spec.output.empty() ? "synthesized.png" : FileUtils::pathToUtf8(fs::u8path(spec.output).filename());
            spec.output = ts::projectRelative(project->getProjectPath(), dir / fs::u8path(name));
        }
        ImGui::EndDragDropTarget();
    }
    tooltip("Project-relative .png (drop a folder from the Project window to save there)");
    for (const ts::PlannedOutput& out : ts::plannedOutputs(spec)) {
        if (out.role == "primary") continue;
        subtle("+ " + out.path + (out.role == "inpaint_mask" ? "  (the painted mask)" : ""));
    }
    for (const ts::Problem& problem : problems) {
        if (problem.field.rfind("output", 0) == 0) errorText(problem.message);
    }
}

void TextureSynthesisWindow::drawActions() {
    ImGui::Separator();
    ImGui::Spacing();
    const float width = ImGui::GetContentRegionAvail().x;
    const float half = (width - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    const float height = ImGui::GetFrameHeight() * 1.4f;
    if (isRunning()) {
        if (ImGui::Button(ICON_FA_STOP " Cancel", ImVec2(half, height))) cancel();
    } else {
        const bool regenerate = !hasResult() && !notices.empty();
        ImGui::BeginDisabled(!problems.empty());
        const ImVec4 accent = Theme::Colors::Selection;
        ImGui::PushStyleColor(ImGuiCol_Button, accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(accent.x * 1.25f, accent.y * 1.25f, accent.z * 1.25f, accent.w));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(accent.x * 0.85f, accent.y * 0.85f, accent.z * 0.85f, accent.w));
        if (ImGui::Button(regenerate ? ICON_FA_ROTATE " Regenerate" : ICON_FA_PLAY " Generate", ImVec2(half, height))) {
            std::string error;
            generate(error);
        }
        ImGui::PopStyleColor(3);
        ImGui::EndDisabled();
        if (!problems.empty()) ImGui::SetItemTooltip("Fix the problems listed above first");
        else tooltip("Synthesize a preview (nothing is written until Save)");
    }
    ImGui::SameLine();
    const bool stale = isStale();
    const bool saved = resultSaved && savedOutput == spec.output;
    ImGui::BeginDisabled(!hasResult() || stale || isRunning() || spec.output.empty() || saved);
    if (ImGui::Button(saved ? ICON_FA_FLOPPY_DISK " Saved" : ICON_FA_FLOPPY_DISK " Save", ImVec2(half, height))) requestSaveWithConfirm();
    ImGui::EndDisabled();
    if (stale) ImGui::SetItemTooltip("The settings changed since this result: Generate again first");
    else if (!hasResult()) ImGui::SetItemTooltip("Generate a result first");
    else if (saved) ImGui::SetItemTooltip("This result is saved (change the output path to save a copy)");
    else tooltip("Write the result, its companion maps and their recipes (one undo step)");
    if (!isRunning() && !problems.empty()) {
        // the reason Generate is disabled, even when its section is scrolled away
        errorText(problems.front().message + (problems.size() > 1 ? " (+" + std::to_string(problems.size() - 1) + " more above)" : std::string()));
    }

    if (run && isRunning()) {
        const ts::Progress p = run->progress();
        char overlay[96];
        std::snprintf(overlay, sizeof(overlay), "%.0f%%", p.fraction * 100.0);
        ImGui::ProgressBar(static_cast<float>(p.fraction), ImVec2(-FLT_MIN, 0), overlay);
        std::string line = p.stageCount > 0 ? "Stage " + std::to_string(p.stage + 1) + " of " + std::to_string(p.stageCount) : std::string("Starting");
        line += " - " + formatSeconds(p.elapsedSeconds) + " elapsed";
        if (p.etaSeconds >= 0.0) line += ", about " + formatSeconds(p.etaSeconds) + " left";
        subtle(line);
    } else if (run && run->finished() && run->result().status == ts::Status::Cancelled) {
        subtle("Cancelled.");
    } else if (run && run->finished() && run->result().status != ts::Status::Ok) {
        for (const ts::Problem& problem : run->result().errors) errorText(problem.field + ": " + problem.message);
    }
    if (stale) warningText("Settings changed since this result: Generate again to save.");
    if (!statusMessage.empty()) {
        if (statusIsError) errorText(statusMessage);
        else subtle(statusMessage);
    }
}

void TextureSynthesisWindow::drawSettings() {
    for (const std::string& notice : notices) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::WarningText);
        ImGui::TextWrapped(ICON_FA_CIRCLE_INFO " %s", notice.c_str());
        ImGui::PopStyleColor();
    }
    drawModeSection();
    drawExamplesSection();
    drawCompanionsSection();
    drawModeSpecificSection();
    drawOutputSection();
}

// ====================================================================== preview

void TextureSynthesisWindow::drawMetrics() {
    if (!hasResult()) {
        if (!run) subtle(spec.examples.empty() || spec.examples[0].path.empty() ? "Pick an example image (or right-click an image in the Project window > Synthesize Texture...)."
                                                                                : "Generate to see a preview. Nothing is written until Save.");
        return;
    }
    const ts::Result& result = run->result();
    const ts::Assessment a = ts::assess(runSpec, result);
    std::string line = std::to_string(result.width) + " x " + std::to_string(result.height) + " in " + formatSeconds(result.seconds) +
                       ", seed " + std::to_string(result.seed);
    for (const std::string& item : a.lines) line += "   |   " + item;
    ImGui::TextWrapped("%s", line.c_str());
    if (!a.hint.empty()) warningText(a.hint);
    for (const std::string& warning : result.warnings) warningText(warning);
    if (resultSaved && !lastWritten.empty()) subtle("Saved: " + lastWritten.front() + (lastWritten.size() > 1 ? " + " + std::to_string(lastWritten.size() - 1) + " more" : ""));
}

void TextureSynthesisWindow::drawCanvas(const ImVec2& size) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##canvas", ImVec2(std::max(size.x, 1.0f), std::max(size.y, 1.0f)),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(28, 28, 28, 255));
    // checkerboard hint for transparency is skipped: the synthesis output is opaque in practice

    const bool paintMode = spec.mode == ts::Mode::Inpaint && spec.maskSource == ts::MaskSource::Painted;
    const bool showExample = view == View::Example || view == View::SideBySide;
    const bool showResult = view == View::Result || view == View::SideBySide;
    const float gap = Theme::dpi(8.0f);
    ImVec2 exampleArea = origin, exampleSize = size, resultArea = origin, resultSize = size;
    if (view == View::SideBySide) {
        exampleSize = ImVec2((size.x - gap) * 0.5f, size.y);
        resultSize = exampleSize;
        resultArea = ImVec2(origin.x + exampleSize.x + gap, origin.y);
    }
    draw->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);

    // the example, with the inpaint mask or the make-tileable band
    ImVec2 exMin(0, 0), exMax(0, 0);
    if (showExample) {
        if (exampleGpu.valid() && !exampleRgba.empty()) {
            placeImage(exampleArea, exampleSize, static_cast<float>(exampleRgba.width), static_cast<float>(exampleRgba.height), zoom, pan, exMin, exMax);
            Widgets::addImage(draw, Backend::getImGuiTexture(exampleGpu.texture.getRender()), exMin, exMax);
            if (spec.mode == ts::Mode::Inpaint && spec.maskSource == ts::MaskSource::Painted && maskGpu.valid()) {
                draw->AddImage(Backend::getImGuiTexture(maskGpu.texture.getRender()), exMin, exMax);
            }
            if (spec.mode == ts::Mode::MakeTileable) {
                const float border = ts::resolveParams(spec.quality, spec.overrides).borderFraction;
                const float bw = (exMax.x - exMin.x) * border, bh = (exMax.y - exMin.y) * border;
                const ImU32 band = IM_COL32(255, 200, 40, 70), edge = IM_COL32(255, 200, 40, 200);
                draw->AddRectFilled(exMin, ImVec2(exMax.x, exMin.y + bh), band);
                draw->AddRectFilled(ImVec2(exMin.x, exMax.y - bh), exMax, band);
                draw->AddRectFilled(ImVec2(exMin.x, exMin.y + bh), ImVec2(exMin.x + bw, exMax.y - bh), band);
                draw->AddRectFilled(ImVec2(exMax.x - bw, exMin.y + bh), ImVec2(exMax.x, exMax.y - bh), band);
                draw->AddRect(ImVec2(exMin.x + bw, exMin.y + bh), ImVec2(exMax.x - bw, exMax.y - bh), edge, 0.0f, 0, 1.5f);
            }
            draw->AddText(ImVec2(exampleArea.x + 6, exampleArea.y + 4), IM_COL32(220, 220, 220, 200), "Example");
        } else {
            draw->AddText(ImVec2(exampleArea.x + 10, exampleArea.y + 10), IM_COL32(160, 160, 160, 255), "No example image");
        }
    }

    // the result (or the live partial output), tiled 3x3 on demand, with the uncertainty overlay
    if (showResult) {
        if (resultGpu.valid()) {
            const int tiles = tile3x3 ? 3 : 1;
            ImVec2 rMin, rMax;
            placeImage(resultArea, resultSize, static_cast<float>(resultGpu.width * tiles), static_cast<float>(resultGpu.height * tiles), zoom, pan, rMin, rMax);
            const float tw = (rMax.x - rMin.x) / tiles, th = (rMax.y - rMin.y) / tiles;
            const ImTextureID texture = Backend::getImGuiTexture(resultGpu.texture.getRender());
            const ImTextureID overlay = showUncertainty && uncertaintyGpu.valid() && !isRunning() ? Backend::getImGuiTexture(uncertaintyGpu.texture.getRender()) : ImTextureID{};
            for (int ty = 0; ty < tiles; ty++) {
                for (int tx = 0; tx < tiles; tx++) {
                    const ImVec2 a(rMin.x + tw * tx, rMin.y + th * ty), b(a.x + tw, a.y + th);
                    Widgets::addImage(draw, texture, a, b);
                    if (overlay != ImTextureID{}) draw->AddImage(overlay, a, b);
                }
            }
            if (tile3x3) draw->AddRect(ImVec2(rMin.x + tw, rMin.y + th), ImVec2(rMin.x + 2 * tw, rMin.y + 2 * th), IM_COL32(255, 255, 255, 60));
            const char* label = isRunning() ? "Generating..." : (isStale() ? "Result (settings changed since)" : "Result");
            draw->AddText(ImVec2(resultArea.x + 6, resultArea.y + 4), IM_COL32(220, 220, 220, 200), label);
        } else {
            const char* text = isRunning() ? "Generating..." : "Generate to see the result here";
            const ImVec2 t = ImGui::CalcTextSize(text);
            draw->AddText(ImVec2(resultArea.x + (resultSize.x - t.x) * 0.5f, resultArea.y + (resultSize.y - t.y) * 0.5f), IM_COL32(150, 150, 150, 255), text);
        }
    }

    // input: wheel zoom, right/middle drag pans (left drag too when not painting), painting
    ImGuiIO& io = ImGui::GetIO();
    const bool overExample = showExample && exMax.x > exMin.x && ImGui::IsMouseHoveringRect(exMin, exMax);
    const bool canPaint = paintMode && overExample && !isRunning();
    if (hovered && io.MouseWheel != 0.0f) {
        if (io.KeyCtrl && paintMode) {
            brushSize = std::clamp(brushSize * (io.MouseWheel > 0 ? 1.15f : 1.0f / 1.15f), 1.0f, 256.0f);
        } else {
            // zoom about the cursor
            const float factor = io.MouseWheel > 0 ? 1.2f : 1.0f / 1.2f;
            const float next = std::clamp(zoom * factor, 0.1f, 64.0f);
            const ImVec2 areaCenter = view == View::SideBySide && !overExample
                                          ? ImVec2(resultArea.x + resultSize.x * 0.5f, resultArea.y + resultSize.y * 0.5f)
                                          : ImVec2(exampleArea.x + exampleSize.x * 0.5f, exampleArea.y + exampleSize.y * 0.5f);
            const ImVec2 rel(io.MousePos.x - areaCenter.x - pan.x, io.MousePos.y - areaCenter.y - pan.y);
            const float applied = next / zoom;
            pan.x -= rel.x * (applied - 1.0f);
            pan.y -= rel.y * (applied - 1.0f);
            zoom = next;
        }
    }
    if (active && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) || (ImGui::IsMouseDragging(ImGuiMouseButton_Right) && !canPaint) ||
                   (ImGui::IsMouseDragging(ImGuiMouseButton_Left) && !canPaint && !painting))) {
        pan.x += io.MouseDelta.x;
        pan.y += io.MouseDelta.y;
    }
    if (canPaint || painting) {
        const float scale = (exMax.x - exMin.x) / std::max(1, exampleRgba.width);
        const ImVec2 pixel((io.MousePos.x - exMin.x) / scale, (io.MousePos.y - exMin.y) / scale);
        if (canPaint) draw->AddCircle(io.MousePos, brushSize * scale, IM_COL32(255, 255, 255, 200), 48, 1.5f);
        const bool left = ImGui::IsMouseDown(ImGuiMouseButton_Left), right = ImGui::IsMouseDown(ImGuiMouseButton_Right);
        if (active && (left || right) && (painting || canPaint)) {
            const bool erase = brushErase || right;
            if (!painting || lastPaint.x < 0) {
                paintAt(pixel, erase);
            } else {
                // a dab every quarter brush along the drag
                const float dx = pixel.x - lastPaint.x, dy = pixel.y - lastPaint.y;
                const float distance = std::sqrt(dx * dx + dy * dy);
                const int steps = std::max(1, static_cast<int>(distance / std::max(1.0f, brushSize * 0.25f)));
                for (int s = 1; s <= steps; s++) paintAt(ImVec2(lastPaint.x + dx * s / steps, lastPaint.y + dy * s / steps), erase);
            }
            painting = true;
            lastPaint = pixel;
        } else {
            painting = false;
            lastPaint = ImVec2(-1, -1);
        }
        if (canPaint) ImGui::SetMouseCursor(ImGuiMouseCursor_None);
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !paintMode) {
        zoom = 1.0f;
        pan = ImVec2(0, 0);
    }
    draw->PopClipRect();
}

void TextureSynthesisWindow::drawPreview() {
    // toolbar (wraps when the preview is narrow)
    const ImGuiStyle& style = ImGui::GetStyle();
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    auto next = [&](float width) {
        ImGui::SameLine();
        if (ImGui::GetCursorScreenPos().x + width > right) ImGui::NewLine();
    };
    auto checkWidth = [&](const char* label) { return ImGui::GetFrameHeight() + style.ItemInnerSpacing.x + ImGui::CalcTextSize(label).x; };
    const char* views[] = {"Result", "Example", "Side by Side"};
    for (int v = 0; v < 3; v++) {
        if (v) next(checkWidth(views[v]));
        if (ImGui::RadioButton(views[v], static_cast<int>(view) == v)) view = static_cast<View>(v);
    }
    next(checkWidth("Tile 3x3") + style.ItemSpacing.x * 2.0f);
    ImGui::Checkbox("Tile 3x3", &tile3x3);
    tooltip("Repeat the result 3 x 3 to reveal seams and repetition");
    next(checkWidth("Uncertainty"));
    ImGui::BeginDisabled(!hasResult());
    ImGui::Checkbox("Uncertainty", &showUncertainty);
    ImGui::EndDisabled();
    tooltip("Tint the pixels that matched the examples poorly (red = worse): where artifacts are likely");
    char zoomText[32];
    std::snprintf(zoomText, sizeof(zoomText), "%.0f%%", zoom * 100.0f);
    next(ImGui::CalcTextSize("Fit").x + style.FramePadding.x * 2.0f + style.ItemSpacing.x + ImGui::CalcTextSize(zoomText).x);
    if (ImGui::Button("Fit")) {
        zoom = 1.0f;
        pan = ImVec2(0, 0);
    }
    tooltip("Fit the view (also a double click). Mouse wheel zooms, right or middle drag pans");
    ImGui::SameLine();
    ImGui::TextUnformatted(zoomText);

    const float metricsHeight = ImGui::GetTextLineHeightWithSpacing() * 3.2f;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    avail.y = std::max(avail.y - metricsHeight, Theme::dpi(80.0f));
    drawCanvas(avail);
    drawMetrics();
}

// ====================================================================== show

void TextureSynthesisWindow::show() {
    releaseRetired();
    if (!windowOpen) return;

    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 64.0f, ImGui::GetFontSize() * 40.0f), ImGuiCond_FirstUseEver);
    if (focusRequested) {
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }
    const bool visible = ImGui::Begin(WINDOW_NAME, &windowOpen);
    if (!visible) {
        ImGui::End();
        if (!windowOpen) setOpen(false);
        return;
    }

    // ---- model upkeep
    loadExampleForDisplay();
    if (spec.mode == ts::Mode::Inpaint && spec.maskSource == ts::MaskSource::Painted) ensureMaskCanvas();
    if (maskDirty) {
        maskDirty = false;
        if (maskCanvas) {
            ts::Image tint = ts::Image::make(maskCanvas->width, maskCanvas->height, 4, 8);
            for (size_t i = 0; i < maskCanvas->pixelCount(); i++) {
                const uint8_t v = maskCanvas->data[i];
                uint8_t* p = tint.data.data() + i * 4;
                p[0] = 255;
                p[1] = 40;
                p[2] = 40;
                p[3] = v >= 128 ? 120 : 0;
            }
            updateGpuImage(maskGpu, fitRgba8(tint, 2048), 1);
        } else {
            retire(maskGpu);
        }
        problemsCheckedAt = -1.0;
    }
    if (run) {
        const uint64_t base = (resultSerial << 32);
        if (run->finished()) {
            if (resultGpu.serial != (base | FINAL_SERIAL)) {
                const ts::Result& result = run->result();
                if (result.status == ts::Status::Ok) {
                    updateGpuImage(resultGpu, fitRgba8(result.primary, 2048), base | FINAL_SERIAL);
                    ts::Image heat = ts::Image::make(result.uncertaintyMap.width, result.uncertaintyMap.height, 4, 8);
                    for (size_t i = 0; i < heat.pixelCount() && i < result.uncertaintyMap.data.size(); i++) {
                        const uint8_t v = result.uncertaintyMap.data[i * result.uncertaintyMap.bytesPerPixel()];
                        uint8_t* p = heat.data.data() + i * 4;
                        p[0] = 255;
                        p[1] = static_cast<uint8_t>(255 - v);
                        p[2] = 0;
                        p[3] = static_cast<uint8_t>(std::min(230, v * 2));
                    }
                    updateGpuImage(uncertaintyGpu, fitRgba8(heat, 2048), base | FINAL_SERIAL);
                } else if (result.status == ts::Status::Cancelled) {
                    resultGpu.serial = base | FINAL_SERIAL;   // keep the partial preview
                } else {
                    retire(resultGpu);
                    resultGpu.serial = base | FINAL_SERIAL;
                }
            }
        } else {
            Backend::getApp().requestRedraw();
            uint64_t serial = 0;
            std::shared_ptr<const ts::Image> preview = run->latestPreview(&serial);
            if (preview && resultGpu.serial != (base | serial)) updateGpuImage(resultGpu, *preview, base | serial);
        }
    }
    refreshProblems(false);

    // ---- layout: settings | preview
    if (ImGui::BeginTable("##layout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("settings", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 23.0f);
        ImGui::TableSetupColumn("preview", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        // the settings scroll; Generate / Save and the progress stay in view below them
        // (the footer's height as measured last frame: it grows with the progress and messages)
        const float minimum = ImGui::GetFrameHeight() * 1.4f + ImGui::GetStyle().ItemSpacing.y * 3.0f;
        ImGui::BeginChild("##settings", ImVec2(0, -std::max(footerHeight, minimum)), ImGuiChildFlags_None);
        drawSettings();
        ImGui::EndChild();
        const float footerTop = ImGui::GetCursorPosY();
        drawActions();
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
