// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Project window's version control overlays (docs/specs/S29-version-control.md R25, R27): a
// file's or folder's state glyph and its lock, looked up by key (the folder's key once, the
// entry's name appended), with a tooltip naming the state and the lock owner.

#include "ProjectWindow.h"

#include "Project.h"
#include "vcs/VcsService.h"
#include "window/vcs/VcsOverlay.h"

using namespace jasperengine;

float editor::ProjectWindow::drawVcsOverlay(const FileEntry& entry, ImVec2 pos, float size, bool listView) {
    const vcs::VcsService& service = project->getVcs();
    if (!service.active() || !service.snapshot()) return 0.0f;
    if (vcsFolderKeyPath != currentPath || vcsFolderRoot != service.repoRoot()) {
        vcsFolderKeyPath = currentPath;
        vcsFolderRoot = service.repoRoot();
        vcsFolderInRepository = service.toKey(currentPath, vcsFolderKey);
    }
    const float reserved = listView ? vcs::badgeWidth(size) : 0.0f;
    if (!vcsFolderInRepository) return reserved;
    std::string key = vcsFolderKey;
    if (!key.empty()) key.push_back('/');
    key += entry.name;

    vcs::Badge badge = vcs::Badge::None;
    vcs::LockState lock = vcs::LockState::Unsupported;
    std::string tooltip;
    if (entry.isDirectory) {
        const vcs::DirState state = service.dirStateKey(key);
        badge = vcs::badgeOf(state);
        if (state != vcs::DirState::Clean) tooltip = vcs::tooltipOf(state);
    } else if (const vcs::VcsChange* change = service.findKey(key)) {
        badge = vcs::badgeOf(*change);
        lock = change->lock;
        tooltip = vcs::tooltipOf(*change);
    }
    const float glyphSize = listView ? size * 0.8f : size;
    const ImVec2 at(pos.x, listView ? pos.y + (size - glyphSize) * 0.5f : pos.y);
    const float width = vcs::drawBadges(ImGui::GetWindowDrawList(), at, glyphSize, badge, lock);
    if (width > 0.0f && !tooltip.empty() && ImGui::IsMouseHoveringRect(at, ImVec2(at.x + width, at.y + glyphSize)) && !ImGui::GetDragDropPayload()) {
        ImGui::SetTooltip("%s", tooltip.c_str());
    }
    return listView ? std::max(reserved, width) : width;
}
