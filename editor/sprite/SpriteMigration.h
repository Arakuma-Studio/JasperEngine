// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Legacy inline sprite frames (docs/specs/S45-sprite-atlas.md R21, R22): finding the sprites that
// still carry their own frame rectangles, and converting them to texture sheets. The conversion
// moves a sprite's rects into its texture's Sprite Mode (multiple, names and order kept) and makes
// the sprite (and every other sprite of the project using that texture with the same rect list:
// open and closed scenes, prefabs, found through the dependency index) a sheet sprite showing the
// same frame by index, so clip keys and Sprite Animations keep working. One project undo step
// restores the meta, the open documents and the files.
// `check --sprites` reads the scenes, prefabs and atlases without an editor (scanProject).

#pragma once

#include "pixelart/PixelProblem.h"

#include "asset/AssetFileWrite.h"
#include "command/Command.h"
#include "ecs/Entity.h"
#include "sprite/SpriteSheetSources.h"
#include "util/SpriteFrameData.h"

#include "yaml-cpp/yaml.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::sprite {

    struct LegacySprite {
        std::string owner;            // the scene or prefab, project-relative
        uint64_t entity = 0;          // its file entity id (the loaded entity in an open document)
        std::string entityPath;       // "Player/Body"
        std::string texture;          // asset-relative
        std::vector<SpriteFrameData> frames;
        int shown = 0;                // the frame its texture rect shows (-1: none of them)
        bool hasShownRect = false;    // the rect it shows, in pixels (kept as a frame when it is none of them)
        Rect shownRect;
        bool open = false;            // in an open scene, prefab stage or loaded prefab
    };

    // The frames as pixel rects (normalized ones scaled by the texture's size)
    std::vector<SpriteFrameData> legacyPixelFrames(const std::vector<SpriteFrameData>& frames, int textureWidth, int textureHeight);
    bool sameRects(const std::vector<SpriteFrameData>& a, const std::vector<SpriteFrameData>& b);
    // the texture sheet holding these frames (mode multiple, names kept; unnamed: "<stem>_<i>"; a
    // name used twice gets " 2", " 3"... as the sheet's frames do)
    TextureSheetSettings sheetFromFrames(const std::string& texture, const std::vector<SpriteFrameData>& pixelFrames);

    // the legacy sprites of one scene or prefab file (any thread, no editor)
    // (assetsRoot: where texture sizes are read for normalized rects; empty: not read)
    std::vector<LegacySprite> scanSceneFile(const std::filesystem::path& file, const std::string& owner, const std::filesystem::path& assetsRoot = {});

    // what `check --sprites` reports
    struct SpriteCheckReport {
        std::vector<LegacySprite> legacy;
        std::vector<std::pair<std::string, pixelart::PixelProblem>> atlasErrors;     // atlas, problem
        std::vector<std::pair<std::string, pixelart::PixelProblem>> atlasWarnings;
        int atlases = 0, files = 0;
    };
    SpriteCheckReport scanProject(const std::filesystem::path& projectRoot, const std::filesystem::path& assetsRoot);

    // The editor's view: open documents from memory, closed ones from their files
    std::vector<LegacySprite> findLegacySprites(Project& project);

    struct ConvertPlan {
        std::string texture;                  // asset-relative
        TextureSheetSettings sheet;
        std::vector<std::string> frameNames;  // the sheet's frame names, in order
        std::vector<LegacySprite> targets;    // the sprites converted (the one asked first)
        std::vector<int> targetFrames;        // the frame each target shows
        std::vector<std::string> warnings;    // e.g. a shown rect the kept sheet does not have
    };
    // The plan for one sprite (`all_users`: every sprite of the texture with the same rects);
    // false with `sheet_conflict` when the texture already has another sheet
    bool planConversion(Project& project, const LegacySprite& sprite, bool allUsers, ConvertPlan& plan, pixelart::PixelProblem& problem);
    // a loaded sprite's description (scene id, entity) for planConversion
    bool legacySpriteOf(Project& project, uint32_t sceneId, Entity entity, LegacySprite& out, pixelart::PixelProblem& problem);

    struct ConvertReport {
        std::string error;
        std::vector<std::string> filesWritten;
        int converted = 0;
    };

    // R21's one undo step, on the project history
    class ConvertInlineFramesCmd : public Command {
    public:
        ConvertInlineFramesCmd(Project* project, ConvertPlan plan, std::shared_ptr<ConvertReport> report = nullptr);
        ~ConvertInlineFramesCmd() override;
        bool execute() override;
        void undo() override;
        bool affectsStructure() const override { return false; }
        std::string describe() const override;
        const char* typeName() const override { return "ConvertInlineFramesCmd"; }

    private:
        struct OwnerUndo {
            std::string owner;
            bool closedFile = false;
            std::string bytes;                // a closed file's bytes before
            assetio::ReadStamp written;       // the file as the conversion wrote it
            bool wasModified = false;
            std::vector<std::pair<Entity, YAML::Node>> sprites;   // each SpriteComponent before (file or document form)
        };
        Project* project;
        ConvertPlan plan;
        std::shared_ptr<ConvertReport> report;
        std::unique_ptr<Command> meta;
        std::vector<OwnerUndo> undoData;

        void revert();
        // the sprites put back into a file saved since the conversion (its other edits kept)
        bool restoreInFile(const OwnerUndo& entry, std::string& error);
    };

}
