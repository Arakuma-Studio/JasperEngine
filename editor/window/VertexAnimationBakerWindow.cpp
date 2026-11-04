// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VertexAnimationBakerWindow.h"

#include "App.h"
#include "Backend.h"
#include "EditorHost.h"
#include "Out.h"
#include "Project.h"
#include "Theme.h"
#include "animation/ClipConverters.h"
#include "animation/ClipEditorUtil.h"
#include "external/IconsFontAwesome6.h"
#include "render/preview/VatPreviewRender.h"
#include "util/EntityPath.h"
#include "util/EntityPayload.h"
#include "util/FileUtils.h"
#include "util/UIProbe.h"
#include "vat/VatAppHooks.h"
#include "window/ProjectWindow.h"

#include "Scene.h"
#include "animation/ModelSubClips.h"
#include "component/AnimationPlayerComponent.h"
#include "component/ModelComponent.h"
#include "component/PoseAnimatorComponent.h"
#include "component/Transform.h"
#include "pool/AnimationClipPool.h"
#include "pool/VertexAnimationPool.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "json.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::vat;
using jasperengine::vat::VatClip;
using jasperengine::vat::VatData;
using jasperengine::vat::VatMode;
using jasperengine::vat::VatPrecision;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

    // the live windows: the newest one answers the hooks (the App's; a test's own window takes
    // over while it lives, then the App's again)
    std::vector<VertexAnimationBakerWindow*>& liveWindows() {
        static std::vector<VertexAnimationBakerWindow*> windows;
        return windows;
    }
    VertexAnimationBakerWindow* hooksOwner = nullptr;

    double now() {
        return ImGui::GetCurrentContext() ? ImGui::GetTime() : 0.0;
    }

    std::string lower(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return text;
    }

    bool endsWith(const std::string& text, const std::string& suffix) {
        return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    std::string trim(const std::string& text) {
        size_t a = 0, b = text.size();
        while (a < b && std::isspace((unsigned char)text[a])) a++;
        while (b > a && std::isspace((unsigned char)text[b - 1])) b--;
        std::string out = text.substr(a, b - a);
        // a path pasted with quotes
        if (out.size() >= 2 && (out.front() == '"' || out.front() == '\'') && out.back() == out.front()) out = out.substr(1, out.size() - 2);
        return out;
    }

    std::string bytesText(uint64_t bytes) {
        char buffer[64];
        if (bytes >= 1024ull * 1024ull) std::snprintf(buffer, sizeof(buffer), "%.1f MB", (double)bytes / (1024.0 * 1024.0));
        else if (bytes >= 1024ull) std::snprintf(buffer, sizeof(buffer), "%.1f KB", (double)bytes / 1024.0);
        else std::snprintf(buffer, sizeof(buffer), "%llu B", (unsigned long long)bytes);
        return buffer;
    }

    std::string thousands(uint64_t value) {
        std::string digits = std::to_string(value);
        std::string out;
        for (size_t i = 0; i < digits.size(); i++) {
            if (i > 0 && (digits.size() - i) % 3 == 0) out += ',';
            out += digits[i];
        }
        return out;
    }

    std::string number(float value) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.3g", value);
        return buffer;
    }

    const char* wrapLabel(ClipWrapMode wrap) {
        switch (wrap) {
            case ClipWrapMode::Default: return "Default";
            case ClipWrapMode::Once: return "Once";
            case ClipWrapMode::Loop: return "Loop";
            case ClipWrapMode::PingPong: return "Ping Pong";
            case ClipWrapMode::ClampForever: return "Clamp Forever";
        }
        return "Default";
    }

    const char* modeLabel(VatMode mode) {
        return mode == VatMode::Bones ? "Bones" : "Vertices";
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

    // a text field over a std::string; true when it changed (or Enter with enterOnly)
    bool inputString(const char* id, const char* hint, std::string& value, float width, bool enterOnly = false) {
        char buffer[1024];
        std::snprintf(buffer, sizeof(buffer), "%s", value.c_str());
        ImGui::SetNextItemWidth(width);
        const ImGuiInputTextFlags flags = enterOnly ? ImGuiInputTextFlags_EnterReturnsTrue : ImGuiInputTextFlags_None;
        const bool changed = ImGui::InputTextWithHint(id, hint, buffer, sizeof(buffer), flags);
        if (enterOnly) {
            // the text follows the typing; only Enter (or leaving the field) applies it
            value = buffer;
            return changed || ImGui::IsItemDeactivatedAfterEdit();
        }
        if (changed) value = buffer;
        return changed;
    }

    bool fileExists(const fs::path& path) {
        std::error_code ec;
        return fs::is_regular_file(path, ec);
    }

    VatClipOverride* findOverride(VatBakeSpec& spec, const std::string& name) {
        for (VatClipOverride& override : spec.clipOverrides) {
            if (override.name == name) return &override;
        }
        return nullptr;
    }

    // the override row of a clip, made when missing; dropped again when back to the defaults
    VatClipOverride& overrideRow(VatBakeSpec& spec, const std::string& name) {
        if (VatClipOverride* found = findOverride(spec, name)) return *found;
        VatClipOverride row;
        row.name = name;
        spec.clipOverrides.push_back(row);
        return spec.clipOverrides.back();
    }

    void pruneOverrides(VatBakeSpec& spec) {
        spec.clipOverrides.erase(std::remove_if(spec.clipOverrides.begin(), spec.clipOverrides.end(), [](const VatClipOverride& o) {
            return o.wrap == ClipWrapMode::Default && o.fps <= 0.0f && o.interpolate;
        }), spec.clipOverrides.end());
    }

    // the accent-coloured main button
    bool accentButton(const char* label, const ImVec2& size) {
        const ImVec4 accent = Theme::Colors::Selection;
        ImGui::PushStyleColor(ImGuiCol_Button, accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(accent.x * 1.25f, accent.y * 1.25f, accent.z * 1.25f, accent.w));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(accent.x * 0.85f, accent.y * 0.85f, accent.z * 0.85f, accent.w));
        const bool pressed = ImGui::Button(label, size);
        ImGui::PopStyleColor(3);
        return pressed;
    }

}

// ====================================================================== lifecycle

VertexAnimationBakerWindow::VertexAnimationBakerWindow(Project* project)
    : project(project) {
    importSpec.mode = "auto";
    liveWindows().push_back(this);
    hooksOwner = this;
    vatBakerHooks().bake = [](Project*, const VatBakeEntry& entry, bool bakeNow) {
        if (!hooksOwner) return;
        std::string error;
        if (!hooksOwner->openEntry(entry, bakeNow, error)) {
            Out::error("Vertex Animation: %s", error.c_str());
        }
    };
    // the .vat asset view (Open in Baker, Rebake) and the Project window (double-click, Bake
    // Vertex Animation..., Import as Vertex Animation...)
    vatAppHooks().openBaker = [](Project*, const fs::path& path, VatBakerSource) {
        if (!hooksOwner) return;
        hooksOwner->askBeforeReplacing(FileUtils::pathToUtf8(path.filename()), [path]() {
            if (!hooksOwner) return;
            std::string error;
            // the source kind follows from the file (a .vat opens its recipe, an export imports)
            if (!hooksOwner->setSourcePath(FileUtils::pathToUtf8(path), error)) {
                Out::error("Vertex Animation Baker: can't open %s: %s", FileUtils::pathToUtf8(path.filename()).c_str(), error.c_str());
                return;
            }
            hooksOwner->open();
        });
    };
    vatAppHooks().rebake = [](Project*, const fs::path& vatPath) {
        if (!hooksOwner) return;
        hooksOwner->askBeforeReplacing(FileUtils::pathToUtf8(vatPath.filename()), [vatPath]() {
            if (!hooksOwner) return;
            std::string error;
            if (!hooksOwner->openVat(FileUtils::pathToUtf8(vatPath), error) || !hooksOwner->bake(true, error)) {
                hooksOwner->setStatus("Can't rebake: " + error, true);
                Out::error("Vertex Animation: the rebake of %s can't start: %s", FileUtils::pathToUtf8(vatPath.filename()).c_str(), error.c_str());
            }
        });
    };
}

VertexAnimationBakerWindow::~VertexAnimationBakerWindow() {
    auto& windows = liveWindows();
    windows.erase(std::remove(windows.begin(), windows.end(), this), windows.end());
    hooksOwner = windows.empty() ? nullptr : windows.back();
    if (!hooksOwner) {
        vatBakerHooks().bake = nullptr;
        vatAppHooks().openBaker = nullptr;
        vatAppHooks().rebake = nullptr;
    }
    if (run) run->cancel();
    if (importJob && importJob->thread.joinable()) importJob->thread.join();
    for (auto& job : abandoned) {
        if (job->thread.joinable()) job->thread.join();
    }
    releasePreview();
}

bool VertexAnimationBakerWindow::holdsUnsavedResult() const {
    if (isRunning()) return true;
    if (!written.vat.empty()) return false;
    // only the result Bake would still write (the shown settings'): a stale one is gone already
    if (mode == Mode::Bake && run && run->state() == VatBakeState::Done) return runKey == specKey();
    return mode == Mode::Import && importResult && importResult->ok() && importKey == importSpecKey();
}

void VertexAnimationBakerWindow::askBeforeReplacing(const std::string& what, std::function<void()> replace) {
    if (!holdsUnsavedResult()) {
        replace();
        return;
    }
    open();
    getEditorHost().registerConfirmAlert(
        "Replace the Baker's Result?",
        std::string(isRunning() ? "The baker is running a bake or import" : "The baker holds a result that was not written") +
            ". Open " + what + " anyway? (the " + (isRunning() ? "run is cancelled and its " : "") + "result is dropped)",
        [replace]() {
            if (hooksOwner && hooksOwner->isRunning()) {
                hooksOwner->cancel();
                hooksOwner->tick(0.0);
            }
            replace();
        });
}

void VertexAnimationBakerWindow::open() {
    setOpen(true);
    focusRequested = true;
}

void VertexAnimationBakerWindow::setOpen(bool openWindow) {
    if (openWindow) {
        if (!windowOpen) focusRequested = true;
        windowOpen = true;
        return;
    }
    windowOpen = false;
    focusRequested = false;
    // the preview's scenes and textures go with the window (a running bake goes on)
    releasePreview();
    sourceView.reset();
    vatView.reset();
}

bool VertexAnimationBakerWindow::isOpen() const {
    return windowOpen;
}

void VertexAnimationBakerWindow::resetForProjectSwitch() {
    cancel();
    tick(0.0);
    run.reset();
    runKey.clear();
    for (auto& job : abandoned) {
        if (job->thread.joinable()) job->thread.join();
    }
    abandoned.clear();
    importResult.reset();
    releasePreview();
    sourceView.reset();
    vatView.reset();
    mode = Mode::None;
    sourceLabel.clear();
    playOnLabel.clear();
    spec = VatBakeSpec();
    written = VatWriteResult();
    resetSource();
    status.clear();
    statusError = false;
}

void VertexAnimationBakerWindow::setStatus(const std::string& message, bool error) {
    status = message;
    statusError = error;
}

void VertexAnimationBakerWindow::markChanged() {
    changedAt = now();
}

void VertexAnimationBakerWindow::resetSource() {
    probe.reset();
    probeKey.clear();
    issues.clear();
    estimate = VatEstimate();
    clips.clear();
    summary.clear();
    planned.clear();
    sourceNotes.clear();
    rebakePath.clear();
    rebakeKey.clear();
    noneChecked = false;
    plannedRewrite = false;
    reimport = false;
    detection = VatImportDetection();
    importSpec = VatImportSpec();
    importOut = VatImportWriteOptions();
    importKey.clear();
    spec.clips.clear();
    spec.clipOverrides.clear();
    spec.sockets.clear();
    spec.output.clear();
    spec.overwrite = false;
    if (spec.place == VatPlace::Replace) spec.place = VatPlace::None;
    markChanged();
}

bool VertexAnimationBakerWindow::scenePlaying(std::string* reason) const {
    if (project && project->isAnyScenePlaying()) {
        if (reason) *reason = "Stop the scene to bake: the baker poses the scene and writes files (one undo step).";
        return true;
    }
    return false;
}

// ====================================================================== sources

namespace {

    std::string projectRelative(Project* project, const fs::path& absolute) {
        return FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(absolute.lexically_normal()));
    }

    fs::path absoluteOf(Project* project, const std::string& path) {
        const fs::path given = FileUtils::pathFromUtf8(path);
        return (given.is_absolute() ? given : project->getProjectPath() / given).lexically_normal();
    }

    bool insideProject(Project* project, const fs::path& absolute) {
        const std::string relative = projectRelative(project, absolute);
        return !relative.empty() && relative.compare(0, 2, "..") != 0 && !FileUtils::pathFromUtf8(relative).is_absolute();
    }

    std::string sceneLabel(Project* project, uint32_t sceneId) {
        SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
        return sceneProject ? sceneProject->name : std::string("scene");
    }

}

bool VertexAnimationBakerWindow::setSourcePath(const std::string& given, std::string& error) {
    if (!project) {
        error = "no project is open";
        return false;
    }
    if (isRunning()) {
        error = "a bake is running: cancel it first";
        return false;
    }
    const std::string path = trim(given);
    if (path.empty()) {
        mode = Mode::None;
        sourceLabel.clear();
        resetSource();
        return true;
    }
    const fs::path absolute = absoluteOf(project, path);
    const std::string extension = lower(FileUtils::pathToUtf8(absolute.extension()));
    const std::string name = lower(FileUtils::pathToUtf8(absolute.filename()));
    if (extension == ".vat" && !endsWith(name, ".vat.glb")) return openVat(path, error);

    // a project file the baker reads
    const bool inside = insideProject(project, absolute);
    const bool bakeable = extension == ".gltf" || extension == ".glb" || extension == ".fbx" || extension == ".prefab" ||
                          extension == ".anim" || extension == ".poseanim";
    // a Houdini / OpenVAT export (a folder, its mesh or its textures)
    if (!(bakeable && inside && (extension == ".prefab" || extension == ".anim" || extension == ".poseanim"))) {
        VatImportDetection detected = detectVatImport(FileUtils::pathToUtf8(absolute));
        // a model file is an export's mesh only when the export's data sits beside it (the
        // detection names any mesh a candidate, with the missing textures as its problems)
        const VatImportFiles& files = detected.files;
        const bool exportData = !files.position.empty() || !files.material.empty() || !files.dataJson.empty() || !files.vat.empty() ||
                                !files.remapInfo.empty() || !files.lookup.empty();
        if (detected.found() && (!bakeable || !inside || exportData)) {
            resetSource();
            mode = Mode::Import;
            sourceLabel = path;
            detection = detected;
            importSpec = VatImportSpec();
            importSpec.path = FileUtils::pathToUtf8(absolute);
            importSpec.projectRoot = FileUtils::pathToUtf8(project->getProjectPath());
            importSpec.clipName = detected.assetName;
            importSpec.preset = presetName(spec.preset);
            importSpec.normals = spec.normals;
            importOut.place = spec.place == VatPlace::Crowd ? VatPlace::Crowd : (spec.place == VatPlace::Beside ? VatPlace::Beside : VatPlace::None);
            summary = detected.summary;
            setStatus("", false);
            return true;
        }
    }
    if (!bakeable) {
        error = "'" + path + "' is not a model, prefab, .anim, .poseanim or .vat file, nor a Houdini / OpenVAT export";
        return false;
    }
    std::error_code ec;
    if (!inside || !fs::is_regular_file(absolute, ec)) {
        error = inside ? "'" + path + "' does not exist" : "'" + path + "' is outside the project";
        return false;
    }
    const std::string relative = projectRelative(project, absolute);
    const VatBakeSpec previous = spec;
    resetSource();
    mode = Mode::Bake;
    sourceLabel = relative;
    spec.source = VatSourceSpec();
    if (extension == ".anim" || extension == ".poseanim") {
        spec.source.kind = extension == ".anim" ? VatSourceKind::Anim : VatSourceKind::Pose;
        spec.source.anims = {relative};
        // played on what the previous source played them on, else the clip's preview model
        if ((previous.source.kind == VatSourceKind::Anim || previous.source.kind == VatSourceKind::Pose) && !previous.source.path.empty()) {
            spec.source.path = previous.source.path;
            spec.source.scene = previous.source.scene;
            spec.source.sceneId = previous.source.sceneId;
        } else if (extension == ".anim") {
            std::string loadError;
            std::shared_ptr<const AnimationClip> clip = AnimationClipPool::getOrLoad(ClipConverters::assetRelative(project, absolute), &loadError);
            if (clip && !clip->getPreviewModel().empty()) {
                spec.source.path = projectRelative(project, project->resolveAssetPath(FileUtils::pathFromUtf8(clip->getPreviewModel())));
            }
        } else {
            // a pose animation names the model it is previewed on
            std::string text;
            if (clipui::readWholeFile(absolute, text)) {
                const Json json = Json::parse(text, nullptr, false);
                if (json.is_object() && json.contains("preview_model") && json["preview_model"].is_string() &&
                    !json["preview_model"].get<std::string>().empty()) {
                    spec.source.path = projectRelative(project, project->resolveAssetPath(FileUtils::pathFromUtf8(json["preview_model"].get<std::string>())));
                }
            }
        }
        playOnLabel = isGameObjectSource(spec.source) ? sceneLabel(project, spec.source.sceneId) + ": " + spec.source.path : spec.source.path;
    } else {
        spec.source.kind = VatSourceKind::Model;
        spec.source.path = relative;
    }
    setStatus("", false);
    refreshEstimate();
    return true;
}

bool VertexAnimationBakerWindow::setSourceEntity(uint32_t sceneId, Entity entity, std::string& error) {
    if (isRunning()) {
        error = "a bake is running: cancel it first";
        return false;
    }
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    if (!sceneProject || !sceneProject->scene || entity == NULL_ENTITY || !sceneProject->scene->isEntityCreated(entity)) {
        error = "no such GameObject";
        return false;
    }
    resetSource();
    mode = Mode::Bake;
    spec.source = VatSourceSpec();
    spec.source.kind = VatSourceKind::Entity;
    spec.source.path = EntityPath::of(*sceneProject, entity);
    spec.source.sceneId = sceneId;
    sourceLabel = sceneProject->name + ": " + spec.source.path;
    setStatus("", false);
    refreshEstimate();
    return true;
}

bool VertexAnimationBakerWindow::setPlayOnPath(const std::string& given, std::string& error) {
    if (spec.source.kind != VatSourceKind::Anim && spec.source.kind != VatSourceKind::Pose) {
        error = "Play on is for .anim and .poseanim sources";
        return false;
    }
    const std::string path = trim(given);
    const fs::path absolute = absoluteOf(project, path);
    const std::string extension = lower(FileUtils::pathToUtf8(absolute.extension()));
    if (!path.empty() && (!insideProject(project, absolute) || !(extension == ".gltf" || extension == ".glb" || extension == ".fbx" ||
                                                                  extension == ".prefab"))) {
        error = "'" + path + "' is not a model or prefab of the project";
        return false;
    }
    spec.source.path = path.empty() ? std::string() : projectRelative(project, absolute);
    spec.source.scene.clear();
    spec.source.sceneId = 0;
    playOnLabel = spec.source.path;
    spec.output.clear();
    markChanged();
    refreshEstimate();
    return true;
}

bool VertexAnimationBakerWindow::setPlayOnEntity(uint32_t sceneId, Entity entity, std::string& error) {
    if (spec.source.kind != VatSourceKind::Anim && spec.source.kind != VatSourceKind::Pose) {
        error = "Play on is for .anim and .poseanim sources";
        return false;
    }
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    if (!sceneProject || !sceneProject->scene || entity == NULL_ENTITY || !sceneProject->scene->isEntityCreated(entity)) {
        error = "no such GameObject";
        return false;
    }
    spec.source.path = EntityPath::of(*sceneProject, entity);
    spec.source.scene.clear();
    spec.source.sceneId = sceneId;
    playOnLabel = sceneProject->name + ": " + spec.source.path;
    markChanged();
    refreshEstimate();
    return true;
}

bool VertexAnimationBakerWindow::openVat(const std::string& vatPath, std::string& error) {
    if (!project) {
        error = "no project is open";
        return false;
    }
    if (isRunning()) {
        error = "a bake is running: cancel it first";
        return false;
    }
    const fs::path absolute = absoluteOf(project, vatPath);
    const std::string relative = projectRelative(project, absolute);
    std::string text;
    if (!clipui::readWholeFile(absolute, text)) {
        error = "can't read '" + relative + "'";
        return false;
    }
    jasperengine::vat::VatParseResult parsed = jasperengine::vat::parseVat(text);
    if (!parsed.ok()) {
        error = "'" + relative + "': " + parsed.error;
        return false;
    }
    const VatData& data = *parsed.data;
    const std::vector<VatIssue> staleness = checkVatStaleness(project, relative);

    if (data.recipe.sources.size() == 1 && (data.recipe.sources[0].kind == "houdini_vat3" || data.recipe.sources[0].kind == "openvat")) {
        VatImportSpec recipeSpec;
        if (!importSpecFromRecipe(data.recipe, FileUtils::pathToUtf8(project->getProjectPath()), recipeSpec, error)) return false;
        resetSource();
        mode = Mode::Import;
        detection = detectVatImport(recipeSpec.path);
        importSpec = recipeSpec;
        importSpec.projectRoot = FileUtils::pathToUtf8(project->getProjectPath());
        importOut.output = relative;
        importOut.overwrite = true;
        reimport = true;
        rebakePath = relative;
        sourceLabel = recipeSpec.path;
        summary = detection.summary;
    } else {
        VatBakeSession loader(project);
        VatBakeSpec loaded;
        std::string loadError;
        if (loader.loadRebake(relative, loadError)) {
            loaded = loader.spec();
        } else {
            // the recipe as it is: the problem shows in the estimate (a closed scene, a missing file)
            std::string recipeError;
            if (!specFromRecipe(data.recipe, loaded, recipeError)) {
                error = recipeError;
                return false;
            }
            loaded.output = relative;
            loaded.overwrite = true;
        }
        resetSource();
        mode = Mode::Bake;
        const VatPlace place = spec.place;
        spec = loaded;
        spec.place = place == VatPlace::Replace && !isGameObjectSource(spec.source) ? VatPlace::None : place;
        spec.output = relative;
        spec.overwrite = true;
        rebakePath = relative;
        rebakeKey = specKey();
        if (spec.source.kind == VatSourceKind::Anim || spec.source.kind == VatSourceKind::Pose) {
            std::string anims;
            for (const std::string& anim : spec.source.anims) anims += (anims.empty() ? "" : ", ") + anim;
            sourceLabel = anims;
            playOnLabel = spec.source.path + (spec.source.scene.empty() ? std::string() : " (" + spec.source.scene + ")");
        } else {
            sourceLabel = spec.source.path + (spec.source.scene.empty() ? std::string() : " (" + spec.source.scene + ")");
        }
        if (!loadError.empty()) sourceNotes.push_back("The recipe can't run as it is: " + loadError);
    }
    for (const VatIssue& issue : staleness) {
        sourceNotes.push_back((issue.kind == "vat-source-changed" ? "Source changed since baking: " : "") + issue.message +
                              (issue.fix.empty() ? std::string() : " (" + issue.fix + ")"));
    }
    if (staleness.empty()) sourceNotes.push_back("'" + relative + "' is up to date with its sources.");
    setStatus("", false);
    refreshEstimate();
    open();
    return true;
}

bool VertexAnimationBakerWindow::openEntry(const VatBakeEntry& entry, bool bakeNow, std::string& error) {
    if (!project) {
        error = "no project is open";
        return false;
    }
    bool ok = false;
    switch (entry.kind) {
        case VatBakeEntry::Kind::AnimationPlayer:
            ok = setSourceEntity(entry.sceneId, entry.entity, error);
            break;
        case VatBakeEntry::Kind::PoseAnimator: {
            SceneProject* sceneProject = project->getScene(entry.sceneId);
            if (!sceneProject || !sceneProject->scene || !sceneProject->scene->isEntityCreated(entry.entity)) {
                error = "no such GameObject";
                return false;
            }
            if (isRunning()) {
                error = "a bake is running: cancel it first";
                return false;
            }
            resetSource();
            mode = Mode::Bake;
            spec.source = VatSourceSpec();
            spec.source.kind = VatSourceKind::Pose;
            spec.source.path = EntityPath::of(*sceneProject, entry.entity);
            spec.source.sceneId = entry.sceneId;
            spec.source.anims = {projectRelative(project, project->resolveAssetPath(FileUtils::pathFromUtf8(entry.asset)))};
            sourceLabel = spec.source.anims[0];
            playOnLabel = sceneProject->name + ": " + spec.source.path;
            refreshEstimate();
            ok = true;
            break;
        }
        case VatBakeEntry::Kind::LatticeModifier: {
            SceneProject* sceneProject = project->getScene(entry.sceneId);
            if (!sceneProject || !sceneProject->scene || !sceneProject->scene->isEntityCreated(entry.entity)) {
                error = "no such GameObject";
                return false;
            }
            // the lattice tracks live in the clips of the nearest animated GameObject at or above it
            Scene* scene = sceneProject->scene;
            Entity animated = NULL_ENTITY;
            for (Entity current = entry.entity; current != NULL_ENTITY && animated == NULL_ENTITY;) {
                if (scene->findComponent<AnimationPlayerComponent>(current) || scene->findComponent<PoseAnimatorComponent>(current)) {
                    animated = current;
                    break;
                }
                if (const ModelComponent* model = scene->findComponent<ModelComponent>(current)) {
                    if (!model->animations.empty()) {
                        animated = current;
                        break;
                    }
                }
                const Transform* transform = scene->findComponent<Transform>(current);
                current = transform ? transform->parent : NULL_ENTITY;
            }
            ok = setSourceEntity(entry.sceneId, animated != NULL_ENTITY ? animated : entry.entity, error);
            break;
        }
        case VatBakeEntry::Kind::ModelClip:
        {
            const fs::path model = FileUtils::pathFromUtf8(entry.asset);
            ok = setSourcePath(projectRelative(project, model.is_absolute() ? model : project->resolveAssetPath(model)), error);
        }
            if (ok && !entry.clip.empty()) setCheckedClips({entry.clip});
            break;
    }
    if (!ok) return false;
    open();
    if (!bakeNow) return true;
    // Shift-click: the last-used settings, placed beside the source
    spec.mode = last.mode;
    spec.fps = last.fps;
    spec.normals = last.normals;
    spec.precision = last.precision;
    spec.preset = last.preset;
    spec.toleranceMm = last.toleranceMm;
    spec.place = VatPlace::Beside;
    spec.output.clear();
    markChanged();
    return bake(true, error);
}

// ====================================================================== settings

void VertexAnimationBakerWindow::setSpec(const VatBakeSpec& next) {
    const VatSourceSpec source = spec.source;
    spec = next;
    spec.source = source;
    markChanged();
}

void VertexAnimationBakerWindow::setCheckedClips(const std::vector<std::string>& names) {
    // listed in source order; all of them is "every clip" (an empty list, as recipes store it)
    refreshEstimate();
    std::vector<std::string> ordered;
    for (const ClipRow& row : clips) {
        if (std::find(names.begin(), names.end(), row.name) != names.end()) ordered.push_back(row.name);
    }
    for (const std::string& name : names) {
        if (std::find(ordered.begin(), ordered.end(), name) == ordered.end()) ordered.push_back(name);
    }
    if (!clips.empty() && ordered.size() == clips.size() && !names.empty()) ordered.clear();
    spec.clips = ordered;
    noneChecked = names.empty();
    markChanged();
}

bool VertexAnimationBakerWindow::applyFix(const VatFix& fix, std::string& error) {
    Json value;
    try {
        value = Json::parse(fix.value);
    } catch (const std::exception&) {
        error = "the fix value of '" + fix.label + "' is not JSON: " + fix.value;
        return false;
    }
    const std::string& setting = fix.setting;
    if (setting == "fps" && value.is_number()) {
        const float fps = value.get<float>();
        if (!(fps > 0.0f) || fps > 240.0f) {
            error = "fps out of range";
            return false;
        }
        spec.fps = fps;
        // clips sampled faster than the new rate follow it (the estimate's lowering)
        for (VatClipOverride& override : spec.clipOverrides) {
            if (override.fps > fps) override.fps = fps;
        }
    } else if (setting == "normals" && value.is_boolean()) {
        spec.normals = value.get<bool>();
    } else if (setting == "mode" && value.is_string()) {
        if (!modeChoiceFromName(value.get<std::string>(), spec.mode)) {
            error = "unknown mode " + fix.value;
            return false;
        }
    } else if (setting == "precision" && value.is_string()) {
        if (!precisionChoiceFromName(value.get<std::string>(), spec.precision)) {
            error = "unknown precision " + fix.value;
            return false;
        }
    } else if (setting == "preset" && value.is_string()) {
        if (!presetFromName(value.get<std::string>(), spec.preset)) {
            error = "unknown preset " + fix.value;
            return false;
        }
    } else if (setting == "clips" && value.is_array()) {
        std::vector<std::string> names;
        for (const Json& name : value) {
            if (name.is_string()) names.push_back(name.get<std::string>());
        }
        setCheckedClips(names);
    } else if (setting == "interpolate" && value.is_object() && value.contains("clip") && value["clip"].is_string() &&
               value.contains("value") && value["value"].is_boolean()) {
        overrideRow(spec, value["clip"].get<std::string>()).interpolate = value["value"].get<bool>();
        pruneOverrides(spec);
    } else if (setting == "overwrite" && value.is_boolean()) {
        spec.overwrite = value.get<bool>();
    } else if (setting == "output" && value.is_string()) {
        spec.output = value.get<std::string>();
    } else if (setting == "source" && value.is_string()) {
        return setSourcePath(value.get<std::string>(), error);
    } else {
        error = "the fix '" + fix.label + "' (" + setting + " = " + fix.value + ") can't be applied here";
        return false;
    }
    setStatus("Applied: " + fix.label, false);
    markChanged();
    refreshEstimate();
    return true;
}

int VertexAnimationBakerWindow::addSelectedSockets(std::string& error) {
    if (mode != Mode::Bake || !project) {
        error = "choose a source first";
        return 0;
    }
    const uint32_t sceneId = isGameObjectSource(spec.source) ? spec.source.sceneId : project->getSelectedSceneId();
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) {
        error = "no open scene";
        return 0;
    }
    Scene* scene = sceneProject->scene;
    // the GameObject the paths are relative to: the source's, or (file sources) a GameObject
    // whose Model is the source's model file
    Entity sourceRoot = isGameObjectSource(spec.source) ? EntityPath::find(*sceneProject, spec.source.path) : NULL_ENTITY;
    const std::string sourceFile = isGameObjectSource(spec.source) ? std::string() : lower(spec.source.path);
    int added = 0;
    for (Entity selected : project->getSelectedEntities(sceneId)) {
        Entity rootFor = sourceRoot;
        if (rootFor == NULL_ENTITY && !sourceFile.empty()) {
            for (Entity current = selected; current != NULL_ENTITY;) {
                if (const ModelComponent* model = scene->findComponent<ModelComponent>(current)) {
                    if (!model->filename.empty() &&
                        lower(projectRelative(project, project->resolveAssetPath(FileUtils::pathFromUtf8(model->filename.path)))) == sourceFile) {
                        rootFor = current;
                        break;
                    }
                }
                const Transform* transform = scene->findComponent<Transform>(current);
                current = transform ? transform->parent : NULL_ENTITY;
            }
        }
        std::string path;
        if (rootFor == NULL_ENTITY || selected == rootFor || !EntityPath::relative(scene, rootFor, selected, path) || path.empty()) continue;
        if (std::find(spec.sockets.begin(), spec.sockets.end(), path) == spec.sockets.end()) {
            spec.sockets.push_back(path);
            added++;
        }
    }
    if (added == 0) {
        error = "select nodes under the source in the Hierarchy (a GameObject using the source's model counts too)";
        return 0;
    }
    markChanged();
    return added;
}

// ====================================================================== estimate

std::string VertexAnimationBakerWindow::specKey() const {
    Json j;
    j["kind"] = sourceKindName(spec.source.kind);
    j["path"] = spec.source.path;
    j["scene"] = spec.source.scene;
    j["sceneId"] = spec.source.sceneId;
    j["anims"] = spec.source.anims;
    j["clips"] = spec.clips;
    Json overrides = Json::array();
    for (const VatClipOverride& o : spec.clipOverrides) overrides.push_back({o.name, (int)o.wrap, o.fps, o.interpolate});
    j["overrides"] = overrides;
    j["mode"] = modeChoiceName(spec.mode);
    j["fps"] = spec.fps;
    j["normals"] = spec.normals;
    j["precision"] = precisionChoiceName(spec.precision);
    j["tolerance"] = spec.toleranceMm;
    j["preset"] = presetName(spec.preset);
    j["sockets"] = spec.sockets;
    j["output"] = spec.output;
    j["overwrite"] = spec.overwrite;
    j["place"] = placeName(spec.place);
    j["crowd"] = {spec.crowd.count, spec.crowd.area, spec.crowd.seed};
    j["none"] = noneChecked;
    return j.dump();
}

std::string VertexAnimationBakerWindow::importSpecKey() const {
    Json j;
    j["path"] = importSpec.path;
    j["clip"] = importSpec.clipName;
    j["fps"] = importSpec.fps;
    j["axis"] = importSpec.axis;
    j["mode"] = importSpec.mode;
    j["precision"] = importSpec.precision;
    j["preset"] = importSpec.preset;
    j["normals"] = importSpec.normals;
    j["colors"] = importSpec.colors;
    j["upsample"] = importSpec.upsample;
    return j.dump();
}

void VertexAnimationBakerWindow::refreshEstimate() {
    if (mode != Mode::Bake || !project) return;
    const std::string key = specKey();
    if (key == probeKey) return;
    probeKey = key;
    probeLoading = false;
    issues.clear();
    clips.clear();
    estimate = VatEstimate();
    planned.clear();
    probe.reset();
    if ((spec.source.kind == VatSourceKind::Anim || spec.source.kind == VatSourceKind::Pose) && spec.source.path.empty()) {
        VatIssue issue;
        issue.kind = "invalid_source";
        issue.error = true;
        issue.message = "the clips need a model, prefab or GameObject to play on";
        issue.fix = "drop one on Play on (or use the Hierarchy selection)";
        issues.push_back(issue);
        updateSummary();
        return;
    }
    probe = std::make_unique<VatBakeSession>(project);
    probe->setSpec(spec);
    issues = probe->check();
    for (const VatIssue& issue : issues) probeLoading = probeLoading || issue.kind == "source_loading";
    for (const VatSourceClip& clip : probe->sourceClips()) clips.push_back({clip.name, clip.origin, clip.length, clip.wrap});
    if (!hasErrors(issues) || !probe->sourceClips().empty()) estimate = probe->estimate();
    if (noneChecked && !clips.empty()) {
        VatIssue issue;
        issue.kind = "no_animation";
        issue.error = true;
        issue.message = "no clip is checked";
        issue.fix = "check the clips to bake (All)";
        issues.insert(issues.begin(), issue);
    }
    bool rewrite = false;
    std::string error;
    if (!probe->plannedOutput(planned, rewrite, error)) planned.clear();
    plannedRewrite = rewrite;
    updateSummary();
}

void VertexAnimationBakerWindow::updateSummary() {
    if (mode == Mode::Import) {
        summary = detection.summary;
        return;
    }
    if (clips.empty() && hasErrors(issues)) {
        for (const VatIssue& issue : issues) {
            if (issue.error) {
                summary = issue.message;
                return;
            }
        }
    }
    std::string label = sourceLabel;
    const size_t slash = label.find_last_of('/');
    if (slash != std::string::npos && !isGameObjectSource(spec.source)) label = label.substr(slash + 1);
    summary = label + ": " + std::to_string(estimate.renderers) + (estimate.renderers == 1 ? " mesh, " : " meshes, ") +
              thousands((uint64_t)estimate.bones) + " bones, " + thousands((uint64_t)estimate.vertices) + " vertices, " +
              std::to_string(clips.size()) + (clips.size() == 1 ? " clip" : " clips");
}

// ====================================================================== running

bool VertexAnimationBakerWindow::isRunning() const {
    if (importJob) return true;
    return run && (run->state() == VatBakeState::Posing || run->state() == VatBakeState::Encoding);
}

bool VertexAnimationBakerWindow::isStale() const {
    if (mode == Mode::Import) return importResult && importKey != importSpecKey();
    const VatBakeResult* result = run ? run->result() : nullptr;
    return result && runKey != specKey();
}

void VertexAnimationBakerWindow::rememberSettings() {
    last.mode = spec.mode;
    last.fps = spec.fps;
    last.normals = spec.normals;
    last.precision = spec.precision;
    last.preset = spec.preset;
    last.toleranceMm = spec.toleranceMm;
}

bool VertexAnimationBakerWindow::bake(bool write, std::string& error) {
    if (mode == Mode::Import) return runImport(error);
    if (mode != Mode::Bake) {
        error = "choose a source first";
        return false;
    }
    if (isRunning()) {
        error = "a bake is running";
        return false;
    }
    if (scenePlaying(&error)) return false;
    if (noneChecked) {
        error = "no clip is checked: check the clips to bake";
        return false;
    }
    // an explicit output that exists: asked first (a rebake writes over its own file: no question)
    if (write && !spec.output.empty() && !spec.overwrite) {
        const fs::path absolute = absoluteOf(project, spec.output);
        std::vector<std::string> existing;
        if (fileExists(absolute)) existing.push_back(spec.output);
        const std::string mesh = meshPathFor(FileUtils::pathToUtf8(absolute));
        if (fileExists(FileUtils::pathFromUtf8(mesh))) existing.push_back(projectRelative(project, FileUtils::pathFromUtf8(mesh)));
        if (!existing.empty()) {
            std::string list;
            for (const std::string& path : existing) list += "\n  " + path;
            getEditorHost().registerConfirmAlert("Replace Files?", "These files exist and would be replaced:" + list +
                                                 "\n\nReplace them? (Undo puts them back.)", [this]() {
                spec.overwrite = true;
                markChanged();
                std::string startError;
                if (!startBake(true, startError)) setStatus("Can't bake: " + startError, true);
            });
            return true;
        }
    }
    return startBake(write, error);
}

bool VertexAnimationBakerWindow::startBake(bool write, std::string& error) {
    try {
        const std::string key = specKey();
        // an in-memory result whose source is gone is never written (it would name a GameObject
        // that no longer exists): dropped, and the bake below reports the source
        if (run && run->state() == VatBakeState::Done) {
            const std::string gone = sourceGone(run->spec().source);
            if (!gone.empty()) dropStoredResult(gone);
        }
        return startBakeFor(key, write, error);
    } catch (const std::exception& exception) {
        failWith("bake", exception.what());
        error = status;
        return false;
    }
}

bool VertexAnimationBakerWindow::startBakeFor(const std::string& key, bool write, std::string& error) {
    // the same settings as an in-memory result: written as it is, no second bake
    if (write && run && run->state() == VatBakeState::Done && runKey == key) {
        writeWhenDone = true;
        runFinished = false;
        finishRun();
        if (statusError) error = status;
        return !statusError;
    }
    releasePreview();
    run = std::make_unique<VatBakeSession>(project);
    std::string loadError;
    if (!rebakePath.empty() && key == rebakeKey && run->loadRebake(rebakePath, loadError)) {
        // the recipe exactly as loaded: its clip matching keeps every hand edit
    } else {
        run->setSpec(spec);
    }
    runKey = key;
    writeWhenDone = write;
    runFinished = false;
    written = VatWriteResult();
    rememberSettings();
    if (!run->start()) {
        error = run->failureIssues().empty() ? run->error() : issuesText(run->failureIssues(), true);
        runFinished = true;
        setStatus("Can't bake: " + error, true);
        return false;
    }
    setStatus("", false);
    return true;
}

void VertexAnimationBakerWindow::cancel() {
    if (run && (run->state() == VatBakeState::Posing || run->state() == VatBakeState::Encoding)) {
        run->cancel();
    }
    if (importJob) {
        // the import can't be stopped half way: its result is dropped when it ends
        abandoned.push_back(importJob);
        importJob.reset();
        setStatus("Import cancelled: nothing was written.", false);
    }
}

void VertexAnimationBakerWindow::tick(double budgetMs) {
    for (auto it = abandoned.begin(); it != abandoned.end();) {
        if ((*it)->done) {
            if ((*it)->thread.joinable()) (*it)->thread.join();
            it = abandoned.erase(it);
        } else {
            ++it;
        }
    }
    if (importJob) {
        if (budgetMs <= 0.0 && importJob->thread.joinable()) importJob->thread.join();
        if (importJob->done) finishImport();
    }
    watchSource();
    if (!run || runFinished) return;
    try {
        if (run->state() == VatBakeState::Posing || run->state() == VatBakeState::Encoding) {
            if (!run->advance(budgetMs)) return;
        }
        finishRun();
    } catch (const std::exception& exception) {
        if (run && (run->state() == VatBakeState::Posing || run->state() == VatBakeState::Encoding)) run->cancel();
        failWith("bake", exception.what());
    }
}

void VertexAnimationBakerWindow::failWith(const std::string& what, const std::string& message) {
    runFinished = true;
    writeWhenDone = false;
    const std::string text = "The " + what + " stopped on an error: " + message;
    setStatus(text, true);
    Out::error("Vertex Animation: %s", text.c_str());
}

std::string VertexAnimationBakerWindow::sourceGone(const VatSourceSpec& source) const {
    if (!project) return "no project is open";
    if (source.sceneId != 0) {
        SceneProject* sceneProject = project->getScene(source.sceneId);
        if (!sceneProject || !sceneProject->scene) return "the scene of '" + source.path + "' was closed";
        if (!source.path.empty() && EntityPath::find(*sceneProject, source.path) == NULL_ENTITY) {
            return "the GameObject '" + source.path + "' is gone from '" + sceneProject->name + "'";
        }
    } else if (source.scene.empty() && !source.path.empty() && !fileExists(absoluteOf(project, source.path))) {
        return "'" + source.path + "' is gone";
    }
    for (const std::string& anim : source.anims) {
        if (!fileExists(absoluteOf(project, anim))) return "'" + anim + "' is gone";
    }
    return "";
}

void VertexAnimationBakerWindow::dropStoredResult(const std::string& why) {
    releasePreview();
    run.reset();
    runKey.clear();
    runFinished = false;
    writeWhenDone = false;
    probeKey.clear();
    markChanged();
    setStatus("The baked result was dropped: " + why + ".", true);
    Out::error("Vertex Animation: %s", status.c_str());
}

void VertexAnimationBakerWindow::watchSource() {
    if (!project || isRunning()) return;
    const double time = now();
    if (sourceCheckedAt >= 0.0 && time - sourceCheckedAt < 0.2) return;
    sourceCheckedAt = time;
    // a GameObject source whose scene closed: the window starts over (nothing of it is reachable)
    if (mode == Mode::Bake && spec.source.sceneId != 0) {
        SceneProject* sceneProject = project->getScene(spec.source.sceneId);
        if (!sceneProject || !sceneProject->scene) {
            const std::string label = sourceLabel;
            releasePreview();
            run.reset();
            runKey.clear();
            runFinished = false;
            writeWhenDone = false;
            mode = Mode::None;
            sourceLabel.clear();
            playOnLabel.clear();
            resetSource();
            spec.source = VatSourceSpec();
            setStatus("The scene of the source (" + label + ") was closed: choose a source.", false);
            return;
        }
    }
    if (run && run->state() == VatBakeState::Done) {
        const std::string gone = sourceGone(run->spec().source);
        if (!gone.empty()) dropStoredResult(gone);
    }
    // a GameObject source that was still loading (source_loading): its clips and estimate once
    // its meshes are built
    if (mode == Mode::Bake && probeLoading && !vatSourceLoading(project, spec.source)) {
        probeKey.clear();
        refreshEstimate();
    }
}

std::string VertexAnimationBakerWindow::stageLine() const {
    if (importJob) return "Stage 1 of 1: Importing " + detection.assetName;
    if (!run) return "";
    const VatBakeProgress progress = run->progress();
    switch (run->state()) {
        case VatBakeState::Posing: return "Stage 1 of 2: " + progress.stage;
        case VatBakeState::Encoding: return "Stage 2 of 2: " + progress.stage;
        default: return progress.stage;
    }
}

std::string VertexAnimationBakerWindow::statusLine(const VatWriteResult& out, const VatData& data, bool import) {
    std::string clipNames;
    for (size_t i = 0; i < data.clips.size() && i < 4; i++) clipNames += (i ? ", " : "") + data.clips[i].name;
    if (data.clips.size() > 4) clipNames += ", ...";
    const std::string first = data.clips.empty() ? std::string("Clip") : data.clips[0].name;
    std::string line = std::string(import ? "Imported" : "Baked") + " '" + out.vat + "' (" + modeLabel(data.mode) + ", " +
                       std::to_string(data.clips.size()) + (data.clips.size() == 1 ? " clip: " : " clips: ") + clipNames + "; " +
                       std::to_string(data.layout.width) + " x " + std::to_string(data.layout.height) + " " +
                       textureFormatName(data.layout.precision) + ", " + bytesText(data.stats.bytes) + ")" +
                       (out.rewritten ? ", written over the previous bake (GUID and edits kept)" : "") + "; one undo step (" +
                       out.undoName + ")";
    if (out.entity != NULL_ENTITY) {
        line += "; placed in the scene with a Vertex Animator; in a Lua script of it: GameObject(self.scene, self.entity)"
                ":getVertexAnimator():play(\"" + first + "\")";
    } else {
        line += "; drop it on the Scene or Hierarchy to place it, then in a Lua script: GameObject(self.scene, self.entity)"
                ":getVertexAnimator():play(\"" + first + "\")";
    }
    return line;
}

void VertexAnimationBakerWindow::reveal(const std::string& relative) {
    if (relative.empty() || !project || !ImGui::GetCurrentContext()) return;
    if (ProjectWindow* projectWindow = Backend::getApp().getProjectWindow()) {
        projectWindow->revealAsset(absoluteOf(project, relative));
    }
}

void VertexAnimationBakerWindow::finishRun() {
    if (!run || runFinished) return;
    runFinished = true;
    switch (run->state()) {
        case VatBakeState::Done: {
            const VatBakeResult* result = run->result();
            if (!writeWhenDone) {
                setStatus("Baked in memory (" + std::string(modeLabel(result->data.mode)) + ", " + bytesText(result->report.bytes) +
                          "): compare it in Preview; Bake writes this result without baking again.", false);
                return;
            }
            std::string error;
            VatIssue issue;
            if (!run->write(written, error, &issue)) {
                if (!issue.fix.empty() && error.find(issue.fix) == std::string::npos) error += " (" + issue.fix + ")";
                setStatus("The bake can't be written: " + error, true);
                Out::error("Vertex Animation: the bake can't be written: %s", error.c_str());
                return;
            }
            writeWhenDone = false;
            VatData data = result->data;
            data.stats.bytes = result->report.bytes;
            const std::string line = statusLine(written, data, false);
            setStatus(line, false);
            Out::info("%s", line.c_str());
            reveal(written.vat);
            // the file exists now: the default output and the existing-file rule change
            probeKey.clear();
            markChanged();
            break;
        }
        case VatBakeState::Cancelled:
            setStatus("Cancelled: nothing was written.", false);
            break;
        case VatBakeState::Failed: {
            const std::string error = run->failureIssues().empty() ? run->error() : issuesText(run->failureIssues(), true);
            setStatus("The bake failed: " + error, true);
            Out::error("Vertex Animation: the bake failed: %s", error.c_str());
            break;
        }
        default:
            runFinished = false;
            break;
    }
}

// ====================================================================== import

std::string VertexAnimationBakerWindow::defaultImportOutputPath() const {
    return defaultImportOutput(project, detection);
}

bool VertexAnimationBakerWindow::runImport(std::string& error) {
    if (mode != Mode::Import) {
        error = "the source is not a Houdini / OpenVAT export";
        return false;
    }
    if (isRunning()) {
        error = "an import is running";
        return false;
    }
    if (scenePlaying(&error)) return false;
    // what the options fix (fps, axis) goes to the importer; any other missing piece stops here
    for (const VatIssue& issue : detection.issues) {
        if (!issue.error) continue;
        const bool fixed = (issue.kind == "fps_unknown" && importSpec.fps > 0.0f) || (issue.kind == "unknown_target" && !importSpec.axis.empty());
        if (!fixed) {
            error = issue.message + (issue.fix.empty() ? std::string() : " (" + issue.fix + ")");
            return false;
        }
    }
    if (!importOut.output.empty() && !importOut.overwrite) {
        const fs::path absolute = absoluteOf(project, importOut.output);
        if (fileExists(absolute) || fileExists(FileUtils::pathFromUtf8(meshPathFor(FileUtils::pathToUtf8(absolute))))) {
            getEditorHost().registerConfirmAlert("Replace Files?", "These files exist and would be replaced:\n  " + importOut.output +
                                                 "\n\nReplace them? (Undo puts them back.)", [this]() {
                importOut.overwrite = true;
                std::string startError;
                if (!runImport(startError)) setStatus("Can't import: " + startError, true);
            });
            return true;
        }
    }
    releasePreview();
    importResult.reset();
    written = VatWriteResult();
    importSpec.projectRoot = FileUtils::pathToUtf8(project->getProjectPath());
    auto job = std::make_shared<ImportJob>();
    job->spec = importSpec;
    ImportJob* raw = job.get();
    job->thread = std::thread([raw]() {
        try {
            raw->result = importVat(raw->spec);
        } catch (const std::exception& exception) {
            VatIssue issue;
            issue.kind = "read_failed";
            issue.error = true;
            issue.message = exception.what();
            raw->result.errors.push_back(issue);
        } catch (...) {
            VatIssue issue;
            issue.kind = "read_failed";
            issue.error = true;
            issue.message = "the import stopped on an unknown error";
            raw->result.errors.push_back(issue);
        }
        raw->done = true;
    });
    importJob = job;
    importKey = importSpecKey();
    importStartedAt = now();
    setStatus("", false);
    return true;
}

void VertexAnimationBakerWindow::finishImport() {
    std::shared_ptr<ImportJob> job = importJob;
    importJob.reset();
    if (job->thread.joinable()) job->thread.join();
    importResult = std::make_shared<VatImportResult>(std::move(job->result));
    if (!importResult->ok()) {
        const std::string text = vatImportIssuesText(importResult->errors);
        setStatus("The import failed: " + text, true);
        Out::error("Vertex Animation: the import of '%s' failed: %s", detection.assetName.c_str(), text.c_str());
        return;
    }
    std::string error;
    VatImportWriteOptions options = importOut;
    if (options.place == VatPlace::Crowd) {
        VatCrowd crowd;
        crowd.count = crowdCount;
        crowd.area = crowdArea;
        crowd.seed = crowdSeedValue();
        const std::string problem = crowdPlacementProblem(crowd);
        if (!problem.empty()) {
            setStatus("The import can't be placed: " + problem, true);
            return;
        }
        options.crowd = crowdPlacement(crowd);
    }
    bool wrote = false;
    try {
        wrote = writeVatImport(project, *importResult, detection, options, written, error);
    } catch (const std::exception& exception) {
        failWith("import", exception.what());
        return;
    }
    if (!wrote) {
        setStatus("The import can't be written: " + error, true);
        Out::error("Vertex Animation: the import can't be written: %s", error.c_str());
        return;
    }
    VatData data = importResult->data;
    data.stats.bytes = importResult->report.bytes;
    const std::string line = statusLine(written, data, true);
    setStatus(line, false);
    Out::info("%s", line.c_str());
    reveal(written.vat);
}

// ====================================================================== preview

std::string VertexAnimationBakerWindow::sourcePreviewFile() {
    previewNote.clear();
    if (mode == Mode::Import) {
        previewNote = "A Houdini / OpenVAT export: its source can't be shown here; the VAT plays the imported data.";
        return "";
    }
    const VatSourceSpec& source = run ? run->spec().source : spec.source;
    if (!isGameObjectSource(source)) return source.path;
    SceneProject* sceneProject = nullptr;
    if (source.sceneId != 0) {
        sceneProject = project->getScene(source.sceneId);
    } else {
        for (SceneProject& candidate : project->getScenes()) {
            if (candidate.filepath.empty()) continue;
            const fs::path file = candidate.filepath.is_absolute() ? candidate.filepath : project->getProjectPath() / candidate.filepath;
            if (projectRelative(project, file) == source.scene) sceneProject = &candidate;
        }
    }
    const Entity entity = sceneProject && sceneProject->scene ? EntityPath::find(*sceneProject, source.path) : NULL_ENTITY;
    const ModelComponent* model = entity != NULL_ENTITY ? sceneProject->scene->findComponent<ModelComponent>(entity) : nullptr;
    if (!model || model->filename.empty()) {
        previewNote = "'" + source.path + "' has no Model: the source view shows model and prefab files; the VAT view shows the bake.";
        return "";
    }
    previewNote = "Source: the GameObject's model file (what the scene adds to it is not shown here).";
    return projectRelative(project, project->resolveAssetPath(FileUtils::pathFromUtf8(model->filename.path)));
}

void VertexAnimationBakerWindow::releasePreview() {
    if (sourceView) sourceView->clear();
    if (vatView) vatView->clear();
    previewOf = nullptr;
    previewVatFile.clear();
    previewMeshFile.clear();
    heatVatFile.clear();
    heatMeshFile.clear();
    previewHeat = false;
    previewNote.clear();
}

void VertexAnimationBakerWindow::ensurePreview() {
    const void* key = nullptr;
    const VatData* data = nullptr;
    if (mode == Mode::Import) {
        if (importResult && importResult->ok() && !written.vat.empty()) {
            key = importResult.get();
            data = &importResult->data;
        }
    } else if (run && run->result()) {
        key = run->result();
        data = &run->result()->data;
    }
    if (key == previewOf) {
        applyHeatmap();
        return;
    }
    releasePreview();
    if (!key) return;
    if (!sourceView) sourceView = std::make_unique<VatPreviewView>();
    if (!vatView) vatView = std::make_unique<VatPreviewView>();
    previewOf = key;
    previewClip = 0;
    previewTime = 0.0f;
    std::string error;
    std::string vatFile, meshFile;
    if (mode == Mode::Import) {
        vatFile = FileUtils::pathToUtf8(absoluteOf(project, written.vat));
        meshFile = FileUtils::pathToUtf8(absoluteOf(project, written.mesh));
    } else if (!run->writePreviewFiles(vatFile, meshFile, error)) {
        previewNote = "The VAT can't be previewed: " + error;
        return;
    }
    previewVatFile = vatFile;
    previewMeshFile = meshFile;
    std::string vatError;
    if (!vatView->loadVat(vatFile, meshFile, vatError)) previewNote = vatError;
    const std::string sourceFile = sourcePreviewFile();
    if (!sourceFile.empty() && !sourceView->loadSource(project, sourceFile, error)) previewNote = "The source can't be shown: " + error;
    if (!vatError.empty()) previewNote = vatError;
    Vector3 min = data->bounds.min, max = data->bounds.max;
    if (!data->bounds.valid) {
        min = Vector3(-1, 0, -1);
        max = Vector3(1, 2, 1);
    }
    sourceView->setFraming(min, max);
    vatView->setFraming(min, max);
    applyHeatmap();
}

bool VertexAnimationBakerWindow::heatmapAvailable() const {
    return mode == Mode::Bake && run && run->result() && !run->result()->vertexErrorMm.empty();
}

void VertexAnimationBakerWindow::setHeatmap(bool on) {
    heatmap = on;
    applyHeatmap();
}

void VertexAnimationBakerWindow::applyHeatmap() {
    const bool want = heatmap && heatmapAvailable();
    if (!previewOf || !vatView || want == previewHeat || previewVatFile.empty()) return;
    std::string error;
    if (want && heatVatFile.empty() && !run->writeHeatmapPreviewFiles(heatVatFile, heatMeshFile, error)) {
        heatVatFile.clear();
        heatMeshFile.clear();
        previewNote = "The heatmap can't be shown: " + error;
        heatmap = false;
        return;
    }
    // the view's mesh swapped (its framing stays); the playhead poses it again
    if (!vatView->loadVat(want ? heatVatFile : previewVatFile, want ? heatMeshFile : previewMeshFile, error, want)) {
        previewNote = error;
    }
    previewHeat = want;
}

void VertexAnimationBakerWindow::drawHeatmapLegend() {
    const VatBakeResult* result = run ? run->result() : nullptr;
    if (!result) return;
    const float tolerance = result->report.toleranceMm;
    float worst = 0.0f;
    for (float e : result->vertexErrorMm) worst = std::max(worst, e);
    // the scale: green 0 -> yellow tolerance -> red twice it
    const float width = std::min(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 16.0f);
    const float height = ImGui::GetFontSize() * 0.6f;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    float c0[4], c1[4], c2[4];
    heatmapColor(0.0f, tolerance, c0);
    heatmapColor(tolerance, tolerance, c1);
    heatmapColor(2.0f * tolerance, tolerance, c2);
    const ImU32 green = ImGui::GetColorU32(ImVec4(c0[0], c0[1], c0[2], 1.0f));
    const ImU32 yellow = ImGui::GetColorU32(ImVec4(c1[0], c1[1], c1[2], 1.0f));
    const ImU32 red = ImGui::GetColorU32(ImVec4(c2[0], c2[1], c2[2], 1.0f));
    draw->AddRectFilledMultiColor(at, ImVec2(at.x + width * 0.5f, at.y + height), green, yellow, yellow, green);
    draw->AddRectFilledMultiColor(ImVec2(at.x + width * 0.5f, at.y), ImVec2(at.x + width, at.y + height), yellow, red, red, yellow);
    ImGui::Dummy(ImVec2(width, height));
    UIProbe::item("vatbaker.heatmapLegend");
    ImGui::TextDisabled("0 mm");
    ImGui::SameLine(width * 0.5f - ImGui::CalcTextSize("0.00 mm").x * 0.5f);
    ImGui::TextDisabled("%.3g mm", tolerance);
    ImGui::SameLine(std::max(0.0f, width - ImGui::CalcTextSize("0.00 mm+").x));
    ImGui::TextDisabled("%.3g mm+", 2.0f * tolerance);
    ImGui::TextDisabled("Largest error %.3g mm (tolerance %.3g mm)", worst, tolerance);
}

void VertexAnimationBakerWindow::updatePreview(float dt) {
    ensurePreview();
    if (!previewOf) return;
    const VatData& data = mode == Mode::Import ? importResult->data : run->result()->data;
    if (data.clips.empty()) return;
    previewClip = std::clamp(previewClip, 0, (int)data.clips.size() - 1);
    const VatClip& clip = data.clips[(size_t)previewClip];
    const float length = clip.length();
    if (previewPlaying && dt > 0.0f) {
        previewTime += dt * (clip.speed > 0.0f ? clip.speed : 1.0f);
        previewTime = length > 0.0f ? std::fmod(previewTime, length) : 0.0f;
    }
    previewTime = std::clamp(previewTime, 0.0f, std::max(0.0f, length));
    if (vatView && vatView->hasSubject()) vatView->poseVat(clip.name, previewTime);
    if (sourceView && sourceView->hasSubject() && mode == Mode::Bake) {
        const VatBakeResult* result = run->result();
        std::shared_ptr<const AnimationClip> sourceClip;
        if ((size_t)previewClip < result->sourceClips.size()) {
            for (const VatSourceClip& candidate : run->sourceClips()) {
                if (candidate.name == result->sourceClips[(size_t)previewClip]) sourceClip = candidate.clip;
            }
        }
        sourceView->poseSource(sourceClip, previewTime);
    }
}

// ====================================================================== UI

bool VertexAnimationBakerWindow::sourceField(const char* id, std::string& text, const char* hint, bool playOn) {
    ImGui::PushID(id);
    bool applied = false;
    std::string error;
    const float button = ImGui::CalcTextSize(ICON_FA_ARROW_POINTER " Selection").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    std::string typed = text;
    if (inputString("##path", hint, typed, ImGui::GetContentRegionAvail().x - button - ImGui::GetStyle().ItemSpacing.x, true)) {
        if (typed != text) {
            applied = playOn ? setPlayOnPath(typed, error) : setSourcePath(typed, error);
            if (!applied) {
                setStatus(error, true);
            }
        }
    }
    UIProbe::item(playOn ? "vatbaker.playOn" : "vatbaker.source");
    // drops: files from the Project window, GameObjects from the Hierarchy
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")) {
            std::vector<std::string> files;
            const char* data = static_cast<const char*>(payload->Data);
            size_t offset = 0;
            while (offset < (size_t)payload->DataSize) {
                const std::string file(data + offset, strnlen(data + offset, (size_t)payload->DataSize - offset));
                if (!file.empty()) files.push_back(file);
                offset += file.size() + 1;
            }
            if (!files.empty()) {
                applied = playOn ? setPlayOnPath(files[0], error) : setSourcePath(files[0], error);
                // several .anim files: every one is a clip of the source
                if (applied && !playOn && spec.source.kind == VatSourceKind::Anim) {
                    for (size_t i = 1; i < files.size(); i++) {
                        const fs::path absolute = absoluteOf(project, files[i]);
                        if (lower(FileUtils::pathToUtf8(absolute.extension())) == ".anim") spec.source.anims.push_back(projectRelative(project, absolute));
                    }
                    if (spec.source.anims.size() > 1) {
                        sourceLabel.clear();
                        for (const std::string& anim : spec.source.anims) sourceLabel += (sourceLabel.empty() ? "" : ", ") + anim;
                        markChanged();
                    }
                }
                if (!applied) setStatus(error, true);
            }
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("entity")) {
            if (payload->DataSize >= (int)sizeof(EntityPayload)) {
                const EntityPayload* entityPayload = static_cast<const EntityPayload*>(payload->Data);
                const uint32_t sceneId = payloadSceneId(*entityPayload, project->getSelectedSceneId());
                applied = playOn ? setPlayOnEntity(sceneId, entityPayload->entity, error) : setSourceEntity(sceneId, entityPayload->entity, error);
                if (!applied) setStatus(error, true);
            }
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::SameLine();
    // the Hierarchy selection as the source
    const uint32_t sceneId = project ? project->getSelectedSceneId() : 0;
    const std::vector<Entity> selection = project ? project->getSelectedEntities(sceneId) : std::vector<Entity>();
    ImGui::BeginDisabled(selection.size() != 1);
    if (ImGui::Button(ICON_FA_ARROW_POINTER " Selection", ImVec2(button, 0))) {
        applied = playOn ? setPlayOnEntity(sceneId, selection[0], error) : setSourceEntity(sceneId, selection[0], error);
        if (!applied) setStatus(error, true);
    }
    ImGui::EndDisabled();
    UIProbe::item(playOn ? "vatbaker.playOnSelection" : "vatbaker.useSelection");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", selection.size() == 1 ? "Use the GameObject selected in the Hierarchy" : "Select one GameObject in the Hierarchy");
    }
    ImGui::PopID();
    return applied;
}

void VertexAnimationBakerWindow::drawSource() {
    ImGui::SeparatorText("Source");
    sourceField("##vat_source", sourceLabel, "Drop a model, prefab, GameObject, .anim, .poseanim, .vat or Houdini / OpenVAT export", false);
    tooltip("What to bake: a model (its animations), a prefab or a GameObject (Animation Player, Animations, Pose Animator, lattice\n"
            "tracks), .anim clips (played on a model), a .poseanim, a .vat (its recipe: Rebake), or a Houdini VAT 3 / OpenVAT export\n"
            "(imported). Type a path and press Enter, or drop it here.");
    if (mode == Mode::None) {
        subtle("Bakes animations into a Vertex Animation (.vat): thousands of characters play their own clips on the GPU in one draw "
               "per material. Inspector > Animation Player > Bake also opens it.");
        return;
    }
    if (!summary.empty()) subtle(summary);
    for (const std::string& note : sourceNotes) {
        ImGui::PushStyleColor(ImGuiCol_Text, note.find("up to date") != std::string::npos ? Theme::Colors::SubtleText : Theme::Colors::WarningText);
        ImGui::TextWrapped(ICON_FA_CIRCLE_INFO " %s", note.c_str());
        ImGui::PopStyleColor();
    }
    if (mode == Mode::Import) {
        drawImportSource();
        return;
    }
    if (spec.source.kind == VatSourceKind::Anim || spec.source.kind == VatSourceKind::Pose) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Play on");
        ImGui::SameLine();
        sourceField("##vat_play_on", playOnLabel, "Drop the model, prefab or GameObject the clips play on", true);
    }
}

void VertexAnimationBakerWindow::drawImportSource() {
    const VatImportDetection& d = detection;
    std::string what = std::string(d.kind == VatImportKind::HoudiniVat3 ? "Houdini Labs VAT 3" : "OpenVAT") + " export";
    if (d.mode != HoudiniMode::Unknown) what += ", " + std::string(houdiniModeName(d.mode)) + (d.modeSource.empty() ? "" : " (from the " + d.modeSource + ")");
    ImGui::TextWrapped("%s", what.c_str());
    std::string metadata;
    if (!d.metadataSource.empty()) metadata += "metadata: " + d.metadataSource;
    if (d.frames > 0) metadata += std::string(metadata.empty() ? "" : ", ") + std::to_string(d.frames) + " frames";
    if (d.fps > 0.0f) metadata += std::string(metadata.empty() ? "" : ", ") + number(d.fps) + " fps";
    if (!d.target.empty()) metadata += std::string(metadata.empty() ? "" : ", ") + "target " + d.target;
    if (!d.clips.empty()) {
        std::string names;
        for (const std::string& clip : d.clips) names += (names.empty() ? "" : ", ") + clip;
        metadata += std::string(metadata.empty() ? "" : ", ") + "clips " + names;
    }
    if (!metadata.empty()) subtle(metadata);
    if (ImGui::TreeNode("Files")) {
        const std::pair<const char*, const std::string*> files[] = {
            {"Mesh", &d.files.mesh}, {"Position", &d.files.position}, {"Position 2", &d.files.position2}, {"Rotation", &d.files.rotation},
            {"Colour", &d.files.color}, {"Lookup", &d.files.lookup}, {"Material", &d.files.material}, {"Data JSON", &d.files.dataJson},
            {"VAT", &d.files.vat}, {"Normals", &d.files.normals}, {"Remap info", &d.files.remapInfo}};
        for (const auto& file : files) {
            if (file.second->empty()) continue;
            ImGui::TextDisabled("%s:", file.first);
            ImGui::SameLine();
            ImGui::TextUnformatted(FileUtils::pathToUtf8(FileUtils::pathFromUtf8(*file.second).filename()).c_str());
            ImGui::SetItemTooltip("%s", file.second->c_str());
        }
        ImGui::TreePop();
    }
    for (const VatIssue& issue : d.issues) {
        const std::string text = issue.message + (issue.fix.empty() ? std::string() : " (" + issue.fix + ")");
        if (issue.error) errorText(text);
        else warningText(text);
    }
}

void VertexAnimationBakerWindow::drawClips() {
    ImGui::SeparatorText("Clips");
    if (clips.empty()) {
        subtle(hasErrors(issues) ? "No clips: see the problems below." : "The source has no clips.");
        return;
    }
    if (ImGui::SmallButton("All")) setCheckedClips([&]() { std::vector<std::string> all; for (const ClipRow& row : clips) all.push_back(row.name); return all; }());
    UIProbe::item("vatbaker.clipsAll");
    ImGui::SameLine();
    if (ImGui::SmallButton("None")) setCheckedClips({});
    UIProbe::item("vatbaker.clipsNone");
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##vat_clips", 5, flags)) return;
    ImGui::TableSetupColumn("Clip", ImGuiTableColumnFlags_WidthStretch, 3.0f);
    ImGui::TableSetupColumn("Frames", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 3.5f);
    ImGui::TableSetupColumn("Length", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 4.0f);
    ImGui::TableSetupColumn("Wrap", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 7.0f);
    ImGui::TableSetupColumn("FPS", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 4.5f);
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < clips.size(); i++) {
        const ClipRow& row = clips[i];
        ImGui::PushID((int)i);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        bool checked = !noneChecked && (spec.clips.empty() || std::find(spec.clips.begin(), spec.clips.end(), row.name) != spec.clips.end());
        if (ImGui::Checkbox(row.name.c_str(), &checked)) {
            std::vector<std::string> names;
            for (const ClipRow& other : clips) {
                const bool on = &other == &row ? checked
                              : (!noneChecked && (spec.clips.empty() || std::find(spec.clips.begin(), spec.clips.end(), other.name) != spec.clips.end()));
                if (on) names.push_back(other.name);
            }
            setCheckedClips(names);
        }
        UIProbe::item("vatbaker.clip." + std::to_string(i));
        ImGui::SetItemTooltip("%s", row.origin.c_str());
        VatClipOverride* override = findOverride(spec, row.name);
        const ClipWrapMode wrap = override && override->wrap != ClipWrapMode::Default ? override->wrap : row.wrap;
        const float fps = override && override->fps > 0.0f ? override->fps : spec.fps;
        int frames = 1;
        float sampleFps = 0.0f;
        clipFrames(row.length, wrap, fps, frames, sampleFps);
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%d", frames);
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%.2f s", row.length);
        ImGui::TableSetColumnIndex(3);
        ImGui::SetNextItemWidth(-FLT_MIN);
        const std::string wrapPreview = override && override->wrap != ClipWrapMode::Default ? wrapLabel(override->wrap)
                                                                                            : std::string("Source: ") + wrapLabel(row.wrap);
        if (ImGui::BeginCombo("##wrap", wrapPreview.c_str())) {
            for (ClipWrapMode option : {ClipWrapMode::Default, ClipWrapMode::Loop, ClipWrapMode::Once, ClipWrapMode::PingPong, ClipWrapMode::ClampForever}) {
                const std::string label = option == ClipWrapMode::Default ? std::string("Source: ") + wrapLabel(row.wrap) : wrapLabel(option);
                const bool selected = (override ? override->wrap : ClipWrapMode::Default) == option;
                if (ImGui::Selectable(label.c_str(), selected)) {
                    overrideRow(spec, row.name).wrap = option;
                    pruneOverrides(spec);
                    markChanged();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::TableSetColumnIndex(4);
        float clipFps = override ? override->fps : 0.0f;
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputFloat("##fps", &clipFps, 0.0f, 0.0f, clipFps > 0.0f ? "%.4g" : "bake's")) {
            overrideRow(spec, row.name).fps = std::clamp(clipFps, 0.0f, 240.0f);
            pruneOverrides(spec);
            markChanged();
        }
        tooltip("This clip's sample rate (0: the bake's Sample rate)");
        if (override && !override->interpolate) {
            ImGui::TableSetColumnIndex(0);
            ImGui::SameLine();
            ImGui::TextDisabled("(no interpolation)");
            ImGui::SameLine();
            if (ImGui::SmallButton("Interpolate")) {
                override->interpolate = true;
                pruneOverrides(spec);
                markChanged();
            }
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void VertexAnimationBakerWindow::drawSettings() {
    ImGui::SeparatorText("Settings");
    const float labelWidth = ImGui::GetFontSize() * 7.0f;
    auto label = [&](const char* text) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
        ImGui::SameLine(labelWidth);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    };

    label("Mode");
    const char* modes[] = {"Auto", "Vertices", "Bones"};
    int modeIndex = (int)spec.mode;
    if (ImGui::Combo("##mode", &modeIndex, modes, 3)) {
        spec.mode = (VatModeChoice)modeIndex;
        markChanged();
    }
    UIProbe::item("vatbaker.mode");
    tooltip("Vertices: every vertex per frame (anything that deforms: skin, morphs, lattices). Bones: bone matrices per frame\n"
            "(kilobytes, exact normals; skin and rigid parts only). Auto picks Bones when bones reproduce the motion.");
    const VatBakeResult* result = run && runKey == specKey() ? run->result() : nullptr;
    const std::string decision = result ? std::string(modeLabel(result->report.mode)) + ": " + result->report.modeReason
                                        : (estimate.modeReason.empty() ? std::string() : estimate.modeReason);
    if (!decision.empty()) {
        ImGui::Indent(labelWidth);
        subtle(decision);
        ImGui::Unindent(labelWidth);
    }

    label("Sample rate");
    float fps = spec.fps;
    if (ImGui::InputFloat("##fps", &fps, 5.0f, 10.0f, "%.4g fps")) {
        spec.fps = std::clamp(fps, 1.0f, 240.0f);
        markChanged();
    }
    UIProbe::item("vatbaker.fps");
    tooltip("Samples per second of every clip (a clip's FPS column overrides it). The GPU interpolates between samples.");

    label("Normals");
    if (ImGui::Checkbox("##normals", &spec.normals)) markChanged();
    UIProbe::item("vatbaker.normals");
    tooltip("Vertices mode: a tangent frame per vertex per frame (lighting follows the deformation); off halves the payload");

    label("Precision");
    const char* precisions[] = {"Auto", "Half", "Float"};
    int precisionIndex = (int)spec.precision;
    if (ImGui::Combo("##precision", &precisionIndex, precisions, 3)) {
        spec.precision = (VatPrecisionChoice)precisionIndex;
        markChanged();
    }
    tooltip("Vertices mode: half floats (8 bytes a texel) unless their error exceeds the tolerance (Auto), or always float. Bones are float.");

    label("Preset");
    const char* presets[] = {"Mobile & Web", "Desktop"};
    int presetIndex = (int)spec.preset;
    if (ImGui::Combo("##preset", &presetIndex, presets, 2)) {
        spec.preset = (VatPreset)presetIndex;
        markChanged();
    }
    UIProbe::item("vatbaker.preset");
    tooltip("Mobile & Web: 2048 x 2048 texels (OpenGL ES 3 / WebGL2 guarantees). Desktop: 4096 x 8192.");

    drawSockets();
}

void VertexAnimationBakerWindow::drawSockets() {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Sockets");
    tooltip("Nodes whose transform is baked per frame, so props attach to them (Vertex Animator > Attachments, Lua getSocketPose)");
    ImGui::SameLine(ImGui::GetFontSize() * 7.0f);
    if (ImGui::SmallButton(ICON_FA_PLUS " Add Selected")) {
        std::string error;
        const int added = addSelectedSockets(error);
        setStatus(added > 0 ? "Added " + std::to_string(added) + " socket(s)" : error, added == 0);
    }
    UIProbe::item("vatbaker.addSockets");
    tooltip("The nodes selected in the Hierarchy, under the source (or under a GameObject using the source's model)");
    for (size_t i = 0; i < spec.sockets.size(); i++) {
        ImGui::PushID((int)i);
        ImGui::Indent(ImGui::GetFontSize() * 7.0f);
        if (ImGui::SmallButton(ICON_FA_XMARK)) {
            spec.sockets.erase(spec.sockets.begin() + (long)i);
            markChanged();
            ImGui::Unindent(ImGui::GetFontSize() * 7.0f);
            ImGui::PopID();
            break;
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(spec.sockets[i].c_str());
        ImGui::Unindent(ImGui::GetFontSize() * 7.0f);
        ImGui::PopID();
    }
    ImGui::Indent(ImGui::GetFontSize() * 7.0f);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    if (ImGui::InputTextWithHint("##socket", "node path (Armature/Hips/...)", socketInput, sizeof(socketInput), ImGuiInputTextFlags_EnterReturnsTrue)) {
        const std::string path = trim(socketInput);
        if (!path.empty() && std::find(spec.sockets.begin(), spec.sockets.end(), path) == spec.sockets.end()) {
            spec.sockets.push_back(path);
            markChanged();
        }
        socketInput[0] = 0;
    }
    ImGui::Unindent(ImGui::GetFontSize() * 7.0f);
}

void VertexAnimationBakerWindow::drawIssues(const std::vector<VatIssue>& list, const char* id) {
    ImGui::PushID(id);
    for (size_t i = 0; i < list.size(); i++) {
        const VatIssue& issue = list[i];
        ImGui::PushID((int)i);
        const std::string text = issue.message + (issue.fixes.empty() && !issue.fix.empty() ? " (" + issue.fix + ")" : std::string());
        if (issue.error) errorText(text);
        else warningText(text);
        ImGui::SetItemTooltip("%s", issue.kind.c_str());
        for (size_t f = 0; f < issue.fixes.size(); f++) {
            const VatFix& fix = issue.fixes[f];
            if (f > 0) ImGui::SameLine();
            else ImGui::Indent(ImGui::GetFontSize());
            ImGui::PushID((int)f);
            if (ImGui::SmallButton(fix.label.c_str())) {
                std::string error;
                if (!applyFix(fix, error)) setStatus(error, true);
            }
            UIProbe::item(std::string("vatbaker.fix.") + id + "." + std::to_string(i) + "." + std::to_string(f));
            ImGui::PopID();
            if (f + 1 == issue.fixes.size()) ImGui::Unindent(ImGui::GetFontSize());
        }
        ImGui::PopID();
    }
    ImGui::PopID();
}

void VertexAnimationBakerWindow::drawEstimate() {
    ImGui::SeparatorText("Estimate");
    if (estimate.width > 0) {
        char line[256];
        std::snprintf(line, sizeof(line), "%s, %d x %d %s, %s", modeLabel(estimate.mode), estimate.width, estimate.height,
                      textureFormatName(estimate.precision), bytesText(estimate.bytes).c_str());
        ImGui::TextUnformatted(line);
        UIProbe::item("vatbaker.estimate.texture");
        ImGui::TextDisabled("%s vertices, %s bones, %d samples, %d texel fetches per vertex", thousands((uint64_t)estimate.vertices).c_str(),
                            thousands((uint64_t)estimate.bones).c_str(), estimate.samples, estimate.fetchesPerVertex);
        // the payload against the preset's texels
        const float share = estimate.maxTexels ? (float)((double)estimate.texels / (double)estimate.maxTexels) : 0.0f;
        char overlay[96];
        std::snprintf(overlay, sizeof(overlay), "%s of %s texels (%.0f%%)", thousands(estimate.texels).c_str(),
                      thousands(estimate.maxTexels).c_str(), share * 100.0f);
        if (share > 1.0f) ImGui::PushStyleColor(ImGuiCol_PlotHistogram, Theme::Colors::ErrorText);
        ImGui::ProgressBar(std::min(1.0f, share), ImVec2(-FLT_MIN, 0), overlay);
        if (share > 1.0f) ImGui::PopStyleColor();
        tooltip("The payload texture against the limit of the preset");
        if (estimate.clips.size() > 1 && ImGui::TreeNode("Memory per clip")) {
            for (const VatClipEstimate& clip : estimate.clips) {
                ImGui::TextDisabled("%s: %d frames at %.4g fps, %s", clip.name.c_str(), clip.frames, clip.fps, bytesText(clip.bytes).c_str());
            }
            ImGui::TreePop();
        }
    }
    drawIssues(issues, "check");
    const VatBakeResult* result = run ? run->result() : nullptr;
    if (result) {
        const VatReport& report = result->report;
        ImGui::Spacing();
        ImGui::TextUnformatted(isStale() ? "Last bake (other settings):" : "Measured by the bake:");
        ImGui::TextDisabled("%s, %d x %d %s, %s; error max %.3g mm, mean %.3g mm (tolerance %.3g mm)", modeLabel(report.mode), report.width,
                            report.height, textureFormatName(report.precision), bytesText(report.bytes).c_str(), report.maxErrorMm,
                            report.meanErrorMm, report.toleranceMm);
        std::vector<VatIssue> warnings;
        for (const VatIssue& warning : report.warnings) {
            // the check's own rows are listed above
            bool listed = false;
            for (const VatIssue& issue : issues) listed = listed || (issue.kind == warning.kind && issue.message == warning.message);
            if (!listed) warnings.push_back(warning);
        }
        drawIssues(warnings, "report");
    }
    if (run && run->state() == VatBakeState::Failed) drawIssues(run->failureIssues(), "failure");
}

void VertexAnimationBakerWindow::drawPreview() {
    ImGui::SeparatorText("Preview");
    updatePreview(ImGui::GetIO().DeltaTime);
    if (!previewOf) {
        subtle(mode == Mode::Import ? "Import to see the result play here." : "Bake (or Preview, nothing written) to compare the source with the VAT here.");
        return;
    }
    const VatData& data = mode == Mode::Import ? importResult->data : run->result()->data;
    if (data.clips.empty()) return;
    // clip, play / pause, scrub
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
    if (ImGui::BeginCombo("##preview_clip", data.clips[(size_t)previewClip].name.c_str())) {
        for (size_t i = 0; i < data.clips.size(); i++) {
            if (ImGui::Selectable(data.clips[i].name.c_str(), (int)i == previewClip)) {
                previewClip = (int)i;
                previewTime = 0.0f;
            }
        }
        ImGui::EndCombo();
    }
    UIProbe::item("vatbaker.previewClip");
    ImGui::SameLine();
    if (ImGui::Button(previewPlaying ? ICON_FA_PAUSE "##preview_play" : ICON_FA_PLAY "##preview_play")) previewPlaying = !previewPlaying;
    UIProbe::item("vatbaker.previewPlay");
    tooltip(previewPlaying ? "Pause" : "Play");
    ImGui::SameLine();
    const float length = data.clips[(size_t)previewClip].length();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderFloat("##preview_time", &previewTime, 0.0f, std::max(length, 0.0001f), "%.2f s")) previewPlaying = false;
    UIProbe::item("vatbaker.previewTime");
    if (previewPlaying) Backend::getApp().requestRedraw();
    // Heatmap: the VAT side coloured by each vertex's error
    const bool canHeat = heatmapAvailable();
    bool heat = heatmap && canHeat;
    ImGui::BeginDisabled(!canHeat);
    if (ImGui::Checkbox("Heatmap", &heat)) setHeatmap(heat);
    ImGui::EndDisabled();
    UIProbe::item("vatbaker.heatmap");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", canHeat ? "Colour the VAT by each vertex's largest error over the frames and between them\n"
                                          "(green: none, yellow: the tolerance, red: twice it or more), unlit"
                                        : "Imports carry no per-vertex error: the heatmap is for bakes");
    }
    if (heat) drawHeatmapLegend();

    // the two views
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float width = std::max(32.0f, (ImGui::GetContentRegionAvail().x - spacing) * 0.5f);
    const float height = std::min(width, ImGui::GetFontSize() * 18.0f);
    const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
    const ImVec2 size(width, height);
    VatPreviewView* views[2] = {sourceView.get(), vatView.get()};
    const char* titles[2] = {"Source", "Vertex Animation (GPU)"};
    for (int v = 0; v < 2; v++) {
        if (v == 1) ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextDisabled("%s", titles[v]);
        VatPreviewView* view = views[v];
        const ImVec2 corner = ImGui::GetCursorScreenPos();
        if (view && view->hasSubject()) {
            view->requestDraw((int)(width * scale.x), (int)(height * scale.y));
            ImGui::Dummy(size);
            if (TextureRender* texture = view->texture()) {
                ImGui::GetWindowDrawList()->AddImage(Backend::getImGuiTexture(texture), corner, ImVec2(corner.x + size.x, corner.y + size.y));
            }
        } else {
            ImGui::Dummy(size);
            ImGui::GetWindowDrawList()->AddRectFilled(corner, ImVec2(corner.x + size.x, corner.y + size.y), ImGui::GetColorU32(ImGuiCol_FrameBg));
        }
        UIProbe::item(v == 0 ? "vatbaker.previewSource" : "vatbaker.previewVat");
        ImGui::EndGroup();
    }
    if (!previewNote.empty()) subtle(previewNote);
    if (vatView && !vatView->vatError().empty()) warningText(vatView->vatError());
}

void VertexAnimationBakerWindow::drawOutput() {
    ImGui::SeparatorText("Output");
    const bool importing = mode == Mode::Import;
    std::string& output = importing ? importOut.output : spec.output;
    const std::string hint = importing ? defaultImportOutputPath() : (planned.empty() ? std::string("<source folder>/<source name>.vat") : planned);
    std::string typed = output;
    if (inputString("##output", hint.c_str(), typed, -FLT_MIN)) {
        output = trim(typed);
        if (importing) importOut.overwrite = false;
        else spec.overwrite = false;
        markChanged();
    }
    UIProbe::item("vatbaker.output");
    tooltip("The .vat to write (its mesh goes to <name>.vat.glb). Empty: the default shown; a file baked from the same source is\n"
            "baked again in place (its GUID and edits kept), another file of that name makes it take a free name.");
    if (!importing && output.empty() && plannedRewrite && !planned.empty()) subtle("Bakes '" + planned + "' again (same source: GUID and edits kept).");

    VatPlace& place = importing ? importOut.place : spec.place;
    const char* labels[] = {"Files only", "Beside", "Replace", "Crowd..."};
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Place");
    ImGui::SameLine(ImGui::GetFontSize() * 7.0f);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    if (ImGui::BeginCombo("##place", labels[(int)place])) {
        for (int i = 0; i < 4; i++) {
            const bool replaceInvalid = i == (int)VatPlace::Replace && (importing || !isGameObjectSource(spec.source));
            ImGui::BeginDisabled(replaceInvalid);
            if (ImGui::Selectable(labels[i], (int)place == i)) {
                place = (VatPlace)i;
                markChanged();
            }
            ImGui::EndDisabled();
            if (replaceInvalid && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("Replace needs a GameObject source (a file has nothing to replace)");
            }
        }
        ImGui::EndCombo();
    }
    UIProbe::item("vatbaker.place");
    tooltip("Files only: just the files. Beside: also a new GameObject \"<name> (VAT)\" at the source. Replace: that, and the source is\n"
            "disabled. Crowd: the new GameObject becomes a crowd of instances (count, area, seed). One undo step either way.");
    if (place == VatPlace::Crowd) {
        ImGui::Indent(ImGui::GetFontSize() * 7.0f);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
        if (ImGui::InputInt("Count", &crowdCount)) crowdCount = std::clamp(crowdCount, 1, 100000);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
        if (ImGui::InputFloat("Area (m)", &crowdArea, 1.0f, 10.0f, "%.3g")) crowdArea = std::max(0.1f, crowdArea);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
        if (ImGui::InputInt("Seed", &crowdSeed)) crowdSeed = std::max(crowdSeed, 0);
        ImGui::Unindent(ImGui::GetFontSize() * 7.0f);
        if (spec.crowd.count != crowdCount || spec.crowd.area != crowdArea || spec.crowd.seed != crowdSeedValue()) {
            spec.crowd.count = crowdCount;
            spec.crowd.area = crowdArea;
            spec.crowd.seed = crowdSeedValue();
            markChanged();
        }
    }
}

void VertexAnimationBakerWindow::drawImportSettings() {
    ImGui::SeparatorText("Import options");
    const float labelWidth = ImGui::GetFontSize() * 7.0f;
    auto label = [&](const char* text) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
        ImGui::SameLine(labelWidth);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    };
    label("Clip name");
    std::string clipName = importSpec.clipName;
    if (inputString("##clip", detection.assetName.c_str(), clipName, ImGui::GetFontSize() * 12.0f)) importSpec.clipName = clipName;
    label("FPS");
    float fps = importSpec.fps;
    const std::string fpsHint = detection.fps > 0.0f ? "export's (" + number(detection.fps) + ")" : std::string("required");
    if (ImGui::InputFloat("##fps", &fps, 0.0f, 0.0f, fps > 0.0f ? "%.4g" : fpsHint.c_str())) importSpec.fps = std::clamp(fps, 0.0f, 240.0f);
    UIProbe::item("vatbaker.import.fps");
    tooltip("Frames per second of the export (0: the one its metadata gives; needed when it has none)");
    if (detection.kind == VatImportKind::HoudiniVat3) {
        label("Axis");
        const char* axes[] = {"From the export", "Unity", "Unreal", "Houdini", "Z-up"};
        const char* names[] = {"", "unity", "unreal", "houdini", "zup"};
        int axisIndex = 0;
        for (int i = 0; i < 5; i++) {
            if (importSpec.axis == names[i]) axisIndex = i;
        }
        if (ImGui::Combo("##axis", &axisIndex, axes, 5)) importSpec.axis = names[axisIndex];
        tooltip("The Target Engine the export was made for (when its metadata doesn't say)");
        if (detection.mode == HoudiniMode::Rigid) {
            label("Upsample");
            int upsample = importSpec.upsample;
            if (ImGui::InputInt("##upsample", &upsample)) importSpec.upsample = std::clamp(upsample, 0, VAT_IMPORT_MAX_UPSAMPLE);
            tooltip("Rigid pieces: samples per exported frame (0: automatic, where fast spins need more)");
            label("Mode");
            const char* modes[] = {"Auto", "Bones", "Vertices"};
            const char* modeNames[] = {"auto", "bones", "vertices"};
            int modeIndex = importSpec.mode == "bones" ? 1 : importSpec.mode == "vertices" ? 2 : 0;
            if (ImGui::Combo("##rigid_mode", &modeIndex, modes, 3)) importSpec.mode = modeNames[modeIndex];
        }
    }
    label("Colours");
    ImGui::Checkbox("##colors", &importSpec.colors);
    tooltip("Vertices mode: the export's colour texture as a third texel per sample (vertex colour input)");
    label("Normals");
    ImGui::Checkbox("##normals", &importSpec.normals);
    label("Precision");
    const char* precisions[] = {"Auto", "Half", "Float"};
    const char* precisionNames[] = {"auto", "half", "float"};
    int precisionIndex = importSpec.precision == "half" ? 1 : importSpec.precision == "float" ? 2 : 0;
    if (ImGui::Combo("##precision", &precisionIndex, precisions, 3)) importSpec.precision = precisionNames[precisionIndex];
    label("Preset");
    const char* presets[] = {"Mobile & Web", "Desktop"};
    int presetIndex = importSpec.preset == "desktop" ? 1 : 0;
    if (ImGui::Combo("##preset", &presetIndex, presets, 2)) importSpec.preset = presetIndex == 1 ? "desktop" : "mobile_web";
}

void VertexAnimationBakerWindow::drawImportReport() {
    if (!importResult) return;
    ImGui::SeparatorText("Imported");
    if (!importResult->ok()) {
        for (const VatIssue& issue : importResult->errors) errorText(issue.message + (issue.fix.empty() ? std::string() : " (" + issue.fix + ")"));
        return;
    }
    const VatImportReport& report = importResult->report;
    ImGui::TextDisabled("%s, %d x %d %s, %s; %d frames (%d exported x %d), %d vertices, %d bones", modeLabel(report.mode), report.width,
                        report.height, textureFormatName(report.precision), bytesText(report.bytes).c_str(), report.frames,
                        report.sourceFrames, report.upsample, report.vertices, report.bones);
    if (!report.modeReason.empty()) subtle(report.modeReason);
    drawIssues(report.warnings, "import");
}

void VertexAnimationBakerWindow::drawFooter() {
    ImGui::Separator();
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = ImGui::GetFrameHeight() * 1.4f;
    std::string reason;
    const bool playing = scenePlaying(&reason);
    if (isRunning()) {
        if (ImGui::Button(ICON_FA_STOP " Cancel", ImVec2(width, height))) cancel();
        UIProbe::item("vatbaker.cancel");
        float fraction = 0.0f;
        if (run && !importJob) fraction = run->progress().fraction;
        else fraction = std::fmod((float)(now() - importStartedAt) * 0.25f, 1.0f);  // the importer reports no progress
        char overlay[32];
        std::snprintf(overlay, sizeof(overlay), importJob ? "Importing" : "%.0f%%", fraction * 100.0f);
        ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, 0), overlay);
        subtle(stageLine());
        Backend::getApp().requestRedraw();
    } else {
        const bool importing = mode == Mode::Import;
        if (!importing && reason.empty()) {
            if (mode == Mode::None) reason = "Choose a source first.";
            else if (noneChecked) reason = "Check the clips to bake.";
            else if (hasErrors(issues)) {
                for (const VatIssue& issue : issues) {
                    if (issue.error) {
                        reason = issue.message + " (" + issue.fix + ")";
                        break;
                    }
                }
            }
        }
        if (importing && reason.empty()) {
            for (const VatIssue& issue : detection.issues) {
                const bool fixed = (issue.kind == "fps_unknown" && importSpec.fps > 0.0f) || (issue.kind == "unknown_target" && !importSpec.axis.empty());
                if (issue.error && !fixed) {
                    reason = issue.message + " (" + issue.fix + ")";
                    break;
                }
            }
        }
        const bool rebake = !rebakePath.empty() && (importing ? importOut.output == rebakePath : spec.output == rebakePath);
        const char* label = importing ? (rebake ? ICON_FA_ROTATE " Re-import" : ICON_FA_FILE_IMPORT " Import")
                                      : (rebake ? ICON_FA_ROTATE " Rebake" : ICON_FA_FILM " Bake");
        const float previewWidth = importing ? 0.0f : ImGui::CalcTextSize(ICON_FA_EYE " Preview").x + ImGui::GetStyle().FramePadding.x * 4.0f;
        ImGui::BeginDisabled(!reason.empty());
        if (accentButton(label, ImVec2(width - previewWidth - (importing ? 0.0f : ImGui::GetStyle().ItemSpacing.x), height))) {
            std::string error;
            if (!bake(true, error)) {
                setStatus("Can't " + std::string(importing ? "import" : "bake") + ": " + error, true);
                Out::error("Vertex Animation: %s", status.c_str());
            }
        }
        ImGui::EndDisabled();
        UIProbe::item("vatbaker.bake");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", !reason.empty() ? reason.c_str()
                                    : importing ? "Decode the export and write the .vat and .vat.glb (one undo step)"
                                                : "Bake and write the .vat and .vat.glb (one undo step); then select it in the Project window");
        }
        if (!importing) {
            ImGui::SameLine();
            ImGui::BeginDisabled(!reason.empty());
            if (ImGui::Button(ICON_FA_EYE " Preview", ImVec2(previewWidth, height))) {
                std::string error;
                if (!bake(false, error)) setStatus("Can't bake: " + error, true);
            }
            ImGui::EndDisabled();
            UIProbe::item("vatbaker.previewRun");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("%s", !reason.empty() ? reason.c_str() : "Bake in memory only (nothing written): compare it in Preview, then Bake writes it");
            }
        }
        if (!reason.empty() && playing) warningText(reason);
    }
    if (isStale() && !isRunning()) warningText("Settings changed since this result: bake again to see them.");
    if (!status.empty()) {
        if (statusError) errorText(status);
        else subtle(status);
    }
}

// ====================================================================== show

void VertexAnimationBakerWindow::show() {
    // a running bake or import advances with the window closed too
    tick(FRAME_BUDGET_MS);
    if (isRunning()) Backend::getApp().requestRedraw();
    if (!windowOpen) return;

    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 34.0f, ImGui::GetFontSize() * 44.0f), ImGuiCond_FirstUseEver);
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
    // the estimate follows the settings a moment after the last change
    if (mode == Mode::Bake && !isRunning() && specKey() != probeKey && now() - changedAt > 0.25) refreshEstimate();

    std::string playingReason;
    const bool playing = scenePlaying(&playingReason);
    const float minimum = ImGui::GetFrameHeight() * 1.4f + ImGui::GetStyle().ItemSpacing.y * 3.0f;
    ImGui::BeginChild("##vat_body", ImVec2(0, -std::max(footerHeight, minimum)), ImGuiChildFlags_None);
    if (UIProbe::enabled()) {
        // what shows of the scrolling body, wherever it is scrolled (UI tests scroll it)
        const ImRect inner = ImGui::GetCurrentWindow()->InnerClipRect;
        UIProbe::rect("vatbaker", inner.Min.x, inner.Min.y, inner.Max.x, inner.Max.y);
    }
    ImGui::BeginDisabled(isRunning() || playing);
    drawSource();
    if (mode == Mode::Bake) {
        drawClips();
        drawSettings();
        drawEstimate();
    } else if (mode == Mode::Import) {
        drawImportSettings();
        drawImportReport();
    }
    ImGui::EndDisabled();
    if (mode != Mode::None) {
        drawPreview();
        ImGui::BeginDisabled(isRunning() || playing);
        drawOutput();
        ImGui::EndDisabled();
    }
    ImGui::EndChild();
    const float footerTop = ImGui::GetCursorPosY();
    drawFooter();
    footerHeight = ImGui::GetCursorPosY() - footerTop + ImGui::GetStyle().ItemSpacing.y;
    ImGui::End();
    if (!windowOpen) setOpen(false);
}
