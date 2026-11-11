// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S85-title-animation.md R1-R10, R58, R59, §6.1)
// - Purpose: the `.titleart`'s `animation` member and S85's additions to S80's model, in the
//   `titleart` module. Registers into TitleArtSchema (registerTitleAnimMembers, called by
//   registerProgramMembers): the document's `animation` (a Json member whose normalize is the
//   lenient codec of titleanim/TitleAnimDataIO.h), `transform.stretch` [1, 1], the layer's `tint`
//   "#ffffff" and `flash` {color, amount: 0}, the text layer's `glyph_mode` "cells". Every one is
//   written only when it differs from its default (SchemaMember::omitDefault), so a document
//   that never used them saves byte-identical and TITLE_ART_VERSION stays 1 (R58).
// - Normalize: animations keep unique names and 8-hex ids, tracks sort by (layer order in the
//   document, field), markers by (time, name); a track whose layer or field is gone stays, and
//   readAnimationSet marks it orphan (R59: an undo of the layer's removal brings it back).
// - Thread-safety: pure functions.

#pragma once

#include "Export.h"
#include "titleanim/TitleAnimTypes.h"
#include "titleart/TitleArtDoc.h"
#include "titleart/TitleArtTypes.h"

#include <string>
#include <vector>

namespace jasperengine::titleart {

    class TitleArtSchema;
    struct NormalizeContext;

    JASPERENGINE_API void registerTitleAnimMembers(TitleArtSchema& schema);

    // the member decoded; tracks whose layer or field does not resolve are marked orphan
    JASPERENGINE_API titleanim::TitleAnimationSet readAnimationSet(const TitleArtDoc& doc, std::vector<TitleWarning>* warnings = nullptr);
    // writes the member (removed when the set is empty and has no settings of its own) and
    // normalizes it (ids, names, order)
    JASPERENGINE_API void writeAnimationSet(TitleArtDoc& doc, const titleanim::TitleAnimationSet& set);
    JASPERENGINE_API bool hasAnimations(const TitleArtDoc& doc);

    // a track's field resolved against the document: the layer (null for title tracks), the
    // field's long path; false with `code` (not_animatable, bad_path, layer_missing) and the
    // layer's animatable fields when it does not
    struct JASPERENGINE_API TrackFieldInfo {
        bool ok = false;
        std::string code;
        std::string error;
        std::string longPath;
        std::vector<std::string> validHere;
        std::string nearest;
        const Json* layer = nullptr;
        titleanim::TrackTarget target = titleanim::TrackTarget::Field;
        titleanim::TitleField titleField = titleanim::TitleField::None;
        titleanim::GlyphField glyphField = titleanim::GlyphField::None;
        int component = -1;             // "tint.r": 0
        titleanim::TrackValue value = titleanim::TrackValue::Number;
        int width = 1;
        bool stepped = false;
        FxAnim anim = FxAnim::Rerender;
        ParamCombine combine = ParamCombine::Replace;
        double min = -1e18, max = 1e18;
        bool integer = false;           // a seed or another integer (reseeds)
        bool seed = false;
        std::string label;              // "Rim 1 Width", "Outer Glow Intensity"
        Json rest;                      // the document's value (title and glyph fields: the identity)
    };
    JASPERENGINE_API TrackFieldInfo resolveTrackField(const TitleArtDoc& doc, const std::string& layer, const std::string& field);
    // the fields a layer (or the title, "") can animate, long form
    JASPERENGINE_API std::vector<std::string> animatableFields(const TitleArtDoc& doc, const std::string& layer);

    // the document's text that S85's animation member adds to the content hash of its flipbooks
    // (R16's recipe chunk `animation_hash`): one animation's canonical JSON
    JASPERENGINE_API uint64_t animationHash(const TitleArtDoc& doc, const titleanim::TitleAnimation& animation);

}
