// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_RUNTIME_HTMLANCHORELEMENT_H
#define COMPAT_RUNTIME_HTMLANCHORELEMENT_H

#include "Export.h"

#include <RmlUi/Core/Element.h>

#include <string>
#include <string_view>

namespace jasperengine::htmlui::compat {

    // How an anchor's href is followed (no browser: the document decides through htmlnavigate)
    struct AnchorTarget {
        enum class Kind : uint8_t { None, Fragment, Document, Resource, External };
        Kind kind = Kind::None;
        std::string href;       // as written
        std::string path;       // Document/Resource: the asset-relative file; External: the URL
        std::string fragment;   // without '#'
    };

    // <a> (phase-2 §3.11.2): focusable when it has an href (user-agent sheet); Enter activates
    // it like a click; the click's default action (unless prevented):
    // - "#id" scrolls the element with that id into view ("#" and "#top": the document's top);
    // - anything else dispatches `htmlnavigate` on the document (bubbles, cancelable) with the
    //   parameters href (as written), url (asset-relative file, or the URL), fragment,
    //   document (true for .html/.htm: UIDocument's default action loads it in the same
    //   component, hook H4) and external (http(s), protocol-relative and other schemes: no
    //   browser, HTMLUI-HTML-0023 at load).
    class JASPERENGINE_INTERNAL HtmlAnchorElement : public Rml::Element {
    public:
        RMLUI_RTTI_DeclareWithParent(HtmlAnchorElement, Rml::Element)

        explicit HtmlAnchorElement(const Rml::String& tag);
        ~HtmlAnchorElement() override;

        // an href resolved against the document's path (asset-relative)
        static AnchorTarget resolve(std::string_view href, std::string_view documentPath);
        AnchorTarget getTarget();

        // what a click does (the default action), public for scripts and tests
        void follow();

    protected:
        void ProcessDefaultAction(Rml::Event& event) override;
    };

}

#endif //COMPAT_RUNTIME_HTMLANCHORELEMENT_H
