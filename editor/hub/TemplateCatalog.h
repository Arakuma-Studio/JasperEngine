// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Project templates (docs/specs/S27-editor-tools.md R18-R20, §6.5): the built-in ones embedded
// in the editor from editor/resources/templates/<id>/ (project_templates.h, made by CMake) and
// the user's in <settings dir>/templates/<id>/. A template is a project the editor saved plus
// template.json ({schemaVersion, id, name, description, category, order, accentColor, thumbnail,
// startScene}) and its thumbnail (480x270 PNG). The fields are OpenEngine's manifest entries'
// (Apps/Editor/ProjectTemplates/manifest.json), one file per template.
//
// "Template" here means a starter project; the exported projects' CMake/player scaffolding
// (engine/project, S01) is something else.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor::hub {

    struct ProjectTemplate {
        static constexpr int kSchemaVersion = 1;

        std::string id;
        std::string name;
        std::string description;
        std::string category;
        int order = 0;
        std::string accentColor = "#5A67A7";
        std::string thumbnail = "thumbnail.png";
        std::string startScene = "Main";
        // a pixel-art template (S42 R21): the New Project form asks for its base resolution
        bool pixelArt = false;
        // the New Project form's Base Resolution for a pixel-art template (0: the template's own),
        // written into the canvas and the window (4x) before the first open; never saved
        int baseWidth = 0, baseHeight = 0;
        bool builtIn = false;
        std::filesystem::path directory;   // a user template's folder
        std::vector<uint8_t> thumbnailPng;
    };

    namespace templates {

        constexpr const char* kInfoFile = "template.json";

        std::filesystem::path userDirectory(const std::filesystem::path& settingsDirectory);

        bool parseInfo(const std::string& json, ProjectTemplate& info, std::string& error);
        std::string infoJson(const ProjectTemplate& info);

        // embedded first (by order), then the user's (by name); unreadable user templates are
        // skipped with a line in `errors`
        std::vector<ProjectTemplate> all(const std::filesystem::path& userDir, std::vector<std::string>* errors = nullptr);
        const ProjectTemplate* find(const std::vector<ProjectTemplate>& list, const std::string& id);

        // Copies the template's files into `target` (absent or an empty folder), without
        // template.json and the thumbnail. False with `error` (and nothing left behind) on a
        // failure or a cancel.
        bool materialize(const ProjectTemplate& info, const std::filesystem::path& target, std::string& error,
                         const std::atomic<bool>* cancel = nullptr);

        // Saves a project as a user template <userDir>/<slug of name>: its files without
        // .jasperengine/, build/, Recordings/, .trash/ and .git/ (and the editor's per-user
        // settings), template.json and `thumbnailPng`. `id` is the folder's id.
        bool saveProjectAsTemplate(const std::filesystem::path& project, const std::filesystem::path& userDir, const std::string& name,
                                   const std::string& description, const std::string& startScene, const std::vector<uint8_t>& thumbnailPng,
                                   std::string& id, std::string& error);

        // a project's top-level folder or file a template never takes
        bool excludedFromTemplate(const std::string& topLevelName);

    }

}
