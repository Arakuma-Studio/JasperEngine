// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "recorder/RecorderWindow.h"

#include "Out.h"
#include "Platform.h"
#include "Theme.h"
#include "external/IconsFontAwesome6.h"
#include "recorder/RecorderSession.h"
#include "util/FileDialogs.h"
#include "util/FileUtils.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::recorder;

namespace fs = std::filesystem;

namespace {

    template <typename E>
    bool combo(const char* id, E& value, std::initializer_list<std::pair<E, const char*>> items) {
        const char* preview = "";
        for (const auto& item : items) {
            if (item.first == value) preview = item.second;
        }
        bool changed = false;
        if (ImGui::BeginCombo(id, preview)) {
            for (const auto& item : items) {
                if (ImGui::Selectable(item.second, item.first == value) && item.first != value) {
                    value = item.first;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        return changed;
    }

    void row(const char* label, const char* tooltip = nullptr) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        if (tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-FLT_MIN);
    }

    bool inputText(const char* id, std::string& value) {
        std::array<char, 512> buffer{};
        std::snprintf(buffer.data(), buffer.size(), "%s", value.c_str());
        if (ImGui::InputText(id, buffer.data(), buffer.size())) {
            value = buffer.data();
            return true;
        }
        return false;
    }

}

void RecorderWindow::setProjectPath(const fs::path& path) {
    if (path == projectPath) return;
    projectPath = path;
    current = Settings();
    std::string error;
    if (!path.empty() && !load(settingsFile(path), current, error)) Out::warning("Recorder settings: %s", error.c_str());
    saved = current;
}

void RecorderWindow::saveSettings() {
    if (projectPath.empty() || current == saved) return;
    std::string error;
    if (save(settingsFile(projectPath), current, error)) saved = current;
    else Out::warning("Recorder settings: %s", error.c_str());
}

void RecorderWindow::toggleRecording() {
    RecorderSession& session = RecorderSession::get();
    if (session.recording()) {
        session.stop();
        return;
    }
    if (session.active()) return;   // finalising
    std::string error;
    if (!session.start(current, error)) {
        message = error;
        Out::warning("Recorder: %s", error.c_str());
    } else {
        message.clear();
    }
}

void RecorderWindow::show() {
    RecorderSession& session = RecorderSession::get();
    // a finished recording: the next Take, the settings saved
    if (session.resultSerial() != seenResult) {
        seenResult = session.resultSerial();
        current.take = std::max(current.take, session.settings().take);
        message = session.lastResult().ok ? "Recorded " + std::to_string(session.lastResult().frames) + " frames to " +
                                                FileUtils::pathToUtf8(session.lastResult().output)
                                          : "Recording failed: " + session.lastResult().error;
    }
    saveSettings();
    if (!windowOpen) return;

    ImGui::SetNextWindowSize(ImVec2(Theme::dpi(420.0f), Theme::dpi(520.0f)), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(WINDOW_NAME, &windowOpen)) {
        ImGui::End();
        return;
    }
    const bool locked = session.active();
    const FfmpegInfo& ffmpeg = session.ffmpeg();
    const bool movieAvailable = ffmpeg.probed && !ffmpeg.path.empty() && !ffmpeg.h264.empty();

    // the button first: Start Recording / Stop Recording (F10)
    if (session.recording()) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.15f, 0.15f, 1.0f));
        if (ImGui::Button(ICON_FA_STOP " Stop Recording", ImVec2(-FLT_MIN, Theme::dpi(32.0f)))) toggleRecording();
        ImGui::PopStyleColor();
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", session.overlayText().c_str());
    } else if (session.state() == State::Finalising) {
        ImGui::BeginDisabled();
        ImGui::Button("Finalising...", ImVec2(-FLT_MIN, Theme::dpi(32.0f)));
        ImGui::EndDisabled();
    } else {
        const bool blocked = current.kind == Kind::Movie && !movieAvailable;
        ImGui::BeginDisabled(blocked);
        if (ImGui::Button(ICON_FA_CIRCLE " Start Recording", ImVec2(-FLT_MIN, Theme::dpi(32.0f)))) toggleRecording();
        ImGui::EndDisabled();
    }
    if (session.audioMuted()) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), ICON_FA_VOLUME_XMARK " The audio is being captured: you hear nothing until it stops.");
    }
    if (locked && !session.startWarningText().empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.3f, 1.0f));
        ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION " %s", session.startWarningText().c_str());
        ImGui::PopStyleColor();
    }
    if (!message.empty()) ImGui::TextWrapped("%s", message.c_str());
    ImGui::Separator();

    ImGui::BeginDisabled(locked);
    if (ImGui::BeginTable("##recorderSettings", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(130.0f));
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
        row("Recorder");
        combo("##kind", current.kind, {{Kind::ImageSequence, "Image Sequence"}, {Kind::Movie, "Movie"}});
        row("Capture");
        ImGui::TextUnformatted("Game View");
        row("Output Resolution");
        combo("##resolution", current.resolution,
              {{Resolution::MatchGameView, resolutionLabel(Resolution::MatchGameView)}, {Resolution::P480, resolutionLabel(Resolution::P480)},
               {Resolution::P720, resolutionLabel(Resolution::P720)}, {Resolution::P1080, resolutionLabel(Resolution::P1080)},
               {Resolution::P1440, resolutionLabel(Resolution::P1440)}, {Resolution::P2160, resolutionLabel(Resolution::P2160)},
               {Resolution::Custom, resolutionLabel(Resolution::Custom)}});
        if (current.resolution == Resolution::Custom) {
            row("Width x Height", "16 to 8192 (even for movies)");
            int size[2] = {current.customWidth, current.customHeight};
            if (ImGui::InputInt2("##customSize", size)) {
                current.customWidth = std::clamp(size[0], kMinSize, kMaxSize);
                current.customHeight = std::clamp(size[1], kMinSize, kMaxSize);
                if (current.kind == Kind::Movie) {
                    current.customWidth -= current.customWidth % 2;
                    current.customHeight -= current.customHeight % 2;
                }
            }
        }
        row("Frame Rate", "Constant: the game runs exactly 1/fps per frame while it records");
        int rateChoice = 0;
        static const int kRates[] = {24, 25, 30, 50, 60};
        for (int i = 0; i < 5; i++) {
            if (current.frameRate == kRates[i]) rateChoice = i + 1;
        }
        static const char* const kRateLabels[] = {"Custom", "24", "25", "30", "50", "60"};
        ImGui::SetNextItemWidth(Theme::dpi(100.0f));
        if (ImGui::Combo("##frameRate", &rateChoice, kRateLabels, 6) && rateChoice > 0) current.frameRate = kRates[rateChoice - 1];
        if (rateChoice == 0) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputInt("##customRate", &current.frameRate)) current.frameRate = std::clamp(current.frameRate, kMinFrameRate, kMaxFrameRate);
        }
        row("Recording Mode");
        combo("##mode", current.mode,
              {{Mode::Manual, "Manual"}, {Mode::SingleFrame, "Single Frame"}, {Mode::FrameInterval, "Frame Interval"}, {Mode::TimeInterval, "Time Interval"}});
        if (current.mode == Mode::SingleFrame) {
            row("Frame", "Frame 0 is the first game frame after Play starts");
            if (ImGui::InputInt("##singleFrame", &current.singleFrame)) current.singleFrame = std::max(0, current.singleFrame);
        } else if (current.mode == Mode::FrameInterval) {
            row("Frames", "First and last, both recorded");
            int frames[2] = {current.frameStart, current.frameEnd};
            if (ImGui::InputInt2("##frames", frames)) {
                current.frameStart = std::max(0, frames[0]);
                current.frameEnd = std::max(current.frameStart, frames[1]);
            }
        } else if (current.mode == Mode::TimeInterval) {
            row("Seconds", "From and to, in game seconds");
            float times[2] = {current.timeStart, current.timeEnd};
            if (ImGui::InputFloat2("##times", times, "%.2f")) {
                current.timeStart = std::max(0.0f, times[0]);
                current.timeEnd = std::max(current.timeStart, times[1]);
            }
        }
        row("Exit Play Mode", "Stop Play when the recording ends");
        ImGui::Checkbox("##exitPlay", &current.exitPlayMode);
        row("Record on Play", "Start recording whenever Play starts");
        ImGui::Checkbox("##recordOnPlay", &current.recordOnPlay);

        if (current.kind == Kind::Movie) {
            row("Format");
            Format format = current.format;
            std::initializer_list<std::pair<Format, const char*>> formats = {{Format::Mp4, "MP4 (H.264)"}, {Format::Mov, "MOV (ProRes 422 HQ)"}};
            if (combo("##format", format, formats)) {
                if (format == Format::Mov && !ffmpeg.proRes) message = "MOV needs FFmpeg with prores_ks.";
                else current.format = format;
            }
            row("Quality");
            combo("##quality", current.quality, {{Quality::Low, "Low"}, {Quality::Medium, "Medium"}, {Quality::High, "High"}});
            row("Capture Audio", "Mixed offline in step with the frames: you hear nothing while it records; the recording must start with Play");
            ImGui::Checkbox("##captureAudio", &current.captureAudio);
            row("Encoder");
            if (!ffmpeg.probed) {
                ImGui::TextDisabled("Looking for FFmpeg...");
            } else if (movieAvailable) {
                ImGui::TextDisabled("%s (%s)", current.format == Format::Mov ? "prores_ks" : ffmpeg.h264.c_str(), ffmpeg.path.c_str());
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.3f, 1.0f));
                ImGui::TextWrapped("Movie is not available: %s", ffmpeg.why.c_str());
                ImGui::PopStyleColor();
                if (ImGui::SmallButton("Open Preferences...") && openPreferences) openPreferences();
                ImGui::SameLine();
                if (ImGui::SmallButton("Look Again")) session.refreshFfmpeg();
            }
        }

        row("Output Folder", "Relative to the project, or absolute");
        const float browse = ImGui::CalcTextSize("Browse...").x + ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - browse);
        inputText("##folder", current.outputFolder);
        ImGui::SameLine();
        if (ImGui::Button("Browse...")) {
            const std::string picked = FileDialogs::openFileDialog(FileUtils::pathToUtf8(outputFolder(current, projectPath)), FILE_DIALOG_ALL, true);
            if (!picked.empty()) current.outputFolder = picked;
        }
        row("File Name", "<Scene> <Take> <Date> <Time> <Resolution>, and <Frame> for a sequence");
        inputText("##name", current.kind == Kind::Movie ? current.movieName : current.sequenceName);
        row("Take", "Goes up after each recording");
        if (ImGui::InputInt("##take", &current.take)) current.take = std::max(1, current.take);
        ImGui::EndTable();
    }
    ImGui::EndDisabled();

    // what the next recording writes
    int width = 0, height = 0;
    outputSize(current, 1920, 1080, 0, width, height);
    NameContext context;
    context.scene = "<Scene>";
    context.take = current.take;
    context.date = "<Date>";
    context.time = "<Time>";
    context.width = width;
    context.height = height;
    context.frame = current.kind == Kind::ImageSequence ? 0 : -1;
    ImGui::TextDisabled("%s", FileUtils::pathToUtf8(outputFile(current, projectPath, context)).c_str());
    if (ImGui::SmallButton("Open Output Folder")) {
        std::error_code ec;
        const fs::path folder = outputFolder(current, projectPath);
        fs::create_directories(folder, ec);
        std::string error;
        if (!Platform::openWithSystemDefault(FileUtils::pathToUtf8(folder), &error)) message = error;
    }
    ImGui::End();
}
