// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S82-title-designer.md R36-R39, §6.2)
// - Purpose: editing a Text layer's text in place on the Title Designer's canvas, with the caret,
//   the selection and the IME composition drawn over the real render.
// - Design (R37): an ImGui multi-line text field owns the text, the caret, the selection, the
//   clipboard and the IME. It is submitted invisibly (every colour transparent, 1 px) at a place
//   the window gives and kept focused while editing; TitleTextEdit reads its caret and selection
//   (ImGuiInputTextState, in bytes) and draws them on the canvas from S80's layout of the staged
//   text (cluster pens and advances, line boxes; horizontal lines and vertical columns), points
//   ImGui's IME position at the on-canvas caret (so candidate windows open there) and underlines
//   the composition (ImePreedit), which previewText() splices into the text in place of the
//   selection. The preview is staged, never committed.
// - Keys: Enter is a new line (the field), Ctrl+Enter commits, Esc cancels. While composing, Enter
//   and Esc belong to the input method: they never end the edit (the backends keep them away
//   from ImGui; frame() also locks them for that frame).
// - Mouse: the window routes canvas presses inside the edited layer to clickAt(); they are
//   resolved against the next frame()'s layout (a click before the first frame is kept).
// - The pure helpers below (UTF-8 indexes, words, the preedit splice, caret and selection
//   geometry, lettersRemoved) are what the unit tests check without a window.
// - Thread-safety: main thread (ImGui).

#pragma once

#include "titleart/designer/TitleDesignerTypes.h"
#include "titleart/text/TitleTextLayout.h"

#include "imgui.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor::titledesigner {

    // ---- UTF-8 (code point indexes are S80's CharAdjust keys and LaidOutCluster::index) ----

    // code points; an invalid sequence becomes U+FFFD and a stray continuation byte joins the code
    // point before it (S80's count), so the size always equals codepointCount()
    std::vector<uint32_t> decodeUtf8(const std::string& text);
    // S80's count (set_text): the bytes that do not continue a sequence
    size_t codepointCount(const std::string& text);
    // the number of code points that start before `byte` (a byte inside a sequence rounds up)
    int byteToCodepoint(const std::string& text, int byte);
    // the byte where code point `codepoint` starts (the text's size past the end)
    int codepointToByte(const std::string& text, int codepoint);

    // the word around a caret index (double-click, R36): a run of one class of characters
    // (letters and digits, hiragana, katakana, kanji, punctuation, spaces); the character after
    // the caret, or the one before it at a line end. [first, end) is empty at a line break.
    void wordAt(const std::vector<uint32_t>& codepoints, int index, int& first, int& end);

    // the text the canvas shows while composing: the selection [selectionStartByte,
    // selectionEndByte) (either order) replaced by the preedit
    struct PreeditSplice {
        std::string text;
        int preeditFirst = 0;   // code points of `text`: the composition [preeditFirst, preeditEnd)
        int preeditEnd = 0;
        int caret = 0;          // the input method's caret, a code point of `text`
    };
    PreeditSplice splicePreedit(const std::string& text, int selectionStartByte, int selectionEndByte,
                                const std::string& preedit, int preeditCaretByte);

    // how many character adjustments (a layer's `chars`, keyed by code point index) S80's set_text
    // drops for a text of `newLength` code points: the keys >= newLength. The window names the step
    // "Edit Text (2 Letter Adjustments Removed)" from it (R39).
    size_t lettersRemoved(const Json& chars, size_t newLength);

    // a segment in layer space
    struct LayerSegment {
        TitleVec2 a, b;
    };

    // Caret, selection and underline geometry of a text over its S80 layout, in layer space (y up).
    // A horizontal caret crosses its line box at the cluster's pen (end of a cluster: pen + advance;
    // inside a multi-code-point cluster: interpolated); a vertical caret crosses its column box at
    // the cell top (pen.y, advancing down). Lines are the text's '\n' lines; a layout a frame
    // behind the text (fewer lines, clusters past the end) is tolerated, indexes are clamped.
    class TextCaretGeometry {
    public:
        TextCaretGeometry() = default;
        TextCaretGeometry(const jasperengine::titleart::LaidOutText& layout, const std::string& text);

        bool empty() const { return lines.empty(); }
        bool vertical() const { return isVertical; }
        int codepoints() const { return total; }

        LayerSegment caret(int index) const;
        // one rectangle per line the range touches; a selected line break adds a stub after the
        // line's end (a quarter of the line's thickness)
        std::vector<TitleRect> selection(int first, int end) const;
        // at the bottom of each line box (vertical: the right side of each column)
        std::vector<LayerSegment> underline(int first, int end) const;
        // the caret index nearest a layer-space point (a click)
        int hitTest(TitleVec2 point) const { return hit(point, false); }
        // the character under a layer-space point (a double-click's word); past a line's last
        // character, that line's end
        int characterAt(TitleVec2 point) const { return hit(point, true); }

    private:
        struct Cluster {
            int index = 0;
            int count = 1;
            TitleVec2 pen;
            double advance = 0.0;
        };
        struct Line {
            int start = 0, end = 0;     // code points, without the break
            TitleRect box;
            std::vector<Cluster> clusters;  // by index
        };
        std::vector<Line> lines;
        bool isVertical = false;
        int total = 0;

        int hit(TitleVec2 point, bool character) const;
        int lineOf(int index) const;
        double along(const Line& line, int index) const;     // x (horizontal) or y (vertical)
        LayerSegment across(const Line& line, double at) const;
    };

    struct TextEditFrame {
        const jasperengine::titleart::LaidOutText* layout = nullptr;  // of the staged previewText(); null: no overlay
        Affine2 layerToDoc;     // the edited layer's space -> document space (its world transform)
        CanvasXform xform;      // document -> screen
        ImVec2 fieldPos;        // screen position of the invisible field (inside the canvas window)
    };

    class TitleTextEdit {
    public:
        enum class Result { None, Changed, Commit, Cancel };

        TitleTextEdit() = default;
        ~TitleTextEdit();
        TitleTextEdit(const TitleTextEdit&) = delete;
        TitleTextEdit& operator=(const TitleTextEdit&) = delete;

        // starts editing; `selectAll` selects the whole text (a new layer's placeholder, so typing
        // replaces it), else the caret goes to the end
        void begin(const std::string& layerId, const std::string& text, bool selectAll);
        void end();

        bool active() const { return editing; }
        const std::string& layerId() const { return layer; }
        const std::string& originalText() const { return original; }
        const std::string& text() const { return current; }
        // the field has the keyboard (the frame after begin() or after a click elsewhere)
        bool hasKeyboard() const { return keyboard; }
        // an IME composition into this field is in progress
        bool composing() const;
        // text() with the composition in place of the selection (what the canvas stages)
        std::string previewText() const;
        // the selected code points of text(), [first, end) (empty: the caret); for Selected Characters (R38)
        void selection(int& first, int& end) const;
        int caret() const;
        // selects code points [first, end) of text() (the Letters gizmo's test; Shift+arrows do it by hand)
        void select(int first, int end);
        // text() becomes the edit's original: a step committed mid-edit (the Letters gizmo) is not
        // undone by Esc
        void rebase() { original = current; }

        // Once per frame inside the canvas window while active(), after the canvas's own items:
        // submits the invisible field, resolves clicks, draws the selection, the caret and the
        // composition's underline into `canvasDrawList` (null: draws nothing) and sets ImGui's IME
        // position. Changed: previewText() changed (stage it); Commit: Ctrl+Enter (commit text());
        // Cancel: Esc (restore originalText()). The window calls end() after Commit or Cancel.
        Result frame(const TextEditFrame& frame, ImDrawList* canvasDrawList);

        // a press inside the edited layer: `extend` (Shift, or a drag) moves the caret and keeps
        // the anchor, `word` (double-click) selects the word; ignored while composing
        void clickAt(ImVec2 screen, bool extend, bool word);

        // test hooks (AC17): what a backend does when the user types, composes and converts
        void typeText(const std::string& utf8);
        void compose(const std::string& preedit, int caretByte);
        void commitComposition(const std::string& utf8);

    private:
        struct Click {
            ImVec2 screen;
            bool extend = false;
            bool word = false;
        };

        bool editing = false;
        bool keyboard = false;
        std::string layer;
        std::string original;
        std::string current;
        std::string lastPreview;
        int session = 0;            // the field's ID per edit, so no caret or undo carries over
        int anchorByte = 0;         // selection anchor and caret, bytes of `current` (ImGui's)
        int cursorByte = 0;
        bool pushSelection = false; // anchor / cursor go into the field when it next has a state
        std::vector<Click> clicks;

        void resolveClicks(const TextCaretGeometry& geometry, const TextEditFrame& frame);
    };

}
