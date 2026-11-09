// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S85-title-animation.md R41-R48, §6.8, §6.10)
// - Purpose: the Title Designer's Animation panel, registered through S82's hooks (addPanel, the
//   edit router for Record, the canvas pose source for scrubbing and playing, a lint source for
//   the animation checks). Dopesheet and Curves, the transport, Record, Add Preset, Preview As and
//   the cost badge. Every edit is an edit_title op batch (TitleAnimTimelineOps), one undo step.
// - The canvas shows the sampled pose only while the panel previews: while it plays or has been
//   scrubbed (Stop returns to the document). Poses render synchronously through
//   TitleAnimRenderer at a capped scale and stay in a texture cache keyed by the pose, so
//   re-scrubbing and onion skin never re-render (S85 Deviations).
// - Thread-safety: main thread.

#pragma once

#include "titleart/TitleArtDoc.h"

#include <string>

namespace jasperengine::editor::titledesigner {

    // the panel, the edit router, the pose source and the lint source (once, at startup)
    struct TitleDesignerSettings;
    // `settings` is the window's: the panel keeps its §6.9 preferences there and saves them
    void registerTitleAnimationPanel(TitleDesignerSettings* settings);

    // the catalog's titleAnimation.* commands (§6.10); false when nothing happened
    bool runTitleAnimationCommand(const std::string& command);
    // shows `name` at `time` on the canvas, as a scrub does (time < 0 stops the preview); false with
    // `error` before the panel has drawn, or for an unknown name
    bool previewTitleAnimation(const std::string& name, double time, std::string* error);
    // what the panel shows now, for focus_window's state and scripted input: the animation, the
    // playhead, Record and Curves, the selection and the timeline's place on screen
    titleart::Json titleAnimationPanelState();
    // the panel has the keyboard focus (AppCommands' ShortcutContext::TitleAnimation)
    bool titleAnimationPanelFocused();

}
