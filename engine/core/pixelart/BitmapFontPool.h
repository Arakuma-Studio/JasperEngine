// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef PIXELART_BITMAPFONTPOOL_H
#define PIXELART_BITMAPFONTPOOL_H

// Bitmap fonts shared by path (S51 D2), and a Text's chain (its font and the same-size bitmap
// fonts of its fallback list) merged once into one atlas. Hot reload (S05) invalidates the
// entries that read a changed file; Texts compare generations and load again. Main thread.

#include "Export.h"
#include "pixelart/BitmapFont.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::pixelart {

    class JASPERENGINE_API BitmapFontPool {
    public:
        // nullptr when it does not load (`report` says why; not cached, a fix loads it again)
        static std::shared_ptr<BitmapFont> get(const std::string& path, BitmapFontReport* report = nullptr);
        // `paths` is a Text's font list: the first is a bitmap font; the others that are bitmap
        // fonts of its native size join it (R14), the rest are skipped (`skipped` gets them)
        static std::shared_ptr<BitmapFont> getChain(const std::vector<std::string>& paths, std::vector<std::string>* skipped = nullptr,
                                                    std::string* key = nullptr);
        static std::string chainKey(const std::vector<std::string>& paths);

        static uint32_t generation(const std::string& key);
        // bumped by every invalidate: one compare per frame tells a Text to look at its generation
        static uint32_t revision();
        // drops every entry that read `file` (engine path or absolute path); returns their keys
        static std::vector<std::string> invalidate(const std::string& file);
        static bool isUsingFile(const std::string& file);

        static void clear();
        static void clearUnused();
    };

}

#endif //PIXELART_BITMAPFONTPOOL_H
