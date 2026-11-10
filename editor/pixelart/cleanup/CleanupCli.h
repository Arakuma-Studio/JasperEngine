// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// jasperengine-editor fix-pixel-art and lint-pixel-art (docs/specs/S46-pixel-art-cleanup.md R48):
// the MCP tools for a shell or CI, through the same session (so a CLI write is the same pixels as
// a tool or window write). No engine, window or GPU. Output: JSON on stdout, or `--format text`.
// Exit codes:
//   0  lint: clean; fix: written, or a dry run whose output is clean
//   2  usage, including bad_recipe
//   3  lint: auto-fixable FAILs; fix dry run: a confident fix exists; fix write: written with WARN
//      lints left when --fail-on warn
//   4  needs_person (nothing written without --force)
//   1  could not run (I/O, write failure)
// argv[0] is the subcommand.

#pragma once

#include <iosfwd>
#include <string>

namespace jasperengine::editor::pixelart::cleanup {

    int runFixPixelArt(int argc, char** argv, const std::string& commandName, std::ostream& out, std::ostream& err);
    int runLintPixelArt(int argc, char** argv, const std::string& commandName, std::ostream& out, std::ostream& err);

}
