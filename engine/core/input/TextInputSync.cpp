// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "input/TextInputSync.h"

#include "util/StringUtils.h"

#include <algorithm>

using namespace jasperengine;

void jasperengine::utf16ToCodepoints(const std::u16string& units, std::u32string& text, std::vector<size_t>& unitToCodepoint){
    text.clear();
    text.reserve(units.size());
    unitToCodepoint.assign(units.size() + 1, 0);

    size_t i = 0;
    while (i < units.size()) {
        unitToCodepoint[i] = text.size();
        char32_t unit = units[i];
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < units.size() &&
                units[i + 1] >= 0xDC00 && units[i + 1] <= 0xDFFF) {
            text.push_back(0x10000 + ((unit - 0xD800) << 10) + (units[i + 1] - 0xDC00));
            unitToCodepoint[i + 1] = text.size();
            i += 2;
        } else {
            text.push_back((unit >= 0xD800 && unit <= 0xDFFF) ? 0xFFFD : unit);
            i += 1;
        }
    }
    unitToCodepoint[units.size()] = text.size();
}

std::string jasperengine::codepointsToUtf8(const std::u32string& text, size_t start, size_t end){
    std::string out;
    for (size_t i = start; i < end && i < text.size(); i++) {
        out += StringUtils::toUTF8(text[i]);
    }
    return out;
}

void TextInputSync::reset(const std::u32string& text, size_t caret){
    imeText = text;
    imeCaret = std::min(caret, text.size());
    imeCompStart = imeCompEnd = 0;
    imeCompCursor = -1;
}

// Edits the IME makes away from the caret (a suggestion replacing a word) first move the
// engine's caret there, one code point per step
void TextInputSync::moveCaretTo(size_t target, Output& output){
    if (target != imeCaret)
        output.moveCaret((int)target - (int)imeCaret);
    imeCaret = target;
}

void TextInputSync::update(const std::u16string& units, int32_t selectionStart, int32_t selectionEnd,
                           int32_t composingStart, int32_t composingEnd, Output& output){
    std::u32string text;
    std::vector<size_t> unitToCodepoint;
    utf16ToCodepoints(units, text, unitToCodepoint);

    const int32_t unitCount = (int32_t)units.size();
    auto codepointAt = [&](int32_t unit) {
        return unitToCodepoint[std::max(0, std::min(unit, unitCount))];
    };

    // the composing region in code points, none when undefined (-1) or empty
    size_t compStart = 0;
    size_t compEnd = 0;
    int32_t regionStart = composingStart;
    int32_t regionEnd = composingEnd;
    if (regionStart >= 0 && regionEnd >= 0 && regionStart != regionEnd) {
        compStart = codepointAt(std::min(regionStart, regionEnd));
        compEnd = codepointAt(std::max(regionStart, regionEnd));
    }
    const bool hasComp = compEnd > compStart;

    // the IME's caret: the end of its selection
    const size_t caret = (selectionEnd >= 0) ? codepointAt(selectionEnd) : text.size();
    const bool caretCollapsed = selectionEnd >= 0 && selectionStart == selectionEnd;
    const int compCursor = hasComp ? (int)(std::max(compStart, std::min(caret, compEnd)) - compStart) : -1;

    if (imeCompEnd > imeCompStart) {
        // the engine is composing: is the text around its composition unchanged?
        const size_t tail = imeText.size() - imeCompEnd;
        const bool anchored = text.size() >= imeCompStart + tail &&
                              text.compare(0, imeCompStart, imeText, 0, imeCompStart) == 0 &&
                              text.compare(text.size() - tail, tail, imeText, imeCompEnd, tail) == 0;
        const size_t replacedEnd = text.size() - tail;

        if (anchored && hasComp && compStart == imeCompStart && compEnd == replacedEnd) {
            // the same composition, updated
            if (text.compare(compStart, compEnd - compStart, imeText, imeCompStart, imeCompEnd - imeCompStart) != 0 ||
                    compCursor != imeCompCursor) {
                output.compose(codepointsToUtf8(text, compStart, compEnd), compCursor);
            }
            imeText = text;
            imeCompEnd = compEnd;
            imeCompCursor = compCursor;
            return;
        }

        if (anchored) {
            // the composition ended: what replaced it is committed, except a new composition
            // at its end, which follows the commit
            const bool nextComp = hasComp && compStart >= imeCompStart && compEnd == replacedEnd;
            const size_t commitEnd = nextComp ? compStart : replacedEnd;

            if (commitEnd > imeCompStart) {
                output.insertText(codepointsToUtf8(text, imeCompStart, commitEnd));
            } else {
                output.cancelComposition();
            }
            imeText = text;
            imeCaret = commitEnd;
            imeCompStart = imeCompEnd = 0;
            imeCompCursor = -1;

            if (nextComp) {
                output.compose(codepointsToUtf8(text, compStart, compEnd), compCursor);
                imeCompStart = compStart;
                imeCompEnd = compEnd;
                imeCompCursor = compCursor;
                return;
            }
        } else {
            // text outside the composition changed too: drop the composition, then diff the rest
            output.cancelComposition();
            imeText.erase(imeCompStart, imeCompEnd - imeCompStart);
            imeCaret = imeCompStart;
            imeCompStart = imeCompEnd = 0;
            imeCompCursor = -1;
        }
    }

    // one edit, taken as close to the caret as the text allows: the common prefix stops at
    // the caret, backspaces remove before it
    const size_t common = std::min(imeText.size(), text.size());
    size_t prefix = 0;
    while (prefix < std::min(common, imeCaret) && imeText[prefix] == text[prefix])
        prefix++;
    size_t suffix = 0;
    while (suffix < common - prefix && imeText[imeText.size() - 1 - suffix] == text[text.size() - 1 - suffix])
        suffix++;
    const size_t removedEnd = imeText.size() - suffix;
    const size_t insertedEnd = text.size() - suffix;

    if (removedEnd > prefix || insertedEnd > prefix) {
        moveCaretTo(removedEnd, output);
        for (size_t i = prefix; i < removedEnd; i++)
            output.backspace();
        imeCaret = prefix;

        // a composing region at the end of the new text is composed, the rest committed
        const bool newComp = hasComp && compStart >= prefix && compEnd == insertedEnd;
        const size_t commitEnd = newComp ? compStart : insertedEnd;
        if (commitEnd > prefix)
            output.insertText(codepointsToUtf8(text, prefix, commitEnd));
        imeCaret = commitEnd;

        if (newComp) {
            output.compose(codepointsToUtf8(text, compStart, compEnd), compCursor);
            imeCompStart = compStart;
            imeCompEnd = compEnd;
            imeCompCursor = compCursor;
        }
    }
    imeText = text;

    // the IME moved its caret (cursor keys on the keyboard, a swipe on the space bar)
    if (imeCompEnd == imeCompStart && caretCollapsed)
        moveCaretTo(caret, output);
}
