// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PrefabsWindow.h"
#include "window/EditorModal.h"
#include "Theme.h"
#include "Backend.h"
#include "App.h"
#include "util/UIUtils.h"
#include "window/ProjectWindow.h"
#include "external/IconsFontAwesome6.h"

#include <algorithm>
#include <cctype>

namespace jasperengine::editor {

static constexpr float prefabsDialogWidth = 820.0f;
static constexpr float prefabsDialogHeight = 520.0f;
static constexpr float prefabsDialogButtonWidth = 120.0f;
static constexpr ImGuiWindowFlags prefabsNoScrollFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

static const ImVec4 MISSING_COLOR = ImVec4(0.96f, 0.36f, 0.36f, 1.0f);

static std::string lowered(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return text;
}

// "Level 1 (2), Level 2 (closed, 1 + 3 nested)"
static std::string usageText(const PrefabUsage& usage, bool missing) {
    std::string text;
    for (const PrefabSceneUsage& scene : usage.scenes) {
        const size_t count = missing ? scene.missing : scene.instances;
        if (count == 0 && (missing || scene.nested == 0)) {
            continue;
        }
        std::string item = scene.sceneName + " (";
        if (!scene.loaded) {
            item += "closed, ";
        }
        item += std::to_string(count);
        if (!missing && scene.nested > 0) {
            item += " + " + std::to_string(scene.nested) + " nested";
        }
        item += ")";
        text += text.empty() ? item : ", " + item;
    }
    return text;
}

void PrefabsWindow::open(Project* project) {
    m_isOpen = true;
    m_project = project;
    m_entries.clear();
    m_missing.clear();
    m_filter[0] = '\0';

    std::map<std::string, PrefabUsage> usage = project->collectPrefabUsage();
    std::set<std::string> listed;

    for (const fs::path& prefabPath : project->findPrefabFiles()) {
        PrefabEntry entry;
        entry.path = prefabPath;
        entry.guid = project->getPrefabGuid(prefabPath);
        const std::string key = entry.guid.empty() ? "path:" + prefabPath.generic_string() : entry.guid;
        auto found = usage.find(key);
        if (found != usage.end()) {
            entry.usage = found->second;
            listed.insert(key);
        }

        for (const SceneProject& sceneProject : project->getScenes()) {
            const bool usesPrefab = std::any_of(sceneProject.prefabs.begin(), sceneProject.prefabs.end(),
                [&](const PrefabSceneInfo& info) {
                    return (!entry.guid.empty() && info.guid == entry.guid) || info.prefabPath == prefabPath;
                });
            if (usesPrefab) {
                entry.usedBy += entry.usedBy.empty() ? sceneProject.name : ", " + sceneProject.name;
            }
        }

        entry.included = !entry.usedBy.empty() || project->isStandalonePrefab(prefabPath);
        m_entries.push_back(std::move(entry));
    }

    // References to prefabs no file has: Missing Prefab instances
    for (const auto& [key, entry] : usage) {
        if (!listed.count(key) && (entry.totalMissing() > 0 || !entry.nestedIn.empty())) {
            m_missing.push_back(entry);
        }
    }
}

void PrefabsWindow::show() {
    if (!m_isOpen) return;

    ImGui::OpenPopup("Prefabs##PrefabsModal");

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImVec2 size(
        std::min(Theme::dpi(prefabsDialogWidth), viewport->WorkSize.x * 0.9f),
        std::min(Theme::dpi(prefabsDialogHeight), viewport->WorkSize.y * 0.9f)
    );
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_Modal |
                             ImGuiWindowFlags_NoResize |
                             prefabsNoScrollFlags;

    if (EditorModal::begin("Prefabs##PrefabsModal", &m_isOpen, flags)) {
        if (!m_isOpen) {
            ImGui::CloseCurrentPopup();
        } else {
            EditorModal::message("The project's Prefabs. create_prefab and the prefab tools do the same without this window.");
            drawPrefabs();
        }
        ImGui::EndPopup();
    }
}

void PrefabsWindow::drawUsage(const PrefabUsage& usage, bool missing) {
    const std::string text = usageText(usage, missing);
    std::string nestedIn;
    for (const fs::path& path : usage.nestedIn) {
        nestedIn += (nestedIn.empty() ? "" : ", ") + path.stem().string();
    }
    if (text.empty() && nestedIn.empty()) {
        ImGui::TextDisabled("-");
        return;
    }
    if (!text.empty()) {
        ImGui::TextUnformatted(text.c_str());
    }
    if (!nestedIn.empty()) {
        ImGui::TextDisabled("in %s", nestedIn.c_str());
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) || (!text.empty() && ImGui::IsItemHovered())) {
        ImGui::BeginTooltip();
        for (const PrefabSceneUsage& scene : usage.scenes) {
            const size_t count = missing ? scene.missing : scene.instances;
            if (count == 0 && (missing || scene.nested == 0)) {
                continue;
            }
            ImGui::Text("%s%s: %zu instance%s%s", scene.sceneName.c_str(), scene.loaded ? "" : " (closed, from its file)",
                count, count == 1 ? "" : "s", missing ? " (missing)" : "");
            if (!missing && scene.nested > 0) {
                ImGui::Text("    %zu inside other prefab instances", scene.nested);
            }
        }
        for (const fs::path& path : usage.nestedIn) {
            ImGui::Text("Nested in %s", path.generic_string().c_str());
        }
        ImGui::EndTooltip();
    }
}

void PrefabsWindow::drawPrefabs() {
    const ImGuiStyle& style = ImGui::GetStyle();

    ImGui::TextWrapped("Prefabs used by a scene are always in the build. Check the others to include them too, "
                       "so scripts can instantiate them by name. Prefabs are identified by their asset GUID: "
                       "instances follow a prefab that is renamed or moved.");
    ImGui::Spacing();
    UIUtils::searchInput("##PrefabsFilter", "Search prefabs", m_filter, sizeof(m_filter), false, nullptr, Theme::dpi(260.0f));
    const std::string filter = lowered(m_filter);
    ImGui::Spacing();

    const float footerY = ImGui::GetWindowHeight() - style.WindowPadding.y - ImGui::GetFrameHeight();
    const float listHeight = std::max(1.0f, footerY - style.ItemSpacing.y * 2.0f - ImGui::GetCursorPosY());

    if (ImGui::BeginChild("##PrefabsList", ImVec2(0, listHeight), ImGuiChildFlags_Borders)) {
        if (m_entries.empty() && m_missing.empty()) {
            ImGui::TextDisabled("No Prefab found in this project.");
        } else if (ImGui::BeginTable("##PrefabsTable", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("Prefab", ImGuiTableColumnFlags_WidthStretch, 0.40f);
            ImGui::TableSetupColumn("GUID", ImGuiTableColumnFlags_WidthStretch, 0.14f);
            ImGui::TableSetupColumn("Instances", ImGuiTableColumnFlags_WidthStretch, 0.10f);
            ImGui::TableSetupColumn("Used by", ImGuiTableColumnFlags_WidthStretch, 0.36f);
            ImGui::TableHeadersRow();

            for (PrefabEntry& entry : m_entries) {
                if (!filter.empty() && lowered(entry.path.generic_string()).find(filter) == std::string::npos &&
                        lowered(entry.guid).find(filter) == std::string::npos) {
                    continue;
                }
                const bool locked = !entry.usedBy.empty();
                ImGui::PushID(entry.path.generic_string().c_str());

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::BeginDisabled(locked);
                ImGui::Checkbox(entry.path.generic_string().c_str(), &entry.included);
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip("%s\n%s", entry.path.generic_string().c_str(),
                        locked ? ("In the build: listed by " + entry.usedBy).c_str() : "Check to build it without a scene instance");
                }
                // Prefab Variants: the base it is a variant of (selectable), the variants of it
                if (!entry.usage.base.empty()) {
                    const ImVec2 iconMin = ImGui::GetCursorScreenPos();
                    const float fontSize = ImGui::GetFontSize();
                    ImGui::Dummy(ImVec2(fontSize, fontSize));
                    UIUtils::drawPrefabVariantBadge(ImGui::GetWindowDrawList(), ImVec2(iconMin.x + fontSize * 0.2f, iconMin.y),
                        ImVec2(iconMin.x + fontSize, iconMin.y + fontSize), ImGui::GetColorU32(ImVec4(0.55f, 0.80f, 1.0f, 1.0f)));
                    ImGui::SameLine();
                    if (entry.usage.baseMissing) {
                        ImGui::TextColored(MISSING_COLOR, "Variant of %s (missing)", entry.usage.base.generic_string().c_str());
                    } else {
                        ImGui::TextDisabled("Variant of %s", entry.usage.base.stem().string().c_str());
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Select Base")) {
                            if (ProjectWindow* projectWindow = Backend::getApp().getProjectWindow()) {
                                projectWindow->revealAsset(m_project->getProjectPath() / entry.usage.base);
                            }
                        }
                        ImGui::SetItemTooltip("Selects %s in the Project window", entry.usage.base.generic_string().c_str());
                    }
                }
                if (!entry.usage.variants.empty()) {
                    std::string variants;
                    for (const fs::path& variant : entry.usage.variants) {
                        variants += (variants.empty() ? "" : ", ") + variant.stem().string();
                    }
                    ImGui::TextDisabled("Variants: %s", variants.c_str());
                    if (ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        for (const fs::path& variant : entry.usage.variants) {
                            ImGui::TextUnformatted(variant.generic_string().c_str());
                        }
                        ImGui::EndTooltip();
                    }
                }

                ImGui::TableNextColumn();
                const std::string shortGuid = entry.guid.size() > 8 ? entry.guid.substr(0, 8) + "..." : (entry.guid.empty() ? "-" : entry.guid);
                if (ImGui::Selectable(shortGuid.c_str(), false) && !entry.guid.empty()) {
                    ImGui::SetClipboardText(entry.guid.c_str());
                }
                if (!entry.guid.empty()) {
                    ImGui::SetItemTooltip("%s\nClick to copy", entry.guid.c_str());
                }

                ImGui::TableNextColumn();
                const size_t total = entry.usage.totalInstances();
                if (total > 0) {
                    ImGui::Text("%zu", total);
                } else {
                    ImGui::TextDisabled("0");
                }

                ImGui::TableNextColumn();
                drawUsage(entry.usage, false);
                ImGui::PopID();
            }

            if (!m_missing.empty()) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Spacing();
                ImGui::TextColored(MISSING_COLOR, ICON_FA_TRIANGLE_EXCLAMATION "  Missing Prefabs");
                for (const PrefabUsage& missing : m_missing) {
                    ImGui::PushID(missing.guid.empty() ? missing.path.generic_string().c_str() : missing.guid.c_str());
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextColored(MISSING_COLOR, "%s", missing.path.empty() ? "(unknown path)" : missing.path.generic_string().c_str());
                    ImGui::SetItemTooltip("No prefab file has this GUID: its instances are Missing Prefabs.\n"
                        "Put a .prefab with this GUID back (or use Replace in the Hierarchy) to relink them.");
                    ImGui::TableNextColumn();
                    const std::string shortGuid = missing.guid.size() > 8 ? missing.guid.substr(0, 8) + "..." : (missing.guid.empty() ? "-" : missing.guid);
                    if (ImGui::Selectable(shortGuid.c_str(), false) && !missing.guid.empty()) {
                        ImGui::SetClipboardText(missing.guid.c_str());
                    }
                    if (!missing.guid.empty()) {
                        ImGui::SetItemTooltip("%s\nClick to copy", missing.guid.c_str());
                    }
                    ImGui::TableNextColumn();
                    ImGui::TextColored(MISSING_COLOR, "%zu", missing.totalMissing());
                    ImGui::TableNextColumn();
                    drawUsage(missing, true);
                    ImGui::PopID();
                }
            }

            ImGui::EndTable();
        }
    }
    ImGui::EndChild();

    ImGui::SetCursorPos(ImVec2(style.WindowPadding.x, footerY - style.ItemSpacing.y));
    ImGui::Separator();

    const float footerWidth = std::max(1.0f, ImGui::GetWindowWidth() - style.WindowPadding.x * 2.0f);
    const float buttonWidth = std::clamp((footerWidth - style.ItemSpacing.x) * 0.5f, 1.0f, Theme::dpi(prefabsDialogButtonWidth));
    const float buttonsWidth = buttonWidth * 2.0f + style.ItemSpacing.x;
    // Unity-style footer: buttons on the right, primary first.
    ImGui::SetCursorPos(ImVec2(style.WindowPadding.x + std::max(0.0f, footerWidth - buttonsWidth), footerY));

    if (EditorModal::button("OK", "ok", ImVec2(buttonWidth, 0))) {
        applyPrefabs();
        m_isOpen = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::SameLine();

    if (EditorModal::button("Cancel", "cancel", ImVec2(buttonWidth, 0)) ||
        (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
        m_isOpen = false;
        ImGui::CloseCurrentPopup();
    }
}

void PrefabsWindow::applyPrefabs() {
    if (!m_project) {
        return;
    }

    // Scene prefabs are built through their scene, listing them would keep them after the last
    // instance is deleted
    std::vector<fs::path> prefabPaths;
    for (const PrefabEntry& entry : m_entries) {
        if (entry.included && entry.usedBy.empty()) {
            prefabPaths.push_back(entry.path);
        }
    }

    m_project->setStandalonePrefabs(std::move(prefabPaths));
    m_project->saveProject();
}

}
