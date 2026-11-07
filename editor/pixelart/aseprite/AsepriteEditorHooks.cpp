// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AsepriteEditorHooks.h"

#include "pixelart/aseprite/AsepriteLocator.h"

#include "App.h"
#include "AppSettings.h"
#include "Backend.h"
#include "Out.h"
#include "Platform.h"
#include "Project.h"
#include "util/FileDialogs.h"
#include "util/FileUtils.h"
#include "window/EditorModal.h"

#include "imgui.h"

#include <filesystem>
#include <optional>

namespace fs = std::filesystem;

namespace jasperengine::editor::pixelart::aseprite {

namespace {

    constexpr const char* kAsepriteNotFoundPopup = "Aseprite Not Found##aseprite_open";
    constexpr const char* kAsepriteSite = "https://www.aseprite.org";

    struct AsepriteOpenRequest {
        std::vector<fs::path> files;      // absolute
        std::string previewAsset;         // the first file in the assets, asset-relative
        bool fallbackToPreview = false;
    };

    struct AsepriteOpenState {
        std::optional<AsepriteOpenRequest> waiting;   // detection under way
        std::optional<AsepriteOpenRequest> failed;    // the dialog's: Locate... opens it again
        std::string message;
        bool showDialog = false;
    };

    AsepriteOpenState& asepriteOpenState() {
        static AsepriteOpenState state;
        return state;
    }

    void asepriteOpenFail(AsepriteOpenRequest request, const PixelProblem& problem) {
        AsepriteOpenState& state = asepriteOpenState();
        Out::warning("[Aseprite] %s", problem.message.empty() ? "Aseprite was not found" : problem.message.c_str());
        if (request.fallbackToPreview && !request.previewAsset.empty() && asepriteEditorHooks().openSpritePreview)
            asepriteEditorHooks().openSpritePreview(request.previewAsset);
        // the preview shows already: after Locate... the files only open in Aseprite
        request.fallbackToPreview = false;
        state.failed = std::move(request);
        state.message = problem.message.empty() ? "Aseprite was not found on this computer." : problem.message;
        state.showDialog = true;
    }

    // false while the detection runs
    bool asepriteOpenTry(const AsepriteOpenRequest& request) {
        AsepriteLocator& locator = AsepriteLocator::instance();
        if (!locator.ready()) return false;
        const std::optional<AsepriteInstall> install = locator.result();
        if (!install) {
            asepriteOpenFail(request, locator.problem());
            return true;
        }
        PixelProblem problem;
        if (!openInAseprite(*install, request.files, &problem)) asepriteOpenFail(request, problem);
        return true;
    }

    void asepriteOpenStart(AsepriteOpenRequest request) {
        AsepriteOpenState& state = asepriteOpenState();
        AsepriteLocator::instance().start(asepritePreference());
        // a newer request replaces one still waiting: the last click is what the user wants
        if (asepriteOpenTry(request)) state.waiting.reset();
        else state.waiting = std::move(request);
    }

    void asepriteOpenDialog() {
        AsepriteOpenState& state = asepriteOpenState();
        if (state.showDialog) {
            ImGui::OpenPopup(kAsepriteNotFoundPopup);
            state.showDialog = false;
        }
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (!EditorModal::begin(kAsepriteNotFoundPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
            return;
        constexpr const char* kLocateHint = "Locate its executable (it is kept in Preferences > External Tools > Aseprite), or install it from:";
        EditorModal::message(state.message + " " + kLocateHint + " " + kAsepriteSite);
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
        ImGui::TextUnformatted(state.message.c_str());
        ImGui::Spacing();
        ImGui::TextUnformatted(kLocateHint);
        ImGui::PopTextWrapPos();
        if (ImGui::TextLink(kAsepriteSite)) {
            std::string error;
            if (!Platform::openWithSystemDefault(kAsepriteSite, &error)) Out::warning("[Aseprite] %s", error.c_str());
        }
        ImGui::Separator();
        if (EditorModal::button("Locate...", "locate")) {
            const std::string chosen = FileDialogs::openFileDialog("", FILE_DIALOG_ALL, false);
            if (!chosen.empty()) {
                AppSettings::setExternalToolPath(kAsepriteToolKey, chosen);
                AsepriteLocator::instance().invalidate();
                std::optional<AsepriteOpenRequest> retry = std::move(state.failed);
                state.failed.reset();
                ImGui::CloseCurrentPopup();
                if (retry) asepriteOpenStart(std::move(*retry));
            }
        }
        ImGui::SameLine();
        if (EditorModal::button("Close", "cancel")) {
            state.failed.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

}

AsepriteEditorHooks& asepriteEditorHooks() {
    static AsepriteEditorHooks hooks;
    return hooks;
}

std::string asepritePreference() {
    return AppSettings::getExternalToolPath(kAsepriteToolKey);
}

void openInAsepriteOrFallback(Project* project, const std::vector<std::string>& assetPaths, bool fallbackToPreview) {
    if (!project || assetPaths.empty()) return;
    AsepriteOpenRequest request;
    request.fallbackToPreview = fallbackToPreview;
    for (const std::string& path : assetPaths) {
        const fs::path given = FileUtils::pathFromUtf8(path);
        const fs::path absolute = (given.is_absolute() ? given : project->getAssetsPath() / given).lexically_normal();
        request.files.push_back(absolute);
        if (request.previewAsset.empty() && project->isInsideAssetsPath(absolute))
            request.previewAsset = FileUtils::pathToGenericUtf8(project->normalizeToAssetsRelative(absolute));
    }
    asepriteOpenStart(std::move(request));
}

void updateAsepriteOpen() {
    AsepriteOpenState& state = asepriteOpenState();
    if (state.waiting) {
        if (AsepriteLocator::instance().ready()) {
            const AsepriteOpenRequest request = std::move(*state.waiting);
            state.waiting.reset();
            asepriteOpenTry(request);
        } else {
            // the locator's thread does not wake the loop: draw on until it answers
            Backend::getApp().requestRedraw();
        }
    }
    asepriteOpenDialog();
}

}
