// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <vector>

namespace jasperengine::editor {

    // The user reference of the engine's library shaders (docs/shaders/library.md), generated
    // from their manifests so it never drifts from them: jasperengine-editor shaders
    // --library all --doc <file>. Per shader: purpose, lighting modes, inputs, default render
    // state and its feature rules, properties, textures, features and keywords; before them,
    // how to pick a lighting mode, the per-object light cap and the blend modes.
    class ShaderLibraryDoc {
    public:
        // the Markdown of the named library shaders (names without "library:"), in order
        static std::string generate(const std::vector<std::string>& names);
    };

}
