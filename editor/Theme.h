// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "imgui.h"

namespace jasperengine::editor {

    class Theme {
    public:
        struct Colors {
            static ImVec4 ButtonActivated;
            // Hover/press tint for transparent icon buttons (find close, replace toggle).
            static ImVec4 IconButtonHovered;
            static ImVec4 IconButtonActive;
            static ImVec4 FileCardBackground;
            static ImVec4 FileCardBackgroundHovered;
            static ImVec4 SubtleText;
            static ImVec4 filenameLabel;
            static ImVec4 ExtEntityButton;
            static ImVec4 ExtEntityButtonHovered;
            static ImVec4 ExtEntityButtonActive;
            static ImVec4 NestedHeader;
            static ImVec4 NestedHeaderHovered;
            static ImVec4 NestedHeaderActive;
            static ImVec4 DisabledGreenText;
            static ImVec4 ChildSceneText;
            // Marks the viewport sub-selection (tiles, instances, occluder points)
            // in Properties lists; same orange as the viewport selection outline.
            static ImVec4 SubSelectionText;
            // Unity console yellow; alert notes and the Console's warning icon.
            static ImVec4 WarningText;

            // Unity dark skin extras. ImGuiCol_Header* is the blue selection
            // highlight (Hierarchy, Project, menus); component/foldout headers
            // push FoldoutHeader* instead (see pushFoldoutHeaderColors()).
            static ImVec4 Selection;            // #2C5D87 focused selection
            static ImVec4 SelectionUnfocused;   // grey selection of an unfocused panel
            static ImVec4 FoldoutHeader;        // #3E3E3E Inspector component header
            static ImVec4 FoldoutHeaderHovered;
            static ImVec4 FoldoutHeaderActive;
            static ImVec4 ToolbarBackground;    // panel toolbars (Console, Project, Hierarchy search)
            static ImVec4 ToolbarBorder;        // line under a panel toolbar
            static ImVec4 PrefabText;           // #7BAEFA prefab instance name
            static ImVec4 PrefabTextMuted;      // children of a prefab instance
            static ImVec4 ErrorText;            // Console error red
            static ImVec4 LogIcon;              // Console log (info) icon
            static ImVec4 RowBackgroundAlt;     // odd rows in lists (Console)
            static ImVec4 PlayModeTint;         // multiplied into backgrounds while playing
        };

        static void apply();
        static void applyDpiScale(float dpiScale);

        // Unity darkens the editor while in play mode. Call every frame with the
        // play state; it only rewrites the style when the state changes.
        static void setPlayModeTint(bool active);
        static bool isPlayModeTinted();

        static void pushFoldoutHeaderColors();
        static void popFoldoutHeaderColors();

        static float dpiScale();
        static float dpi(float value);
        static ImVec2 dpi(const ImVec2& value);
    };

}

