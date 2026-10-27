// The editor code check-hardcoded-shortcuts.py must refuse: one problem per "// expect" line.
void menu() {
    if (ImGui::MenuItem("Save", menuShortcut(MENU_SAVE, "Ctrl+S"))) save();   // expect
    if (ImGui::MenuItem("Duplicate", "Ctrl+D")) duplicate();                   // expect
}
void keys() {
    if (ImGui::IsKeyPressed(ImGuiKey_Q)) setTool(Tool::Hand);                  // expect
    if (ImGui::IsKeyPressed(ImGuiMod_Ctrl | ImGuiKey_Z, false)) undo();        // expect
    if (ImGui::IsKeyPressed(ImGuiKey_Delete)) remove();                        // expect
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_K)) openPalette();            // expect
}
