// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The editor's side of 2D skinning (docs/specs/S94-rendering-anim-gaps.md F49): sprite skins as a
// source asset's metadata, and the authoring operations the Skinning Editor, the Sprite Skin
// Inspector and edit_sprite_skin share.
//
// - Storage: `import.spriteSkin` in the meta of the sheet's source (a texture, a PSD/PSB in
//   Layers mode): {version: 1, sprites: {<frame name>: <skin>}}, each skin in its frame's source
//   pixels (SpriteSheetIO's skin form: bones, vertices, indices, weights). The sheet builders
//   attach each skin to the frame of its name, so the runtime record carries it (one rig shared by
//   every Sprite of that frame).
// - Operations are pure functions of a SpriteSkinGeometry; a caller writes the result back with
//   one SetAssetMetaCmd (one undo step).
// - Thread-safety: pure functions; readSpriteSkins reads a file.

#pragma once

#include "pixelart/PixelImage.h"

#include "ecs/Entity.h"
#include "math/Vector2.h"
#include "sprite/SpriteFrameGeometry.h"
#include "sprite/SpriteSheet.h"

#include "yaml-cpp/yaml.h"

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine {
    class Scene;
}

namespace jasperengine::editor {
    class Command;
    class Project;
}

namespace jasperengine::editor::sprite {

    constexpr const char* kSpriteSkinKey = "import.spriteSkin";

    // the skins of one source, by frame name
    struct SpriteSkinSet {
        static constexpr int kVersion = 1;
        std::map<std::string, std::shared_ptr<const SpriteSkinGeometry>> sprites;

        bool empty() const { return sprites.empty(); }
        const SpriteSkinGeometry* find(const std::string& frame) const;
    };

    // false for a newer version or a value that is not a map (`error` says which); a bad sprite
    // entry is left out with a problem in `problems`
    bool skinSetFromYaml(const YAML::Node& node, SpriteSkinSet& out, std::string& error, std::vector<std::string>* problems = nullptr);
    YAML::Node skinSetToYaml(const SpriteSkinSet& set);
    // the source's skins (its meta), empty when it has none
    SpriteSkinSet readSpriteSkins(const std::filesystem::path& absoluteSource, std::vector<std::string>* problems = nullptr);
    // gives each frame of the sheet its skin by name (frames without one keep none)
    void applySpriteSkins(SpriteSheetData& sheet, const SpriteSkinSet& set);
    // a stable text of the set (the sheet caches key on it)
    std::string skinSetKey(const SpriteSkinSet& set);

    // ---- authoring

    // Auto Geometry: a triangle grid of `cellSize` source pixels over the frame, kept where a cell
    // holds a pixel with alpha above `alphaTolerance`, so the mesh follows the sprite's outline to
    // within a cell; vertices land on the frame's packed pixels. Keeps the skin's bones; weights
    // are cleared (Auto Weights or set_weights fills them).
    struct AutoGeometrySettings {
        int cellSize = 8;           // pixels, 1..256: the mesh density
        int alphaTolerance = 0;     // 0..254: pixels at or below it count as empty
    };
    // `page` is the frame's page as RGBA; false (with `error`) when the frame shows no opaque pixel
    bool autoGeometry(const pixelart::PixelImage& page, const SpriteFrame& frame, const AutoGeometrySettings& settings,
                      SpriteSkinGeometry& skin, std::string& error);

    // Auto Weights: each vertex weighted by its distance to each bone's segment (inverse square,
    // the nearest four kept), normalized; false without bones or vertices
    bool autoWeights(SpriteSkinGeometry& skin, std::string& error);

    // Bones: a new bone (a unique name, an earlier parent or -1); removing one hands its weights to
    // its parent (the first bone when it had none) and its children to its parent
    bool addBone(SpriteSkinGeometry& skin, const SpriteBone& bone, std::string& error);
    bool removeBone(SpriteSkinGeometry& skin, const std::string& name, std::string& error);
    int findBone(const SpriteSkinGeometry& skin, const std::string& name);

    // set_weights: these vertices (all when empty) get `weight` of `bone`, the rest of each vertex
    // scaled to fill 1 - weight (1: bound to the bone at 100%)
    bool setWeights(SpriteSkinGeometry& skin, const std::vector<int>& vertices, int bone, float weight, std::string& error);

    // the Weight Brush: within `radius` pixels of `center`, add (or with `subtract`, remove)
    // `strength` x falloff of `bone`, renormalized; Smooth averages each vertex with its neighbours.
    // The vertices it changed, for the caller's preview.
    enum class WeightBrushMode { Add, Subtract, Smooth };
    std::vector<int> brushWeights(SpriteSkinGeometry& skin, Vector2 center, float radius, int bone, float strength, WeightBrushMode mode);

    // the vertices' weights as a list of {bone index: weight} (absent influences omitted)
    std::vector<std::map<int, float>> weightTable(const SpriteSkinGeometry& skin);

    // ---- a Sprite in a scene (the Skinning Editor, the Inspector and edit_sprite_skin)

    // A Sprite whose skin is edited: its sheet, the frame it shows and the source whose meta holds
    // the skin. A legacy Sprite of a plain texture is converted on the first edit (`convert`):
    // its sheet becomes the texture, frame 0.
    struct SkinTarget {
        Project* project = nullptr;
        uint32_t sceneId = 0;
        Entity entity = NULL_ENTITY;
        std::shared_ptr<const SpriteSheetData> sheet;
        int frame = 0;
        std::string source;                 // asset-relative
        std::filesystem::path absoluteSource;
        bool convert = false;

        const SpriteFrame& frameData() const { return sheet->frames[static_cast<size_t>(frame)]; }
    };
    // false with an error naming the fix (no Sprite, a sheet that cannot hold a skin, no frames yet)
    bool resolveSkinTarget(Project* project, uint32_t sceneId, Entity entity, SkinTarget& out, std::string& error);
    // the frame's stored skin (the meta), empty when it has none
    SpriteSkinGeometry storedSkin(const SkinTarget& target);
    // the meta write of the frame's new skin (an empty one removes it), one project undo step
    Command* skinMetaCommand(const SkinTarget& target, const SpriteSkinGeometry& skin);
    // what MeshSystem builds the Sprite's frame with (its size, pivot, flip, Pixels Per Unit)
    SpriteFrameGeometryInput skinGeometryInput(Scene* scene, Entity entity, const SpriteSheetData& sheet, const SpriteFrame& frame);
    // the frame's page as RGBA (Auto Geometry reads its alpha; the Skinning Editor draws it)
    std::optional<pixelart::PixelImage> loadFramePage(Project* project, const SpriteSheetData& sheet, const SpriteFrame& frame, std::string* error);

    // Runs one scene command (the caller's undo step groups them); false when it failed
    using SkinRunCommand = std::function<bool(Command*)>;
    // The Sprite Skin bound to the skin's bones: a GameObject per bone without one, made under
    // its parent bone's (the Sprite for a root) at the rest pose, a bone GameObject whose bone is
    // gone deleted, the component added when missing, its Root Bone and Bones set. `bones` gets
    // the bound list.
    bool bindSkinBones(const SkinTarget& target, const SpriteSkinGeometry& skin, const std::vector<std::string>& removedBones,
                       const SkinRunCommand& run, std::string& error, std::vector<Entity>* bones = nullptr);

    // A skin edit as one undo step of the timeline: the meta (the project's history) and, when
    // `bind` and the skin has bones (or bones went), bindSkinBones in the scene's history, linked.
    // Everything is taken back when a part fails. `changed` says whether anything was written.
    bool applySkinEdit(const SkinTarget& target, const SpriteSkinGeometry& skin, const std::vector<std::string>& removedBones, bool bind,
                       std::string& error, std::vector<Entity>* bones = nullptr, bool* changed = nullptr);

}
