// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A Sprite set up from a sprite sheet (docs/specs/S44-aseprite-import.md R33, R36): the sheet and
// its frame on the Sprite, and for an imported .aseprite file an Animation Player listing the
// tag clips. One code path for set_sprite_frames, the Scene view / Hierarchy drop and the
// Inspector, so an agent and a person get the same GameObject.

#pragma once

#include "pixelart/aseprite/AsepriteImporter.h"

#include "ecs/Entity.h"
#include "math/Vector3.h"
#include "sprite/SpriteSheet.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class Command;
    class Project;
}

namespace jasperengine::editor::pixelart::aseprite {

    struct SheetSpriteRequest {
        std::string sheet;         // asset-relative: an .aseprite file or a texture
        int frame = 0;             // used when frameName is empty
        std::string frameName;     // a frame found by name
        bool clips = true;         // .aseprite: the Animation Player lists the tag clips
        bool resize = true;        // width = height = 0: the frame's native size
    };

    struct SheetSpriteResult {
        std::shared_ptr<const SpriteSheetData> sheet;
        std::shared_ptr<const ImportRecord> record;   // .aseprite only
        int frame = 0;
        std::string frameName;
        bool addedPlayer = false;
        std::vector<std::string> clips;   // the player's entries for the tags ("<file>#<tag>")
        std::string defaultClip;          // set when the call set it
    };

    // The import of an .aseprite file (asset-relative) when it is there and none is running (main
    // thread, never waits: a missing one is started); null with an import_failed problem otherwise.
    // Callers that must wait use AsepriteImporter::whenImported or an MCP call's waitUntil.
    std::shared_ptr<const ImportRecord> importedRecord(const std::string& assetPath, PixelProblem& problem);

    // The tag clip a player starts with: a tag named "idle" (any case), else the first
    std::string defaultTagClip(const std::vector<std::string>& names);

    // The commands that make `entity`'s Sprite show request.sheet at its frame, and (clips, an
    // .aseprite sheet) give it an Animation Player listing the tag clips: entries already listed are
    // kept, a new player starts its Default Clip (defaultTagClip) automatically. Nothing is
    // executed; false + a typed problem (not_a_sheet, unknown_frame, import_failed) when the sheet or
    // the frame is not there.
    bool sheetSpriteCommands(Project* project, uint32_t sceneId, Entity entity, const SheetSpriteRequest& request,
                             std::vector<Command*>& commands, SheetSpriteResult& result, PixelProblem& problem);

    // Create Colliders from Slices (R17): one 2D collider per slice of the Sprite's sheet that no
    // collider follows yet, on its Rigidbody 2D (added, static, when it has none); a slice whose user
    // properties have sensor: true makes an Is Trigger collider. `names`: the slices added.
    bool sliceColliderCommands(Project* project, uint32_t sceneId, Entity entity, std::vector<Command*>& commands,
                               std::vector<std::string>& names, PixelProblem& problem);

    // The drop of an .aseprite file on the Scene view or the Hierarchy (R33), and create_entity
    // {asset}: a GameObject named after the file with a Sprite showing frame 0 (pivot: the
    // frame's), its Animation Player listing the tag clips (Default Clip idle, else the first;
    // Play Automatically), the sprite properties applied, and in layers_as_sprites mode one Sprite
    // child per layer (the root then holds only the player); one undo step. NULL_ENTITY + problem
    // when it fails (nothing left behind).
    Entity createSpriteFromAseprite(Project* project, uint32_t sceneId, const std::string& assetPath, Entity parent,
                                    const Vector3& position, PixelProblem& problem);

    // An .aseprite file dropped on `target` (Scene view, Hierarchy): onto a Sprite its sheet is
    // swapped in (one step), onto another GameObject with an Animation Player the tag clips are
    // added, else (and with no target) createSpriteFromAseprite under `parent` at `position`.
    // The GameObject changed or made; NULL_ENTITY with the problem logged.
    Entity dropAseprite(Project* project, uint32_t sceneId, const std::string& assetPath, Entity target, Entity parent,
                        const Vector3& position);

    // The editor's way to run what these build: the commands executed in order on the scene's
    // history as one undo step (those after a refused one are deleted, the step rolled back)
    bool runAsOneStep(uint32_t sceneId, std::vector<Command*> commands);

    // Apply Sprite Properties (R18): the sheet's sprite-level user properties named
    // "<component>.<property>" (set_properties' Catalog names, e.g. light2d.intensity) set on
    // `entity`, its missing components added first. `run` executes each command (the caller groups
    // them into one undo step); other keys are data only. False when a command failed.
    bool applySpriteProperties(Project* project, uint32_t sceneId, Entity entity, const SpriteSheetData& sheet,
                               const std::function<bool(Command*)>& run, std::vector<std::string>& applied,
                               std::vector<PixelProblem>& problems);

}
