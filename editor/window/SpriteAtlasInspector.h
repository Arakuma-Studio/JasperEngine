// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Sprite Atlas inspector (docs/specs/S45-sprite-atlas.md R29, R32): Objects for Packing
// (files and folders, drop target, +/-, Exclude), Include in Build, Packing (Max Texture Size with
// platform tabs and overrides, Padding, Extrude Edges, Trim, Keep Members Together, Power of Two,
// Square), Texture (Compression, Filter), Pack Preview, Apply / Revert (one project undo step);
// the page preview (page tabs, integer zoom and Fit, checkerboard, frame outlines, trim and
// extrusion overlay, hover tooltip, click selects the member in the Project window,
// double-click opens it), the frame list (search name / member: / page:, sorts, thumbnails),
// the stats line, waste per page and the worst members, warnings with a Fix button.
// SheetPreviewTextures: page images uploaded for ImGui (the Sprite inspector's frame picker too).

#pragma once

#include "pixelart/PixelImage.h"
#include "sprite/SpriteAtlasBuilder.h"
#include "sprite/SpriteAtlasDocument.h"

#include "jobs/Task.h"
#include "texture/Texture.h"

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    // Image files uploaded once (by path and modification time) for ImGui, nearest-filtered
    class SheetPreviewTextures {
    public:
        struct Image {
            Texture texture;
            int width = 0, height = 0;
            std::filesystem::file_time_type time;
            bool valid() const { return width > 0 && !texture.empty(); }
        };
        ~SheetPreviewTextures();
        // a file (absolute, or relative to the assets folder); null when it does not read
        Image* get(Project* project, const std::string& path);
        Image* fromImage(const std::string& key, const pixelart::PixelImage& image);
        void clear();

    private:
        std::map<std::string, Image> images;
    };

    class SpriteAtlasInspector {
    public:
        explicit SpriteAtlasInspector(Project* project);
        ~SpriteAtlasInspector();

        // Draws the selected .spriteatlas (project-relative paths); false when the selection is not one
        bool draw(const std::vector<std::filesystem::path>& selection);
        bool hasPendingEdits() const { return dirty; }

    private:
        Project* project;
        std::string atlasPath;                  // asset-relative
        std::filesystem::path absolute;
        std::filesystem::file_time_type fileTime;
        std::optional<sprite::SpriteAtlasDocument> stored;
        sprite::SpriteAtlasDocument working;
        pixelart::PixelProblem readError;
        bool dirty = false;
        int platformTab = 0;                    // 0 Default, then kAtlasPlatforms

        // the preview: the editor's build, or a Pack Preview in memory
        std::shared_ptr<const sprite::AtlasBuildResult> preview;
        bool previewInMemory = false;
        std::optional<jobs::Task<std::shared_ptr<sprite::AtlasBuildResult>>> packing;
        int page = 0;
        int zoom = 0;                           // 0: Fit
        int selectedFrame = -1;
        char search[128] = "";
        int sortBy = 0;                         // name, size, page, waste
        SheetPreviewTextures textures;

        void select(const std::filesystem::path& selection);
        void apply();
        void revert();
        void startPreview();
        void drawMembers();
        void drawSettings();
        void drawWarnings(const sprite::AtlasBuildResult* result);
        void drawPreview(const sprite::AtlasBuildResult& result);
        void drawFrameList(const sprite::AtlasBuildResult& result);
        void drawStats(const sprite::AtlasBuildResult& result);
        SheetPreviewTextures::Image* pageImage(const sprite::AtlasBuildResult& result, int index);
        void openMember(const std::string& member, bool open);
    };

}
