// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "InputActionsWindow.h"

#include "Engine.h"
#include "Input.h"
#include "Out.h"
#include "Project.h"
#include "Theme.h"
#include "command/CommandHistory.h"
#include "command/type/InputActionsEditCmd.h"
#include "external/IconsFontAwesome6.h"
#include "input/InputActions.h"
#include "input/InputControlPath.h"
#include "util/ProjectChecks.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    const char* const GROUPS[] = {"Keyboard&Mouse", "Gamepad", "Touch"};

    struct KeyMap {
        ImGuiKey imgui;
        int engine;
    };

    // ImGui's keys as the engine's (PlayInput forwards the same table to the game)
    const KeyMap KEY_MAP[] = {
        {ImGuiKey_Tab, D_KEY_TAB}, {ImGuiKey_Backspace, D_KEY_BACKSPACE}, {ImGuiKey_Enter, D_KEY_ENTER},
        {ImGuiKey_KeypadEnter, D_KEY_KP_ENTER}, {ImGuiKey_Escape, D_KEY_ESCAPE}, {ImGuiKey_Space, D_KEY_SPACE},
        {ImGuiKey_LeftArrow, D_KEY_LEFT}, {ImGuiKey_RightArrow, D_KEY_RIGHT}, {ImGuiKey_UpArrow, D_KEY_UP},
        {ImGuiKey_DownArrow, D_KEY_DOWN}, {ImGuiKey_Insert, D_KEY_INSERT}, {ImGuiKey_Home, D_KEY_HOME},
        {ImGuiKey_End, D_KEY_END}, {ImGuiKey_PageUp, D_KEY_PAGE_UP}, {ImGuiKey_PageDown, D_KEY_PAGE_DOWN},
        {ImGuiKey_Delete, D_KEY_DELETE}, {ImGuiKey_Apostrophe, D_KEY_APOSTROPHE}, {ImGuiKey_Comma, D_KEY_COMMA},
        {ImGuiKey_Minus, D_KEY_MINUS}, {ImGuiKey_Period, D_KEY_PERIOD}, {ImGuiKey_Slash, D_KEY_SLASH},
        {ImGuiKey_Semicolon, D_KEY_SEMICOLON}, {ImGuiKey_Equal, D_KEY_EQUAL}, {ImGuiKey_LeftBracket, D_KEY_LEFT_BRACKET},
        {ImGuiKey_Backslash, D_KEY_BACKSLASH}, {ImGuiKey_RightBracket, D_KEY_RIGHT_BRACKET},
        {ImGuiKey_GraveAccent, D_KEY_GRAVE_ACCENT}, {ImGuiKey_CapsLock, D_KEY_CAPS_LOCK},
        {ImGuiKey_ScrollLock, D_KEY_SCROLL_LOCK}, {ImGuiKey_NumLock, D_KEY_NUM_LOCK},
        {ImGuiKey_PrintScreen, D_KEY_PRINT_SCREEN}, {ImGuiKey_Pause, D_KEY_PAUSE},
        {ImGuiKey_LeftShift, D_KEY_LEFT_SHIFT}, {ImGuiKey_LeftCtrl, D_KEY_LEFT_CONTROL}, {ImGuiKey_LeftAlt, D_KEY_LEFT_ALT},
        {ImGuiKey_LeftSuper, D_KEY_LEFT_SUPER}, {ImGuiKey_RightShift, D_KEY_RIGHT_SHIFT},
        {ImGuiKey_RightCtrl, D_KEY_RIGHT_CONTROL}, {ImGuiKey_RightAlt, D_KEY_RIGHT_ALT},
        {ImGuiKey_RightSuper, D_KEY_RIGHT_SUPER}, {ImGuiKey_Menu, D_KEY_MENU},
        {ImGuiKey_KeypadDecimal, D_KEY_KP_DECIMAL}, {ImGuiKey_KeypadDivide, D_KEY_KP_DIVIDE},
        {ImGuiKey_KeypadMultiply, D_KEY_KP_MULTIPLY}, {ImGuiKey_KeypadSubtract, D_KEY_KP_SUBTRACT},
        {ImGuiKey_KeypadAdd, D_KEY_KP_ADD}, {ImGuiKey_KeypadEqual, D_KEY_KP_EQUAL},
    };

    int engineKeyOf(ImGuiKey key){
        if (key >= ImGuiKey_0 && key <= ImGuiKey_9) return D_KEY_0 + (key - ImGuiKey_0);
        if (key >= ImGuiKey_A && key <= ImGuiKey_Z) return D_KEY_A + (key - ImGuiKey_A);
        if (key >= ImGuiKey_F1 && key <= ImGuiKey_F24) return D_KEY_F1 + (key - ImGuiKey_F1);
        if (key >= ImGuiKey_Keypad0 && key <= ImGuiKey_Keypad9) return D_KEY_KP_0 + (key - ImGuiKey_Keypad0);
        for (const KeyMap& entry : KEY_MAP){
            if (entry.imgui == key)
                return entry.engine;
        }
        return D_KEY_UNKNOWN;
    }

    bool containsText(const std::string& text, const char* filter){
        if (!filter || !*filter)
            return true;
        const std::string needle = filter;
        return std::search(text.begin(), text.end(), needle.begin(), needle.end(), [](char a, char b){
            return std::tolower((unsigned char)a) == std::tolower((unsigned char)b);
        }) != text.end();
    }

    InputJson op(const char* name){
        InputJson json = InputJson::object();
        json["op"] = name;
        return json;
    }

    // the paths the picker offers, by device
    std::vector<std::string> pickerPaths(const char* device){
        std::vector<std::string> paths;
        if (std::strcmp(device, "Keyboard") == 0){
            for (int key = 0; key <= D_KEY_LAST; key++){
                const std::string name = InputControlNames::keyName(key);
                if (InputControlNames::parseKey(name) == key && name != std::to_string(key))
                    paths.push_back("keyboard/" + name);
            }
        }else if (std::strcmp(device, "Mouse") == 0){
            for (int button = 0; button <= D_MOUSE_BUTTON_LAST; button++)
                paths.push_back("mouse/" + InputControlNames::mouseButtonName(button));
            for (const char* path : {"mouse/delta", "mouse/delta/x", "mouse/delta/y", "mouse/scroll", "mouse/scroll/x", "mouse/scroll/y"})
                paths.push_back(path);
        }else if (std::strcmp(device, "Gamepad") == 0){
            for (int button = 0; button <= D_GAMEPAD_BUTTON_LAST; button++)
                paths.push_back("gamepad/" + InputControlNames::gamepadButtonName(button));
            paths.push_back("gamepad/left_stick");
            paths.push_back("gamepad/right_stick");
            for (int axis = 0; axis <= D_GAMEPAD_AXIS_LAST; axis++)
                paths.push_back("gamepad/" + InputControlNames::gamepadAxisName(axis));
        }else if (std::strcmp(device, "Touch") == 0){
            for (const char* path : {"touch/primary/press", "touch/primary/position", "touch/primary/delta", "touch/count"})
                paths.push_back(path);
        }
        return paths;
    }

    const char* modifierLabels[] = {"Ctrl", "Shift", "Alt", "Super"};
    const int modifierBits[] = {D_MODIFIER_CONTROL, D_MODIFIER_SHIFT, D_MODIFIER_ALT, D_MODIFIER_SUPER};
    const char* modifierNames[] = {"ctrl", "shift", "alt", "super"};

    const char* phaseLabel(InputActionPhase phase){
        switch (phase){
            case InputActionPhase::WAITING: return "Waiting";
            case InputActionPhase::STARTED: return "Started";
            case InputActionPhase::PERFORMED: return "Performed";
            case InputActionPhase::CANCELED: return "Canceled";
        }
        return "Waiting";
    }

    // a binding's display text in the tree: its control's name, or the composite's
    std::string bindingLabel(const InputBindingDesc& binding){
        if (binding.isComposite())
            return binding.composite == InputCompositeType::DPAD_2D ? "2D Vector" : "1D Axis";
        if (binding.path.empty())
            return "<No Binding>";
        return InputActions::getControlDisplayName(binding.path, binding.requiredMods) + "  (" + binding.path + ")";
    }

}

InputActionsWindow::InputActionsWindow(Project* project): project(project) {
}

void InputActionsWindow::setOpen(bool open){
    if (open && !windowOpen) focusRequested = true;
    windowOpen = open;
}

void InputActionsWindow::openFile(const fs::path& path){
    file = path;
    selectedMap.clear();
    selectedAction.clear();
    selectedBinding.clear();
    listeningBinding.clear();
    reload();
    setOpen(true);
    focusRequested = true;
}

void InputActionsWindow::reset(){
    file.clear();
    text.clear();
    asset = InputActionAssetDesc();
    loadError.clear();
    listeningBinding.clear();
    selectedMap.clear();
    selectedAction.clear();
    selectedBinding.clear();
}

void InputActionsWindow::reload(){
    revision = InputActionsEditCmd::getRevision();
    loadError.clear();
    std::error_code ec;
    if (file.empty() || !fs::exists(file, ec)){
        asset = InputActionAssetDesc();
        text.clear();
        if (!file.empty()) loadError = file.filename().string() + " does not exist.";
        return;
    }
    fileTime = fs::last_write_time(file, ec);
    if (!ProjectChecks::readFile(file, text)){
        loadError = "Could not read " + file.filename().string() + ".";
        return;
    }
    std::string error;
    InputActionAssetDesc parsed;
    if (!InputActionAssetDesc::parse(text, parsed, error)){
        loadError = file.filename().string() + " is not an input actions asset: " + error;
        return;
    }
    asset = std::move(parsed);
    if (!selectedMap.empty() && !asset.findMap(selectedMap)) selectedMap.clear();
    if (!selectedAction.empty() && !asset.findAction(selectedAction)) selectedAction.clear();
    if (!selectedBinding.empty() && !asset.findBinding(selectedBinding)) selectedBinding.clear();
    if (selectedMap.empty() && !asset.maps.empty()) selectedMap = asset.maps.front().name;
}

void InputActionsWindow::checkFile(){
    if (file.empty()) return;
    if (revision != InputActionsEditCmd::getRevision()){
        reload();
        return;
    }
    // an outside edit (a text editor, version control)
    const double now = ImGui::GetTime();
    if (now < nextFileCheck) return;
    nextFileCheck = now + 0.5;
    std::error_code ec;
    const fs::file_time_type time = fs::last_write_time(file, ec);
    if (!ec && time != fileTime) reload();
}

bool InputActionsWindow::commit(const InputJson& edit){
    if (file.empty() || !loadError.empty()) return false;
    InputActionAssetDesc copy = asset;
    std::string name;
    try {
        name = applyInputActionsOp(copy, edit);
    } catch (const std::exception& e) {
        status = e.what();
        return false;
    }
    status.clear();
    project->getProjectCommandHistory()->addCommandNoMerge(new InputActionsEditCmd(file, text, copy.save(), name));
    reload();
    return true;
}

void InputActionsWindow::createAsset(){
    fs::path directory = project->getAssetsPath();
    fs::path candidate = directory / "InputActions.inputactions";
    std::error_code ec;
    for (int n = 2; fs::exists(candidate, ec); n++)
        candidate = directory / ("InputActions " + std::to_string(n) + ".inputactions");
    InputActionAssetDesc fresh;
    InputActionMapDesc map;
    map.name = "Gameplay";
    fresh.maps.push_back(map);
    project->getProjectCommandHistory()->addCommandNoMerge(new InputActionsEditCmd(candidate, "", fresh.save(),
        "Create Input Actions " + candidate.filename().string(), true));
    openFile(candidate);
}

bool InputActionsWindow::visibleInFilter(const InputBindingDesc& binding) const{
    return groupFilter.empty() || binding.groups.empty() || binding.inGroup(groupFilter);
}

void InputActionsWindow::show(){
    if (!windowOpen) return;
    if (focusRequested){
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }
    ImGui::SetNextWindowSize(Theme::dpi(ImVec2(900, 520)), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(WINDOW_NAME, &windowOpen)){
        focused = false;
        ImGui::End();
        return;
    }
    focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    checkFile();
    updateListen();
    drawToolbar();

    if (file.empty()){
        ImGui::TextDisabled("Open an .inputactions asset (double-click it in the Project window), or:");
        if (ImGui::Button(ICON_FA_PLUS " New Input Actions Asset")) createAsset();
    }else if (!loadError.empty()){
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", loadError.c_str());
        if (ImGui::Button("Reload")) reload();
    }else if (tab == 1){
        drawDebug();
    }else if (ImGui::BeginTable("##InputActionsPanes", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)){
        ImGui::TableSetupColumn("Action Maps", ImGuiTableColumnFlags_WidthStretch, 0.25f);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthStretch, 0.4f);
        ImGui::TableSetupColumn("Properties", ImGuiTableColumnFlags_WidthStretch, 0.35f);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        drawMaps();
        ImGui::TableSetColumnIndex(1);
        drawActions();
        ImGui::TableSetColumnIndex(2);
        drawProperties();
        ImGui::EndTable();
    }
    if (!status.empty()){
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "%s", status.c_str());
    }
    ImGui::End();
}

void InputActionsWindow::drawToolbar(){
    if (!file.empty()){
        ImGui::TextUnformatted(file.filename().string().c_str());
        ImGui::SameLine();
    }
    const bool playing = project->isAnyScenePlaying();
    if (ImGui::RadioButton("Asset", tab == 0)) tab = 0;
    ImGui::SameLine();
    ImGui::BeginDisabled(!playing);
    if (ImGui::RadioButton("Debug", tab == 1)) tab = 1;
    ImGui::EndDisabled();
    if (!playing && tab == 1) tab = 0;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(160.0f));
    if (ImGui::BeginCombo("Group", groupFilter.empty() ? "All Control Schemes" : groupFilter.c_str())){
        if (ImGui::Selectable("All Control Schemes", groupFilter.empty())) groupFilter.clear();
        std::set<std::string> groups(std::begin(GROUPS), std::end(GROUPS));
        for (const InputActionMapDesc& map : asset.maps)
            for (const InputActionDesc& action : map.actions)
                for (const InputBindingDesc& binding : action.bindings)
                    groups.insert(binding.groups.begin(), binding.groups.end());
        for (const std::string& group : groups){
            if (ImGui::Selectable(group.c_str(), groupFilter == group)) groupFilter = group;
        }
        ImGui::EndCombo();
    }
    ImGui::Separator();
}

void InputActionsWindow::drawMaps(){
    ImGui::TextUnformatted("Action Maps");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - Theme::dpi(18.0f));
    if (ImGui::SmallButton(ICON_FA_PLUS "##AddMap")){
        std::string name = "New Map";
        for (int n = 2; asset.findMap(name); n++) name = "New Map " + std::to_string(n);
        InputJson add = op("add_map");
        add["name"] = name;
        if (commit(add)){
            selectedMap = name;
            renamingMap = name;
            std::snprintf(renameBuffer, sizeof(renameBuffer), "%s", name.c_str());
            renameFocus = true;
        }
    }
    ImGui::BeginChild("##Maps", ImVec2(0, 0), ImGuiChildFlags_None);
    std::string pendingDelete;
    for (size_t i = 0; i < asset.maps.size(); i++){
        const InputActionMapDesc& map = asset.maps[i];
        ImGui::PushID((int)i);
        if (renamingMap == map.name){
            if (renameFocus){
                ImGui::SetKeyboardFocusHere();
                renameFocus = false;
            }
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputText("##RenameMap", renameBuffer, sizeof(renameBuffer), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll) ||
                ImGui::IsItemDeactivated()){
                const std::string newName = renameBuffer;
                if (!newName.empty() && newName != map.name){
                    InputJson rename = op("rename");
                    rename["map"] = map.name;
                    rename["name"] = newName;
                    if (commit(rename)) selectedMap = newName;
                }
                renamingMap.clear();
            }
        }else{
            const std::string label = map.name + (map.enabled ? "" : "  (off)") + (map.exclusive ? "  " ICON_FA_LOCK : "");
            if (ImGui::Selectable(label.c_str(), selectedMap == map.name)){
                selectedMap = map.name;
                selectedAction.clear();
                selectedBinding.clear();
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)){
                renamingMap = map.name;
                std::snprintf(renameBuffer, sizeof(renameBuffer), "%s", map.name.c_str());
                renameFocus = true;
            }
            if (ImGui::BeginDragDropSource()){
                const int index = (int)i;
                ImGui::SetDragDropPayload("JE_INPUT_MAP", &index, sizeof(index));
                ImGui::TextUnformatted(map.name.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()){
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("JE_INPUT_MAP")){
                    const int from = *static_cast<const int*>(payload->Data);
                    if (from != (int)i){
                        InputJson move = op("move");
                        move["map"] = asset.maps[from].name;
                        move["index"] = (int)i;
                        commit(move);
                    }
                }
                ImGui::EndDragDropTarget();
            }
            if (ImGui::BeginPopupContextItem("##MapMenu")){
                if (ImGui::MenuItem("Rename")){
                    renamingMap = map.name;
                    std::snprintf(renameBuffer, sizeof(renameBuffer), "%s", map.name.c_str());
                    renameFocus = true;
                }
                if (ImGui::MenuItem("Duplicate")){
                    InputJson duplicate = op("duplicate");
                    duplicate["map"] = map.name;
                    commit(duplicate);
                }
                if (ImGui::MenuItem("Delete")) pendingDelete = map.name;
                ImGui::EndPopup();
            }
        }
        ImGui::PopID();
        if (asset.maps.size() <= i) break;   // a commit above reloaded the list
    }
    if (!pendingDelete.empty()){
        InputJson remove = op("remove");
        remove["map"] = pendingDelete;
        commit(remove);
    }
    if (focused && !selectedMap.empty() && selectedAction.empty() && selectedBinding.empty() && renamingMap.empty() &&
        ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Delete, false)){
        InputJson remove = op("remove");
        remove["map"] = selectedMap;
        commit(remove);
    }
    ImGui::EndChild();
}

void InputActionsWindow::drawActions(){
    InputActionMapDesc* map = asset.findMap(selectedMap);
    ImGui::TextUnformatted("Actions");
    if (!map){
        ImGui::TextDisabled("Select or add an action map.");
        return;
    }
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - Theme::dpi(40.0f));
    if (ImGui::SmallButton(ICON_FA_PLUS "##AddAction")){
        std::string name = "New Action";
        auto taken = [&](const std::string& candidate){
            return std::any_of(map->actions.begin(), map->actions.end(), [&](const InputActionDesc& a){ return a.name == candidate; });
        };
        for (int n = 2; taken(name); n++) name = "New Action " + std::to_string(n);
        InputJson add = op("add_action");
        add["map"] = map->name;
        add["name"] = name;
        if (commit(add)){
            selectedAction = selectedMap + "/" + name;
            selectedBinding.clear();
            renamingAction = selectedAction;
            std::snprintf(renameBuffer, sizeof(renameBuffer), "%s", name.c_str());
            renameFocus = true;
        }
        map = asset.findMap(selectedMap);
        if (!map) return;
    }
    ImGui::SameLine();
    const bool hasAction = !selectedAction.empty() && asset.findAction(selectedAction);
    ImGui::BeginDisabled(!hasAction);
    if (ImGui::SmallButton(ICON_FA_CARET_DOWN "##AddBinding")) ImGui::OpenPopup("##AddBindingMenu");
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("##AddBindingMenu")){
        const std::string target = selectedAction;
        if (ImGui::MenuItem("Add Binding")){
            InputJson add = op("add_binding");
            add["action"] = target;
            add["path"] = "keyboard/space";
            if (!groupFilter.empty()) add["groups"] = InputJson::array({groupFilter});
            commit(add);
        }
        if (ImGui::MenuItem("Add Up/Down/Left/Right Composite")){
            InputJson add = op("add_composite");
            add["action"] = target;
            add["composite"] = "dpad2d";
            add["parts"] = {{"up", "keyboard/w"}, {"down", "keyboard/s"}, {"left", "keyboard/a"}, {"right", "keyboard/d"}};
            if (!groupFilter.empty()) add["groups"] = InputJson::array({groupFilter});
            commit(add);
        }
        if (ImGui::MenuItem("Add Negative/Positive Composite")){
            InputJson add = op("add_composite");
            add["action"] = target;
            add["composite"] = "axis1d";
            add["parts"] = {{"negative", "keyboard/q"}, {"positive", "keyboard/e"}};
            if (!groupFilter.empty()) add["groups"] = InputJson::array({groupFilter});
            commit(add);
        }
        ImGui::EndPopup();
    }

    ImGui::BeginChild("##Actions", ImVec2(0, 0), ImGuiChildFlags_None);
    InputJson pending;     // one structural edit per frame, after the tree is drawn
    for (size_t a = 0; a < map->actions.size(); a++){
        const InputActionDesc& action = map->actions[a];
        const std::string path = map->name + "/" + action.name;
        ImGui::PushID(path.c_str());
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (selectedAction == path && selectedBinding.empty()) flags |= ImGuiTreeNodeFlags_Selected;
        bool open = false;
        if (renamingAction == path){
            if (renameFocus){
                ImGui::SetKeyboardFocusHere();
                renameFocus = false;
            }
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputText("##RenameAction", renameBuffer, sizeof(renameBuffer), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll) ||
                ImGui::IsItemDeactivated()){
                const std::string newName = renameBuffer;
                if (!newName.empty() && newName != action.name){
                    pending = op("rename");
                    pending["action"] = path;
                    pending["name"] = newName;
                }
                renamingAction.clear();
            }
            open = true;
        }else{
            const std::string label = action.name + "  [" + inputActionTypeName(action.type) + "]";
            open = ImGui::TreeNodeEx("##Action", flags, "%s", label.c_str());
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()){
                selectedAction = path;
                selectedBinding.clear();
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)){
                renamingAction = path;
                std::snprintf(renameBuffer, sizeof(renameBuffer), "%s", action.name.c_str());
                renameFocus = true;
            }
            if (ImGui::BeginDragDropSource()){
                const int index = (int)a;
                ImGui::SetDragDropPayload("JE_INPUT_ACTION", &index, sizeof(index));
                ImGui::TextUnformatted(action.name.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()){
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("JE_INPUT_ACTION")){
                    const int from = *static_cast<const int*>(payload->Data);
                    if (from != (int)a && from >= 0 && from < (int)map->actions.size()){
                        pending = op("move");
                        pending["action"] = map->name + "/" + map->actions[from].name;
                        pending["index"] = (int)a;
                    }
                }
                ImGui::EndDragDropTarget();
            }
            if (ImGui::BeginPopupContextItem("##ActionMenu")){
                if (ImGui::MenuItem("Rename")){
                    renamingAction = path;
                    std::snprintf(renameBuffer, sizeof(renameBuffer), "%s", action.name.c_str());
                    renameFocus = true;
                }
                if (ImGui::MenuItem("Duplicate")){
                    pending = op("duplicate");
                    pending["action"] = path;
                }
                if (ImGui::MenuItem("Delete")){
                    pending = op("remove");
                    pending["action"] = path;
                }
                ImGui::EndPopup();
            }
        }
        if (open && renamingAction != path){
            for (size_t b = 0; b < action.bindings.size(); b++){
                const InputBindingDesc& binding = action.bindings[b];
                if (!visibleInFilter(binding)) continue;
                ImGui::PushID(binding.id.c_str());
                ImGuiTreeNodeFlags bindingFlags = ImGuiTreeNodeFlags_SpanAvailWidth;
                if (binding.isComposite()) bindingFlags |= ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen;
                else bindingFlags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
                if (selectedBinding == binding.id) bindingFlags |= ImGuiTreeNodeFlags_Selected;
                const bool bindingOpen = ImGui::TreeNodeEx("##Binding", bindingFlags, "%s", bindingLabel(binding).c_str());
                if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()){
                    selectedAction = path;
                    selectedBinding = binding.id;
                }
                if (ImGui::BeginDragDropSource()){
                    const int index = (int)b;
                    ImGui::SetDragDropPayload("JE_INPUT_BINDING", &index, sizeof(index));
                    ImGui::TextUnformatted(bindingLabel(binding).c_str());
                    ImGui::EndDragDropSource();
                }
                if (ImGui::BeginDragDropTarget()){
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("JE_INPUT_BINDING")){
                        const int from = *static_cast<const int*>(payload->Data);
                        if (from != (int)b && from >= 0 && from < (int)action.bindings.size()){
                            pending = op("move");
                            pending["binding"] = action.bindings[from].id;
                            pending["index"] = (int)b;
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
                if (ImGui::BeginPopupContextItem("##BindingMenu")){
                    if (ImGui::MenuItem("Duplicate")){
                        pending = op("duplicate");
                        pending["binding"] = binding.id;
                    }
                    if (ImGui::MenuItem("Delete")){
                        pending = op("remove");
                        pending["binding"] = binding.id;
                    }
                    ImGui::EndPopup();
                }
                if (binding.isComposite() && bindingOpen){
                    for (const auto& [part, desc] : binding.parts){
                        ImGui::PushID(desc.id.c_str());
                        ImGuiTreeNodeFlags partFlags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
                        if (selectedBinding == desc.id) partFlags |= ImGuiTreeNodeFlags_Selected;
                        const std::string label = part + ": " + (desc.path.empty() ? std::string("<No Binding>") : bindingLabel(desc));
                        ImGui::TreeNodeEx("##Part", partFlags, "%s", label.c_str());
                        if (ImGui::IsItemClicked()){
                            selectedAction = path;
                            selectedBinding = desc.id;
                        }
                        ImGui::PopID();
                    }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (focused && ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Delete, false) && renamingAction.empty()){
        if (!selectedBinding.empty()){
            pending = op("remove");
            pending["binding"] = selectedBinding;
        }else if (!selectedAction.empty()){
            pending = op("remove");
            pending["action"] = selectedAction;
        }
    }
    ImGui::EndChild();
    if (!pending.is_null()){
        const bool renamed = pending["op"] == "rename";
        const std::string newPath = renamed ? selectedMap + "/" + pending["name"].get<std::string>() : std::string();
        if (commit(pending) && renamed) selectedAction = newPath;
    }
}

void InputActionsWindow::drawProperties(){
    ImGui::TextUnformatted("Properties");
    ImGui::Separator();
    ImGui::BeginChild("##Properties", ImVec2(0, 0), ImGuiChildFlags_None);
    if (!selectedBinding.empty()){
        if (const InputBindingDesc* binding = asset.findBinding(selectedBinding)){
            bool isPart = true;
            for (const InputActionMapDesc& map : asset.maps)
                for (const InputActionDesc& action : map.actions)
                    for (const InputBindingDesc& top : action.bindings)
                        if (top.id == selectedBinding) isPart = false;
            drawBindingProperties(*binding, isPart);
        }
    }else if (!selectedAction.empty()){
        if (const InputActionDesc* action = asset.findAction(selectedAction)) drawActionProperties(*action);
    }else if (const InputActionMapDesc* map = asset.findMap(selectedMap)){
        drawMapProperties(*map);
    }
    ImGui::EndChild();
}

void InputActionsWindow::drawMapProperties(const InputActionMapDesc& map){
    ImGui::Text("Action Map: %s", map.name.c_str());
    auto setValue = [&](const char* key, InputJson value){
        InputJson set = op("set");
        set["map"] = map.name;
        set["values"] = {{key, value}};
        commit(set);
    };
    int priority = map.priority;
    if (ImGui::InputInt("Priority", &priority) && priority != map.priority) setValue("priority", priority);
    ImGui::SetItemTooltip("Maps are read highest first; a control one uses is unavailable to the lower ones.");
    bool enabled = map.enabled;
    if (ImGui::Checkbox("Enabled At Start", &enabled)) setValue("enabled", enabled);
    bool consume = map.consume;
    if (ImGui::Checkbox("Consume Controls", &consume)) setValue("consume", consume);
    bool exclusive = map.exclusive;
    if (ImGui::Checkbox("Exclusive", &exclusive)) setValue("exclusive", exclusive);
    ImGui::SetItemTooltip("Enabled, it blocks every lower map (a modal menu).");
    bool ignoreUI = map.ignoreWhenUIConsumes;
    if (ImGui::Checkbox("Ignore When UI Consumes", &ignoreUI)) setValue("ignore_when_ui_consumes", ignoreUI);
}

void InputActionsWindow::drawActionProperties(const InputActionDesc& action){
    ImGui::Text("Action: %s", action.name.c_str());
    auto setValue = [&](const char* key, InputJson value){
        InputJson set = op("set");
        set["action"] = selectedAction;
        set["values"] = {{key, value}};
        commit(set);
    };
    const char* types[] = {"Button", "Axis", "Vector2"};
    int type = static_cast<int>(action.type);
    if (ImGui::Combo("Action Type", &type, types, 3)) setValue("type", inputActionTypeName(static_cast<InputActionType>(type)));
    float pressPoint = action.pressPoint;
    ImGui::SliderFloat("Press Point", &pressPoint, 0.01f, 1.0f, "%.2f");
    if (ImGui::IsItemDeactivatedAfterEdit()) setValue("press_point", pressPoint);
    float holdTime = action.holdTime;
    ImGui::DragFloat("Hold Time", &holdTime, 0.01f, 0.0f, 10.0f, "%.2f s");
    if (ImGui::IsItemDeactivatedAfterEdit()) setValue("hold_time", std::max(0.0f, holdTime));
    ImGui::SetItemTooltip("0: performed on press. Else performed after holding this long.");
    if (action.type != InputActionType::BUTTON){
        bool own = action.deadzone.has_value();
        if (ImGui::Checkbox("Override Deadzone", &own)) setValue("deadzone", own ? InputJson(InputActions::getDefaultDeadzone()) : InputJson());
        if (own){
            float deadzone = *action.deadzone;
            ImGui::SliderFloat("Deadzone", &deadzone, 0.0f, 0.95f, "%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) setValue("deadzone", deadzone);
        }
    }
}

void InputActionsWindow::drawModifiers(const char* label, const char* key, const std::string& bindingId, int mods){
    ImGui::TextUnformatted(label);
    ImGui::PushID(key);
    for (int i = 0; i < 4; i++){
        ImGui::SameLine();
        bool on = (mods & modifierBits[i]) != 0;
        if (ImGui::Checkbox(modifierLabels[i], &on)){
            InputJson names = InputJson::array();
            for (int j = 0; j < 4; j++){
                const bool set = j == i ? on : (mods & modifierBits[j]) != 0;
                if (set) names.push_back(modifierNames[j]);
            }
            InputJson set = op("set");
            set["binding"] = bindingId;
            set["values"] = {{key, names}};
            commit(set);
        }
    }
    ImGui::PopID();
}

void InputActionsWindow::drawPathField(const InputBindingDesc& binding){
    ImGui::TextUnformatted("Path");
    ImGui::SameLine();
    const std::string shown = binding.path.empty() ? std::string("<No Binding>") : binding.path;
    if (ImGui::Button((shown + "##PathButton").c_str())) ImGui::OpenPopup("##PathPicker");
    ImGui::SameLine();
    const bool listening = listeningBinding == binding.id;
    if (ImGui::Button(listening ? "Listening... (Esc)" : "Listen")){
        if (listening){
            listeningBinding.clear();
        }else{
            listeningBinding = binding.id;
            // the click that started it is not the control
            listenFrames = 2;
        }
    }
    ImGui::SetItemTooltip("The next key, mouse button or gamepad control becomes the binding's path.");
    if (!binding.path.empty())
        ImGui::TextDisabled("%s", InputActions::getControlDisplayName(binding.path, binding.requiredMods).c_str());
    drawPathPicker(binding.id);
}

void InputActionsWindow::drawPathPicker(const std::string& bindingId){
    if (!ImGui::BeginPopup("##PathPicker")) return;
    ImGui::SetNextItemWidth(Theme::dpi(220.0f));
    ImGui::InputTextWithHint("##PathFilter", "Search", pathFilter, sizeof(pathFilter));
    auto choose = [&](const std::string& path){
        InputJson set = op("set");
        set["binding"] = bindingId;
        set["values"] = {{"path", path}};
        commit(set);
        ImGui::CloseCurrentPopup();
    };
    for (const char* device : {"Keyboard", "Mouse", "Gamepad", "Touch"}){
        if (ImGui::BeginMenu(device)){
            for (const std::string& path : pickerPaths(device)){
                if (!containsText(path, pathFilter)) continue;
                const std::string label = InputActions::getControlDisplayName(path, 0) + "  (" + path + ")";
                if (ImGui::MenuItem(label.c_str())) choose(path);
            }
            ImGui::EndMenu();
        }
    }
    if (ImGui::BeginMenu("Virtual")){
        ImGui::SetNextItemWidth(Theme::dpi(160.0f));
        ImGui::InputTextWithHint("##VirtualName", "name (an on-screen control)", virtualName, sizeof(virtualName));
        ImGui::BeginDisabled(virtualName[0] == 0 || std::strchr(virtualName, '/') != nullptr);
        if (ImGui::MenuItem("Use virtual/<name>")) choose(std::string("virtual/") + virtualName);
        ImGui::EndDisabled();
        ImGui::EndMenu();
    }
    ImGui::EndPopup();
}

void InputActionsWindow::drawBindingProperties(const InputBindingDesc& binding, bool isPart){
    auto setValue = [&](const char* key, InputJson value){
        InputJson set = op("set");
        set["binding"] = binding.id;
        set["values"] = {{key, value}};
        commit(set);
    };
    ImGui::Text("%s  #%s", binding.isComposite() ? (binding.composite == InputCompositeType::DPAD_2D ? "2D Vector Composite" : "1D Axis Composite")
                                                 : (isPart ? "Composite Part" : "Binding"), binding.id.c_str());
    if (!binding.isComposite())
        drawPathField(binding);
    else{
        bool normalize = binding.normalize;
        if (binding.composite == InputCompositeType::DPAD_2D && ImGui::Checkbox("Normalize", &normalize)) setValue("normalize", normalize);
    }
    float scale = binding.scale;
    ImGui::DragFloat("Scale", &scale, 0.01f, -100.0f, 100.0f, "%.2f");
    if (ImGui::IsItemDeactivatedAfterEdit()) setValue("scale", scale);
    bool invert = binding.invert;
    if (ImGui::Checkbox("Invert", &invert)) setValue("invert", invert);
    if (!binding.isComposite()){
        bool own = binding.deadzone.has_value();
        if (ImGui::Checkbox("Override Deadzone", &own)) setValue("deadzone", own ? InputJson(InputActions::getDefaultDeadzone()) : InputJson());
        if (own){
            float deadzone = *binding.deadzone;
            ImGui::SliderFloat("Deadzone", &deadzone, 0.0f, 0.95f, "%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) setValue("deadzone", deadzone);
        }
        drawModifiers("Required Modifiers", "required_mods", binding.id, binding.requiredMods);
        drawModifiers("Forbidden Modifiers", "forbidden_mods", binding.id, binding.forbiddenMods);
    }
    if (!isPart){
        ImGui::TextUnformatted("Control Schemes");
        for (const char* group : GROUPS){
            bool on = binding.inGroup(group);
            ImGui::SameLine();
            if (ImGui::Checkbox(group, &on)){
                InputJson groups = InputJson::array();
                for (const std::string& existing : binding.groups)
                    if (existing != group) groups.push_back(existing);
                if (on) groups.push_back(group);
                setValue("groups", groups);
            }
        }
    }
}

void InputActionsWindow::updateListen(){
    if (listeningBinding.empty()) return;
    if (!asset.findBinding(listeningBinding)){
        listeningBinding.clear();
        return;
    }
    if (listenFrames > 0){
        // the pads as they are when listening starts: only a change counts
        for (int slot = 0; slot < D_INPUT_MAX_GAMEPADS; slot++){
            const int id = Input::getGamepadSlotId(slot);
            for (int b = 0; b < Input::GAMEPAD_BUTTONS; b++)
                padButtonsBefore[slot][b] = id >= 0 && Input::isGamepadButtonPressed(id, b);
            for (int axis = 0; axis < Input::GAMEPAD_AXES; axis++)
                padAxesBefore[slot][axis] = Input::getGamepadSlotAxis(slot, axis, false);
        }
        listenFrames--;
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)){
        listeningBinding.clear();
        return;
    }
    for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; key++){
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(key), false)){
            const int engineKey = engineKeyOf(static_cast<ImGuiKey>(key));
            if (engineKey != D_KEY_UNKNOWN){
                finishListen(InputControlPath{InputControlKind::KEY, engineKey}.toString());
                return;
            }
        }
    }
    for (int button = 0; button < 5; button++){
        if (ImGui::IsMouseClicked(button)){
            finishListen(InputControlPath{InputControlKind::MOUSE_BUTTON, button}.toString());
            return;
        }
    }
    const InputBindingDesc* binding = asset.findBinding(listeningBinding);
    const bool wantsStick = binding && InputControlPath::parse(binding->path).kind == InputControlKind::GAMEPAD_STICK;
    for (int slot = 0; slot < D_INPUT_MAX_GAMEPADS; slot++){
        const int id = Input::getGamepadSlotId(slot);
        if (id < 0 || !Input::isGamepadSlotConnected(slot)) continue;
        for (int b = 0; b < Input::GAMEPAD_BUTTONS; b++){
            const bool down = Input::isGamepadButtonPressed(id, b);
            if (down && !padButtonsBefore[slot][b]){
                finishListen(InputControlPath{InputControlKind::GAMEPAD_BUTTON, b}.toString());
                return;
            }
            padButtonsBefore[slot][b] = down;
        }
        for (int axis = 0; axis < Input::GAMEPAD_AXES; axis++){
            float value = Input::getGamepadSlotAxis(slot, axis, false);
            float before = padAxesBefore[slot][axis];
            if (axis == D_GAMEPAD_AXIS_LEFT_TRIGGER || axis == D_GAMEPAD_AXIS_RIGHT_TRIGGER){
                value = (value + 1.0f) * 0.5f;
                before = (before + 1.0f) * 0.5f;
            }
            padAxesBefore[slot][axis] = Input::getGamepadSlotAxis(slot, axis, false);
            if (std::fabs(value) > 0.5f && std::fabs(before) <= 0.5f){
                const bool stickAxis = axis <= D_GAMEPAD_AXIS_RIGHT_Y;
                if (wantsStick && stickAxis){
                    finishListen(axis <= D_GAMEPAD_AXIS_LEFT_Y ? "gamepad/left_stick" : "gamepad/right_stick");
                }else{
                    finishListen(InputControlPath{InputControlKind::GAMEPAD_AXIS, axis}.toString());
                }
                return;
            }
        }
    }
}

void InputActionsWindow::finishListen(const std::string& path){
    const std::string bindingId = listeningBinding;
    listeningBinding.clear();
    InputJson set = op("set");
    set["binding"] = bindingId;
    set["values"] = {{"path", path}};
    commit(set);
}

void InputActionsWindow::drawDebug(){
    if (!InputActions::isLoaded()){
        ImGui::TextDisabled("The game has no input actions loaded (Project Settings > Input, or InputActions.load).");
        return;
    }
    ImGui::Text("Playing with %s%s", InputActions::getAssetPath().c_str(), InputActions::isRebinding() ? "  (rebinding)" : "");
    if (!ImGui::BeginTable("##InputDebug", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY))
        return;
    ImGui::TableSetupColumn("Action");
    ImGui::TableSetupColumn("Value");
    ImGui::TableSetupColumn("Phase");
    ImGui::TableSetupColumn("Edges");
    ImGui::TableSetupColumn("Consumed By");
    ImGui::TableSetupColumn("Raw This Frame");
    ImGui::TableHeadersRow();
    for (const InputActions::MapState& map : InputActions::getMapStates()){
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled("%s  (priority %d%s%s)", map.name.c_str(), map.priority, map.enabled ? "" : ", off", map.blocked ? ", blocked" : "");
        for (InputAction* action : InputActions::getMap(map.name).getActions()){
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("  %s", action->getName().c_str());
            ImGui::TableSetColumnIndex(1);
            const Vector2 value = action->readVector2();
            if (action->getType() == InputActionType::VECTOR2) ImGui::Text("(%.2f, %.2f)", value.x, value.y);
            else ImGui::Text("%.2f", value.x);
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(phaseLabel(action->getPhase()));
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%s%s%s", action->isPressed() ? "held " : "", action->wasPressedThisFrame() ? "down " : "",
                        action->wasReleasedThisFrame() ? "up" : "");
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(action->getConsumedBy().c_str());
            ImGui::TableSetColumnIndex(5);
            std::string raw;
            for (const InputBinding& binding : action->getBindings()){
                std::vector<InputBinding> controls = binding.isComposite() ? binding.getParts() : std::vector<InputBinding>{binding};
                for (const InputBinding& control : controls){
                    const InputControlPath path = InputControlPath::parse(control.getPath());
                    int index = -1;
                    if (path.kind == InputControlKind::KEY) index = Input::keyControl(path.index);
                    else if (path.kind == InputControlKind::MOUSE_BUTTON) index = Input::mouseControl(path.index);
                    if (index < 0) continue;
                    const InputControlEdges edges = Input::getControlEdges(index, false);
                    if (edges.presses == 0 && edges.releases == 0) continue;
                    raw += path.toString() + (edges.presses ? " down" : "") + (edges.releases ? " up" : "") + (edges.uiPresses ? " (UI)" : "") + "  ";
                }
            }
            ImGui::TextUnformatted(raw.c_str());
        }
    }
    ImGui::EndTable();
}
