// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef THORVGRUNTIME_H
#define THORVGRUNTIME_H

#include "Export.h"

namespace jasperengine {

    // Reference-counted owner of ThorVG's process-wide state (tvg::Initializer), the Lottie
    // renderer of <lottie> elements. HtmlUI::acquire/release take one reference while RmlUi is
    // up. Main thread only; ThorVG runs without worker threads (deterministic output).
    class JASPERENGINE_API ThorVGRuntime {
    public:
        // First call: tvg::Initializer::init(0) and the engine's built-in fonts for Lottie
        // text layers (Roboto as "sans-serif", the default of any text whose font is not
        // embedded, then Noto Sans Arabic). false (logged, no reference taken) on failure.
        static bool acquire();
        // Last call: tvg::Initializer::term(). Every ThorVG object must be destroyed before.
        static void release();
        static bool isInitialized();
        static int getReferenceCount();
    };

}

#endif //THORVGRUNTIME_H
