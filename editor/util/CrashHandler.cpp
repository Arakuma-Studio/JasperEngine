// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CrashHandler.h"
#include "diag/CrashHandler.h"
#include "EditorVersion.h"

void jasperengine::editor::CrashHandler::install(const std::filesystem::path& logFile) {
    auto config = diag::crash::editorConfig(logFile.parent_path(), JASPERENGINE_EDITOR_VERSION);
    config.logFile = logFile;
    diag::crash::install(config);
}
