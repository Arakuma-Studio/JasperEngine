// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "McpActivityWindow.h"

#include "McpServer.h"

#include "AppSettings.h"
#include "Theme.h"
#include "util/FileUtils.h"

#include "imgui.h"

#include <ctime>
#include <string>

using namespace jasperengine::editor;

namespace jasperengine::editor::mcp {

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

std::string httpUrl(int port) {
    return "http://127.0.0.1:" + std::to_string(port) + "/mcp";
}

// Claude Code's command; other clients take the URL and the same header
std::string connectCommand(int port, const std::string& token) {
    return "claude mcp add --transport http jasper " + httpUrl(port) +
           " --header \"Authorization: Bearer " + token + "\"";
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
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::SubtleText);
        ImGui::TextWrapped("stdio: off (an MCP client launches `jasperengine-editor mcp`)");
        ImGui::PopStyleColor();
    }

    // HTTP
    const std::string token = server.httpToken();
    if (status.httpRunning) {
        ImGui::Text("HTTP: %s", httpUrl(status.httpPort).c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy URL")) {
            ImGui::SetClipboardText(httpUrl(status.httpPort).c_str());
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy connect command")) {
            ImGui::SetClipboardText(connectCommand(status.httpPort, token).c_str());
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Claude Code:\n  %s\nOther clients: the URL, with the header \"Authorization: Bearer <token>\".",
                              connectCommand(status.httpPort, showToken ? token : "<token>").c_str());
        }
    } else {
        ImGui::TextColored(Theme::Colors::SubtleText, "HTTP: stopped");
        if (!status.httpError.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(Theme::Colors::ErrorText, "%s", status.httpError.c_str());
        }
    }

    ImGui::Checkbox("Serve over HTTP", &httpEnabled);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.0f);
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

    // Every HTTP request needs the token (stdio clients launched the editor and need none)
    if (token.empty()) {
        ImGui::TextColored(Theme::Colors::SubtleText, "Token: made when HTTP is first enabled");
    } else {
        const std::string shown = showToken ? token : token.substr(0, 4) + std::string(12, '*');
        ImGui::Text("Token: %s", shown.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(showToken ? "Hide" : "Show")) showToken = !showToken;
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy token")) ImGui::SetClipboardText(token.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("New token")) ImGui::OpenPopup("##McpNewToken");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Replaces the token: HTTP clients stop working until they get the new one.");
        if (ImGui::BeginPopup("##McpNewToken")) {
            ImGui::TextUnformatted("Make a new token? HTTP clients need the new one to keep working.");
            if (ImGui::Button("New token")) {
                server.newHttpToken();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    bool allowChanges = status.httpAllowChanges;
    if (ImGui::Checkbox("Allow changes", &allowChanges)) {
        server.setHttpAllowChanges(allowChanges);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Off: HTTP clients only see and run the read-only tools (inspect, screenshots, checks), and\n"
                          "batch is not available. Clients may need to list the tools again. stdio clients keep every tool.");
    }
    if (!status.httpAllowChanges) {
        ImGui::SameLine();
        ImGui::TextColored(Theme::Colors::WarningText, "HTTP clients are read-only");
    }

    int httpSessions = 0;
    for (const McpSession& session : status.sessions) {
        if (session.transport != "http") continue;
        if (httpSessions++ == 0) ImGui::TextUnformatted("HTTP clients:");
        ImGui::BulletText("%s", clientLabel(session).c_str());
    }

    ImGui::Text("Waiting calls: %zu   Running jobs: %zu", status.pendingCalls, status.runningJobs);
}

void McpActivityWindow::drawCallLog() {
    McpServer& server = McpServer::get();
    const bool fromEnvironment = server.callLogFromEnvironment();
    const std::filesystem::path path = server.callLogPath();
    bool logCalls = fromEnvironment ? !path.empty() : AppSettings::getMcpLogCalls();
    ImGui::BeginDisabled(fromEnvironment);
    if (ImGui::Checkbox("Log Calls To File", &logCalls)) {
        AppSettings::setMcpLogCalls(logCalls);
        server.setCallLogEnabled(logCalls);
    }
    ImGui::EndDisabled();
    const std::string shown = FileUtils::pathToUtf8(path.empty() ? McpServer::defaultCallLogPath() : path);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Appends one JSON line per tool call to %s: time, tool, group, ms, result, result size and\n"
                          "client, never the arguments or the results. Past 16 MB it moves to <file>.1.\n"
                          "Summarise it with tools/mcp/call_profile.py.%s",
                          shown.c_str(), fromEnvironment ? "\nSet by JASPER_MCP_LOG for this editor." : "");
    }
    const std::string failure = server.callLogFailure();
    if (!failure.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(Theme::Colors::ErrorText, "stopped: %s", failure.c_str());
    } else if (!path.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(Theme::Colors::SubtleText, "%s%s", shown.c_str(), fromEnvironment ? " (JASPER_MCP_LOG)" : "");
    }
}

void McpActivityWindow::drawToolCounts() {
    const std::vector<McpToolCounts::Row> rows = McpServer::get().toolCounts();
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV |
                                  ImGuiTableFlags_NoSavedSettings;
    if (!ImGui::BeginTable("##McpToolCounts", 3, flags)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    const float em = ImGui::GetFontSize();
    ImGui::TableSetupColumn("Tool", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Calls", ImGuiTableColumnFlags_WidthFixed, em * 2.8f);
    ImGui::TableSetupColumn("Errors", ImGuiTableColumnFlags_WidthFixed, em * 3.2f);
    ImGui::TableHeadersRow();
    for (const McpToolCounts::Row& row : rows) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(row.tool.c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%llu", static_cast<unsigned long long>(row.calls));
        ImGui::TableNextColumn();
        const int percent = static_cast<int>(row.errorRate() * 100.0 + 0.5);
        if (row.errors > 0) {
            ImGui::TextColored(Theme::Colors::ErrorText, "%d%%", percent);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%llu of %llu calls failed", static_cast<unsigned long long>(row.errors),
                                  static_cast<unsigned long long>(row.calls));
            }
        } else {
            ImGui::TextColored(Theme::Colors::SubtleText, "0%%");
        }
    }
    ImGui::EndTable();
}

void McpActivityWindow::drawActivity() {
    McpServer& server = McpServer::get();
    drawCallLog();
    if (ImGui::SmallButton("Clear")) server.clearActivity();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clears the finished calls and the per-tool counts.");
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &autoScroll);

    // the session's calls per tool, next to the recent calls
    const float em = ImGui::GetFontSize();
    ImGui::BeginChild("##McpToolCountsPane", ImVec2(em * 15.0f, 0.0f), ImGuiChildFlags_ResizeX);
    drawToolCounts();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##McpCallsPane", ImVec2(0.0f, 0.0f));
    drawCalls();
    ImGui::EndChild();
}

void McpActivityWindow::drawCalls() {
    McpServer& server = McpServer::get();
    const std::vector<McpActivityEntry> entries = server.activity();
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings;
    if (!ImGui::BeginTable("##McpCalls", 5, flags)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    const float em = ImGui::GetFontSize();
    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, em * 4.2f);
    ImGui::TableSetupColumn("Tool", ImGuiTableColumnFlags_WidthFixed, em * 9.0f);
    ImGui::TableSetupColumn("Result", ImGuiTableColumnFlags_WidthFixed, em * 3.6f);
    ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed, em * 2.6f);
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

}  // namespace jasperengine::editor::mcp
