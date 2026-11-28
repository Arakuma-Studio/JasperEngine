// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUI_HTMLL10N_H
#define HTMLUI_HTMLL10N_H

// Contract (docs/specs/game-ui-localization.md R11, R12, R20, D7)
// - Keyed HTML: an element with data-l10n-id="ns.key" shows the message as its text (inserted as a
//   text node, never parsed as markup), data-l10n-args='{"count": 3}' gives its arguments,
//   data-l10n-attr-<name>="ns.key" sets that attribute. A key no table has keeps the authored text.
//   An element with child elements is left as written (HTMLUI-LOC-0001 at compile; a warning here).
// - The pass runs on the elements as RmlUi makes them (an RmlUi plugin): a document's load, the
//   rows a list-view / grid-view / tree-view / table-view instantiates, and inserted fragments
//   (innerHTML, insertAdjacentHTML) are all translated in the frame they appear, the compiled RML
//   stays language-free. A language change re-translates the same elements in place.
// - The root of a document with keyed elements gets the game language (-rmlui-language) and
//   -rmlui-direction rtl for right-to-left languages.
// - Without String Tables (Localization inactive) every hook returns at once.
// - Thread-safety: main thread (RmlUi's).

#include "Export.h"
#include "localization/Localization.h"
#include "ecs/Entity.h"

#include <string>
#include <utility>
#include <vector>

namespace Rml {
    class Element;
    class ElementDocument;
}

namespace jasperengine {
    class Scene;
}

namespace jasperengine::htmlui {

    class JASPERENGINE_API HtmlL10n {
    public:
        struct DocumentReport {
            std::string language;
            size_t keyed = 0;
            std::vector<std::pair<std::string, std::string>> literal;   // {ref, text}
            std::vector<Localization::Miss> missing;                    // `where` is the element's ref
        };

        static void install();
        static void uninstall();
        // the elements made since the last call (HtmlUIContext::update, after a load)
        static void update();
        // collects the document's keyed elements again and translates them (doc:translate())
        static void translateDocument(Rml::ElementDocument* document);
        // element:setLocalized(key, args): writes both attributes and translates now
        static void setLocalized(Rml::Element* element, const std::string& key, const std::string& argsJson);
        // the keyed elements, the literal text (not keyed, not under translate="no", not produced
        // by Localization) and the misses of one document (inspect_ui l10n, Localization.scan)
        static DocumentReport report(Rml::ElementDocument* document);
        // "#id" or a path of tag:nth-child steps from the nearest element with an id
        static std::string refOf(Rml::Element* element);
        // report() of a scene's UI Document, its keyed elements collected again first: what the
        // editor's tools call (the shared engine exports no symbol with an RmlUi type)
        static DocumentReport reportOf(Scene* scene, Entity entity);
    };

}

#endif //HTMLUI_HTMLL10N_H
