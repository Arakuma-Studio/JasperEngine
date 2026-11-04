// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PaletteUse.h"

#include "Project.h"
#include "command/CommandHandle.h"
#include "command/UndoTimeline.h"
#include "util/FileUtils.h"
#include "util/ProjectFileWrites.h"

#include <filesystem>
#include <utility>
#include <vector>

namespace jasperengine::editor::pixelart {

bool usePalette(Project& project, const std::string& name, ResolvedPalette& out, bool& wroteCopy, std::string& error) {
    wroteCopy = false;
    if (name.empty()) {
        error = "no palette named";
        return false;
    }
    out = resolvePaletteName(project, name);
    if (!out.ok) {
        error = out.error.message + (out.error.fix.empty() ? std::string() : " (" + out.error.fix + ")");
        return false;
    }
    if (!out.needsCopy) return true;

    std::vector<WriteFilesCmd::Item> items;
    items.push_back({project.getProjectPath() / FileUtils::pathFromUtf8(out.path), out.copyText});
    // each file beneath its own folder, as the editor's other windows write: an assets
    // directory linked in from elsewhere is no new refusal (an MCP call's write scope still applies)
    if (!ProjectFileWrites::writeUndoable(&project, std::move(items), error, nullptr, nullptr, {}, true)) return false;
    out.needsCopy = false;
    out.copyText.clear();
    wroteCopy = true;
    return true;
}

void linkPaletteCopy(Project& project, uint32_t sceneId) {
    CommandHistory* projectHistory = project.getProjectCommandHistory();
    CommandHistory* sceneHistory = CommandHandle::get(sceneId);
    if (projectHistory && sceneHistory && projectHistory->top() && sceneHistory->top())
        UndoTimeline::link({projectHistory, sceneHistory});
}

}
