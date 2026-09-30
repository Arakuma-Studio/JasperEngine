// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ComponentAddDialog.h"
#include "external/IconsFontAwesome6.h"
#include "util/UIUtils.h"
#include "Theme.h"
#include <cctype>
#include <cstring>

namespace jasperengine {
namespace editor {

namespace {

    std::string toLower(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return text;
    }

    // One list row: icon, label, optional right arrow; returns true when clicked
    bool listRow(const char* id, const char* icon, const std::string& label, bool enabled, bool highlighted, bool arrow) {
        ImGuiStyle& style = ImGui::GetStyle();
        ImVec2 pos = ImGui::GetCursorScreenPos();
        float width = ImGui::GetContentRegionAvail().x;
        float height = ImGui::GetFrameHeight();

        ImGui::BeginDisabled(!enabled);
        bool clicked = ImGui::Selectable(id, highlighted, ImGuiSelectableFlags_None, ImVec2(width, height));
        bool hovered = ImGui::IsItemHovered();
        ImGui::EndDisabled();

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImU32 textColor = ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled);
        float textY = pos.y + (height - ImGui::GetFontSize()) * 0.5f;
        float iconWidth = ImGui::GetFontSize() * 1.25f;

        if (icon) {
            ImVec2 iconSize = ImGui::CalcTextSize(icon);
            drawList->AddText(ImVec2(pos.x + style.FramePadding.x + (iconWidth - iconSize.x) * 0.5f, textY), textColor, icon);
        }
        drawList->AddText(ImVec2(pos.x + style.FramePadding.x + iconWidth + style.ItemInnerSpacing.x, textY), textColor, label.c_str());
        if (arrow) {
            ImVec2 arrowSize = ImGui::CalcTextSize(ICON_FA_CHEVRON_RIGHT);
            drawList->AddText(ImVec2(pos.x + width - arrowSize.x - style.FramePadding.x, textY), ImGui::GetColorU32(ImGuiCol_TextDisabled), ICON_FA_CHEVRON_RIGHT);
        }

        (void)hovered;
        return clicked && enabled;
    }

}

ComponentAddDialog::ComponentAddDialog() {
    initializeCategories();
}

// Unity's Add Component menu groups; names come from Catalog::getComponentDisplayName
void ComponentAddDialog::initializeCategories() {
    m_categories = {
        {"Mesh", ICON_FA_SHAPES, {
            {ComponentType::MeshComponent, "Draws a mesh with its materials"},
            {ComponentType::ModelComponent, "Loads a glTF or OBJ model"},
            {ComponentType::InstancedMeshComponent, "Draws many copies of the mesh in one call"},
            {ComponentType::MeshPolygonComponent, "Builds a flat mesh from polygon points"},
            {ComponentType::TerrainComponent, "Heightmap terrain with texture layers"},
        }},
        {"Rendering", ICON_FA_CAMERA, {
            {ComponentType::CameraComponent, "Renders the scene to the screen or a texture"},
            {ComponentType::LightComponent, "Directional, point or spot light"},
            {ComponentType::SpriteComponent, "Draws a 2D sprite"},
            {ComponentType::TilemapComponent, "Grid of tiles from a texture atlas"},
            {ComponentType::LinesComponent, "Draws line segments"},
            {ComponentType::PointsComponent, "Draws a point cloud"},
            {ComponentType::SkyComponent, "Skybox cube map and scene environment"},
            {ComponentType::FogComponent, "Distance fog"},
            {ComponentType::MirrorComponent, "Planar reflection surface"},
            {ComponentType::ReflectionProbeComponent, "Local environment reflection"},
            {ComponentType::Light2DComponent, "2D light with radius falloff"},
            {ComponentType::Occluder2DComponent, "Casts shadows from 2D lights"},
        }},
        {"Effects", ICON_FA_FIRE, {
            {ComponentType::ParticlesComponent, "Emits and animates particles"},
        }},
        {"Physics", ICON_FA_CUBE, {
            {ComponentType::Body3DComponent, "Rigidbody with colliders, simulated by Jolt"},
            {ComponentType::Joint3DComponent, "Connects two Rigidbodies"},
        }},
        {"Physics 2D", ICON_FA_CIRCLE, {
            {ComponentType::Body2DComponent, "2D Rigidbody with colliders, simulated by Box2D"},
            {ComponentType::Joint2DComponent, "Connects two 2D Rigidbodies"},
        }},
        {"Audio", ICON_FA_VOLUME_HIGH, {
            {ComponentType::SoundComponent, "Plays an audio clip"},
        }},
        {"UI", ICON_FA_WINDOW_RESTORE, {
            {ComponentType::UILayoutComponent, "Size, anchors and pivot of a UI element"},
            {ComponentType::UIComponent, "Draws a UI element"},
            {ComponentType::ImageComponent, "Displays an image"},
            {ComponentType::TextComponent, "Displays text"},
            {ComponentType::ButtonComponent, "Clickable button"},
            {ComponentType::TextEditComponent, "Editable text field"},
            {ComponentType::PanelComponent, "Movable, resizable window"},
            {ComponentType::ScrollbarComponent, "Scroll control"},
            {ComponentType::ProgressbarComponent, "Progress indicator"},
            {ComponentType::UIContainerComponent, "Arranges child elements in a row, column or grid"},
            {ComponentType::PolygonComponent, "Filled UI polygon"},
        }},
        {"Animation", ICON_FA_FILM, {
            {ComponentType::AnimationComponent, "Plays a sequence of actions"},
            {ComponentType::SpriteAnimationComponent, "Flips through sprite frames"},
            {ComponentType::ActionComponent, "Start, pause and stop state of an animation step"},
            {ComponentType::TimedActionComponent, "Duration, easing and looping of a tween"},
            {ComponentType::PositionActionComponent, "Moves between two positions"},
            {ComponentType::RotationActionComponent, "Rotates between two rotations"},
            {ComponentType::ScaleActionComponent, "Scales between two sizes"},
            {ComponentType::ColorActionComponent, "Fades between two colors"},
            {ComponentType::AlphaActionComponent, "Fades between two opacities"},
            {ComponentType::KeyframeTracksComponent, "Keyframe times for tracks"},
            {ComponentType::TranslateTracksComponent, "Position keyframes"},
            {ComponentType::RotateTracksComponent, "Rotation keyframes"},
            {ComponentType::ScaleTracksComponent, "Scale keyframes"},
            {ComponentType::MorphTracksComponent, "Blend shape keyframes"},
            {ComponentType::BoneComponent, "Skeleton bone of a model"},
        }},
        {"Scripts", ICON_FA_CODE, {
            {ComponentType::ScriptComponent, "Runs C++ or Lua scripts"},
        }},
        {"Miscellaneous", ICON_FA_PUZZLE_PIECE, {
            {ComponentType::Transform, "Position, rotation, scale and parent"},
        }},
    };
}

void ComponentAddDialog::open(
    std::function<void(ComponentType)> onAdd,
    std::function<bool(ComponentType)> canAddComponent,
    std::function<void()> onCancel,
    std::function<void()> onNewScript,
    ImVec2 anchorMin,
    ImVec2 anchorMax
) {
    m_isOpen = true;
    m_justOpened = true;
    m_focusSearch = false;
    memset(m_searchBuffer, 0, sizeof(m_searchBuffer));
    m_category = -1;
    m_highlighted = 0;
    m_onAdd = onAdd;
    m_canAddComponent = canAddComponent;
    m_onCancel = onCancel;
    m_onNewScript = onNewScript;
    m_anchorMin = anchorMin;
    m_anchorMax = anchorMax;
}

void ComponentAddDialog::finish(bool cancelled) {
    memset(m_searchBuffer, 0, sizeof(m_searchBuffer));
    m_category = -1;
    m_highlighted = 0;
    m_justOpened = false;
    m_isOpen = false;
    if (cancelled && m_onCancel) m_onCancel();
    ImGui::CloseCurrentPopup();
}

void ComponentAddDialog::show() {
    if (!m_isOpen) return;

    const char* popupId = "Add Component##ComponentAddPopup";
    if (m_justOpened) {
        ImGui::OpenPopup(popupId);
    }

    ImGuiStyle& style = ImGui::GetStyle();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float width = std::max(Theme::dpi(260.0f), std::min(m_anchorMax.x - m_anchorMin.x, Theme::dpi(320.0f)));
    const float height = Theme::dpi(340.0f);

    // below the button like Unity, above it when the screen ends first
    ImVec2 pos(0.5f * (m_anchorMin.x + m_anchorMax.x) - width * 0.5f, m_anchorMax.y + style.ItemSpacing.y);
    if (m_anchorMax.x <= m_anchorMin.x) {
        pos = ImVec2(viewport->GetCenter().x - width * 0.5f, viewport->GetCenter().y - height * 0.5f);
    } else if (pos.y + height > viewport->WorkPos.y + viewport->WorkSize.y) {
        pos.y = std::max(viewport->WorkPos.y, m_anchorMin.y - height - style.ItemSpacing.y);
    }
    pos.x = std::clamp(pos.x, viewport->WorkPos.x, viewport->WorkPos.x + std::max(0.0f, viewport->WorkSize.x - width));
    ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(width, height));

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Theme::dpi(ImVec2(6.0f, 6.0f)));
    const bool visible = ImGui::BeginPopup(popupId, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();
    if (!visible) {
        // closed by a click outside or Escape
        if (!m_justOpened) {
            m_isOpen = false;
            if (m_onCancel) m_onCancel();
        }
        return;
    }

    // Search field, focused on open and after changing page
    bool focusSearch = m_justOpened || m_focusSearch;
    m_justOpened = false;
    m_focusSearch = false;
    char previousSearch[sizeof(m_searchBuffer)];
    memcpy(previousSearch, m_searchBuffer, sizeof(m_searchBuffer));
    UIUtils::searchInput("##component_search", "Search", m_searchBuffer, sizeof(m_searchBuffer), focusSearch);
    if (strcmp(previousSearch, m_searchBuffer) != 0) {
        m_highlighted = 0;
    }
    const std::string search = toLower(m_searchBuffer);
    const bool searching = !search.empty();

    // Rows of the current page
    struct Row {
        enum Kind { Category, Component, NewScript } kind;
        int index;          // category index
        ComponentType type; // component rows
        const char* description;
        bool enabled;
    };
    std::vector<Row> rows;

    auto canAdd = [&](ComponentType type) {
        return !m_canAddComponent || m_canAddComponent(type);
    };

    if (searching) {
        for (int c = 0; c < (int)m_categories.size(); c++) {
            for (const ComponentEntry& entry : m_categories[c].entries) {
                std::string name = toLower(Catalog::getComponentDisplayName(entry.type));
                if (name.find(search) == std::string::npos) continue;
                if (!canAdd(entry.type)) continue;
                rows.push_back({Row::Component, c, entry.type, entry.description, true});
            }
        }
    } else if (m_category < 0) {
        for (int c = 0; c < (int)m_categories.size(); c++) {
            bool any = false;
            for (const ComponentEntry& entry : m_categories[c].entries) {
                if (canAdd(entry.type)) { any = true; break; }
            }
            rows.push_back({Row::Category, c, ComponentType::Transform, nullptr, any});
        }
    } else {
        for (const ComponentEntry& entry : m_categories[m_category].entries) {
            rows.push_back({Row::Component, m_category, entry.type, entry.description, canAdd(entry.type)});
        }
    }
    if (m_onNewScript && (m_category < 0 || searching)) {
        rows.push_back({Row::NewScript, -1, ComponentType::ScriptComponent, "Creates a C++ or Lua script and adds it", true});
    }

    // Keyboard: arrows move, Enter picks, Left/Backspace go back, Right opens
    if (!rows.empty()) {
        m_highlighted = std::clamp(m_highlighted, 0, (int)rows.size() - 1);
    }
    bool activateHighlighted = false;
    bool goBack = false;
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
            for (int i = m_highlighted + 1; i < (int)rows.size(); i++) {
                if (rows[i].enabled) { m_highlighted = i; m_scrollToHighlighted = true; break; }
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
            for (int i = m_highlighted - 1; i >= 0; i--) {
                if (rows[i].enabled) { m_highlighted = i; m_scrollToHighlighted = true; break; }
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
            activateHighlighted = true;
        }
        if (!searching && m_category >= 0 &&
                (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_Backspace))) {
            goBack = true;
        }
        if (!searching && m_category < 0 && ImGui::IsKeyPressed(ImGuiKey_RightArrow) &&
                !rows.empty() && rows[m_highlighted].kind == Row::Category) {
            activateHighlighted = true;
        }
    }

    // Header: page title, with a back arrow inside a category
    {
        const char* title = searching ? "Search" : (m_category < 0 ? "Component" : m_categories[m_category].name);
        ImVec2 headerMin = ImGui::GetCursorScreenPos();
        float headerWidth = ImGui::GetContentRegionAvail().x;
        float headerHeight = ImGui::GetFrameHeight();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(headerMin, ImVec2(headerMin.x + headerWidth, headerMin.y + headerHeight),
                                ImGui::GetColorU32(ImGuiCol_Header), style.FrameRounding);

        if (!searching && m_category >= 0) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            if (ImGui::Button(ICON_FA_CHEVRON_LEFT "##component_back", ImVec2(headerHeight, headerHeight))) {
                goBack = true;
            }
            ImGui::PopStyleColor();
            ImGui::SetItemTooltip("Back");
            ImGui::SetCursorScreenPos(headerMin);
        }

        ImVec2 titleSize = ImGui::CalcTextSize(title);
        ImVec2 titlePos(headerMin.x + (headerWidth - titleSize.x) * 0.5f, headerMin.y + (headerHeight - titleSize.y) * 0.5f);
        ImU32 titleColor = ImGui::GetColorU32(ImGuiCol_Text);
        drawList->AddText(titlePos, titleColor, title);
        drawList->AddText(ImVec2(titlePos.x + 1.0f, titlePos.y), titleColor, title); // bold
        ImGui::Dummy(ImVec2(headerWidth, headerHeight));
    }

    int picked = -1;
    ImGui::BeginChild("##component_rows", ImVec2(0, 0), ImGuiChildFlags_None);
    for (int i = 0; i < (int)rows.size(); i++) {
        const Row& row = rows[i];
        ImGui::PushID(i);

        if (row.kind == Row::NewScript && i > 0) {
            ImGui::Separator();
        }

        const char* icon = nullptr;
        std::string label;
        bool arrow = false;
        if (row.kind == Row::Category) {
            icon = m_categories[row.index].icon;
            label = m_categories[row.index].name;
            arrow = true;
        } else if (row.kind == Row::Component) {
            icon = Catalog::getComponentIcon(row.type);
            label = Catalog::getComponentDisplayName(row.type);
        } else {
            icon = ICON_FA_FILE_CIRCLE_PLUS;
            label = "New Script";
            arrow = true;
        }

        if (listRow("##row", icon, label, row.enabled, i == m_highlighted, arrow)) {
            picked = i;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (row.enabled && ImGui::GetIO().MouseDelta.x != 0.0f) m_highlighted = i;
            if (row.kind == Row::Component && !row.enabled) {
                ImGui::SetTooltip("Already added, or not allowed on this selection");
            } else if (row.description) {
                ImGui::SetTooltip("%s", row.description);
            }
        }
        if (i == m_highlighted && m_scrollToHighlighted) {
            ImGui::SetScrollHereY();
            m_scrollToHighlighted = false;
        }

        ImGui::PopID();
    }
    if (rows.empty()) {
        ImGui::Dummy(ImVec2(0, style.ItemSpacing.y * 4));
        const char* message = "No components match the search";
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(message).x) * 0.5f);
        ImGui::TextDisabled("%s", message);
    }
    ImGui::EndChild();

    if (activateHighlighted && picked < 0 && !rows.empty() && rows[m_highlighted].enabled) {
        picked = m_highlighted;
    }

    if (goBack) {
        m_highlighted = m_category >= 0 ? m_category : 0;
        m_category = -1;
        m_focusSearch = true;
    } else if (picked >= 0) {
        const Row row = rows[picked];
        if (row.kind == Row::Category) {
            m_category = row.index;
            m_highlighted = 0;
            for (int i = 0; i < (int)m_categories[row.index].entries.size(); i++) {
                if (canAdd(m_categories[row.index].entries[i].type)) { m_highlighted = i; break; }
            }
            m_focusSearch = true;
        } else if (row.kind == Row::Component) {
            if (m_onAdd) m_onAdd(row.type);
            finish(false);
        } else {
            std::function<void()> onNewScript = m_onNewScript;
            finish(false);
            if (onNewScript) onNewScript();
        }
    }

    ImGui::EndPopup();
}

} // namespace editor
} // namespace jasperengine
