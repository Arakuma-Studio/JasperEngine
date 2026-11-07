// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleRuntimeResources.h"

// the only includer: the arrays are defined once
#include "titleart_runtime_resources.h"

using namespace jasperengine;

std::string_view editor::titleJoyoKanjiText() {
    return std::string_view(reinterpret_cast<const char*>(title_runtime_joyo_kanji), sizeof(title_runtime_joyo_kanji) - 1);
}

std::string_view editor::titleOflText() {
    return std::string_view(reinterpret_cast<const char*>(title_runtime_ofl_1_1), sizeof(title_runtime_ofl_1_1) - 1);
}
