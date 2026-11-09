// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S85-title-animation.md R42, R43, R45, R48)
// - Purpose: the Animation panel's edits as pure functions from the animation to an edit_title op
//   batch: moving and scaling selected keys in time (with frame snapping), deleting, duplicating
//   and pasting them, setting eases and tangent modes, and Record's rewrite of the Designer's
//   `set` ops into keys. The panel commits their batches as one undo step, so a drag, a menu and
//   an agent's edit_title leave the same document (S82 R77).
// - Whole tracks are rewritten (`set_track` with `keys`): a move or scale collapses onto an
//   existing key's time (within the op's merge tolerance) and the moved key wins, as Unity's does.
// - Record (R45): a `set` of an animatable field becomes `set_key` at the playhead; a field with
//   no track yet and a playhead past 0 also keys its rest value at 0, so the motion starts from
//   the still. Other ops pass through unchanged and are reported as not keyed.
// - Thread-safety: pure functions.

#pragma once

#include "titleanim/TitleAnimTypes.h"
#include "titleart/TitleArtDoc.h"

#include <string>
#include <vector>

namespace jasperengine::editor {

    using TitleAnimJson = titleart::Json;

    struct TitleKeyRef {
        int track = -1;
        int key = -1;
        bool operator<(const TitleKeyRef& o) const { return track != o.track ? track < o.track : key < o.key; }
        bool operator==(const TitleKeyRef& o) const { return track == o.track && key == o.key; }
    };

    // `t` on the frame grid of `fps`
    double snapTitleKeyTime(double t, int fps);
    // the times of a segment's ease curve glyph: `count` samples of the ease over [0, 1]
    std::vector<float> titleEaseSamples(EaseType ease, int count);

    // ---- key edits: each is a batch of set_track ops (empty: nothing changed) ----

    // `shift` seconds onto the selected keys; `snapFps` > 0 snaps each moved key's time to frames
    TitleAnimJson titleMoveKeysOps(const titleanim::TitleAnimation& animation, const std::vector<TitleKeyRef>& selection, double shift, int snapFps);
    // the selected keys' times about `pivot` by `factor` (a box edge's drag, R42)
    TitleAnimJson titleScaleKeysOps(const titleanim::TitleAnimation& animation, const std::vector<TitleKeyRef>& selection, double pivot, double factor, int snapFps);
    TitleAnimJson titleDeleteKeysOps(const titleanim::TitleAnimation& animation, const std::vector<TitleKeyRef>& selection);
    // Ctrl+D: a copy `delta` seconds later
    TitleAnimJson titleDuplicateKeysOps(const titleanim::TitleAnimation& animation, const std::vector<TitleKeyRef>& selection, double delta);
    // Ctrl+C: the selected keys, times relative to the first
    struct TitleKeyClipboard {
        std::string layer, field;
        std::vector<titleanim::TitleKey> keys;
    };
    std::vector<TitleKeyClipboard> titleCopyKeys(const titleanim::TitleAnimation& animation, const std::vector<TitleKeyRef>& selection);
    // Ctrl+V at the playhead (keys merge by time; a clipboard track missing here is skipped)
    TitleAnimJson titlePasteKeysOps(const titleanim::TitleAnimation& animation, const std::vector<TitleKeyClipboard>& clipboard, double at);
    TitleAnimJson titleSetEaseOps(const titleanim::TitleAnimation& animation, const std::vector<TitleKeyRef>& selection, EaseType ease);
    TitleAnimJson titleSetTangentModeOps(const titleanim::TitleAnimation& animation, const std::vector<TitleKeyRef>& selection, ClipTangentMode mode);

    // the segment starting at `key` as curve keys (R43's Convert Eases to Curves: within 0.005 of
    // the ease): replaces the track's eased keys with sampled ones
    TitleAnimJson titleConvertEasesOps(const titleanim::TitleAnimation& animation, int track);

    // ---- Record (R45) ----

    struct TitleRecordResult {
        TitleAnimJson ops = TitleAnimJson::array();     // what to commit
        std::string stepName;                           // "Record Key: Title/Main Scale", "" nothing keyed
        std::vector<std::string> notKeyed;              // labels of the edits that changed the document instead
    };
    // the Designer's batch with its animatable `set`s turned into keys at `time`
    TitleRecordResult titleRecordRewrite(const titleart::TitleArtDoc& doc, const titleanim::TitleAnimation& animation, double time, const TitleAnimJson& ops);

}
