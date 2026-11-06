// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Theme.h"

#include "imgui_internal.h"

namespace {

ImGuiStyle g_baseStyle;
bool g_hasBaseStyle = false;
float g_appliedDpiScale = 0.0f;
// Editor Settings > UI Scale, on top of the monitor DPI
float g_uiScale = 1.0f;
bool g_playModeTint = false;

// Unity dark skin swatch, 0xRRGGBB.
ImVec4 hex(unsigned int rgb, float alpha = 1.0f) {
    return ImVec4(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, alpha);
}

// Surfaces that take the play-mode tint. Text, accents and the selection keep
// their colour so the editor stays readable, as with Unity's tint.
const ImGuiCol_ kTintedColors[] = {
    ImGuiCol_WindowBg, ImGuiCol_PopupBg, ImGuiCol_Border,
    ImGuiCol_FrameBg, ImGuiCol_FrameBgHovered, ImGuiCol_FrameBgActive,
    ImGuiCol_TitleBg, ImGuiCol_TitleBgActive, ImGuiCol_TitleBgCollapsed, ImGuiCol_MenuBarBg,
    ImGuiCol_ScrollbarBg, ImGuiCol_Button, ImGuiCol_ButtonHovered, ImGuiCol_ButtonActive,
    ImGuiCol_Separator, ImGuiCol_Tab, ImGuiCol_TabHovered, ImGuiCol_TabSelected,
    ImGuiCol_TabDimmed, ImGuiCol_TabDimmedSelected, ImGuiCol_TabDimmedSelectedOverline,
    ImGuiCol_DockingEmptyBg, ImGuiCol_TableHeaderBg, ImGuiCol_TableBorderStrong, ImGuiCol_TableBorderLight,
};

ImVec4 tinted(const ImVec4& c, const ImVec4& tint) {
    return ImVec4(c.x * tint.x, c.y * tint.y, c.z * tint.z, c.w);
}

// Custom surface colours (Theme::Colors) that take the tint, with their
// untinted values captured by apply().
using ThemeColors = jasperengine::editor::Theme::Colors;
ImVec4* const kTintedCustomColors[] = {
    &ThemeColors::ToolbarBackground, &ThemeColors::ToolbarBorder, &ThemeColors::RowBackgroundAlt,
    &ThemeColors::FoldoutHeader, &ThemeColors::FoldoutHeaderHovered, &ThemeColors::FoldoutHeaderActive,
    &ThemeColors::FileCardBackground, &ThemeColors::FileCardBackgroundHovered, &ThemeColors::filenameLabel,
    &ThemeColors::NestedHeader, &ThemeColors::NestedHeaderHovered, &ThemeColors::NestedHeaderActive,
    &ThemeColors::ButtonActivated, &ThemeColors::SelectionUnfocused,
};
constexpr size_t kTintedCustomCount = sizeof(kTintedCustomColors) / sizeof(kTintedCustomColors[0]);
ImVec4 g_baseCustomColors[kTintedCustomCount];

// Rebuilds the style colours from the untinted base, tinted if play mode is on.
void applyPlayModeTint(ImGuiStyle& style) {
    for (int i = 0; i < ImGuiCol_COUNT; i++) {
        style.Colors[i] = g_baseStyle.Colors[i];
    }
    for (size_t i = 0; i < kTintedCustomCount; i++) {
        *kTintedCustomColors[i] = g_baseCustomColors[i];
    }
    if (!g_playModeTint) {
        return;
    }
    const ImVec4& tint = ThemeColors::PlayModeTint;
    for (ImGuiCol_ idx : kTintedColors) {
        style.Colors[idx] = tinted(style.Colors[idx], tint);
    }
    for (size_t i = 0; i < kTintedCustomCount; i++) {
        *kTintedCustomColors[i] = tinted(g_baseCustomColors[i], tint);
    }
}

}

ImVec4 jasperengine::editor::Theme::Colors::ButtonActivated;
ImVec4 jasperengine::editor::Theme::Colors::IconButtonHovered;
ImVec4 jasperengine::editor::Theme::Colors::IconButtonActive;
ImVec4 jasperengine::editor::Theme::Colors::FileCardBackground;
ImVec4 jasperengine::editor::Theme::Colors::FileCardBackgroundHovered;
ImVec4 jasperengine::editor::Theme::Colors::SubtleText;
ImVec4 jasperengine::editor::Theme::Colors::filenameLabel;
ImVec4 jasperengine::editor::Theme::Colors::ExtEntityButton;
ImVec4 jasperengine::editor::Theme::Colors::ExtEntityButtonHovered;
ImVec4 jasperengine::editor::Theme::Colors::ExtEntityButtonActive;
ImVec4 jasperengine::editor::Theme::Colors::NestedHeader;
ImVec4 jasperengine::editor::Theme::Colors::NestedHeaderHovered;
ImVec4 jasperengine::editor::Theme::Colors::NestedHeaderActive;
ImVec4 jasperengine::editor::Theme::Colors::DisabledGreenText;
ImVec4 jasperengine::editor::Theme::Colors::ChildSceneText;
ImVec4 jasperengine::editor::Theme::Colors::SubSelectionText;
ImVec4 jasperengine::editor::Theme::Colors::WarningText;
ImVec4 jasperengine::editor::Theme::Colors::Selection;
ImVec4 jasperengine::editor::Theme::Colors::SelectionUnfocused;
ImVec4 jasperengine::editor::Theme::Colors::FoldoutHeader;
ImVec4 jasperengine::editor::Theme::Colors::FoldoutHeaderHovered;
ImVec4 jasperengine::editor::Theme::Colors::FoldoutHeaderActive;
ImVec4 jasperengine::editor::Theme::Colors::ToolbarBackground;
ImVec4 jasperengine::editor::Theme::Colors::ToolbarBorder;
ImVec4 jasperengine::editor::Theme::Colors::PrefabText;
ImVec4 jasperengine::editor::Theme::Colors::PrefabTextMuted;
ImVec4 jasperengine::editor::Theme::Colors::ErrorText;
ImVec4 jasperengine::editor::Theme::Colors::LogIcon;
ImVec4 jasperengine::editor::Theme::Colors::RowBackgroundAlt;
ImVec4 jasperengine::editor::Theme::Colors::PlayModeTint;
ImVec4 jasperengine::editor::Theme::Colors::PhysicsAccent;
ImVec4 jasperengine::editor::Theme::Colors::PhysicsAccentHovered;
ImVec4 jasperengine::editor::Theme::Colors::AnimationAudio;
ImVec4 jasperengine::editor::Theme::Colors::AnimationSnapshot;

void jasperengine::editor::Theme::apply() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    const ImVec4 panel     = hex(0x383838); // panel background
    const ImVec4 darkStrip = hex(0x282828); // tab strip and title bars
    const ImVec4 border    = hex(0x191919); // outlines and the gaps between panels
    const ImVec4 field     = hex(0x2A2A2A); // input fields
    const ImVec4 selection = hex(0x2C5D87); // Unity selection blue
    const ImVec4 accent    = hex(0x3A79BB); // focus accents: active tab line, nav, drops

    colors[ImGuiCol_Text]                       = hex(0xD2D2D2);
    colors[ImGuiCol_TextDisabled]               = hex(0x808080);
    colors[ImGuiCol_WindowBg]                   = panel;
    colors[ImGuiCol_ChildBg]                    = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_PopupBg]                    = hex(0x2F2F2F);
    colors[ImGuiCol_Border]                     = border;
    colors[ImGuiCol_BorderShadow]               = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg]                    = field;
    colors[ImGuiCol_FrameBgHovered]             = hex(0x303030);
    colors[ImGuiCol_FrameBgActive]              = hex(0x252525);
    colors[ImGuiCol_TitleBg]                    = darkStrip;
    colors[ImGuiCol_TitleBgActive]              = darkStrip;
    colors[ImGuiCol_TitleBgCollapsed]           = darkStrip;
    colors[ImGuiCol_MenuBarBg]                  = hex(0x202020);
    colors[ImGuiCol_ScrollbarBg]                = hex(0x333333);
    colors[ImGuiCol_ScrollbarGrab]              = hex(0x5F5F5F);
    colors[ImGuiCol_ScrollbarGrabHovered]       = hex(0x6E6E6E);
    colors[ImGuiCol_ScrollbarGrabActive]        = hex(0x7C7C7C);
    colors[ImGuiCol_CheckMark]                  = hex(0xC4C4C4);
    colors[ImGuiCol_SliderGrab]                 = hex(0x8F8F8F);
    colors[ImGuiCol_SliderGrabActive]           = hex(0xB5B5B5);
    colors[ImGuiCol_Button]                     = hex(0x585858);
    colors[ImGuiCol_ButtonHovered]              = hex(0x676767);
    colors[ImGuiCol_ButtonActive]               = hex(0x464646);
    colors[ImGuiCol_Header]                     = selection;
    colors[ImGuiCol_HeaderHovered]              = hex(0x3B5A78);
    colors[ImGuiCol_HeaderActive]               = hex(0x336A9A);
    colors[ImGuiCol_Separator]                  = border;
    colors[ImGuiCol_SeparatorHovered]           = hex(0x4A4A4A);
    colors[ImGuiCol_SeparatorActive]            = accent;
    colors[ImGuiCol_ResizeGrip]                 = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_ResizeGripHovered]          = hex(0x676767);
    colors[ImGuiCol_ResizeGripActive]           = accent;
    colors[ImGuiCol_InputTextCursor]            = colors[ImGuiCol_Text];
    colors[ImGuiCol_Tab]                        = hex(0x303030);
    colors[ImGuiCol_TabHovered]                 = hex(0x454545);
    colors[ImGuiCol_TabSelected]                = panel;
    colors[ImGuiCol_TabSelectedOverline]        = accent;
    colors[ImGuiCol_TabDimmed]                  = hex(0x2D2D2D);
    colors[ImGuiCol_TabDimmedSelected]          = panel;
    colors[ImGuiCol_TabDimmedSelectedOverline]  = panel;
    colors[ImGuiCol_DockingPreview]             = ImVec4(accent.x, accent.y, accent.z, 0.55f);
    colors[ImGuiCol_DockingEmptyBg]             = hex(0x191919);
    colors[ImGuiCol_PlotLines]                  = hex(0x9C9C9C);
    colors[ImGuiCol_PlotLinesHovered]           = hex(0xFF8A65);
    colors[ImGuiCol_PlotHistogram]              = hex(0x5A7FA6);
    colors[ImGuiCol_PlotHistogramHovered]       = hex(0x7BAEFA);
    colors[ImGuiCol_TableHeaderBg]              = hex(0x3C3C3C);
    colors[ImGuiCol_TableBorderStrong]          = border;
    colors[ImGuiCol_TableBorderLight]           = hex(0x2A2A2A);
    colors[ImGuiCol_TableRowBg]                 = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt]              = ImVec4(1.00f, 1.00f, 1.00f, 0.025f);
    colors[ImGuiCol_TextLink]                   = hex(0x4C9BEA);
    colors[ImGuiCol_TextSelectedBg]             = ImVec4(selection.x, selection.y, selection.z, 0.85f);
    colors[ImGuiCol_TreeLines]                  = hex(0x505050);
    colors[ImGuiCol_DragDropTarget]             = accent;
    colors[ImGuiCol_DragDropTargetBg]           = ImVec4(accent.x, accent.y, accent.z, 0.15f);
    colors[ImGuiCol_UnsavedMarker]              = colors[ImGuiCol_Text];
    colors[ImGuiCol_NavCursor]                  = accent;
    colors[ImGuiCol_NavWindowingHighlight]      = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg]          = ImVec4(0.00f, 0.00f, 0.00f, 0.35f);
    colors[ImGuiCol_ModalWindowDimBg]           = ImVec4(0.00f, 0.00f, 0.00f, 0.45f);
    colors[ImGuiCol_CheckboxSelectedBg]         = colors[ImGuiCol_FrameBg];

    Colors::Selection = selection;
    Colors::SelectionUnfocused = hex(0x4D4D4D);
    Colors::FoldoutHeader = hex(0x3E3E3E);
    Colors::FoldoutHeaderHovered = hex(0x474747);
    Colors::FoldoutHeaderActive = hex(0x505050);
    Colors::ToolbarBackground = hex(0x3C3C3C);
    Colors::ToolbarBorder = hex(0x232323);
    Colors::PrefabText = hex(0x7BAEFA);
    Colors::PrefabTextMuted = hex(0x7BAEFA, 0.75f);
    Colors::ErrorText = hex(0xFF6B6B);
    Colors::LogIcon = hex(0xC8C8C8);
    Colors::RowBackgroundAlt = hex(0x3E3E3E);
    Colors::PlayModeTint = ImVec4(0.78f, 0.80f, 0.87f, 1.00f);
    Colors::PhysicsAccent = hex(0xC8701E);
    Colors::PhysicsAccentHovered = hex(0xD98232);
    // Unity's Timeline audio track green, and a mixer purple
    Colors::AnimationAudio = hex(0x3E9E5B);
    Colors::AnimationSnapshot = hex(0x8A63C9);

    Colors::ButtonActivated = hex(0x46607C);
    Colors::IconButtonHovered = ImVec4(1.0f, 1.0f, 1.0f, 0.08f);
    Colors::IconButtonActive = ImVec4(1.0f, 1.0f, 1.0f, 0.14f);
    Colors::FileCardBackground = hex(0x323232);
    Colors::FileCardBackgroundHovered = hex(0x3F3F3F);
    Colors::SubtleText = hex(0x9A9A9A);
    Colors::filenameLabel = field;
    Colors::ExtEntityButton = hex(0x4F5D6E);
    Colors::ExtEntityButtonHovered = hex(0x5C6B7D);
    Colors::ExtEntityButtonActive = hex(0x45505E);
    Colors::NestedHeader = hex(0x353535);
    Colors::NestedHeaderHovered = hex(0x404040);
    Colors::NestedHeaderActive = hex(0x4A4A4A);
    Colors::DisabledGreenText = ImLerp(colors[ImGuiCol_TextDisabled],
                                       ImVec4(0.38f, 0.72f, 0.46f, 1.00f),
                                       0.58f);
    Colors::ChildSceneText = ImVec4(0.55f, 0.80f, 0.85f, 1.0f); // Soft teal for child scenes
    Colors::SubSelectionText = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
    Colors::WarningText = hex(0xFFC83D);

    // main: Unity is dense, rows of 18-20px with little padding
    style.WindowPadding = ImVec2(6, 5);
    style.FramePadding = ImVec2(6, 3);
    style.ItemSpacing = ImVec2(6, 4);
    style.ItemInnerSpacing = ImVec2(4, 4);
    style.TouchExtraPadding = ImVec2(0, 0);
    style.IndentSpacing = 14;
    style.ScrollbarSize = 11;
    style.GrabMinSize = 10;

    // borders: 1px dark outlines on fields, buttons and floating windows
    style.WindowBorderSize = 1;
    style.ChildBorderSize = 1;
    style.PopupBorderSize = 1;
    style.FrameBorderSize = 1;
    style.TabBorderSize = 0;
    style.TabBarBorderSize = 1;
    style.TabBarOverlineSize = 2;
    // Unity tabs carry no close cross; show it only under the mouse
    style.TabCloseButtonMinWidthSelected = 0;
    style.TabCloseButtonMinWidthUnselected = 0;

    // rounding
    style.WindowRounding = 0;
    style.ChildRounding = 2;
    style.FrameRounding = 3;
    style.PopupRounding = 2;
    style.ScrollbarRounding = 6;
    style.GrabRounding = 2;
    style.TabRounding = 2;

    // tables
    style.CellPadding = ImVec2(4, 2);
    style.TableAngledHeadersAngle = 0.61; //35 deg
    style.TableAngledHeadersTextAlign = ImVec2(0.50, 0.00);

    // widgets
    style.WindowTitleAlign = ImVec2(0.00, 0.50);
    style.WindowMenuButtonPosition = ImGuiDir::ImGuiDir_Right;
    style.ColorButtonPosition = ImGuiDir::ImGuiDir_Right;
    style.ButtonTextAlign = ImVec2(0.50, 0.50);
    style.SelectableTextAlign = ImVec2(0.00, 0.00);
    style.SeparatorTextBorderSize = 1;
    style.SeparatorTextAlign = ImVec2(0.00, 0.50);
    style.SeparatorTextPadding = ImVec2(12, 0);
    style.LogSliderDeadzone = 4;

    // docking: thin dark gaps between panels, as in Unity
    style.DockingSeparatorSize = 2;

    g_baseStyle = style;
    for (size_t i = 0; i < kTintedCustomCount; i++) {
        g_baseCustomColors[i] = *kTintedCustomColors[i];
    }
    g_hasBaseStyle = true;
    g_appliedDpiScale = 0.0f;
    applyPlayModeTint(style);
}

void jasperengine::editor::Theme::applyDpiScale(float dpiScale) {
    if (!g_hasBaseStyle) {
        return;
    }
    if (dpiScale < 0.5f || dpiScale > 8.0f) {
        dpiScale = 1.0f;
    }
    if (g_appliedDpiScale == dpiScale) {
        return;
    }

    ImGuiStyle& style = ImGui::GetStyle();
    const float fontScaleDpi = style.FontScaleDpi;
    style = g_baseStyle;
    style.ScaleAllSizes(dpiScale * g_uiScale);
    // Fonts are rebuilt at the new size from here (dynamic font atlas)
    style.FontScaleMain = g_uiScale;
    applyPlayModeTint(style);
    // ConfigDpiScaleFonts overwrites FontScaleDpi per window; keep the current
    // value so this reset does not briefly unscale fonts.
    style.FontScaleDpi = fontScaleDpi;
    g_appliedDpiScale = dpiScale;
}

void jasperengine::editor::Theme::setUiScale(float uiScale) {
    g_uiScale = uiScale;
    // applyDpiScale() rescales the style on its next call
    g_appliedDpiScale = 0.0f;
}

float jasperengine::editor::Theme::uiScale() {
    return g_uiScale;
}

void jasperengine::editor::Theme::setPlayModeTint(bool active) {
    if (g_playModeTint == active || !g_hasBaseStyle) {
        return;
    }
    g_playModeTint = active;
    applyPlayModeTint(ImGui::GetStyle());
}

void jasperengine::editor::Theme::pushFoldoutHeaderColors() {
    ImGui::PushStyleColor(ImGuiCol_Header, Colors::FoldoutHeader);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, Colors::FoldoutHeaderHovered);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, Colors::FoldoutHeaderActive);
}

void jasperengine::editor::Theme::popFoldoutHeaderColors() {
    ImGui::PopStyleColor(3);
}

bool jasperengine::editor::Theme::foldoutHeader(const char* label, int flags) {
    pushFoldoutHeaderColors();
    const bool open = ImGui::CollapsingHeader(label, flags);
    popFoldoutHeaderColors();
    return open;
}

float jasperengine::editor::Theme::dpiScale() {
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (ctx && ctx->CurrentWindow && ctx->CurrentWindow->Viewport && ctx->CurrentWindow->Viewport->DpiScale > 0.0f) {
        return ctx->CurrentWindow->Viewport->DpiScale * g_uiScale;
    }
    return ((g_appliedDpiScale > 0.0f) ? g_appliedDpiScale : 1.0f) * g_uiScale;
}

float jasperengine::editor::Theme::dpi(float value) {
    return value * dpiScale();
}

ImVec2 jasperengine::editor::Theme::dpi(const ImVec2& value) {
    const float scale = dpiScale();
    return ImVec2(value.x * scale, value.y * scale);
}
