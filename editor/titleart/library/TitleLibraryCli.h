// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S84-title-library.md R20, R21, R46, AC11)
// - Purpose: `jasperengine-editor title-library <subcommand>`, windowless: it runs before the
//   backend starts (no window, no GL) and reads no project.
//     port-pachislop --dump <json> --out <dir> [--repo <dir>] [--check]
//         maps a Pachislop dump onto library files under <dir> (repo-relative paths: looks,
//         templates, color schemes, provenance/pachislop.json) and writes the port report there;
//         --check ports into a temporary directory, normalizes it and compares it byte for byte
//         with the committed files of <repo> (default: the working directory), naming each file
//         that differs, is missing or is stale
//     normalize [--check] <dir>
//         rewrites every .titlelook and .titletemplate under <dir> in canonical form; --check
//         only lists the files that are not canonical
// - Subcommands are rows of one table (name, usage, summary, function): a new one is a row and
//   a function.
// - Exit codes: 0 ok, 1 findings (port errors, differences, files not canonical), 2 usage or I/O
//   error.

#pragma once

namespace jasperengine::editor {

    class TitleLibraryCli {
    public:
        // argv[0] is "title-library"; executableName names the command in the help and messages
        static int run(int argc, char** argv, const char* executableName);
    };

}
