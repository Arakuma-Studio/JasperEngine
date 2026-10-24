// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <iosfwd>
#include <string>

namespace jasperengine::editor::texsynth {

    // jasperengine-editor synthesize-texture --example <image> --out <png> [options]: the texture
    // synthesis without a window, GPU or engine. Problems go to `out` compiler-style (or one JSON
    // object with --format json), progress and the summary to `err`. Exit code 0 written, 1 the
    // synthesis or a write failed, 2 usage error (bad option or value, an existing output without
    // --overwrite). argv[0] is the subcommand.
    int runSynthesizeTexture(int argc, char** argv, const std::string& commandName, std::ostream& out, std::ostream& err);
    void printSynthesizeTextureUsage(const std::string& commandName, std::ostream& out);

}
