// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "RemoteImporter.h"

#include <string>
#include <vector>

namespace jasperengine::editor::htmlui {

    // A document's remote reference against the manifest on disk (the Inspector panel's list)
    struct RemoteResourceState {
        DiscoveredReference reference;
        bool imported = false;        // a manifest entry whose file exists
        bool fileMissing = false;     // a manifest entry whose file is gone (HTMLUI-IMP-0009: re-import)
        std::string localPath;        // asset-relative file of the entry ("" when not imported)
        std::string licenseName;
    };

    std::vector<RemoteResourceState> remoteResourceStates(const ImportTarget& target, const std::string& documentPath);

    // Every .html/.htm/.css/.rml/.rcss file of the assets (hidden and build directories skipped), asset-relative
    std::vector<std::string> findProjectDocuments(const ImportTarget& target);

    struct RemoveUnusedResult {
        std::vector<std::string> removedUrls;     // manifest entries removed
        std::vector<std::string> deletedFiles;    // asset-relative files deleted
        std::string error;
    };

    // The Inspector's "Remove unused" (phase-2 §3.13.2): removes the manifest entries no project
    // document references (directly, or through the files of an entry that is referenced, like
    // the Google Fonts CSS an imported stylesheet @imports) and deletes their files (a file
    // another kept entry lists stays). References are what compat collects plus, to never
    // delete something a document still uses, the phase-1 scanner's remote references.
    RemoveUnusedResult removeUnusedImports(const ImportTarget& target);

}
