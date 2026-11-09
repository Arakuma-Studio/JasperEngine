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

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace jasperengine::editor {

namespace {

    constexpr pixelart::fx::Backdrop kBackdrops[7] = {pixelart::fx::Backdrop::Checker, pixelart::fx::Backdrop::Dark, pixelart::fx::Backdrop::Light,
                                                      pixelart::fx::Backdrop::Sky, pixelart::fx::Backdrop::Ground, pixelart::fx::Backdrop::Black,
                                                      pixelart::fx::Backdrop::Scene};

    // the backdrop names as the window shows them (Unity-like capitalised words)
    constexpr const char* kBackdropLabels[7] = {"Checker", "Dark", "Light", "Sky", "Ground", "Black", "Scene Colour"};

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
}

void ContactSheetWindow::capture() {
    SceneProject* scene = project->getScene(sceneId);
    std::string error;
    std::unique_ptr<pixelart::fx::CaptureTarget> target = pixelart::fx::makeEntityTarget(project, scene, entity, "", error);
    if (!target) {
        notice = "Can't capture: " + error + ".";
        noticeIsError = true;
        return;
    }
    pixelart::fx::CaptureSettings settings;
    settings.mode = sceneMode ? pixelart::fx::CaptureMode::Scene : pixelart::fx::CaptureMode::Isolated;
    settings.duration = std::clamp((double)duration, 1.0 / 60.0, 60.0);
    settings.loop = loop;
    settings.times = pixelart::fx::sheetTimes(std::clamp(frames, 1, 64), settings.duration, loop);
    settings.lints = lints;
    settings.project = project;
    settings.pixelsPerUnit = std::max(1, project->getPixelsPerUnit());
    if (sceneMode && project->getPlaySessionSceneId() != NULL_PROJECT_SCENE) {
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
    SceneProject* scene = project->getScene(sceneId);
    const std::string name = scene && scene->scene ? scene->scene->getEntityName(entity) : std::string("Contact Sheet");
    const std::string chosen = FileDialogs::saveFileDialog(project->getAssetsPath().string(), name + " Contact Sheet.png", "PNG image", "png");
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

void ContactSheetWindow::show() {
    for (Texture& old : retired) old.destroy();
    retired.clear();
    if (!windowOpen) return;
    collect();
    ImGui::SetNextWindowSize(ImVec2(Theme::dpi(720.0f), Theme::dpi(560.0f)), ImGuiCond_FirstUseEver);
    bool keepOpen = true;
    if (ImGui::Begin("Contact Sheet###ContactSheetWindow", &keepOpen, ImGuiWindowFlags_NoCollapse)) {
        SceneProject* scene = project->getScene(sceneId);
        const bool valid = scene && scene->scene && scene->scene->isEntityCreated(entity);
        ImGui::TextDisabled("GameObject: %s", valid ? scene->scene->getEntityName(entity).c_str() : "(none)");
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
        ImGui::Checkbox("Scene Mode##cs_scene", &sceneMode);
        ImGui::SetItemTooltip("Through the scene's camera, the whole scene; off: the GameObject alone on the backdrops");
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
