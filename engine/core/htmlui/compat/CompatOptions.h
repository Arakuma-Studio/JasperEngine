// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_COMPATOPTIONS_H
#define COMPAT_COMPATOPTIONS_H

#include "htmlui/compat/ForkFeatures.h"

#include <cstddef>
#include <functional>
#include <string>

namespace jasperengine::htmlui::compat {

    // A size in CSS px (x = width, y = height, like Rml::Vector2i, without RmlUi types: the
    // compat API is reached from the editor through the shared engine).
    struct CompatSize {
        int x = 1280;
        int y = 720;

        bool operator==(const CompatSize& other) const { return x == other.x && y == other.y; }
        bool operator!=(const CompatSize& other) const { return !(*this == other); }
    };

    // How a compile reads files (phase-2 §3.12.1). Paths are asset-relative, like everywhere
    // in compat. Empty functions = the engine's Data/FileData (asset directory, resource
    // packs, Android assets, Web preload). When set (phase 4's `check --ui` outside a loaded
    // project), every read of the compile goes through it: documents, stylesheets, the
    // existence of images and fonts, and the import manifest. Virtual files ("@htmlui/...")
    // are always served from VirtualFiles first.
    struct FileAccess {
        std::function<bool(const std::string& assetPath, std::string& content)> read;
        std::function<bool(const std::string& assetPath)> exists;
    };

    // Everything that changes the output of a compile (phase-2 §3.12.1).
    struct CompatOptions {
        ForkFeatures fork = ForkFeatures::present();
        CompatSize viewport{1280, 720};      // canvas size in CSS px at load
        float dpRatio = 1.0f;
        bool touchDevice = false;             // System::isTouchDevice()
        std::string colorScheme = "light";    // initial theme activated after load
        bool reducedMotion = false;           // initial "reduced-motion" theme state
        bool verboseDiagnostics = false;      // Info lines logged one by one
        size_t diagnosticLimit = 1000;
        bool dropTransitionsForTesting = false;   // golden tests: transitions removed, as the reference script does
    };

}

#endif //COMPAT_COMPATOPTIONS_H
