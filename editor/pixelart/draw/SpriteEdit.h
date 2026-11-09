// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// edit_sprite's op batch (docs/specs/S47-agent-sprite-drawing.md R11-R25, §6.2): the closed
// per-op field tables, checked before anything runs, and the ops themselves on a working copy of
// a sprite. Nothing here writes files or knows the editor: the tool copies the session's document,
// runs applyOps on the copy and keeps it only when every op succeeded (R12).
//
// Addressing (R3): x, y from the top-left, from 0; rect [x, y, w, h]; frames from 1, as Aseprite
// shows them (an integer, a list, a tag name or "all"); layers by name, group path ("Body/Arm") or
// index from 0 at the bottom; colours an integer palette index, "#rrggbb", "#rrggbbaa" or
// "transparent". Ops outside the canvas are clipped.

#pragma once

#include "SpriteDoc.h"

#include "pixelart/Palette.h"

#include "json.hpp"

#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor::pixelart::draw {

    inline constexpr int kMaxOps = 256;
    inline constexpr int kMaxOpPoints = 16384;

    // The 27 op names, in the order the schema lists them
    const std::vector<std::string>& opNames();
    // "paint", "transform" or "structure"; "" for an unknown op
    const char* opFamily(const std::string& op);
    // The fields an op takes (besides "op"), in the guide's order
    const std::vector<std::string>& opFields(const std::string& op);

    struct DirtyRect {
        int frame = 1;      // from 1
        int layer = 0;
        IntRect rect;
        int pixels = 0;
    };

    struct EditRequest {
        nlohmann::json ops = nlohmann::json::array();
        nlohmann::json frame;        // the default frame(s) of ops that name none (null: 1)
        nlohmann::json layer;        // the default layer (null: the topmost visible pixel layer)
        nlohmann::json symmetry;     // {x, y, diagonal} (null: none)
    };

    // What the ops need from the project: S43 palettes for `palette {action: load}` and
    // `shade {ramp_name}`
    struct EditEnv {
        // a .palette asset; false with the problem
        std::function<bool(const std::string& asset, jasperengine::pixelart::Palette& out, PixelProblem& problem)> loadPalette;
        // a named S43 ramp's colours (empty when there is none; `known` lists the ramp names there are)
        std::function<std::vector<PixelRgba>(const std::string& name, std::string& known)> findRamp;
    };

    struct EditWarning {
        std::string kind;            // "nothing_drawn", "tag_removed", "nothing_selected", ...
        std::string message;
        int opIndex = -1;
    };

    struct EditResult {
        std::vector<DirtyRect> changed;      // per (frame, layer): the union rect of the pixels that changed
        int pixels = 0;
        std::vector<std::string> opKinds;    // each op's name, in order
        std::vector<EditWarning> warnings;
        bool structureChanged = false;       // a layer, frame, tag, slice, cel, palette or canvas op changed something
        int applied = 0;
        bool changedAnything() const { return pixels > 0 || structureChanged; }
    };

    // Every op's fields against its table, before anything runs (bad_op_field naming the op's
    // fields, unknown op names, too_many_ops, value types and ranges). False with `problem` and
    // the failing op's index.
    bool validateOps(const nlohmann::json& ops, PixelProblem& problem, int& opIndex);

    // Runs the batch on `working` (a copy; the caller drops it on failure). False with `problem`
    // and the failing op's index (-1: the request itself).
    bool applyOps(SpriteDoc& working, const EditRequest& request, const EditEnv& env, EditResult& result, PixelProblem& problem, int& opIndex);

    // "Edit Sprite (hero.aseprite: line, fill ×2)"'s op part: "line, fill ×2"
    std::string summarizeOps(const std::vector<std::string>& opKinds);

}
