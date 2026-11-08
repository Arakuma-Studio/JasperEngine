// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S82-title-designer.md R2, R19, R30-R32, R40, R67, R70, R84, D15)
// - Purpose: what the Title Designer remembers that is not the document (R84: S82 adds no member
//   to the `.titleart`):
//   * TitleDesignerSettings, per user (settings.yaml `title_designer`): region sizes and collapses,
//     section open states per node kind, the preview budget, Quality, Stats, snap increments;
//   * TitleDesignerWorkspace, per user and project (`.jasperengine/user/title-designer.yaml`, not
//     committed, beside the editor's workspace file): the recent documents and, per document GUID,
//     its view (twirls, backdrop, device, guides, zoom and pan, Preview As, Library tab and filters).
// - Old files load: a missing block or key takes its default; unknown keys are kept on save.
// - Thread-safety: main thread.

#pragma once

#include "TitleDesignerTypes.h"

#include "yaml-cpp/yaml.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace jasperengine::editor::titledesigner {

    enum class Quality : uint8_t { Auto, Draft, Full };
    const char* qualityName(Quality quality);
    bool qualityFromName(const std::string& name, Quality& out);

    struct TitleDesignerSettings {
        float layersWidth = 260.0f;
        float propertiesWidth = 340.0f;
        float libraryHeight = 196.0f;
        bool layersCollapsed = false;
        bool propertiesCollapsed = false;
        bool libraryCollapsed = false;
        std::map<std::string, bool> sections;   // "<node kind>/<section id>" -> open
        float previewBudgetMs = 4.0f;           // Preferences > Title Designer > Preview Budget, 1..16
        bool budgetSet = false;                 // the user chose it (the software-renderer default stays out of the way)
        Quality quality = Quality::Auto;
        bool qualitySet = false;
        bool stats = false;
        float snapMove = 10.0f, snapRotate = 15.0f, snapScale = 0.1f;
        bool softwareNoticeShown = false;
        bool windowOpen = false;                // reopened as it was (R64)

        YAML::Node encode() const;
        static TitleDesignerSettings decode(const YAML::Node& node);
        // AppSettings' `title_designer` node
        static TitleDesignerSettings load();
        void save() const;
    };

    struct Guide {
        bool vertical = true;                   // a vertical line at x, else horizontal at y (document space)
        double position = 0.0;
        friend bool operator==(const Guide& a, const Guide& b) { return a.vertical == b.vertical && a.position == b.position; }
    };

    // the backdrop choice (R30)
    struct BackdropChoice {
        std::string kind = "checker";           // checker, black, white, gray, color, image, game_scene, in_context
        std::string color = "#808080";
        std::string image;                      // a Texture asset path
        std::string scene;                      // a scene path (Game Scene)
        bool operator==(const BackdropChoice& o) const { return kind == o.kind && color == o.color && image == o.image && scene == o.scene; }
    };

    // the Device view (R31)
    struct DeviceChoice {
        std::string profile;                    // "" = None; a device profile id, or "res:<name>" (a Game view resolution)
        std::string orientation = "landscape_left";
        double displayHeight = 25.0;            // % of the screen height, or px when displayHeightPx
        bool displayHeightPx = false;
        int anchor = 4;                         // 0..8, row-major from top-left (4 = centre)
        bool operator==(const DeviceChoice& o) const {
            return profile == o.profile && orientation == o.orientation && displayHeight == o.displayHeight &&
                   displayHeightPx == o.displayHeightPx && anchor == o.anchor;
        }
    };

    struct DocumentView {
        std::map<std::string, bool> twirls;     // layer id (or "<id>/parts", "<id>/effects") -> open
        BackdropChoice backdrop;
        DeviceChoice device;
        std::vector<Guide> guides;
        bool rulers = true, showGuides = true, safeAreas = true, pixelGrid = false, effectReach = false;
        bool snap = true;
        bool fit = true;                        // the zoom follows the window (Fit)
        float zoom = 1.0f;
        double panX = 0.0, panY = 0.0;          // the document point at the canvas centre
        std::string previewAs = "realtime";
        std::string libraryTab = "looks";
        std::string libraryFamily;
        std::string libraryQuery;
        std::string libraryDimension = "all";   // all, 2d, 3d
        std::string tileSize = "m";             // s, m, l
        bool applyFont = false;                 // S84 R42 (per user)
        bool keepColorScheme = true;
        std::string colorSchemeScope;           // "" = by selection (R45), "selected", "whole"
        std::vector<std::string> selection;     // node paths, restored on reopen (R64)

        YAML::Node encode() const;
        static DocumentView decode(const YAML::Node& node);
    };

    class TitleDesignerWorkspace {
    public:
        static constexpr size_t MAX_RECENT = 10;

        void load(const std::filesystem::path& projectPath);
        bool save() const;
        void clear();

        // the 10 most recent documents (assets-relative paths), newest first
        const std::vector<std::string>& recent() const { return recentDocs; }
        void touch(const std::string& assetPath);
        void forget(const std::string& assetPath);
        // the view of a document (made with defaults on first use)
        DocumentView& view(const std::string& guid);
        bool hasView(const std::string& guid) const { return views.count(guid) > 0; }
        const std::filesystem::path& file() const { return path; }

    private:
        std::filesystem::path path;
        std::vector<std::string> recentDocs;
        std::map<std::string, DocumentView> views;
        YAML::Node unknown;
    };

}
