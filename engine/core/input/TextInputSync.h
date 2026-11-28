// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef TEXTINPUTSYNC_H
#define TEXTINPUTSYNC_H

// A soft keyboard that keeps the field's text (Android's GameTextInput, the iOS text field) reports
// snapshots: the whole text, its selection and composing region. TextInputSync turns each snapshot
// into the edits the engine's field must make to follow it (text-entry spec D3), in order: caret
// moves, backspaces, inserted text, IME composition. The edits go through the field's own input
// path, so its length limit and filters stay its own; the engine then sends the field's real text
// back when it differs (RmlTextInputHandler, System::updateVirtualKeyboardText) and calls reset.
// Plain C++: the core tests run it on any host (TextInputSyncTest).

#include "Export.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine {

    // Code points of UTF-16 units. unitToCodepoint has units.size() + 1 entries: the code point
    // index at each unit (the second unit of a pair maps past it). Lone surrogates become U+FFFD.
    JASPERENGINE_API void utf16ToCodepoints(const std::u16string& units, std::u32string& text, std::vector<size_t>& unitToCodepoint);
    // UTF-8 of the code points [start, end)
    JASPERENGINE_API std::string codepointsToUtf8(const std::u32string& text, size_t start, size_t end);

    class JASPERENGINE_API TextInputSync {
    public:
        // what the field is asked to do; text in UTF-8, counts in code points
        class Output {
        public:
            virtual ~Output() = default;
            // one code point to the right (positive) or left (negative) per step
            virtual void moveCaret(int steps) = 0;
            // removes the code point before the caret
            virtual void backspace() = 0;
            // commits text at the caret (a commit while composing replaces the composition)
            virtual void insertText(const std::string& utf8) = 0;
            // starts or updates the composition; cursor in code points from its start
            virtual void compose(const std::string& utf8, int cursor) = 0;
            // drops the composition, restoring the text it replaced
            virtual void cancelComposition() = 0;
        };

        // starts over from this text (code points), caret at `caret`, nothing composed: the
        // keyboard was given the field's text (opening, a resync)
        void reset(const std::u32string& text, size_t caret);

        // a snapshot of the keyboard: UTF-16 units, the selection and the composing region in
        // units (-1: undefined; an empty region is none). Edits outside the composing region are
        // diffed against the text the engine has, as close to the caret as the text allows:
        // removals become backspaces, additions inserted text. The composing region is the
        // engine's composition; when it ends, the text that replaced it is committed, or the
        // composition is cancelled when nothing replaced it. A region the IME puts over text the
        // engine already has (Gboard re-selecting a word) is not a composition for the engine:
        // later edits in it are plain text edits.
        void update(const std::u16string& units, int32_t selectionStart, int32_t selectionEnd,
                    int32_t composingStart, int32_t composingEnd, Output& output);

        // the keyboard's text as the engine has it, composition included, in code points
        const std::u32string& text() const { return imeText; }
        // where the engine's caret is taken to be when nothing is composed
        size_t caret() const { return imeCaret; }
        bool isComposing() const { return imeCompEnd > imeCompStart; }

    private:
        void moveCaretTo(size_t target, Output& output);

        std::u32string imeText;
        size_t imeCaret = 0;
        // the engine's composition (empty when none) and its cursor
        size_t imeCompStart = 0;
        size_t imeCompEnd = 0;
        int imeCompCursor = -1;
    };

}

#endif //TEXTINPUTSYNC_H
