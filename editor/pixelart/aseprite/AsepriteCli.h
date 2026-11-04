// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// jasperengine-editor aseprite (docs/specs/S44-aseprite-import.md R37): the .aseprite codec from a
// shell, without a project, engine or window; the golden tests and agents use it.
// - info <file> [--json] [--cels]: a short summary, or exactly describe(doc, Summary) (Cels with
//   --cels) as JSON on stdout (the read warnings then go to stderr only).
// - render <file> <out.png> [--frame N | --tag T] [--layers a,b] [--sheet horizontal|rows]
//   [--scale N] [--classic-blend] [--legacy-hsl] [--flatten-groups]: one frame, or a tag's
//   playback (once) as a sheet; the PNG is written atomically.
// - resave <file> <out> [--expect-identical]: read and write back; with --expect-identical a
//   written file that differs from the input exits 4 and names where the first difference is.
// Exit code: 0 ok, 1 the output could not be encoded or written, 2 usage (unknown subcommand,
// option, tag or layer, a missing argument, a bad number), 3 read error (its kind, message and
// fix on stderr), 4 resave --expect-identical wrote different bytes. argv[0] is the subcommand.

#include <iosfwd>
#include <string>

namespace jasperengine::editor::pixelart::aseprite {

    int runAsepriteCommand(int argc, char** argv, const std::string& commandName, std::ostream& out, std::ostream& err);
    void printAsepriteUsage(const std::string& commandName, std::ostream& out);

}
