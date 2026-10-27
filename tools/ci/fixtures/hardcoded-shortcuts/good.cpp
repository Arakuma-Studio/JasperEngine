// Editor code check-hardcoded-shortcuts.py accepts: catalog texts, local navigation keys and a
// justified exception.
void menu(CommandCatalog& catalog) {
    if (ImGui::MenuItem("Save", catalog.menuShortcutText("file.save").c_str())) save();
    if (ImGui::MenuItem("About")) about();
}
void dialog() {
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) close();
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) accept();
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) next();
    // a comment may say IsKeyPressed(ImGuiKey_Q) or menuShortcut(x, "Ctrl+S")
    if (ImGui::IsKeyPressed(ImGuiKey_F)) frame();   // shortcut-lint: allow a fixture's justified exception
}
