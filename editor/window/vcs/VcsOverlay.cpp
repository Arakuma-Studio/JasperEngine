// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "window/vcs/VcsOverlay.h"

#include "external/IconsFontAwesome6.h"

namespace jasperengine::editor::vcs {

namespace {

    bool any(const VcsChange& change, FileState state) {
        return change.index == state || change.worktree == state;
    }

    void outlinedText(ImDrawList* drawList, ImFont* font, float size, ImVec2 pos, ImU32 color, const char* text) {
        const ImU32 shadow = IM_COL32(0, 0, 0, 200);
        for (int dx = -1; dx <= 1; dx++) {
            for (int dy = -1; dy <= 1; dy++) {
                if (dx || dy) drawList->AddText(font, size, ImVec2(pos.x + static_cast<float>(dx), pos.y + static_cast<float>(dy)), shadow, text);
            }
        }
        drawList->AddText(font, size, pos, color, text);
    }

}

Badge badgeOf(const VcsChange& change) {
    if (change.conflict) return Badge::Conflict;
    if (change.deleted()) return Badge::Deleted;
    if (any(change, FileState::Added)) return Badge::Added;
    if (any(change, FileState::Modified) || any(change, FileState::Renamed)) return Badge::Modified;
    if (any(change, FileState::Untracked)) return Badge::Untracked;
    if (change.remoteChanged) return Badge::RemoteChanged;
    return Badge::None;
}

Badge badgeOf(DirState state) {
    switch (state) {
        case DirState::Conflict: return Badge::Conflict;
        case DirState::Changed: return Badge::Modified;
        case DirState::Untracked: return Badge::Untracked;
        case DirState::LockedByOther:
        case DirState::Clean: return Badge::None;
    }
    return Badge::None;
}

Badge badgeOf(const scene::ObjectDiff& object) {
    switch (object.state) {
        case scene::ChangeState::Added: return Badge::Added;
        case scene::ChangeState::Removed: return Badge::Deleted;
        case scene::ChangeState::Modified: return Badge::Modified;
        case scene::ChangeState::Unchanged: return object.reparented || object.reordered ? Badge::Modified : Badge::None;
    }
    return Badge::None;
}

const char* glyphOf(Badge badge) {
    switch (badge) {
        case Badge::Modified: return ICON_FA_PEN;
        case Badge::Added: return ICON_FA_PLUS;
        case Badge::Untracked: return ICON_FA_QUESTION;
        case Badge::Deleted: return ICON_FA_MINUS;
        case Badge::Conflict: return ICON_FA_TRIANGLE_EXCLAMATION;
        case Badge::RemoteChanged: return ICON_FA_ARROW_DOWN;
        case Badge::None: return "";
    }
    return "";
}

ImU32 colorOf(Badge badge) {
    switch (badge) {
        case Badge::Modified: return IM_COL32(240, 176, 40, 255);
        case Badge::Added: return IM_COL32(90, 200, 100, 255);
        case Badge::Untracked: return IM_COL32(160, 160, 160, 255);
        case Badge::Deleted: return IM_COL32(230, 80, 70, 255);
        case Badge::Conflict: return IM_COL32(240, 70, 60, 255);
        case Badge::RemoteChanged: return IM_COL32(90, 160, 255, 255);
        case Badge::None: return 0;
    }
    return 0;
}

const char* nameOf(Badge badge) {
    switch (badge) {
        case Badge::Modified: return "Modified";
        case Badge::Added: return "Added";
        case Badge::Untracked: return "Untracked";
        case Badge::Deleted: return "Deleted";
        case Badge::Conflict: return "Conflicted";
        case Badge::RemoteChanged: return "The server has a newer version";
        case Badge::None: return "";
    }
    return "";
}

ImU32 lockColor(LockState lock) {
    return lock == LockState::LockedByMe ? IM_COL32(90, 200, 100, 255) : IM_COL32(230, 80, 70, 255);
}

std::string tooltipOf(const VcsChange& change) {
    std::string text = nameOf(badgeOf(change));
    if (!change.oldPath.empty()) text += " (renamed from " + change.oldPath + ")";
    if (change.remoteChanged && badgeOf(change) != Badge::RemoteChanged) text += (text.empty() ? "" : "; ") + std::string("the server has a newer version");
    if (change.lock == LockState::LockedByMe) text += (text.empty() ? "" : ", ") + std::string("locked by you");
    else if (change.lock == LockState::LockedByOther) text += (text.empty() ? "" : ", ") + std::string("locked by ") + change.lockOwner;
    else if (change.lock == LockState::Unlocked && text.empty()) text = "Not locked (lockable)";
    return text.empty() ? std::string("Unchanged") : text;
}

std::string tooltipOf(DirState state) {
    switch (state) {
        case DirState::Conflict: return "A file below it is conflicted";
        case DirState::LockedByOther: return "A file below it is locked by someone else";
        case DirState::Changed: return "Files below it changed";
        case DirState::Untracked: return "New files below it";
        case DirState::Clean: return "Unchanged";
    }
    return "Unchanged";
}

std::string tooltipOf(const scene::ObjectDiff& object) {
    std::string text = object.state == scene::ChangeState::Added ? "Added since the baseline" :
                       object.state == scene::ChangeState::Modified ? "Modified since the baseline" : "";
    if (object.reparented) text += (text.empty() ? "" : ", ") + std::string("reparented");
    if (object.reordered) text += (text.empty() ? "" : ", ") + std::string("reordered");
    size_t properties = 0;
    for (const scene::ComponentDiff& component : object.components) properties += component.properties.size();
    if (properties > 0) text += " (" + std::to_string(properties) + (properties == 1 ? " property)" : " properties)");
    return text;
}

float badgeWidth(float size) {
    return size * 1.05f;
}

float drawBadges(ImDrawList* drawList, ImVec2 pos, float size, Badge badge, LockState lock) {
    ImFont* font = ImGui::GetFont();
    float x = pos.x;
    if (badge != Badge::None) {
        const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, glyphOf(badge));
        outlinedText(drawList, font, size, ImVec2(x + (badgeWidth(size) - extent.x) * 0.5f, pos.y), colorOf(badge), glyphOf(badge));
        x += badgeWidth(size);
    }
    if (lock == LockState::LockedByMe || lock == LockState::LockedByOther) {
        const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, ICON_FA_LOCK);
        outlinedText(drawList, font, size, ImVec2(x + (badgeWidth(size) - extent.x) * 0.5f, pos.y), lockColor(lock), ICON_FA_LOCK);
        x += badgeWidth(size);
    }
    return x - pos.x;
}

}
