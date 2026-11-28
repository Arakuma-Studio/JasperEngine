// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "recorder/InputRecordingInspector.h"

#include "App.h"
#include "Backend.h"
#include "Project.h"
#include "Theme.h"
#include "external/IconsFontAwesome6.h"
#include "recorder/CaptureScreenshotsDialog.h"
#include "util/FileUtils.h"
#include "window/CodeEditor.h"

#include "imgui.h"

#include <fstream>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace jasperengine::editor::recordinput {

namespace fs = std::filesystem;

namespace {

    void inspectorRow(const char* label, const std::string& value) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(label);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(value.c_str());
    }

    FileSummary readSummary(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            FileSummary failed;
            failed.error = "cannot read " + FileUtils::pathToGenericUtf8(path);
            return failed;
        }
        std::ostringstream text;
        text << in.rdbuf();
        return summarize(text.str());
    }

}

bool isInputFile(const fs::path& path) {
    const std::string name = FileUtils::pathToUtf8(path.filename());
    static const std::string suffix = ".input.json";
    return name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool InputRecordingInspector::draw(const std::vector<fs::path>& selection) {
    if (selection.size() != 1 || !isInputFile(selection[0])) return false;
    const fs::path file = (selection[0].is_absolute() ? selection[0] : project->getProjectPath() / selection[0]).lexically_normal();
    std::error_code ec;
    const fs::file_time_type stamp = fs::last_write_time(file, ec);
    // another file, or this one changed: read it again (off the main thread: a recording can be large)
    if (file != absolute || (!ec && stamp != fileTime)) {
        absolute = file;
        relative = FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(file));
        fileTime = stamp;
        summary.reset();
        message.clear();
        if (reading.valid()) reading.wait();
        reading = std::async(std::launch::async, &readSummary, file);
    }
    if (reading.valid() && reading.wait_for(std::chrono::seconds(0)) == std::future_status::ready) summary = reading.get();

    ImGui::TextUnformatted(ICON_FA_CIRCLE_DOT " Input Recording");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", FileUtils::pathToUtf8(file.filename()).c_str());
    ImGui::Separator();
    if (!summary) {
        ImGui::TextDisabled("Reading...");
        return true;
    }
    const FileSummary& s = *summary;
    if (!s.ok) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " %s", s.error.c_str());
        ImGui::PopTextWrapPos();
    } else if (ImGui::BeginTable("##InputRecordingSummary", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(120.0f));
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        inspectorRow("Kind", s.kind == "recording" ? "Recording" : "Script");
        if (!s.start.empty()) inspectorRow("Start", s.start == "play" ? "Play's start" : "Mid-game (from InputRecording.start)");
        if (!s.scene.empty()) inspectorRow("Scene", s.scene);
        inspectorRow("Frame Rate", std::to_string(s.fixedFps) + " fps");
        char seconds[64];
        std::snprintf(seconds, sizeof(seconds), "%u (%.1f game s)", s.frames, s.fixedFps > 0 ? static_cast<double>(s.frames) / s.fixedFps : 0.0);
        inspectorRow("Frames", seconds);
        inspectorRow("Events", std::to_string(s.events));
        for (const auto& [type, count] : s.eventsByType) inspectorRow(("    " + type).c_str(), std::to_string(count));
        inspectorRow("Checkpoints", std::to_string(s.checkpoints) + (s.probes ? " (" + std::to_string(s.probes) + " probes)" : std::string()));
        if (!s.platform.empty()) inspectorRow("Platform", s.platform);
        if (s.hasSeed) inspectorRow("Seed", std::to_string(s.seed));
        inspectorRow("Truncated", s.truncated ? "Yes (the recording reached its size cap)" : "No");
        ImGui::EndTable();
    }
    ImGui::Spacing();
    const bool playing = project->getPlaySessionSceneId() != NULL_PROJECT_SCENE;
    ImGui::BeginDisabled(!s.ok || playing);
    if (ImGui::Button(ICON_FA_PLAY " Replay##InputRecordingReplay")) {
        std::string error;
        message = RecordInput::get().replayFile(relative, error) ? std::string() : error;
    }
    if (playing && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("A replay starts its own session: stop Play first");
    ImGui::SameLine();
    if (ImGui::Button("Capture Screenshots...##InputRecordingCapture")) CaptureScreenshotsDialog::get().open(relative);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Open As Text##InputRecordingText")) {
        if (CodeEditor* editor = Backend::getApp().getCodeEditor()) editor->openFile(FileUtils::pathToUtf8(file));
    }
    if (!message.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(Theme::Colors::WarningText, "%s", message.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::TextDisabled("Read-only: edit the file as text.");
    return true;
}

}  // namespace jasperengine::editor::recordinput
