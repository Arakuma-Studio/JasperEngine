// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// HtmlUIClock's element side: a document's Update Mode is its RmlUi animation clock. Apart from
// HtmlUIClock.cpp, which a build without the HTML UI keeps (Engine advances the clock).

#include "HtmlUIClock.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>

using namespace jasperengine;

double HtmlUIClock::now(Rml::Element* element){
    return now(modeOf(element));
}

UIDocumentUpdateMode HtmlUIClock::modeOf(Rml::Element* element){
    Rml::ElementDocument* document = element ? element->GetOwnerDocument() : nullptr;
    return document ? modeOfAnimationClock(document->GetAnimationClock()) : UIDocumentUpdateMode::UNSCALED_TIME;
}
