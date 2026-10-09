// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_RUNTIME_LUCIDEICONS_H
#define COMPAT_RUNTIME_LUCIDEICONS_H

#include "Export.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::htmlui::compat {

    // One attribute of an element or of the <svg> made for it, in order.
    struct LucideAttribute {
        std::string name;
        std::string value;

        bool operator==(const LucideAttribute& other) const { return name == other.name && value == other.value; }
    };

    // The built-in Lucide icons (phase-2 §3.11.8): engine/libs/lucide/lucide.pack, built by
    // tools/htmlui/build_lucide_pack.py from a Lucide release and embedded at build time
    // (htmlui_lucide.h). The pack is inflated on first use (stbi_zlib_decode_malloc, once,
    // thread-safe); names are looked up as lucide.createIcons() does: the name converted to
    // PascalCase against the icons and their aliases, so "arrow-right", "ArrowRight" and
    // "arrowRight" are the same icon, and "home" (an alias) is "house".
    class JASPERENGINE_API LucideIcons {
    public:
        // the embedded pack inflated and valid (false in builds with the empty stand-in header)
        static bool available();
        static const std::string& version();          // the Lucide release tag ("1.53.0"); "" when unavailable
        static size_t iconCount();
        static size_t aliasCount();
        static std::vector<std::string> names();      // the icons' kebab-case names, sorted

        // The canonical (kebab-case) icon name for a name as an author writes it (an icon, an
        // alias, PascalCase or camelCase); "" when unknown.
        static std::string resolve(std::string_view name);
        // The icon's inner shapes (`<path d="..."/>...`, Lucide's root attributes stripped).
        static std::optional<std::string_view> find(std::string_view name);
        // The `count` icon names closest to `name` (edit distance on the kebab-case form, then
        // alphabetical), for HTMLUI-HTML-0021 suggestions.
        static std::vector<std::string> closest(std::string_view name, size_t count = 3);

        // The root <svg> attributes lucide.createIcons() gives an element with
        // data-lucide="name" (packages/lucide/src/replaceElement.ts): Lucide's defaults, then
        // data-lucide, then aria-hidden="true" unless the element has an aria-*, role or title
        // attribute, then the element's own attributes over them (same order and overriding
        // rules as the JavaScript object spread), and class = "lucide lucide-<name> <element
        // classes>" without duplicates. `name` is the data-lucide value as written.
        static std::vector<LucideAttribute> svgAttributes(std::string_view name, const std::vector<LucideAttribute>& elementAttributes);
        // The whole <svg> replacing such an element; nullopt for an unknown icon. Attribute
        // values are escaped (& < > ").
        static std::optional<std::string> svgMarkup(std::string_view name, const std::vector<LucideAttribute>& elementAttributes = {});

        // tests: how many times the embedded pack was inflated (1 after first use)
        static size_t inflateCount();
        // tests: whether bytes are a valid pack (the reason in `error` otherwise)
        static bool validatePack(const unsigned char* data, size_t size, std::string* error = nullptr);
        // The htmlui_lucide.h header of the embedded pack, byte for byte what
        // engine/libs/lucide/embed_lucide.cmake generates (the editor's Apple export writes it).
        static std::string buildEmbeddedHeader();
        static std::string buildEmbeddedHeader(const unsigned char* data, size_t size);

        // Lucide's name conversions (@lucide/shared): toPascalCase and toKebabCase
        static std::string toPascalCase(std::string_view name);
        static std::string toKebabCase(std::string_view name);
    };

}

#endif //COMPAT_RUNTIME_LUCIDEICONS_H
