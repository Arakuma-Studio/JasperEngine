// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AboutWindow.h"
#include "window/EditorModal.h"

#include "Backend.h"
#include "EditorVersion.h"
#include "Theme.h"
#include "resources/icons/jasperengine-logo_png.h"

#include <algorithm>

namespace jasperengine::editor {

namespace {
    static constexpr float aboutDialogWidth = 440.0f;
    static constexpr float logoWidth = 150.0f;
    static constexpr float buttonWidth = 120.0f;

    void centeredText(const char* text) {
        float offset = (ImGui::GetWindowSize().x - ImGui::CalcTextSize(text).x) * 0.5f;
        ImGui::SetCursorPosX(std::max(offset, ImGui::GetStyle().WindowPadding.x));
        ImGui::TextUnformatted(text);
    }
}

void AboutWindow::open() {
    m_isOpen = true;

    if (!m_logoLoaded) {
        TextureData data;
        data.loadTextureFromMemory(jasperengine_logo_png, jasperengine_logo_png_len);
        m_logo.setData("editor:about:logo", data);
        m_logo.load();
        m_logoLoaded = true;
    }
}

void AboutWindow::show() {
    if (!m_isOpen) return;

    ImGui::OpenPopup("About JasperEngine##AboutModal");

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(Theme::dpi(aboutDialogWidth), 0.0f), ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_Modal;

    if (EditorModal::begin("About JasperEngine##AboutModal", &m_isOpen, flags)) {
        if (!m_isOpen) {
            ImGui::CloseCurrentPopup();
        } else {
            drawAbout();
        }
        ImGui::EndPopup();
    }
}

void AboutWindow::drawAbout() {
    TextureRender* logoRender = m_logo.getRender();
    if (logoRender && logoRender->isCreated() && m_logo.getWidth() > 0) {
        float width = Theme::dpi(logoWidth);
        float height = width * m_logo.getHeight() / m_logo.getWidth();
        ImGui::SetCursorPosX((ImGui::GetWindowSize().x - width) * 0.5f);
        ImGui::Image(Backend::getImGuiTexture(logoRender), ImVec2(width, height));
        ImGui::Spacing();
    }

    centeredText("Version: " JASPERENGINE_EDITOR_VERSION);
    centeredText("Developed by JasperDev");
    centeredText("Based on Doriax by Eduardo Doria and contributors");
    EditorModal::message("JasperEngine editor " JASPERENGINE_EDITOR_VERSION);

    ImGui::Separator();

    float okWidth = Theme::dpi(buttonWidth);
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - okWidth));
    if (EditorModal::button("OK", "ok", ImVec2(okWidth, 0)) ||
        (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
        m_isOpen = false;
        ImGui::CloseCurrentPopup();
    }
}

}
