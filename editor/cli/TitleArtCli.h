// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S87-title-localisation-interchange.md R62, §6.5)
// - Purpose: Title Art's batch import and export from a shell:
//     import-title-art <file|glob>... --project <dir> [--into <assets dir>]
//         [--kind auto|pachislop|psd|ps_styles|svg|strings] [--title <x.titleart>] [--add-languages]
//         [--overwrite] [--json]
//     export-title-art <x.titleart|glob|dir>... --project <dir> --format png|psd|svg|sheet|atlas|
//         sequence|ktx2|csv|xliff [--languages all|en,de] [--out <dir>] [--bit-depth 8|16] [--scale <s>]
//         [--alpha straight|premultiplied] [--no-bleed] [--alpha-threshold <n>] [--split-titles]
//         [--parts] [--animation <name>] [--fps <n>] [--family desktop|mobile|web] [--zip]
//         [--overwrite] [--json]
//   Each flag is the MCP parameter or `options` key of the same name, dashes for underscores.
// - Imports (Pachislop, PSD, Photoshop styles, SVG) and the strings formats run offline: no editor
//   process, the same importers and ops as edit_title (TitleArtOps), the files written directly.
//   The render formats run over one one-shot headless editor for the whole batch
//   (CallCommand::runCalls), so a batch of 20 titles starts one editor.
// - Exit codes: 0 every input done, 1 some failed (each listed), 2 usage. `--json` prints each
//   result's structured content, one JSON line per input.
// - Thread-safety: the process's main thread.

#pragma once

namespace jasperengine::editor {

    class TitleArtCli {
    public:
        // argv[0] is the subcommand
        static int runImport(int argc, char** argv, const char* executableName);
        static int runExport(int argc, char** argv, const char* executableName);
    };

}
