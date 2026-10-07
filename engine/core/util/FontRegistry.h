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

    // Process-wide font faces by family, weight and style. Family names are compared
    // case-insensitively with surrounding quotes and whitespace stripped. The built-in fonts
    // (Roboto Regular as family "Roboto", Noto Sans Arabic) are registered lazily on first use.
    class JASPERENGINE_API FontRegistry {
    public:
        // family empty -> face's own family; weight 0 -> face's own weight. A face registered
        // again under the same family, weight and style replaces the previous one.
        // fallback: also used, after the families a text asks for, for codepoints they lack.
        static void registerFace(std::shared_ptr<FontFace> face, const std::string& family = "",
                                 int weight = 0, int italic = -1 /* -1: from face */, bool fallback = false);
        // CSS font matching (CSS Fonts 4 §5.2 weight rules): exact style preferred, else the other
        // style; then weight: 400 -> 500 -> lighter desc -> heavier asc; 500 -> 400 -> lighter -> heavier;
        // < 400 -> lighter desc -> heavier asc; > 500 -> heavier asc -> lighter desc.
        // A generic family is looked up as "Roboto" (so a Roboto Bold loaded by the project
        // serves sans-serif bold). nullptr when the family has no face.
        static std::shared_ptr<FontFace> find(const std::string& family, int weight, bool italic);
        static bool hasFamily(const std::string& family);
        // sans-serif, serif, monospace, system-ui, ui-sans-serif, ui-serif, ui-monospace, cursive,
        // fantasy -> built-in "Roboto" (no serif/monospace face is built in; documented)
        static bool isGenericFamily(const std::string& family);
        // lowercase, quotes and surrounding whitespace removed
        static std::string normalizeFamily(const std::string& family);
        // faces registered with fallback = true, in registration order
        static const std::vector<std::shared_ptr<FontFace>>& getFallbackFaces();
        static std::shared_ptr<FontFace> getBuiltInLatin();    // Roboto, family "Roboto"
        static std::shared_ptr<FontFace> getBuiltInArabic();   // Noto Sans Arabic
        // cache for STBText: the same file parsed once while someone holds it. On failure
        // nullptr and `error` (see FontFace::loadFile), nothing logged.
        static std::shared_ptr<FontFace> getOrLoadFile(const std::string& path, int faceIndex = 0, std::string* error = nullptr);
        static void clear();   // tests; Engine::clearPools does not touch it
    };

}

#endif //FONTREGISTRY_H
