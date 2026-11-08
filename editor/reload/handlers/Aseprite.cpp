// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// .aseprite / .ase files (S44 R22): a save from Aseprite (any program) imports the file again on
// a worker. A file that does not read yet (half written) throws: the service keeps the previous
// import, logs "[Hot Reload] <path>: <reason> (kept the previous version)" and tries again after
// 500 ms. On success the importer swaps the new version in once its pages are loaded.

#include "ReloadHandlers.h"

#include "Project.h"
#include "pixelart/aseprite/AsepriteImporter.h"

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

void reload::reloadAseprite(Context& ctx, const std::vector<fs::path>& files) {
    pixelart::aseprite::AsepriteImporter& importer = pixelart::aseprite::AsepriteImporter::get();
    if (importer.getProject() != &ctx.project) return;
    for (const fs::path& changed : files) {
        // a changed .aseprite.meta: its file's settings
        const fs::path path = changed.extension() == ".meta" ? changed.parent_path() / changed.stem() : changed;
        std::optional<pixelart::aseprite::ImportRequest> request = importer.requestOf(path.string());
        if (!request) continue;
        if (path != changed) {
            // the asset database rewrites a meta for its own fields (GUIDs, a new file's first
            // meta): only a change of the import settings imports again
            std::shared_ptr<const pixelart::aseprite::ImportRecord> current = importer.record(request->assetPath);
            if (current && current->sourceGuid == request->sourceGuid && current->settings == request->settings.toJson()) continue;
        }
        ctx.service.startJob("aseprite", path, [request = *request]() -> HotReloadService::AdoptFn {
            std::shared_ptr<pixelart::aseprite::ImportRecord> record = pixelart::aseprite::AsepriteImporter::runImport(request);
            return [record](std::string&) {
                // a file that reads but does not import (an unknown layer...) is an outcome, not a
                // reload to retry: adopt keeps the previous version and reports the problem to
                // external_changes.problems, as it does every import's warnings
                pixelart::aseprite::AsepriteImporter::get().adopt(record);
                return true;
            };
        });
    }
}
