// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Inspector's "Bake ▾ › Vertex Animation…" entries (vertex animation spec 5.2): drawn only
// once the baker window set VatBakerHooks::bake; a Shift-click bakes at once with the last
// settings (placed beside), a plain click opens the Vertex Animation Baker prefilled. Disabled
// with the reason while the scene plays. Used by InspectorWindow.cpp and InspectorWindowClips.cpp.

#pragma once

#include "external/IconsFontAwesome6.h"
#include "util/UIProbe.h"
#include "vat/VatBakerHooks.h"

#include "imgui.h"

#include <string>

namespace jasperengine::editor::inspectorvat {

    inline bool available() {
        return (bool)vatBakerHooks().bake;
    }

    inline const char* stoppedReason() {
        return "Stop the scene to bake a Vertex Animation: the baker poses the scene and writes files.";
    }

    // the "Vertex Animation..." item of an open Bake popup
    inline void menuItem(Project* project, const VatBakeEntry& entry, bool stopped, const std::string& probe) {
        if (!available()) return;
        if (ImGui::MenuItem(ICON_FA_PERSON_RUNNING " Vertex Animation...", nullptr, false, stopped)) {
            const bool now = ImGui::GetIO().KeyShift;
            // after the popup closes: the hook may open the baker window
            vatBakerHooks().bake(project, entry, now);
        }
        UIProbe::item(probe);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", stopped ? vatBakeEntryTooltip() : stoppedReason());
        }
    }

    // a "Bake ▾" button whose popup holds only the Vertex Animation entry
    inline void bakeButton(Project* project, const VatBakeEntry& entry, bool stopped, const char* id, const std::string& probe, bool small = false) {
        if (!available()) return;
        const std::string label = std::string(ICON_FA_FILM " Bake " ICON_FA_CARET_DOWN "##") + id;
        const std::string popup = std::string("##") + id + "_menu";
        if (small ? ImGui::SmallButton(label.c_str()) : ImGui::Button(label.c_str())) ImGui::OpenPopup(popup.c_str());
        UIProbe::item(probe);
        ImGui::SetItemTooltip("Bake the animation (Vertex Animation: GPU playback for crowds)");
        if (ImGui::BeginPopup(popup.c_str())) {
            menuItem(project, entry, stopped, probe + "Menu.vat");
            ImGui::EndPopup();
        }
    }

}
