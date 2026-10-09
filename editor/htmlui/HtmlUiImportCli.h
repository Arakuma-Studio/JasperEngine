// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <iosfwd>
#include <string>

namespace jasperengine::editor::htmlui {

    // jasperengine-editor import-ui --project <dir> <html|css> [<html|css>…] (phase-2 §3.13.4,
    // hook H11): imports every remote reference of the documents synchronously, prints the
    // HTMLUI-IMP diagnostics compiler-style (project-relative paths) on `out`, a summary on
    // `err`. Exit code 0 without errors, 1 with an error (a failed download, curl missing), 2 for
    // a usage error. argv[0] is the subcommand.
    int runImportUi(int argc, char** argv, const std::string& commandName, std::ostream& out, std::ostream& err);
    void printImportUiUsage(const std::string& commandName, std::ostream& out);

}
