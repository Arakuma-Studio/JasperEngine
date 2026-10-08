// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ThorVGRuntime.h"

#include "DefaultFont.h"
#include "DefaultFontArabic.h"
#include "Log.h"

#include <thorvg.h>

using namespace jasperengine;

namespace {

    int referenceCount = 0;

    // Registration order matters: a text layer whose font is neither embedded nor registered
    // falls back to the first font ThorVG has (tvg::Text::font(nullptr))
    struct BuiltinFont {
        const char* name;
        const unsigned char* data;
        unsigned int size;
    };

    const BuiltinFont builtinFonts[] = {
        {"sans-serif", roboto_regular_ttf, roboto_regular_ttf_len},
        {"sans-serif-arabic", noto_sans_arabic_ttf, noto_sans_arabic_ttf_len},
    };

}

bool ThorVGRuntime::acquire(){
    if (referenceCount > 0){
        referenceCount++;
        return true;
    }

    // no worker threads: the calling thread rasterises, so frames are deterministic
    if (tvg::Initializer::init(0) != tvg::Result::Success){
        Log::error("[HTML UI] ThorVG could not be initialised");
        return false;
    }

    // the arrays are static, so ThorVG borrows them (copy = false)
    for (const BuiltinFont& font : builtinFonts){
        if (tvg::Text::load(font.name, reinterpret_cast<const char*>(font.data), font.size, "ttf", false) != tvg::Result::Success){
            Log::warn("[HTML UI] ThorVG could not load the built-in font '%s'", font.name);
        }
    }

    referenceCount = 1;
    return true;
}

void ThorVGRuntime::release(){
    if (referenceCount <= 0){
        Log::warn("[HTML UI] ThorVGRuntime::release called without a matching acquire");
        return;
    }

    referenceCount--;
    if (referenceCount > 0)
        return;

    for (const BuiltinFont& font : builtinFonts){
        tvg::Text::unload(font.name);
    }
    tvg::Initializer::term();
}

bool ThorVGRuntime::isInitialized(){
    return referenceCount > 0;
}

int ThorVGRuntime::getReferenceCount(){
    return referenceCount;
}
