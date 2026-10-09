// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "HtmlUiImportQueue.h"

#include "Entity.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <utility>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::htmlui {

    // The editor's automatic imports (phase-2 §3.13.4). Every loop iteration (App::pollHtmlUI)
    // it notices each document of every open scene that loaded or reloaded (scene open, play,
    // an Inspector change, a hot reload, McpAutoSync) and, with AppSettings::htmlUiAutoImport,
    // queues the remote references missing from the manifest on the background worker
    // ("[HTML UI] importing …" in the Console). When a batch finishes it reloads the manifest,
    // refreshes the asset database and reloads the documents that use the imports.
    class HtmlUiAutoImport {
    public:
        static HtmlUiAutoImport& get();

        // true when documents were reloaded (the views redraw)
        bool update(Project* project);
        // the Inspector's Import (missing and failed references, retried) and Re-import all
        size_t importDocument(Project* project, const std::string& documentPath, bool reimportAll);
        // queues the missing references of a document (what a load triggers); manual retries failures
        size_t queueMissing(const ImportTarget& target, const std::string& documentPath, bool manual);

        ImportQueue& queue() { return importQueue; }
        // App::shutdownBackgroundWork: running downloads are cancelled, the worker stops
        void shutdown();

        static ImportTarget targetFor(const Project* project);

    private:
        HtmlUiAutoImport() = default;
        void applyBatch(Project* project, const ImportQueue::Batch& batch);

        ImportQueue importQueue;
        std::filesystem::path projectPath;
        // (scene id, entity) -> (filename, load count) last seen
        std::map<std::pair<uint32_t, Entity>, std::pair<std::string, uint32_t>> seen;
    };

}
