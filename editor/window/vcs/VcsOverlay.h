// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The version control overlays of the Project window, the Hierarchy and the Inspector
// (docs/specs/S29-version-control.md R27, D4): which glyph a file's state flags show, in a fixed
// precedence (the providers never collapse states; this is the one place that does), their
// colours and tooltips, and the drawing of a glyph with a dark outline.
//
//   modified amber pen, added green plus, untracked grey question mark, deleted red minus,
//   conflicted red triangle, server has changes blue arrow; a lock is a second glyph: green when
//   it is yours, red when someone else's

#include "vcs/VcsTypes.h"
#include "vcs/scene/SceneDiff.h"

#include "imgui.h"

#include <string>

namespace jasperengine::editor::vcs {

    enum class Badge : uint8_t { None, Modified, Added, Untracked, Deleted, Conflict, RemoteChanged };

    // D4: Conflict > Deleted > Added > Modified (renamed too) > Untracked > RemoteChanged
    Badge badgeOf(const VcsChange& change);
    // a folder: its worst file (R6)
    Badge badgeOf(DirState state);
    // a GameObject of the scene diff (R27: Added / Modified / Reparented in the Hierarchy)
    Badge badgeOf(const scene::ObjectDiff& object);

    const char* glyphOf(Badge badge);
    ImU32 colorOf(Badge badge);
    const char* nameOf(Badge badge);          // "Modified", "Added", ... ("" for None)
    ImU32 lockColor(LockState lock);          // green yours, red another user's

    // "Modified, locked by alice", "Untracked", "Server has a newer version"
    std::string tooltipOf(const VcsChange& change);
    std::string tooltipOf(DirState state);
    std::string tooltipOf(const scene::ObjectDiff& object);

    // Draws the badge (and the lock glyph after it) at `pos` (top-left) in a font of `size`
    // pixels; returns the width drawn (0 for nothing)
    float drawBadges(ImDrawList* drawList, ImVec2 pos, float size, Badge badge, LockState lock);
    // The width drawBadges takes for one glyph of that size
    float badgeWidth(float size);

}
