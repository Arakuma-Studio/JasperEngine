// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S87's registration (docs/specs/S87-title-localisation-interchange.md §6.2): the language and
// strings ops, then each importer of create.from and its ops, one line per module.

#include "TitleImport.h"
#include "titleart/localisation/TitleLanguageOps.h"
#include "titleart/interchange/svg/SvgImport.h"

void jasperengine::editor::registerTitleInterchange() {
    registerTitleLanguageOps();
    registerSvgInterchange();
}
