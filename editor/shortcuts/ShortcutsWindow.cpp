// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ShortcutsWindow.h"

#include "Out.h"
#include "Theme.h"
#include "external/IconsFontAwesome6.h"
#include "search/SearchScorer.h"
#include "shortcuts/ShortcutOverrides.h"

#include "imgui_internal.h"
#include "window/EditorModal.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    std::string title(const CommandDef& def) {
        return def.group + " / " + def.name;
    }

    constexpr const char* CONFLICT_POPUP = "Shortcut in Use##ShortcutConflict";

}

ShortcutsWindow::ShortcutsWindow(CommandCatalog& catalog, ShortcutDispatcher& dispatcher) : catalog(catalog), dispatcher(dispatcher) {
}

void ShortcutsWindow::openEditor() {
    open = true;
    openRequested = true;
    reference = false;
}

void ShortcutsWindow::openReference() {
    open = true;
    openRequested = true;
    reference = true;
}

bool ShortcutsWindow::save(CommandCatalog& catalog, std::string* error) {
    std::string why;
    if (!ShortcutOverrides::save(ShortcutOverrides::defaultFile(), catalog, &why)) {
        Out::warning("Shortcuts: %s", why.c_str());
        if (error) *error = why;
        return false;
    }
    return true;
}

bool ShortcutsWindow::reassign(CommandCatalog& catalog, const std::string& id, const std::vector<Chord>& chords, std::string* error) {
    // every command that has one of the chords in the same context lets go of it
    for (Chord chord : chords) {
        bool shadowed = false;
        while (const CommandDef* other = catalog.findConflict(id, chord, &shadowed)) {
            if (shadowed) break;
            std::vector<Chord> theirs(catalog.bindings(other->id).begin(), catalog.bindings(other->id).end());
            theirs.erase(std::remove(theirs.begin(), theirs.end(), chord), theirs.end());
            if (!catalog.setBindings(other->id, theirs, error)) return false;
        }
    }
    if (!catalog.setBindings(id, chords, error)) return false;
    return save(catalog, error);
}

void ShortcutsWindow::beginCapture(const std::string& id, int slot) {
    captureId = id;
    captureSlot = slot;
    captured = Chord();
    dispatcher.setCaptureActive(true);
}

void ShortcutsWindow::endCapture(bool keep) {
    const std::string id = captureId;
    const int slot = captureSlot;
    const Chord chord = captured;
    captureId.clear();
    dispatcher.setCaptureActive(false);
    if (!keep || id.empty()) return;

    std::vector<Chord> chords(catalog.bindings(id).begin(), catalog.bindings(id).end());
    if (chord.empty()) {
        // Backspace: the binding goes
        if (slot < static_cast<int>(chords.size())) chords.erase(chords.begin() + slot);
    } else if (slot < static_cast<int>(chords.size())) {
        chords[slot] = chord;
    } else {
        chords.push_back(chord);
    }
    bool shadowed = false;
    const CommandDef* other = chord.empty() ? nullptr : catalog.findConflict(id, chord, &shadowed);
    if (other && !shadowed) {
        // asks first (R7): the new chords wait for Reassign
        conflictPending = true;
        conflictWith = other->id;
        captureId = id;
        captured = chord;
        pendingChords = chords;
        return;
    }
    apply(id, chords);
}

void ShortcutsWindow::apply(const std::string& id, std::vector<Chord> chords) {
    std::string error;
    if (!catalog.setBindings(id, std::move(chords), &error)) {
        Out::warning("Shortcuts: %s", error.c_str());
        return;
    }
    save(catalog, nullptr);
}

void ShortcutsWindow::pollCapture() {
    if (captureId.empty() || conflictPending) return;
    const ImGuiIO& io = ImGui::GetIO();
    const ImGuiID owner = ImGui::GetID("##ShortcutCapture");
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_GamepadStart; k++) {
        const ImGuiKey key = static_cast<ImGuiKey>(k);
        if (Chord::isModifierKey(key) || !ImGui::IsKeyPressed(key, false)) continue;
        // the key belongs to the capture: no window or command sees it, now or while it is held
        ImGui::SetKeyOwner(key, owner, ImGuiInputFlags_LockThisFrame | ImGuiInputFlags_LockUntilRelease);
        dispatcher.noteCaptureConsumedKey(key);
        const ImGuiKeyChord mods = io.KeyMods;
        if (key == ImGuiKey_Escape && mods == 0) {
            endCapture(false);
            return;
        }
        if ((key == ImGuiKey_Enter || key == ImGuiKey_KeypadEnter) && mods == 0) {
            if (!captured.empty()) endCapture(true);
            return;
        }
        if (key == ImGuiKey_Backspace && mods == 0) {
            captured = Chord();
            endCapture(true);
            return;
        }
        if (Chord::isBindableKey(key)) captured = Chord(mods | key);
    }
}

void ShortcutsWindow::drawConflictPopup() {
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!EditorModal::begin(CONFLICT_POPUP, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }
    const CommandDef* other = catalog.find(conflictWith);
    ImGui::Text("%s is already used by %s.", captured.format(catalog.isMac()).c_str(), other ? title(*other).c_str() : conflictWith.c_str());
    EditorModal::message(captured.format(catalog.isMac()) + " is already used by " + (other ? title(*other) : conflictWith) +
                         ". Reassign takes it from that command.");
    ImGui::TextDisabled("Reassign takes it from that command.");
    ImGui::Separator();
    const float width = ImGui::GetFontSize() * 6.0f;
    if (EditorModal::button("Reassign", "ok", ImVec2(width, 0.0f))) {
        std::string error;
        if (!reassign(catalog, captureId, pendingChords, &error)) Out::warning("Shortcuts: %s", error.c_str());
        conflictPending = false;
        captureId.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (EditorModal::button("Cancel", "cancel", ImVec2(width, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        // both bindings stay as they were
        conflictPending = false;
        captureId.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void ShortcutsWindow::drawTable() {
    const std::string query = search::normalize(searchBuffer);
    const bool mac = catalog.isMac();
    if (!ImGui::BeginTable("##ShortcutsTable", reference ? 3 : 4,
                           ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp)) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthStretch, 1.4f);
    ImGui::TableSetupColumn("Shortcut", ImGuiTableColumnFlags_WidthStretch, 1.2f);
    ImGui::TableSetupColumn("Context", ImGuiTableColumnFlags_WidthStretch, 0.6f);
    if (!reference) ImGui::TableSetupColumn("##Reset", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
    ImGui::TableHeadersRow();

    std::string lastGroup;
    for (const CommandDef& def : catalog.commands()) {
        if (!group.empty() && def.group != group) continue;
        const std::span<const Chord> chords = catalog.bindings(def.id);
        if (reference && chords.empty() && !def.readOnly) continue;
        if (!query.empty()) {
            const std::string text = search::normalize(def.group + " " + def.name + " " + def.id + " " + catalog.bindingsText(def.id));
            if (!search::filterMatch(query, text)) continue;
        }
        if (def.group != lastGroup) {
            lastGroup = def.group;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", def.group.c_str());
        }
        ImGui::PushID(def.id.c_str());
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(def.name.c_str());
        if (ImGui::IsItemHovered()) {
            const std::string path = catalog.menuPath(def.id);
            ImGui::SetTooltip("%s%s%s%s", def.id.c_str(), path.empty() ? "" : ("\n" + path).c_str(),
                              def.description.empty() ? "" : "\n", def.description.c_str());
        }

        ImGui::TableNextColumn();
        if (def.readOnly) {
            ImGui::TextDisabled("%s", def.gesture.c_str());
            ImGui::SetItemTooltip("A reference row: this gesture is not rebindable");
        } else if (reference) {
            ImGui::TextUnformatted(catalog.bindingsText(def.id).c_str());
        } else {
            // a button per binding (a click captures), then + for one more
            const int slots = static_cast<int>(chords.size());
            for (int slot = 0; slot <= std::min(slots, 1); slot++) {
                if (slot > 0) ImGui::SameLine();
                const bool listening = captureId == def.id && captureSlot == slot && !conflictPending;
                std::string text;
                if (listening) text = captured.empty() ? "Press a key..." : captured.format(mac) + "  (Enter)";
                else if (slot < slots) text = chords[slot].format(mac);
                else text = slots == 0 ? "None" : "+";
                ImGui::PushID(slot);
                if (listening) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                if (ImGui::Button(text.c_str())) {
                    if (listening) endCapture(!captured.empty());
                    else beginCapture(def.id, slot);
                }
                if (listening) ImGui::PopStyleColor();
                if (!listening) {
                    bool shadowed = false;
                    const CommandDef* other = slot < slots ? catalog.findConflict(def.id, chords[slot], &shadowed) : nullptr;
                    if (other) {
                        ImGui::SameLine(0.0f, 2.0f);
                        ImGui::TextColored(shadowed ? ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled) : Theme::Colors::WarningText,
                                           shadowed ? ICON_FA_LAYER_GROUP : ICON_FA_TRIANGLE_EXCLAMATION);
                        ImGui::SetItemTooltip(shadowed ? "Also bound in %s: that window's binding wins while it has focus"
                                                       : "Also bound to %s in the same context",
                                              title(*other).c_str());
                    }
                    ImGui::SetItemTooltip(slot < slots ? "Click, then press the new keys (Enter keeps, Esc cancels, Backspace clears)"
                                                       : "Click, then press the keys of another shortcut");
                }
                ImGui::PopID();
            }
        }

        ImGui::TableNextColumn();
        std::string context = shortcutContextName(def.context);
        for (ShortcutContext extra : def.alsoIn) context += std::string(", ") + shortcutContextName(extra);
        ImGui::TextDisabled("%s", context.c_str());

        if (!reference) {
            ImGui::TableNextColumn();
            if (!def.readOnly && catalog.isOverridden(def.id)) {
                if (ImGui::SmallButton(ICON_FA_ROTATE_LEFT)) {
                    catalog.resetBindings(def.id);
                    save(catalog, nullptr);
                }
                ImGui::SetItemTooltip("Reset to Default (%s)", [&]() {
                    std::string text;
                    for (Chord chord : catalog.defaults(def.id)) text += (text.empty() ? "" : " / ") + chord.format(mac);
                    return text.empty() ? std::string("none") : text;
                }().c_str());
            }
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void ShortcutsWindow::show() {
    if (!open) {
        if (!captureId.empty()) endCapture(false);
        return;
    }
    pollCapture();

    if (openRequested) {
        openRequested = false;
        ImGui::SetNextWindowFocus();
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(std::min(Theme::dpi(820.0f), viewport->WorkSize.x * 0.9f),
                                        std::min(Theme::dpi(560.0f), viewport->WorkSize.y * 0.85f)), ImGuiCond_Always);
    }
    const char* name = reference ? "Keyboard Shortcuts###ShortcutsWindow" : "Shortcuts###ShortcutsWindow";
    if (ImGui::Begin(name, &open, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse)) {
        // a click outside the capturing button keeps what was pressed
        if (!captureId.empty() && !conflictPending && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            !ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
            endCapture(!captured.empty());
        }
        ImGui::SetNextItemWidth(Theme::dpi(260.0f));
        ImGui::InputTextWithHint("##ShortcutSearch", "Search commands and keys", searchBuffer, sizeof(searchBuffer));
        ImGui::SameLine();
        if (reference) {
            if (ImGui::Button("Edit Shortcuts...")) reference = false;
            ImGui::SetItemTooltip("Edit > Shortcuts...: rebind them");
        } else {
            if (ImGui::Button("Reset All to Default")) {
                catalog.resetAll();
                save(catalog, nullptr);
            }
            ImGui::SetItemTooltip("Every command back to its default shortcut");
        }
        ImGui::TextDisabled("%s", catalog.isMac() ? "Cmd is the Command key." : "Shortcuts follow the keyboard layout's letters.");

        // the groups, then the table
        const float listWidth = Theme::dpi(150.0f);
        ImGui::BeginChild("##ShortcutGroups", ImVec2(listWidth, 0.0f), ImGuiChildFlags_Borders);
        if (ImGui::Selectable("All", group.empty())) group.clear();
        std::vector<std::string> groups;
        for (const CommandDef& def : catalog.commands()) {
            if (std::find(groups.begin(), groups.end(), def.group) == groups.end()) groups.push_back(def.group);
        }
        for (const std::string& name : groups) {
            if (ImGui::Selectable(name.c_str(), group == name)) group = name;
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##ShortcutRows", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
        drawTable();
        ImGui::EndChild();
        if (conflictPending && !ImGui::IsPopupOpen(CONFLICT_POPUP)) ImGui::OpenPopup(CONFLICT_POPUP);
        drawConflictPopup();
    }
    ImGui::End();
}
