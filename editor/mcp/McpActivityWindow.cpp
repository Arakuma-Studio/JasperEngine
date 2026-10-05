// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "McpActivityWindow.h"

#include "McpServer.h"

#include "AppSettings.h"
#include "Theme.h"

#include "imgui.h"

#include <ctime>
#include <string>

using namespace jasperengine::editor;
using namespace jasperengine::editor::mcp;

namespace {

std::string clockTime(std::chrono::system_clock::time_point time) {
    const std::time_t t = std::chrono::system_clock::to_time_t(time);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    char buffer[16];
    std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &local);
    return buffer;
}

std::string clientLabel(const McpSession& session) {
    std::string label = session.clientName.empty() ? "(not initialized)" : session.clientName;
    if (!session.clientVersion.empty()) label += " " + session.clientVersion;
    return label;
}

}

void McpActivityWindow::setOpen(bool open) {
    if (open && !windowOpen) focusRequested = true;
    windowOpen = open;
}

void McpActivityWindow::show() {
    if (!windowOpen) return;

    if (focusRequested) {
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }
    if (!ImGui::Begin(WINDOW_NAME, &windowOpen)) {
        ImGui::End();
        return;
    }

    if (!settingsLoaded) {
        httpEnabled = AppSettings::getMcpHttpEnabled();
        httpPort = AppSettings::getMcpHttpPort();
        settingsLoaded = true;
    }

    drawStatus();
    ImGui::Separator();
    drawActivity();

    ImGui::End();
}

void McpActivityWindow::drawStatus() {
    McpServer& server = McpServer::get();
    const McpServerStatus status = server.status();

    // stdio: only when the editor was launched by an MCP client
    if (status.stdio) {
        std::string client = "waiting for initialize";
        for (const McpSession& session : status.sessions) {
            if (session.transport == "stdio") client = clientLabel(session);
        }
        ImGui::Text("stdio: %s", status.stdioOpen ? "connected" : "disconnected");
        ImGui::SameLine();
        ImGui::TextColored(Theme::Colors::SubtleText, "(%s)", client.c_str());
    } else {
        ImGui::TextColored(Theme::Colors::SubtleText, "stdio: off (launch with `jasperengine-editor mcp` to use it)");
    }

    // HTTP
    if (status.httpRunning) {
        ImGui::Text("HTTP: http://127.0.0.1:%d/mcp", status.httpPort);
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy URL")) {
            ImGui::SetClipboardText(("http://127.0.0.1:" + std::to_string(status.httpPort) + "/mcp").c_str());
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Claude Code: claude mcp add --transport http jasperengine http://127.0.0.1:%d/mcp", status.httpPort);
        }
    } else {
        ImGui::TextColored(Theme::Colors::SubtleText, "HTTP: stopped");
        if (!status.httpError.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(Theme::Colors::ErrorText, "%s", status.httpError.c_str());
        }
    }

    ImGui::Checkbox("Serve over HTTP", &httpEnabled);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
    ImGui::InputInt("Port", &httpPort, 0, 0);
    if (httpPort < 1) httpPort = 1;
    if (httpPort > 65535) httpPort = 65535;
    ImGui::SameLine();
    const bool changed = httpEnabled != AppSettings::getMcpHttpEnabled() || httpPort != AppSettings::getMcpHttpPort() ||
                         httpEnabled != status.httpRunning || (httpEnabled && httpPort != status.httpPort);
    ImGui::BeginDisabled(!changed);
    if (ImGui::Button("Apply")) {
        AppSettings::setMcpHttp(httpEnabled, httpPort);
        server.setHttpEnabled(httpEnabled, httpPort);
    }
    ImGui::EndDisabled();

    int httpSessions = 0;
    for (const McpSession& session : status.sessions) {
        if (session.transport != "http") continue;
        if (httpSessions++ == 0) ImGui::TextUnformatted("HTTP clients:");
        ImGui::BulletText("%s", clientLabel(session).c_str());
    }

    ImGui::Text("Waiting calls: %zu   Running jobs: %zu", status.pendingCalls, status.runningJobs);
}

void McpActivityWindow::drawActivity() {
    McpServer& server = McpServer::get();
    if (ImGui::SmallButton("Clear")) server.clearActivity();
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &autoScroll);

    const std::vector<McpActivityEntry> entries = server.activity();
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("##McpActivity", 5, flags)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Time");
    ImGui::TableSetupColumn("Tool");
    ImGui::TableSetupColumn("Result");
    ImGui::TableSetupColumn("ms");
    ImGui::TableSetupColumn("Arguments / message", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();

    for (const McpActivityEntry& entry : entries) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(clockTime(entry.time).c_str());
        if (ImGui::IsItemHovered() && !entry.client.empty()) ImGui::SetTooltip("%s", entry.client.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(entry.tool.c_str());
        ImGui::TableNextColumn();
        if (entry.status == "error") {
            ImGui::TextColored(Theme::Colors::ErrorText, "error");
        } else if (entry.status == "running") {
            ImGui::TextColored(Theme::Colors::WarningText, "running");
        } else if (entry.status == "cancelled") {
            ImGui::TextColored(Theme::Colors::SubtleText, "cancelled");
        } else {
            ImGui::TextUnformatted("ok");
        }
        ImGui::TableNextColumn();
        if (entry.status != "running") ImGui::Text("%.0f", entry.durationMs);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(entry.args.c_str());
        if (ImGui::IsItemHovered() && !entry.message.empty()) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
            ImGui::TextUnformatted(entry.message.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }
    if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndTable();
}
