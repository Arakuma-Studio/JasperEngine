// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "NativePluginDialog.h"

#include "Out.h"
#include "Project.h"
#include "Theme.h"
#include "external/IconsFontAwesome6.h"
#include "util/FileUtils.h"

#include "imgui.h"

#include <cstdio>
#include <cstring>

using namespace jasperengine::editor;

namespace {

    // identifiers only: anything else becomes '_'
    int identifierFilter(ImGuiInputTextCallbackData* data) {
        const ImWchar c = data->EventChar;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) data->EventChar = '_';
        return 0;
    }

    int packageFilter(ImGuiInputTextCallbackData* data) {
        const ImWchar c = data->EventChar;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.')) data->EventChar = '_';
        return 0;
    }

    const char* const androidLanguages[] = {"Java", "Kotlin"};
    const char* const appleLanguages[] = {"Objective-C", "Swift"};

}

void NativePluginDialog::open(Project* project) {
    m_project = project;
    m_isOpen = true;
    m_popupOpened = false;
    std::snprintf(m_name, sizeof(m_name), "%s", "MyPlugin");
    std::snprintf(m_package, sizeof(m_package), "%s", "com.example.plugins");
    m_methods.clear();
    m_methods.push_back(Method());
    std::snprintf(m_methods[0].name, sizeof(m_methods[0].name), "%s", "getValue");
    m_methods.push_back(Method());
    std::snprintf(m_methods[1].name, sizeof(m_methods[1].name), "%s", "fetch");
    m_methods[1].async = true;
    m_overwrite = false;
    m_error.clear();
}

std::filesystem::path NativePluginDialog::consumeCreatedFile() {
    std::filesystem::path created = m_created;
    m_created.clear();
    return created;
}

NativePlugins::CreateOptions NativePluginDialog::options() const {
    NativePlugins::CreateOptions o;
    o.name = m_name;
    if (m_android) o.platforms.push_back("android");
    if (m_ios) o.platforms.push_back("ios");
    if (m_macos) o.platforms.push_back("macos");
    if (m_web) o.platforms.push_back("web");
    if (m_cpp) o.platforms.push_back("cpp");
    o.androidLanguage = m_androidLanguage == 1 ? "kotlin" : "java";
    o.appleLanguage = m_appleLanguage == 1 ? "swift" : "objc";
    o.package = m_package;
    for (const Method& method : m_methods) {
        if (method.name[0] != '\0') o.methods.push_back({method.name, method.async});
    }
    o.overwrite = m_overwrite;
    return o;
}

void NativePluginDialog::create() {
    if (!m_project) return;
    std::vector<NativePlugins::CreatedFile> written;
    std::string error;
    if (!NativePlugins::create(m_project->getProjectPath(), m_project->getLuaPath(), options(), written, error)) {
        m_error = error;
        return;
    }
    for (const NativePlugins::CreatedFile& file : written) {
        Out::info("Native plugin %s: %s %s", m_name, file.overwritten ? "replaced" : "wrote", file.path.c_str());
    }
    if (!written.empty()) m_created = m_project->getProjectPath() / FileUtils::pathFromUtf8(written.back().path);
    m_isOpen = false;
}

void NativePluginDialog::show() {
    if (!m_isOpen) return;
    const char* title = "Create Native Plugin##NativePluginDialog";
    if (!m_popupOpened) {
        ImGui::OpenPopup(title);
        m_popupOpened = true;
    }
    ImGui::SetNextWindowSize(ImVec2(Theme::dpi(560.0f), 0.0f), ImGuiCond_Appearing);
    bool open = true;
    if (!ImGui::BeginPopupModal(title, &open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
        if (!open) m_isOpen = false;
        return;
    }
    if (!open) {
        m_isOpen = false;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    ImGui::TextWrapped("Writes the plugin's file for each platform into plugins/native and a Lua mock into the Lua folder "
                       "(native_mocks), which answers Native.call in editor Play and desktop builds.");
    ImGui::Separator();

    const float labelWidth = Theme::dpi(150.0f);
    auto label = [labelWidth](const char* text) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
        ImGui::SameLine(labelWidth);
    };
    label("Name");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##NativePluginName", m_name, sizeof(m_name), ImGuiInputTextFlags_CallbackCharFilter, identifierFilter);

    label("Platforms");
    ImGui::Checkbox("Android", &m_android);
    ImGui::SameLine();
    ImGui::Checkbox("iOS", &m_ios);
    ImGui::SameLine();
    ImGui::Checkbox("macOS", &m_macos);
    ImGui::SameLine();
    ImGui::Checkbox("Web", &m_web);
    ImGui::SameLine();
    ImGui::Checkbox("C++", &m_cpp);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("A C++ plugin answers everywhere first: it is compiled only where no platform plugin of the name is chosen here.");
    }

    ImGui::BeginDisabled(!m_android);
    label("Android language");
    ImGui::SetNextItemWidth(Theme::dpi(160.0f));
    ImGui::Combo("##NativePluginAndroidLanguage", &m_androidLanguage, androidLanguages, 2);
    label("Java package");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##NativePluginPackage", m_package, sizeof(m_package), ImGuiInputTextFlags_CallbackCharFilter, packageFilter);
    ImGui::EndDisabled();

    ImGui::BeginDisabled(!m_ios && !m_macos);
    label("Apple language");
    ImGui::SetNextItemWidth(Theme::dpi(160.0f));
    ImGui::Combo("##NativePluginAppleLanguage", &m_appleLanguage, appleLanguages, 2);
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::TextUnformatted("Methods");
    int remove = -1;
    for (size_t i = 0; i < m_methods.size(); i++) {
        ImGui::PushID(static_cast<int>(i));
        ImGui::SetNextItemWidth(Theme::dpi(260.0f));
        ImGui::InputText("##MethodName", m_methods[i].name, sizeof(m_methods[i].name), ImGuiInputTextFlags_CallbackCharFilter, identifierFilter);
        ImGui::SameLine();
        ImGui::Checkbox("Async", &m_methods[i].async);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Native.callAsync: the method gets a call to complete later");
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_MINUS)) remove = static_cast<int>(i);
        ImGui::PopID();
    }
    if (remove >= 0) m_methods.erase(m_methods.begin() + remove);
    if (ImGui::SmallButton(ICON_FA_PLUS " Method")) m_methods.push_back(Method());

    ImGui::Separator();
    const NativePlugins::CreateOptions o = options();
    const std::string invalid = NativePlugins::validate(o);
    std::vector<std::string> planned;
    if (invalid.empty() && m_project) planned = NativePlugins::plannedFiles(m_project->getProjectPath(), m_project->getLuaPath(), o);
    if (!planned.empty()) {
        ImGui::TextDisabled("Files:");
        for (const std::string& path : planned) ImGui::BulletText("%s", path.c_str());
    }
    ImGui::Checkbox("Replace existing files", &m_overwrite);
    if (!invalid.empty()) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "%s", invalid.c_str());
    if (!m_error.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "%s", m_error.c_str());
        ImGui::PopTextWrapPos();
    }

    const float buttonWidth = Theme::dpi(120.0f);
    ImGui::BeginDisabled(!invalid.empty());
    if (ImGui::Button("Create", ImVec2(buttonWidth, 0.0f))) {
        create();
        if (!m_isOpen) ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(buttonWidth, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        m_isOpen = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
