// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Window layouts (S25 R15-R19, §6.4, §6.6): the pure part, tested without a GPU. A layout is
// the dear ImGui ini text of the editor's windows (docking, sizes) without the document
// windows (Scene tabs, scripts, images: ###<id> windows, which differ per project), the UI scale
// it was made at, and which panels are open:
//
//   # <configDir>/layouts/animation.layout.yaml  (a preset, user-wide)
//   layoutVersion: 1
//   name: Animation
//   uiScale: 1.0
//   panels: {Hierarchy: true, Inspector: true, Animation: true}
//   ini: |
//     [Window][Inspector] ...
//
// <project>/.jasperengine/user/layout.yaml is the project's working layout in the same format,
// with `activePreset:` (the preset it was switched to, empty when none). Save Layout to File
// writes a preset as such, or every preset as a bundle {layoutBundleVersion: 1, presets: [...]}.
// The OpenEngine layout presets (EditorApplication_LayoutPresets.cpp) gave the UX rules:
// unique names on import, a prompt before leaving a modified layout, delete keeps the layout.

#include <map>
#include <string>
#include <vector>

namespace jasperengine::editor {

    struct LayoutPreset {
        std::string name;
        float uiScale = 1.0f;
        std::map<std::string, bool> panels;   // window name -> open
        std::string ini;                      // filtered (filterDocumentWindows)
        std::string activePreset;             // the working layout's only
    };

    namespace LayoutPresets {

        constexpr const char* DEFAULT_NAME = "Default";
        constexpr int currentVersion = 1;
        constexpr int bundleVersion = 1;
        constexpr const char* FILE_SUFFIX = ".layout.yaml";

        // The ini without the sections of document windows ([Window][###...]: ###Scene<id>,
        // ###<script path>, ###ImageViewer:<path>, ###PrefabMode), line endings normalised
        std::string filterDocumentWindows(const std::string& ini);
        // name, or "name (2)", "name (3)"... the first one existing (case-insensitively) lacks
        std::string uniqueName(const std::string& name, const std::vector<std::string>& existing);
        // A file name for a preset: lower case, runs of other characters as one '-'
        // ("My Layout" -> "my-layout"; "layout" when nothing is left)
        std::string slug(const std::string& name);
        // 1-64 characters after trimming, no control characters, not "Default"
        bool isValidName(const std::string& name, std::string* why = nullptr);

        std::string serialize(const LayoutPreset& preset, bool working);
        bool parse(const std::string& text, LayoutPreset& preset, std::string* error = nullptr);
        std::string serializeBundle(const std::vector<LayoutPreset>& presets);
        // A preset file or a bundle: the presets it holds; false with *error when neither
        bool parseFile(const std::string& text, std::vector<LayoutPreset>& presets, std::string* error = nullptr);

    }

}
