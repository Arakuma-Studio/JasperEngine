// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// TEST-ONLY seam (Exporter.h befriends it): one definition for every test that reaches the
// exporter's internals

#include "Exporter.h"

#include <filesystem>
#include <system_error>

namespace jasperengine::editor {

    struct ExporterTestAccess {
        static void walkTree(Exporter& exporter, const std::filesystem::path& root, const Exporter::TreeVisitor& visit, std::error_code& ec) {
            exporter.walkTree(root, visit, ec);
        }
        static std::filesystem::path treeListFailure(const Exporter& exporter) { return exporter.treeListFailure; }
        using TreeStep = Exporter::TreeStep;

        // a source export's Android Studio project written into targetDir (its workspaces/ and
        // platform/ copied there first); the export's error on failure
        static bool writeAndroidProjectSettings(Exporter& exporter, Project* project, const std::filesystem::path& targetDir, std::string& error) {
            exporter.project = project;
            exporter.config.mode = ExportMode::SourceCode;
            exporter.config.targetDir = targetDir;
            const bool ok = exporter.writeAndroidProjectSettings();
            error = exporter.getProgress().errorMessage;
            return ok;
        }
    };

}
