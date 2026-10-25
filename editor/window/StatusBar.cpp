// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "StatusBar.h"

#include "Theme.h"
#include "io/watch/FileWatchService.h"
#include "reload/HotReloadService.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <cmath>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;

StatusBar::StatusBar(std::function<void()> openAssetPipeline)
    : openAssetPipeline(std::move(openAssetPipeline)) {
}

void StatusBar::show() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (!viewport) return;
    const float height = ImGui::GetFrameHeight();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Theme::dpi(ImVec2(8.0f, 2.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    const bool open = ImGui::BeginViewportSideBar("##StatusBar", viewport, ImGuiDir_Down, height, flags);
    ImGui::PopStyleVar(2);
    if (open) {
        const FileWatchService::Stats stats = FileWatchService::get().stats();
        HotReloadService& reloads = HotReloadService::get();
        std::string label;
        if (!reloads.getAutoRefresh()) {
            label = "Auto Refresh off: " + std::to_string(reloads.pendingExternalChanges()) + " changes";
        } else if (stats.state == FileWatchService::State::Watching) {
            label = "Watching (" + stats.backend + ")";
        } else if (stats.state == FileWatchService::State::Polling || stats.state == FileWatchService::State::Degraded) {
            char text[64];
            const double seconds = stats.pollInterval.count() / 1000.0;
            std::snprintf(text, sizeof(text), std::fmod(seconds, 1.0) == 0.0 ? "Polling %.0f s" : "Polling %.2g s", seconds);
            label = text;
        } else {
            label = "Not watching files";
        }
        if (reloads.reloadingCount() > 0) label = "Reloading " + std::to_string(reloads.reloadingCount()) + "... " + label;

        const float width = ImGui::CalcTextSize(label.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - width));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 0.0f));
        if (ImGui::Button(label.c_str()) && openAssetPipeline) openAssetPipeline();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        if (ImGui::BeginItemTooltip()) {
            ImGui::Text("File watcher: %s%s", FileWatchService::stateName(stats.state),
                        stats.state == FileWatchService::State::Degraded ? " (native events failed, polling)" : "");
            if (!stats.backend.empty()) ImGui::Text("Backend: %s, %zu watch%s", stats.backend.c_str(), stats.watches, stats.watches == 1 ? "" : "es");
            ImGui::Text("Changes: %llu events, %llu coalesced, %llu own writes ignored, %llu overflows",
                        static_cast<unsigned long long>(stats.events), static_cast<unsigned long long>(stats.coalesced),
                        static_cast<unsigned long long>(stats.echoesSuppressed), static_cast<unsigned long long>(stats.overflows));
            ImGui::Text("Reloads: %llu", static_cast<unsigned long long>(reloads.reloadCount()));
            if (!reloads.getAutoRefresh()) ImGui::TextUnformatted("Assets > Refresh (Ctrl+R) applies the changes.");
            ImGui::TextDisabled("Click: Edit > Preferences > Asset Pipeline");
            ImGui::EndTooltip();
        }
    }
    ImGui::End();
}
