// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ContactSheetWindow.h"

#include "Out.h"
#include "Project.h"
#include "Theme.h"
#include "pixelart/PixelProblem.h"
#include "pixelart/RgbaImage.h"
#include "pixelart/fx/ContactSheet.h"
#include "pixelart/fx/EffectCapture.h"
#include "pixelart/lint/EffectLints.h"
#include "util/FileDialogs.h"
#include "util/FileUtils.h"
#include "util/Util.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace jasperengine::editor {

namespace {

    constexpr pixelart::fx::Backdrop kBackdrops[7] = {pixelart::fx::Backdrop::Checker, pixelart::fx::Backdrop::Dark, pixelart::fx::Backdrop::Light,
                                                      pixelart::fx::Backdrop::Sky, pixelart::fx::Backdrop::Ground, pixelart::fx::Backdrop::Black,
                                                      pixelart::fx::Backdrop::Scene};

    // the backdrop names as the window shows them (Unity-like capitalised words)
    constexpr const char* kBackdropLabels[7] = {"Checker", "Dark", "Light", "Sky", "Ground", "Black", "Scene Colour"};

    constexpr const char* kTargetLabels[4] = {"GameObject", "Animation Clip", "Particles", "Transition"};

    constexpr jasperengine::pixelart::TransitionKind kTransitionKinds[jasperengine::pixelart::kTransitionKindCount] = {
        jasperengine::pixelart::TransitionKind::IRIS, jasperengine::pixelart::TransitionKind::BLINDS, jasperengine::pixelart::TransitionKind::SLASH,
        jasperengine::pixelart::TransitionKind::PIXEL_DISSOLVE, jasperengine::pixelart::TransitionKind::CRT_OFF,
        jasperengine::pixelart::TransitionKind::BURN_AWAY, jasperengine::pixelart::TransitionKind::FADE};
    constexpr const char* kTransitionLabels[jasperengine::pixelart::kTransitionKindCount] = {"Iris", "Blinds", "Slash", "Pixel Dissolve", "CRT Off",
                                                                                              "Burn Away", "Fade"};
    constexpr const char* kPhaseLabels[3] = {"Play", "Cover", "Reveal"};
    constexpr const char* kShapeLabels[3] = {"Circle", "Diamond", "Square"};
    constexpr const char* kEaseLabels[4] = {"Linear", "In", "Out", "In Out"};

    // a combo over labels; the picked index in `index`
    void labelCombo(const char* label, int& index, const char* const* labels, int count) {
        ImGui::SetNextItemWidth(Theme::dpi(130.0f));
        if (ImGui::BeginCombo(label, labels[index])) {
            for (int i = 0; i < count; i++) {
                const bool selected = i == index;
                if (ImGui::Selectable(labels[i], selected)) index = i;
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    // a combo over project files, shown by file name; the picked path in `path`
    void fileCombo(const char* label, std::string& path, const std::vector<std::string>& files, const char* empty) {
        namespace fs = std::filesystem;
        const std::string shown = path.empty() ? std::string(files.empty() ? empty : "(pick one)") : FileUtils::pathToUtf8(fs::path(path).filename());
        ImGui::SetNextItemWidth(Theme::dpi(220.0f));
        if (ImGui::BeginCombo(label, shown.c_str())) {
            for (const std::string& file : files) {
                const bool selected = file == path;
                if (ImGui::Selectable(FileUtils::pathToUtf8(fs::path(file).filename()).c_str(), selected)) path = file;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", file.c_str());
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

}

ContactSheetWindow::ContactSheetWindow(Project* project) : project(project) {}

ContactSheetWindow::~ContactSheetWindow() {
    if (running) running->cancel();
    image.retire(retired);
    for (Texture& old : retired) old.destroy();
}

void ContactSheetWindow::open(uint32_t scene, Entity target) {
    sceneId = scene;
    entity = target;
    windowOpen = true;
    notice.clear();
    transition = jasperengine::pixelart::TransitionOptions::fromDefaults();
    scanTargetFiles();
}

void ContactSheetWindow::scanTargetFiles() {
    namespace fs = std::filesystem;
    clipFiles.clear();
    particleFiles.clear();
    std::error_code ec;
    for (fs::recursive_directory_iterator it(project->getAssetsPath(), ec), end; it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const fs::path file = it->path();
        if (file.extension() == ".anim") {
            clipFiles.push_back(fs::relative(file, project->getProjectPath(), ec).generic_string());
        } else if (Util::isParticleEffectFile(file.string())) {
            particleFiles.push_back(project->normalizeToAssetsRelative(file).generic_string());
        }
    }
    std::sort(clipFiles.begin(), clipFiles.end());
    std::sort(particleFiles.begin(), particleFiles.end());
}

std::string ContactSheetWindow::sheetName() const {
    namespace fs = std::filesystem;
    const SceneProject* scene = project->getScene(sceneId);
    switch (targetKind) {
        case TargetKind::Particles: return particlesPath.empty() ? std::string("Particles") : FileUtils::pathToUtf8(fs::path(particlesPath).stem());
        case TargetKind::Transition: return std::string(kTransitionLabels[std::find(kTransitionKinds, kTransitionKinds + jasperengine::pixelart::kTransitionKindCount, transitionKind) - kTransitionKinds]) + " Transition";
        default: return scene && scene->scene ? scene->scene->getEntityName(entity) : std::string("Contact Sheet");
    }
}

void ContactSheetWindow::capture() {
    if (pixelart::fx::EffectCapture::anyRunning()) {
        notice = "Another capture is running: wait for it to end, then capture again.";
        noticeIsError = true;
        return;
    }
    SceneProject* scene = project->getScene(sceneId);
    std::string error;
    std::unique_ptr<pixelart::fx::CaptureTarget> target;
    const bool hasGameObject = targetKind == TargetKind::GameObject || targetKind == TargetKind::Clip;
    if (hasGameObject) {
        target = pixelart::fx::makeEntityTarget(project, scene, entity, targetKind == TargetKind::Clip ? clipPath : std::string(), error);
    } else if (targetKind == TargetKind::Particles) {
        target = pixelart::fx::makeParticlesTarget(project, particlesPath, error);
    } else {
        jasperengine::pixelart::TransitionOptions options = transition;
        // no Pixel Perfect Camera draws a capture: pixel size 0 is one screen pixel per art pixel
        options.pixelSize = std::max(options.pixelSize, 1);
        const pixelart::fx::CaptureTransitionPhase phases[3] = {pixelart::fx::CaptureTransitionPhase::Play, pixelart::fx::CaptureTransitionPhase::Cover,
                                                                 pixelart::fx::CaptureTransitionPhase::Reveal};
        target = pixelart::fx::makeTransitionTarget(transitionKind, options, phases[std::clamp(transitionPhase, 0, 2)],
                                                    std::clamp((double)duration, 1.0 / 60.0, 60.0), artWidth, artHeight);
    }
    if (!target) {
        notice = "Can't capture: " + error + ".";
        noticeIsError = true;
        return;
    }
    if (!pixelart::fx::captureFitsLimits(*target, std::clamp(frames, 1, 64), error)) {
        notice = "Can't capture: " + error + ". Lower the Art Width, Art Height or Pixel Size, or the Frames.";
        noticeIsError = true;
        return;
    }
    pixelart::fx::CaptureSettings settings;
    const bool useSceneMode = sceneMode && hasGameObject;
    settings.mode = useSceneMode ? pixelart::fx::CaptureMode::Scene : pixelart::fx::CaptureMode::Isolated;
    settings.duration = std::clamp((double)duration, 1.0 / 60.0, 60.0);
    settings.loop = loop;
    settings.times = pixelart::fx::sheetTimes(std::clamp(frames, 1, 64), settings.duration, loop);
    settings.lints = lints;
    settings.project = project;
    settings.pixelsPerUnit = std::max(1, project->getPixelsPerUnit());
    if (useSceneMode && project->getPlaySessionSceneId() != NULL_PROJECT_SCENE) {
        notice = "Scene mode captures a stopped scene here; in Play, use capture_contact_sheet (it steps the paused game).";
        noticeIsError = true;
        return;
    }
    running = std::make_shared<pixelart::fx::EffectCapture>(settings, std::move(target));
    running->start();
    notice = "Capturing...";
    noticeIsError = false;
}

void ContactSheetWindow::collect() {
    if (!running || !running->isFinished()) return;
    std::shared_ptr<pixelart::fx::EffectCapture> done = std::move(running);
    if (done->isFailed()) {
        notice = done->getError();
        noticeIsError = true;
        return;
    }
    pixelart::fx::SheetSettings settings;
    settings.scale = scale;
    settings.maxWidth = 1024;
    settings.backdrops.clear();
    for (int i = 0; i < 7; i++) {
        if (backdrops[i]) settings.backdrops.push_back(kBackdrops[i]);
    }
    if (settings.backdrops.empty()) settings.backdrops.push_back(pixelart::fx::Backdrop::Checker);
    sheet = pixelart::fx::composeCapture(done->getOutput(), settings);
    report = pixelart::fx::lintCapture(done->getOutput(), done->getSettings());
    verdict = done->getSettings().lints ? pixelart::verdictName(pixelart::lint::effectVerdict(report)) : "";
    pixelart::RgbaImage rgba(sheet.width(), sheet.height());
    for (int y = 0; y < sheet.height(); y++) {
        for (int x = 0; x < sheet.width(); x++) {
            const pixelart::PixelRgba p = sheet.at(x, y);
            rgba.set(x, y, jasperengine::pixelart::Rgba8{p[0], p[1], p[2], p[3]});
        }
    }
    image.upload(rgba, retired);
    notice = std::to_string(done->getOutput().frames.size()) + " frames of " + done->getTarget().label() + ".";
    noticeIsError = false;
}

void ContactSheetWindow::saveAs() {
    if (sheet.empty()) return;
    const std::string chosen = FileDialogs::saveFileDialog(project->getAssetsPath().string(), sheetName() + " Contact Sheet.png", "PNG image", "png");
    if (chosen.empty()) return;
    std::filesystem::path target = FileUtils::pathFromUtf8(chosen);
    if (target.extension() != ".png") target += ".png";
    std::string error;
    if (!PaletteWindow::writeFiles(project, {{target, sheet.encodePng()}}, "Contact Sheet (" + FileUtils::pathToUtf8(target.filename()) + ")", "", error)) {
        notice = error;
        noticeIsError = true;
        Out::error("Contact Sheet: %s", error.c_str());
        return;
    }
    notice = "Saved " + FileUtils::pathToUtf8(target.filename()) + ".";
    noticeIsError = false;
}

void ContactSheetWindow::drawTargetRow(bool& canCapture) {
    SceneProject* scene = project->getScene(sceneId);
    const bool gameObject = scene && scene->scene && scene->scene->isEntityCreated(entity);
    const char* preview = kTargetLabels[(int)targetKind];
    ImGui::SetNextItemWidth(Theme::dpi(150.0f));
    if (ImGui::BeginCombo("Target##cs_target", preview)) {
        for (int i = 0; i < 4; i++) {
            if (ImGui::Selectable(kTargetLabels[i], i == (int)targetKind)) {
                targetKind = (TargetKind)i;
                scanTargetFiles();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    switch (targetKind) {
        case TargetKind::GameObject:
            ImGui::TextDisabled("GameObject: %s", gameObject ? scene->scene->getEntityName(entity).c_str() : "(none)");
            canCapture = gameObject;
            break;
        case TargetKind::Clip:
            ImGui::TextDisabled("On %s", gameObject ? scene->scene->getEntityName(entity).c_str() : "(no GameObject)");
            ImGui::SameLine();
            fileCombo("Clip##cs_clip", clipPath, clipFiles, "(no .anim in the project)");
            canCapture = gameObject && !clipPath.empty();
            break;
        case TargetKind::Particles:
            fileCombo("Effect##cs_particles", particlesPath, particleFiles, "(no .particles in the project)");
            canCapture = !particlesPath.empty();
            break;
        case TargetKind::Transition:
            ImGui::TextDisabled("the Project Settings' transition defaults, drawn over transparent black");
            canCapture = true;
            drawTransitionOptions();
            break;
    }
}

void ContactSheetWindow::drawTransitionOptions() {
    using namespace jasperengine::pixelart;
    int kind = (int)(std::find(kTransitionKinds, kTransitionKinds + kTransitionKindCount, transitionKind) - kTransitionKinds);
    labelCombo("Kind##cs_transition_kind", kind, kTransitionLabels, kTransitionKindCount);
    transitionKind = kTransitionKinds[kind];
    ImGui::SameLine();
    labelCombo("Phase##cs_transition_phase", transitionPhase, kPhaseLabels, 3);
    ImGui::SameLine();
    int ease = (int)transition.ease;
    labelCombo("Ease##cs_transition_ease", ease, kEaseLabels, 4);
    transition.ease = (TransitionEase)ease;

    float colour[4] = {transition.ink.x, transition.ink.y, transition.ink.z, 1.0f};
    if (ImGui::ColorEdit3("Colour##cs_transition_colour", colour, ImGuiColorEditFlags_NoInputs)) transition.ink = Vector4(colour[0], colour[1], colour[2], 1.0f);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(120.0f));
    char ramp[256];
    std::snprintf(ramp, sizeof(ramp), "%s", transition.ramp.c_str());
    if (ImGui::InputText("Ramp##cs_transition_ramp", ramp, sizeof(ramp))) transition.ramp = ramp;
    ImGui::SetItemTooltip("A ramp name or a <palette>.palette#ramp:<name> path; empty: the built-in ember ramp");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(90.0f));
    ImGui::SliderInt("Rim##cs_transition_rim", &transition.rim, 0, kTransitionMaxRim);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(90.0f));
    ImGui::SliderFloat("Glow##cs_transition_glow", &transition.glow, 0.0f, 1.0f, "%.2f");

    ImGui::SetNextItemWidth(Theme::dpi(90.0f));
    ImGui::SliderInt("Pixel Size##cs_transition_pixel_size", &transition.pixelSize, 0, 16, transition.pixelSize == 0 ? "Auto" : "%d");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(90.0f));
    ImGui::InputInt("Art Width##cs_transition_width", &artWidth, 0, 0);
    artWidth = std::clamp(artWidth, 8, 1024);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(90.0f));
    ImGui::InputInt("Art Height##cs_transition_height", &artHeight, 0, 0);
    artHeight = std::clamp(artHeight, 8, 1024);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(90.0f));
    int seed = (int)transition.seed;
    if (ImGui::InputInt("Seed##cs_transition_seed", &seed, 0, 0)) transition.seed = (uint32_t)std::max(seed, 0);

    if (transitionKind == TransitionKind::IRIS) {
        int shape = (int)transition.shape;
        labelCombo("Shape##cs_transition_shape", shape, kShapeLabels, 3);
        transition.shape = (IrisShape)shape;
        ImGui::SameLine();
        float center[2] = {transition.center.x, transition.center.y};
        ImGui::SetNextItemWidth(Theme::dpi(160.0f));
        if (ImGui::DragFloat2("Centre##cs_transition_center", center, 0.01f, 0.0f, 1.0f, "%.2f")) transition.center = Vector2(center[0], center[1]);
    } else if (transitionKind == TransitionKind::BLINDS) {
        ImGui::SetNextItemWidth(Theme::dpi(90.0f));
        ImGui::SliderInt("Count##cs_transition_count", &transition.count, 1, 64);
        ImGui::SameLine();
        ImGui::Checkbox("Vertical##cs_transition_vertical", &transition.vertical);
    } else if (transitionKind == TransitionKind::SLASH) {
        ImGui::SetNextItemWidth(Theme::dpi(90.0f));
        ImGui::DragFloat("Angle##cs_transition_angle", &transition.angle, 1.0f, -360.0f, 360.0f, "%.0f°");
    } else if (transitionKind == TransitionKind::PIXEL_DISSOLVE) {
        ImGui::SetNextItemWidth(Theme::dpi(90.0f));
        ImGui::SliderInt("Cell##cs_transition_cell", &transition.cell, 1, 256);
    } else if (transitionKind == TransitionKind::FADE) {
        ImGui::Checkbox("Through Ramp##cs_transition_through_ramp", &transition.throughRamp);
    }
    if (transitionPhase == 0) {
        ImGui::SetNextItemWidth(Theme::dpi(90.0f));
        // below 0: 10 % of the duration (TransitionOptions::hold)
        ImGui::DragFloat("Hold##cs_transition_hold", &transition.hold, 0.01f, -1.0f, 60.0f, transition.hold < 0.0f ? "Auto" : "%.2f s");
    }
}

void ContactSheetWindow::show() {
    for (Texture& old : retired) old.destroy();
    retired.clear();
    if (!windowOpen) return;
    collect();
    ImGui::SetNextWindowSize(ImVec2(Theme::dpi(720.0f), Theme::dpi(560.0f)), ImGuiCond_FirstUseEver);
    bool keepOpen = true;
    if (ImGui::Begin("Contact Sheet###ContactSheetWindow", &keepOpen, ImGuiWindowFlags_NoCollapse)) {
        bool valid = false;
        drawTargetRow(valid);
        ImGui::SetNextItemWidth(Theme::dpi(120.0f));
        ImGui::SliderInt("Frames##cs_frames", &frames, 1, 64);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(Theme::dpi(120.0f));
        ImGui::DragFloat("Duration##cs_duration", &duration, 0.01f, 1.0f / 60.0f, 60.0f, "%.2f s");
        ImGui::SameLine();
        ImGui::Checkbox("Loop##cs_loop", &loop);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(Theme::dpi(110.0f));
        ImGui::SliderInt("Scale##cs_scale", &scale, 0, 16, scale == 0 ? "Auto" : "x%d");
        ImGui::TextUnformatted("Backdrops");
        for (int i = 0; i < 7; i++) {
            ImGui::SameLine();
            ImGui::Checkbox((std::string(kBackdropLabels[i]) + "##cs_backdrop" + std::to_string(i)).c_str(), &backdrops[i]);
        }
        // a .particles asset or a transition has no scene of its own to show
        ImGui::BeginDisabled(targetKind == TargetKind::Particles || targetKind == TargetKind::Transition);
        ImGui::Checkbox("Scene Mode##cs_scene", &sceneMode);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Through the scene's camera, the whole scene; off: the target alone on the backdrops");
        ImGui::SameLine();
        ImGui::Checkbox("Lints##cs_lints", &lints);
        ImGui::SetItemTooltip("Spill, one-shot end, loop seam, determinism, flicker, palette, partial alpha and frame cost checks");
        ImGui::SameLine();
        ImGui::BeginDisabled(!valid || running != nullptr);
        if (ImGui::Button("Capture##cs_capture")) capture();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(sheet.empty());
        if (ImGui::Button("Save As...##cs_save")) saveAs();
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Write the sheet as a PNG (one undo step)");
        if (!notice.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, noticeIsError ? Theme::Colors::ErrorText : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", notice.c_str());
            ImGui::PopStyleColor();
        }
        if (!report.results.empty()) {
            ImGui::Text("Verdict: %s", verdict.c_str());
            if (ImGui::BeginTable("##cs_checks", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Check");
                ImGui::TableSetupColumn("Status");
                ImGui::TableSetupColumn("Measured");
                ImGui::TableSetupColumn("Threshold");
                ImGui::TableSetupColumn("Message");
                ImGui::TableHeadersRow();
                for (const pixelart::lint::LintResult& r : report.results) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(r.id.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(pixelart::lint::lintStatusName(r.status));
                    ImGui::TableNextColumn();
                    if (r.status != pixelart::lint::LintStatus::Skip) ImGui::Text("%g", r.measured);
                    ImGui::TableNextColumn();
                    if (r.status != pixelart::lint::LintStatus::Skip) ImGui::Text("%g", r.threshold);
                    ImGui::TableNextColumn();
                    ImGui::TextWrapped("%s%s%s", r.message.c_str(), r.fix.empty() ? "" : " Fix: ", r.fix.c_str());
                }
                ImGui::EndTable();
            }
        }
        if (image.valid() && ImGui::BeginChild("##cs_image", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar)) {
            ImGui::Image(image.imguiId(), ImVec2((float)image.width, (float)image.height));
            ImGui::EndChild();
        } else if (image.valid()) {
            ImGui::EndChild();
        }
    }
    ImGui::End();
    if (!keepOpen) windowOpen = false;
}

}
