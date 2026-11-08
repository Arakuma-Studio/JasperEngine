// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S87's export_title formats (docs/specs/S87-title-localisation-interchange.md R35): one line per
// format module.

#include "TitleExport.h"
#include "png/TitlePngExport.h"
#include "sheet/TitleSheetExport.h"
#include "titleart/localisation/TitleStrings.h"

void jasperengine::editor::registerTitleExportFormats() {
    registerTitlePngExport();
    registerTitleKtx2Export();
    registerTitleSheetExport();
    registerTitleStringsExport();
}
