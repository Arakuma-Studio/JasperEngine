// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "recorder/CaptureScreenshotsDialog.h"

#include "Project.h"
#include "Theme.h"
#include "recorder/InputCapture.h"
#include "recorder/RecordInput.h"
#include "util/FileDialogs.h"
#include "util/FileUtils.h"
#include "window/GameResolutions.h"
#include "window/dialog/EditorSettingsWindow.h"

#include "imgui.h"

#include <array>
#include <cstdio>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace jasperengine::editor::recordinput {

namespace {

    constexpr const char* CAPTURE_WINDOW = "Capture Screenshots";

    void captureLabel(const char* label) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::TableNextColumn();
    }

    bool captureTextField(const char* id, std::string& value, float width) {
        std::array<char, 1024> buffer{};
        std::snprintf(buffer.data(), buffer.size(), "%s", value.c_str());
        ImGui::SetNextItemWidth(width);
        if (!ImGui::InputText(id, buffer.data(), buffer.size())) return false;
        value = buffer.data();
        return true;
    }

    std::string defaultCaptureFolder(const std::string& file) {
        return "Recordings/Screenshots/" + (file.empty() ? std::string("capture") : fileStem(FileUtils::pathFromUtf8(file))) + "/";
    }

}

CaptureScreenshotsDialog& CaptureScreenshotsDialog::get() {
    static CaptureScreenshotsDialog instance;
    return instance;
}

void CaptureScreenshotsDialog::open(const std::string& newFile) {
    if (!newFile.empty() && newFile != file) {
        file = newFile;
        output.clear();
    }
    windowOpen = true;
    ImGui::SetWindowFocus(CAPTURE_WINDOW);
}

void CaptureScreenshotsDialog::capture(Project* project) {
    message.clear();
    messageIsError = true;
    CaptureRequest request;
    request.input = file;
    std::string error;
    if (!RecordInput::get().loadReplay(file, request.script, error)) {
        message = error;
        return;
    }
    for (const GameResolution& resolution : builtInGameResolutions()) {
        if (sizes.count(resolution.name)) request.sizes.push_back({resolution.width, resolution.height});
    }
    std::stringstream list(custom);
    std::string item;
    while (std::getline(list, item, ',')) {
        item.erase(0, item.find_first_not_of(" \t"));
        item.erase(item.find_last_not_of(" \t") + 1);
        if (item.empty()) continue;
        CaptureSize size;
        if (!parseResolution(item, size.width, size.height, error)) {
            message = "Custom: " + error;
            return;
        }
        request.sizes.push_back(size);
    }
    if (request.sizes.empty()) {
        message = "Check at least one resolution (or give a custom WxH).";
        return;
    }
    fs::path folder = FileUtils::pathFromUtf8(output.empty() ? defaultCaptureFolder(file) : output);
    if (folder.is_relative()) folder = project->getProjectPath() / folder;
    request.output = folder;
    request.shots = true;
    InputCapture::get().setOnFinished([this, folder](const InputCapture::Result& result) {
        capturing = false;
        messageIsError = !result.ok;
        if (result.ok) {
            message = "Captured " + std::to_string(result.files.size()) + " screenshot(s) into " + FileUtils::pathToGenericUtf8(folder) + ".";
            EditorSettingsWindow::openInFileManager(folder);
        } else {
            message = result.cancelled ? std::string("Cancelled.") : result.error;
        }
    });
    if (!InputCapture::get().start(std::move(request), error)) {
        message = error + ".";
        return;
    }
    capturing = true;
    messageIsError = false;
}

void CaptureScreenshotsDialog::show(Project* project) {
    if (!windowOpen || !project) return;
    ImGui::SetNextWindowSize(Theme::dpi(ImVec2(520.0f, 520.0f)), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(CAPTURE_WINDOW, &windowOpen, ImGuiWindowFlags_NoDocking)) {
        ImGui::End();
        return;
    }
    const float browseWidth = Theme::dpi(28.0f);
    ImGui::BeginDisabled(capturing);
    if (ImGui::BeginTable("##CaptureScreenshots", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(110.0f));
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        captureLabel("Input File");
        const float fieldWidth = ImGui::GetContentRegionAvail().x - browseWidth - ImGui::GetStyle().ItemSpacing.x;
        if (captureTextField("##CaptureInput", file, fieldWidth)) output.clear();
        ImGui::SameLine();
        if (ImGui::Button("...##CaptureInputBrowse", ImVec2(browseWidth, 0.0f))) {
            const fs::path folder = recordingsFolder(project->getProjectPath());
            std::error_code ec;
            const std::string picked = FileDialogs::openFileDialogFiltered(
                FileUtils::pathToUtf8(fs::is_directory(folder, ec) ? folder : project->getProjectPath()), "Input recordings and scripts", "json");
            if (!picked.empty()) {
                file = FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(FileUtils::pathFromUtf8(picked)));
                output.clear();
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("A .input.json whose shot steps name the screenshots");

        captureLabel("Resolutions");
        if (ImGui::BeginChild("##CaptureSizes", ImVec2(0.0f, Theme::dpi(220.0f)), ImGuiChildFlags_Borders)) {
            for (const GameResolution& resolution : builtInGameResolutions()) {
                bool checked = sizes.count(resolution.name) > 0;
                if (ImGui::Checkbox(gameResolutionLabel(resolution).c_str(), &checked)) {
                    if (checked) sizes.insert(resolution.name);
                    else sizes.erase(resolution.name);
                }
            }
        }
        ImGui::EndChild();

        captureLabel("Custom");
        captureTextField("##CaptureCustom", custom, -FLT_MIN);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("More sizes, comma-separated: 1290x2796, 2048x2732");

        captureLabel("Output Folder");
        std::string shown = output.empty() ? defaultCaptureFolder(file) : output;
        if (captureTextField("##CaptureOutput", shown, fieldWidth)) output = shown;
        ImGui::SameLine();
        if (ImGui::Button("...##CaptureOutputBrowse", ImVec2(browseWidth, 0.0f))) {
            const std::string picked = FileDialogs::openFileDialog(FileUtils::pathToUtf8(project->getProjectPath()), FILE_DIALOG_ALL, true);
            if (!picked.empty()) output = FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(FileUtils::pathFromUtf8(picked)));
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("One folder per resolution inside it (1290x2796/01_menu.png)");
        ImGui::EndTable();
    }
    ImGui::EndDisabled();

    ImGui::Separator();
    if (capturing) {
        ImGui::ProgressBar(static_cast<float>(InputCapture::get().progress()), ImVec2(-FLT_MIN, 0.0f), InputCapture::get().progressText().c_str());
        if (ImGui::Button("Cancel##CaptureCancel")) InputCapture::get().cancel();
    } else {
        const bool playing = project->getPlaySessionSceneId() != NULL_PROJECT_SCENE;
        ImGui::BeginDisabled(file.empty() || playing || InputCapture::get().active());
        if (ImGui::Button("Capture##CaptureStart", ImVec2(Theme::dpi(120.0f), 0.0f))) capture(project);
        ImGui::EndDisabled();
        if (playing && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Stop Play first: each resolution replays the file in its own session");
    }
    if (!message.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        if (messageIsError) ImGui::TextColored(Theme::Colors::WarningText, "%s", message.c_str());
        else ImGui::TextUnformatted(message.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::End();
}

}  // namespace jasperengine::editor::recordinput
