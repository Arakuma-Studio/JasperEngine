// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The Project Hub (docs/specs/S27-editor-tools.md R15-R23): an ImGui window filling the main
// window, drawn alone while it is open (App::show returns before the editor's windows, as it
// does while a project loads). Projects (recent projects: thumbnail, name, path, last opened,
// pin, search, sort; missing ones greyed with Locate... and Remove from List), New Project
// (Built-in and My Templates cards, Project Name, Location), Open (a folder with project.yaml)
// and Import (zip or Git URL, progress, log tail, Cancel), with the leftover staging folders of
// stopped copies on Projects and Import (S66). What it does to the editor goes through Actions (App:
// requestProjectChange, the loading window).

#include "hub/ProjectImporter.h"
#include "hub/RecentProjects.h"
#include "hub/StagingFolders.h"
#include "hub/TemplateCatalog.h"

#include "texture/Texture.h"

#include <array>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {

    namespace hub {
        // the plain editor's start flags (editor/main.cpp): --hub opens the hub whatever the
        // preference; --project <dir> opens that project and never the hub
        struct LaunchOptions {
            bool forceHub = false;
            std::filesystem::path project;
        };
        LaunchOptions& launchOptions();
    }

    class ProjectHubWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Project Hub";

        enum class Page : uint8_t { Projects, NewProject, Open, Import };

        struct Actions {
            // open a project (without the scenes its workspace had open)
            std::function<void(const std::filesystem::path& dir, bool withoutLastScene)> openProject;
            // make a project from a template at <location>/<folder of name> and open it
            std::function<void(const hub::ProjectTemplate& info, const std::string& name, const std::filesystem::path& location)> createProject;
            // a project is open behind the hub (Esc returns to it)
            std::function<bool()> hasProject;
        };

        void setActions(Actions actions) { this->actions = std::move(actions); }

        void open(Page page);
        void close();
        bool isOpen() const { return visible; }
        Page currentPage() const { return page; }

        // the whole window (App::show while open)
        void show();

        // ---- the parts the tests and MCP reach
        // the folder a new project goes to by default: the last parent used, else
        // ~/JasperEngine Projects
        static std::filesystem::path defaultLocation();
        // why Create is disabled for <location>/<name> ("" when it is not)
        static std::string createProblem(const std::string& name, const std::filesystem::path& location);
        hub::ProjectImporter& importer() { return import; }
        // what a sweep of staging folders looks at (S66 R5): the folders projects are known to be
        // in, and the user templates' staging folder
        static hub::staging::Scope leftoverScope();
        // Sweeps off the main thread, so start-up never waits for a removal: with `knownParents`
        // the whole scope, else the journal alone (a start without the hub: one folder listing)
        void startSweep(bool knownParents);
        // every loop iteration: true when a sweep ended and the window has something new to show
        bool pollSweep();
        const std::vector<hub::staging::Leftover>& getLeftovers() const { return leftovers; }

    private:
        struct Thumbnail {
            std::future<std::shared_ptr<TextureData>> pending;
            std::unique_ptr<Texture> texture;
            bool failed = false;
        };

        Actions actions;
        bool visible = false;
        Page page = Page::Projects;

        // Projects
        std::array<char, 128> search{};
        hub::recent::Sort sort = hub::recent::Sort::LastOpened;
        std::string projectsMessage;

        // New Project
        std::vector<hub::ProjectTemplate> templateList;
        bool templatesLoaded = false;
        std::string selectedTemplate = "3d";
        int pixelArtBase = 1;   // S42 R21: the Base Resolution of a pixel-art template (320 x 180); past the list: Custom
        int customBaseWidth = 320, customBaseHeight = 180;
        std::array<char, 128> projectName{};
        std::array<char, 512> location{};
        std::string createMessage;

        // Open
        std::string openMessage;

        // Import
        hub::ProjectImporter import;
        std::array<char, 1024> importSource{};
        std::array<char, 512> importLocation{};
        bool importOpened = false;

        // Leftover staging folders
        struct SweepDone {
            std::vector<hub::staging::Leftover> leftovers;
            std::string removalError;
        };
        std::future<SweepDone> sweepTask;
        bool sweepAgain = false;
        std::vector<std::filesystem::path> removalRequests;
        std::vector<hub::staging::Leftover> leftovers;
        std::filesystem::path removalToConfirm;
        std::string leftoverMessage;

        // thumbnails by file (projects and templates), decoded off the main thread (R38)
        std::map<std::string, Thumbnail> thumbnails;

        Texture* thumbnail(const std::string& key, const std::filesystem::path& file);
        Texture* thumbnailFromBytes(const std::string& key, const std::vector<uint8_t>& png);

        void drawNavigation();
        void drawProjects();
        void drawNewProject();
        void drawOpen();
        void drawImport();
        void drawLeftovers();
        void openFolder(const std::filesystem::path& dir);
    };

}
