// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LoadingWindow.h"
#include "Backend.h"
#include "Theme.h"
#include "resources/icons/doriax-logo_png.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <vector>

using namespace doriax::editor;

LoadingWindow::LoadingWindow() {
}

LoadingWindow::~LoadingWindow() {
}

void LoadingWindow::show() {
    App& app = Backend::getApp();
    if (app.isProjectLoading()) {
        drawProjectOverlay(app.getLoadingStatus());
        return;
    }

    bool hasBuilds = ResourceProgress::hasActiveBuilds();

    // Build progress comes from worker threads, so nothing else keeps the loop awake
    if (hasBuilds) {
        app.requestRedraw();
    }

    bool dragDropActive = ImGui::IsDragDropActive();

    if (hasBuilds && !dragDropActive) {
        if (!wasShowing) {
            ImGui::OpenPopup("Loading");
            wasShowing = true;
        }

        OverallBuildProgress overallProgress = ResourceProgress::getOverallProgress();
        drawProgressModal(overallProgress);
    } else {
        if (wasShowing) {
            ImGui::CloseCurrentPopup();
            wasShowing = false;
        }
    }
}

void LoadingWindow::drawProjectOverlay(const std::string& status) {
    if (!logoLoaded) {
        TextureData data;
        data.loadTextureFromMemory(doriax_logo_png, doriax_logo_png_len);
        logo.setData("editor:loading:logo", data);
        logo.load();
        logoLoaded = true;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoInputs;

    if (ImGui::Begin("##ProjectLoading", nullptr, flags)) {
        const ImVec2 windowSize = ImGui::GetWindowSize();
        const float barWidth = std::min(Theme::dpi(360.0f), windowSize.x);
        const float barHeight = Theme::dpi(6.0f);

        TextureRender* logoRender = logo.getRender();
        const float logoWidth = std::min(Theme::dpi(150.0f), windowSize.x);
        const float logoHeight = logo.getWidth() > 0
            ? logoWidth * logo.getHeight() / logo.getWidth() : 0.0f;
        const char* statusText = status.empty() ? "Loading..." : status.c_str();
        const ImVec2 statusSize = ImGui::CalcTextSize(statusText);

        const float contentHeight = logoHeight + Theme::dpi(12.0f) + statusSize.y + Theme::dpi(18.0f) + barHeight;
        ImGui::SetCursorPos(ImVec2(
            (windowSize.x - logoWidth) * 0.5f,
            (windowSize.y - contentHeight) * 0.45f));
        if (logoRender && logoRender->isCreated()) {
            ImGui::Image(Backend::getImGuiTexture(logoRender), ImVec2(logoWidth, logoHeight));
        } else {
            ImGui::Dummy(ImVec2(logoWidth, logoHeight));
        }

        ImGui::Dummy(ImVec2(0.0f, Theme::dpi(8.0f)));
        ImGui::SetCursorPosX((windowSize.x - statusSize.x) * 0.5f);
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::SubtleText);
        ImGui::TextUnformatted(statusText);
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0.0f, Theme::dpi(16.0f)));
        ImGui::SetCursorPosX((windowSize.x - barWidth) * 0.5f);

        ImGui::ProgressBar(-static_cast<float>(ImGui::GetTime()),
                           ImVec2(barWidth, barHeight), "");
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
}

void LoadingWindow::drawProgressModal(const OverallBuildProgress& progress) {
    // Center the modal
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(400, 0), ImGuiCond_Always);

    // Set transparency
    ImGui::SetNextWindowBgAlpha(0.85f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_Modal |
                            ImGuiWindowFlags_NoResize |
                            ImGuiWindowFlags_NoMove |
                            ImGuiWindowFlags_NoCollapse |
                            ImGuiWindowFlags_NoSavedSettings |
                            ImGuiWindowFlags_AlwaysAutoResize;

    if (ImGui::BeginPopupModal("Loading", nullptr, flags)) {
        if (progress.hasActiveBuilds) {
            // Create a formatted string first, then use it with TreeNode
            char buildingText[256];
            snprintf(buildingText, sizeof(buildingText), "Building %d resources", progress.totalBuilds);

            if (ImGui::TreeNode("resource_list", "%s", buildingText)) {
                // Get all active builds for the dropdown
                std::vector<ResourceBuildInfo> allBuilds = ResourceProgress::getAllActiveBuilds();

                for (const auto& build : allBuilds) {
                    ImGui::Bullet();
                    ImGui::Text("%s - %s - %.1f%%",
                        ResourceProgress::getResourceTypeName(build.type).c_str(),
                        build.name.c_str(),
                        build.progress * 100.0f);
                }
                ImGui::TreePop();
            }

            ImGui::Spacing();

            // Overall progress bar
            ImGui::ProgressBar(progress.totalProgress, ImVec2(-1.0f, 0.0f), "");

            // Progress text
            ImGui::Text("Progress: %.1f%%", progress.totalProgress * 100.0f);

            ImGui::Spacing();
        }

        ImGui::EndPopup();
    }
}
