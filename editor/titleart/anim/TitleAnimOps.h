// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S85-title-animation.md R1-R10, R22, R45, R50, §6.7.1)
// - Purpose: the animation ops of edit_title, registered into TitleArtOps (registerTitleAnimOps):
//   add_animation, set_animation, remove_animation, duplicate_animation, set_default_animation,
//   set_track, set_key, remove_track, remove_keys, retime, set_marker, remove_marker (and
//   apply_animation_preset, TitleAnimPresets.h). They run inside S80's all-or-nothing batches
//   with dry_run and expect_rev, as one named undo step; the Title Designer's Animation panel and
//   Record, MCP and editor Lua all edit animations through them.
// - Errors list the valid alternatives: animations by name (or `id:<8 hex>`), fields with
//   did-you-mean and the layer's animatable fields (`not_animatable`), `component_conflict`
//   naming the existing track, `relative_stepped`, `glyph_color_needs_cells`, `reseed_not_seed`.
// - Thread-safety: main thread.

#pragma once

#include "titleanim/TitleAnimTypes.h"
#include "titleart/TitleArtDoc.h"
#include "titleart/TitleArtOps.h"

#include <string>
#include <vector>

namespace jasperengine::editor {

    // the op handlers into TitleArtOps (once; the MCP tools' registration calls it)
    void registerTitleAnimOps();

    // helpers the panel, Record and the presets share with the ops
    struct TitleAnimTrackRef {
        std::string animation;          // name
        std::string layer;              // layer id, "" the title
        std::string field;              // long form
    };
    // the animation named `ref` (a name or "id:<8 hex>") in `set`, -1 when none
    int findTitleAnimation(const titleanim::TitleAnimationSet& set, const std::string& ref);
    // the track on (layer, field) of `animation`, -1 when none
    int findTitleTrack(const titleanim::TitleAnimation& animation, const std::string& layer, const std::string& field);
    // the track whose field contains or is contained in `field` ("tint" and "tint.r"), -1 when none
    int conflictingTitleTrack(const titleanim::TitleAnimation& animation, const std::string& layer, const std::string& field);
    // the value of (layer, field) at `time` in `animation` (the sampled pose, else the rest):
    // a number, [x, y], "#rrggbb" or the stepped value's JSON
    TitleJson sampleTitleTrackValue(const titleart::TitleArtDoc& doc, const titleanim::TitleAnimation& animation, const std::string& layer,
                                    const std::string& field, double time);
    // a key from an op's value: numbers checked against the field's width, colours as "#rrggbb"
    bool titleKeyFromValue(const TitleJson& value, int width, bool stepped, bool color, titleanim::TitleKey& out, std::string& error);
    // "Intro" -> "Intro 2" when taken
    std::string uniqueTitleAnimationName(const titleanim::TitleAnimationSet& set, const std::string& name);

}
