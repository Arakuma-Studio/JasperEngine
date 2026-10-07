// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef FONTREGISTRY_H
#define FONTREGISTRY_H

#include "Export.h"
#include "FontFace.h"

#include <memory>
#include <string>
#include <vector>

namespace jasperengine {

    // Process-wide font faces by family, weight and style
    class JASPERENGINE_API FontRegistry {
    public:
        // family empty -> face's own family; weight 0 -> face's own weight
        static void registerFace(std::shared_ptr<FontFace> face, const std::string& family = "",
                                 int weight = 0, int italic = -1 /* -1: from face */, bool fallback = false);
        // CSS font matching (CSS Fonts 4 §5.2 weight rules): exact style preferred, else the other
        // style; then weight: 400 -> 500 -> lighter desc -> heavier asc; 500 -> 400 -> lighter -> heavier;
        // < 400 -> lighter desc -> heavier asc; > 500 -> heavier asc -> lighter desc.
        static std::shared_ptr<FontFace> find(const std::string& family, int weight, bool italic);
        // sans-serif, serif, monospace, system-ui, ui-sans-serif, ui-serif, ui-monospace, cursive,
        // fantasy -> built-in "Roboto" (no serif/monospace face is built in; documented)
        static bool isGenericFamily(const std::string& family);
        static const std::vector<std::shared_ptr<FontFace>>& getFallbackFaces();
        static std::shared_ptr<FontFace> getBuiltInLatin();    // Roboto, family "Roboto"
        static std::shared_ptr<FontFace> getBuiltInArabic();   // Noto Sans Arabic
        // cache for STBText: the same file parsed once
        static std::shared_ptr<FontFace> getOrLoadFile(const std::string& path, int faceIndex = 0);
        static void clear();   // tests; Engine::clearPools does not touch it
    };

}

#endif //FONTREGISTRY_H
