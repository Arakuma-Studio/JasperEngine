// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ImePreedit.h"

#include <utility>

using jasperengine::editor::ImePreedit;

namespace {

    ImePreedit imePreeditCurrent;
    bool imePreeditInlineWanted = false;

}

const ImePreedit& ImePreedit::current() {
    return imePreeditCurrent;
}

void ImePreedit::set(ImePreedit preedit) {
    if (preedit.text.empty()) {
        clear();
        return;
    }
    int caret = preedit.caretByte;
    if (caret < 0) caret = 0;
    if (caret > (int)preedit.text.size()) caret = (int)preedit.text.size();
    // a caret inside a multi-byte sequence moves back to that code point's first byte
    while (caret > 0 && caret < (int)preedit.text.size() && ((unsigned char)preedit.text[caret] & 0xC0) == 0x80) caret--;
    preedit.caretByte = caret;
    preedit.active = true;
    imePreeditCurrent = std::move(preedit);
}

void ImePreedit::clear() {
    imePreeditCurrent = ImePreedit();
}

void ImePreedit::setInlineWanted(bool wanted) {
    imePreeditInlineWanted = wanted;
}

bool ImePreedit::inlineWanted() {
    return imePreeditInlineWanted;
}
