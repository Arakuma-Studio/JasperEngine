// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The canvas text editor of the Title Designer (docs/specs/S82-title-designer.md R36-R39, AC17):
// the pure helpers over synthetic S80 layouts (UTF-8 indexes, hit tests, words, caret, selection
// and underline geometry for horizontal and vertical text, the preedit splice, the letter
// adjustments set_text drops), and the editor itself in headless ImGui frames, driven as a
// backend would (typed characters, ImePreedit compositions, keys).

#include <gtest/gtest.h>

#include "titleart/designer/TitleTextEdit.h"
#include "titleart/TitleArtOps.h"
#include "titleart/TitleArtIO.h"
#include "util/ImePreedit.h"

#include "imgui_internal.h"

#include <string>
#include <vector>

using namespace jasperengine::editor::titledesigner;
using jasperengine::editor::ImePreedit;
namespace ta = jasperengine::titleart;

namespace {

    ta::LaidOutCluster cluster(uint32_t index, uint32_t line, double penX, double penY, double advance, int count = 1) {
        ta::LaidOutCluster c;
        c.index = index;
        c.line = line;
        c.codepoints.assign((size_t)count, (uint32_t)'x');
        c.pen = {penX, penY};
        c.advance = advance;
        return c;
    }

    ta::LaidOutLine line(uint32_t first, uint32_t count, TitleRect box) {
        ta::LaidOutLine l;
        l.firstCluster = first;
        l.clusterCount = count;
        l.box = box;
        return l;
    }

    // "AB\nCDE": 10 units per letter, baselines 0 and -25, line boxes 20 tall
    ta::LaidOutText horizontalLayout() {
        ta::LaidOutText t;
        t.clusters = {cluster(0, 0, 0, 0, 10), cluster(1, 0, 10, 0, 10),
                      cluster(3, 1, 0, -25, 10), cluster(4, 1, 10, -25, 10), cluster(5, 1, 20, -25, 10)};
        t.lines = {line(0, 2, {0, -5, 20, 20}), line(2, 3, {0, -30, 30, 20})};
        return t;
    }

    // "あい\nう": columns right to left, cells 10 tall from the column top (pen.y)
    ta::LaidOutText verticalLayout() {
        ta::LaidOutText t;
        t.vertical = true;
        t.clusters = {cluster(0, 0, 0, 0, 10), cluster(1, 0, 0, -10, 10), cluster(3, 1, -12, 0, 10)};
        t.lines = {line(0, 2, {-5, -20, 10, 20}), line(2, 1, {-17, -10, 10, 10})};
        return t;
    }

    void expectPoint(TitleVec2 p, double x, double y, const char* what) {
        EXPECT_NEAR(p.x, x, 1e-9) << what;
        EXPECT_NEAR(p.y, y, 1e-9) << what;
    }

    void expectRect(const TitleRect& r, double x, double y, double w, double h, const char* what) {
        EXPECT_NEAR(r.x, x, 1e-9) << what;
        EXPECT_NEAR(r.y, y, 1e-9) << what;
        EXPECT_NEAR(r.width, w, 1e-9) << what;
        EXPECT_NEAR(r.height, h, 1e-9) << what;
    }

    // one canvas window per frame, the canvas origin (document 0, 0) at (400, 300), zoom 1, the
    // layer at the origin
    struct Harness {
        ImGuiContext* previous = ImGui::GetCurrentContext();
        ImGuiContext* context = ImGui::CreateContext();
        const ta::LaidOutText* layout = nullptr;

        Harness() {
            ImGui::SetCurrentContext(context);
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.DisplaySize = ImVec2(800, 600);
            unsigned char* pixels;
            int width, height;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
            ImePreedit::clear();
        }

        ~Harness() {
            ImePreedit::clear();
            ImePreedit::setInlineWanted(false);
            ImGui::DestroyContext(context);
            ImGui::SetCurrentContext(previous);
        }

        TextEditFrame frameData() const {
            TextEditFrame f;
            f.layout = layout;
            f.xform.zoom = 1.0f;
            f.xform.origin = ImVec2(400, 300);
            f.fieldPos = ImVec2(20, 20);
            return f;
        }

        ImVec2 screen(double x, double y) const { return frameData().xform.toScreen({x, y}); }

        TitleTextEdit::Result frame(TitleTextEdit& edit) {
            ImGui::SetCurrentContext(context);
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2(800, 600));
            ImGui::Begin("Canvas");
            const TitleTextEdit::Result result = edit.frame(frameData(), ImGui::GetWindowDrawList());
            ImGui::End();
            ImGui::EndFrame();
            return result;
        }

        // begin() takes a frame to ask for the keyboard and one for the field to take it (one
        // more while a field that is no longer submitted lets go)
        void start(TitleTextEdit& edit, const std::string& text, bool selectAll = false) {
            edit.begin("00000001", text, selectAll);
            for (int i = 0; i < 5 && !edit.hasKeyboard(); i++) frame(edit);
        }

        // ctrl: the shortcut modifier, io.KeyCtrl (Cmd on macOS, where ConfigMacOSXBehaviors swaps
        // the two: a Ctrl event there is the Control key, io.KeySuper)
        TitleTextEdit::Result press(TitleTextEdit& edit, ImGuiKey key, bool ctrl = false) {
            ImGuiIO& io = ImGui::GetIO();
            const ImGuiKey shortcut = io.ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl;
            if (ctrl) io.AddKeyEvent(shortcut, true);
            io.AddKeyEvent(key, true);
            const TitleTextEdit::Result result = frame(edit);
            io.AddKeyEvent(key, false);
            if (ctrl) io.AddKeyEvent(shortcut, false);
            frame(edit);
            return result;
        }
    };

}

// ---- pure helpers ----

TEST(TitleTextEditTest, Utf8IndexesCountCodePointsLikeSetText) {
    const std::string text = "激熱A";
    // red control: counting every byte makes this 7 and every CharAdjust key past the kanji wrong
    EXPECT_EQ(codepointCount(text), 3u);
    const std::vector<uint32_t> cps = decodeUtf8(text);
    ASSERT_EQ(cps.size(), 3u);
    EXPECT_EQ(cps[0], 0x6FC0u);
    EXPECT_EQ(cps[1], 0x71B1u);
    EXPECT_EQ(cps[2], (uint32_t)'A');
    EXPECT_EQ(byteToCodepoint(text, 3), 1);
    EXPECT_EQ(byteToCodepoint(text, 4), 2) << "a byte inside 熱 rounds up";
    EXPECT_EQ(byteToCodepoint(text, 99), 3);
    EXPECT_EQ(codepointToByte(text, 1), 3);
    EXPECT_EQ(codepointToByte(text, 2), 6);
    EXPECT_EQ(codepointToByte(text, 9), 7);
    EXPECT_EQ(decodeUtf8("\xE6" "a").size(), codepointCount("\xE6" "a")) << "a cut sequence still counts as S80 counts";
}

TEST(TitleTextEditTest, HorizontalCaretSitsAtPensAndLineEnds) {
    const ta::LaidOutText layout = horizontalLayout();
    const TextCaretGeometry g(layout, "AB\nCDE");
    ASSERT_FALSE(g.empty());
    EXPECT_FALSE(g.vertical());
    LayerSegment s = g.caret(0);
    expectPoint(s.a, 0, -5, "caret 0 bottom");
    expectPoint(s.b, 0, 15, "caret 0 top");
    expectPoint(g.caret(1).a, 10, -5, "caret 1 at B's pen");
    // red control: placing a caret at the next cluster's pen puts the line end on line 1
    expectPoint(g.caret(2).a, 20, -5, "line end: A + B's advance");
    s = g.caret(3);
    expectPoint(s.a, 0, -30, "caret 3 starts line 1");
    expectPoint(s.b, 0, -10, "line 1's box");
    expectPoint(g.caret(6).a, 30, -30, "text end");
    expectPoint(g.caret(99).a, 30, -30, "clamped");
}

TEST(TitleTextEditTest, HorizontalHitTestPicksLineAndNearestEdge) {
    const ta::LaidOutText layout = horizontalLayout();
    const TextCaretGeometry g(layout, "AB\nCDE");
    EXPECT_EQ(g.hitTest({4, 0}), 0);
    // red control: truncating instead of rounding keeps a click on A's right half before A
    EXPECT_EQ(g.hitTest({6, 0}), 1);
    EXPECT_EQ(g.hitTest({25, 0}), 2) << "past line 0's end";
    EXPECT_EQ(g.hitTest({12, -20}), 4);
    EXPECT_EQ(g.hitTest({100, -100}), 6) << "below every line: the nearest one's end";
    EXPECT_EQ(g.hitTest({-50, 50}), 0);
    EXPECT_EQ(g.characterAt({16, 0}), 1) << "the character under the point, not the nearest edge";
    EXPECT_EQ(g.characterAt({4, 0}), 0);
}

TEST(TitleTextEditTest, SelectionCoversEachLineAndTheBreak) {
    const ta::LaidOutText layout = horizontalLayout();
    const TextCaretGeometry g(layout, "AB\nCDE");
    std::vector<TitleRect> rects = g.selection(4, 1);
    ASSERT_EQ(rects.size(), 2u);
    expectRect(rects[0], 10, -5, 15, 20, "B and a stub for the selected break");
    expectRect(rects[1], 0, -30, 10, 20, "C");
    rects = g.selection(2, 3);
    ASSERT_EQ(rects.size(), 1u) << "only the break";
    expectRect(rects[0], 20, -5, 5, 20, "the stub");
    EXPECT_TRUE(g.selection(2, 2).empty());
}

TEST(TitleTextEditTest, VerticalCaretSelectionAndHitTestRunDownColumns) {
    const ta::LaidOutText layout = verticalLayout();
    const TextCaretGeometry g(layout, "あい\nう");
    ASSERT_TRUE(g.vertical());
    LayerSegment s = g.caret(0);
    // red control: drawing a vertical caret like a horizontal one stands it up at x = pen.x
    expectPoint(s.a, -5, 0, "across the column at the cell top");
    expectPoint(s.b, 5, 0, "column right edge");
    expectPoint(g.caret(1).a, -5, -10, "down one cell");
    expectPoint(g.caret(2).a, -5, -20, "column end");
    expectPoint(g.caret(3).a, -17, 0, "next column (to the left) starts at its top");
    EXPECT_EQ(g.hitTest({0, -4}), 0);
    EXPECT_EQ(g.hitTest({0, -6}), 1);
    EXPECT_EQ(g.hitTest({0, -30}), 2);
    EXPECT_EQ(g.hitTest({-12, -3}), 3);
    EXPECT_EQ(g.hitTest({-12, -8}), 4);
    const std::vector<TitleRect> rects = g.selection(0, 2);
    ASSERT_EQ(rects.size(), 1u);
    expectRect(rects[0], -5, -20, 10, 20, "both cells");
    const std::vector<LayerSegment> lines = g.underline(0, 1);
    ASSERT_EQ(lines.size(), 1u);
    expectPoint(lines[0].a, 4.4, 0, "underline on the column's right side");
    expectPoint(lines[0].b, 4.4, -10, "one cell long");
}

TEST(TitleTextEditTest, EmptyLinesAndMultiCodePointClusters) {
    ta::LaidOutText layout;
    // "A\n\nB": the middle line has no cluster, its box zero wide at the alignment (x = 10)
    layout.clusters = {cluster(0, 0, 5, 0, 10), cluster(3, 2, 5, -50, 10)};
    layout.lines = {line(0, 1, {5, -5, 10, 20}), line(0, 0, {10, -30, 0, 20}), line(1, 1, {5, -55, 10, 20})};
    const TextCaretGeometry g(layout, "A\n\nB");
    expectPoint(g.caret(2).a, 10, -30, "the empty line's place");
    EXPECT_EQ(g.hitTest({40, -20}), 2);

    // a tate-chu-yoko pair is one cluster of two code points: the caret between them interpolates
    ta::LaidOutText pair;
    pair.clusters = {cluster(0, 0, 0, 0, 20, 2)};
    pair.lines = {line(0, 1, {0, -5, 20, 20})};
    const TextCaretGeometry p(pair, "12");
    expectPoint(p.caret(1).a, 10, -5, "half way");
    EXPECT_EQ(p.hitTest({11, 0}), 1);

    // a layout a frame behind the text (one line, the text already has two) still draws
    const ta::LaidOutText behind = [] {
        ta::LaidOutText t;
        t.clusters = {cluster(0, 0, 0, 0, 10)};
        t.lines = {line(0, 1, {0, -5, 10, 20})};
        return t;
    }();
    const TextCaretGeometry late(behind, "A\nB");
    EXPECT_FALSE(late.empty());
    expectPoint(late.caret(3).a, 5, -5, "the new line borrows the last box");
}

TEST(TitleTextEditTest, DoubleClickWordsFollowScriptRuns) {
    int first = 0, end = 0;
    const std::vector<uint32_t> mixed = decodeUtf8("激熱 Title!");
    wordAt(mixed, 4, first, end);
    EXPECT_EQ(first, 3);
    EXPECT_EQ(end, 8);
    wordAt(mixed, 0, first, end);
    EXPECT_EQ(first, 0) << "kanji run";
    EXPECT_EQ(end, 2);
    wordAt(mixed, 9, first, end);
    EXPECT_EQ(first, 8) << "at the end: the character before";
    EXPECT_EQ(end, 9);
    // red control: one class for every letter would select げきあつ激熱 as one word
    wordAt(decodeUtf8("げきあつ激熱"), 1, first, end);
    EXPECT_EQ(first, 0);
    EXPECT_EQ(end, 4);
    wordAt(decodeUtf8("ab\ncd"), 2, first, end);
    EXPECT_EQ(first, 0) << "at a line end: the word before";
    EXPECT_EQ(end, 2);
    wordAt(decodeUtf8("a\n\nb"), 2, first, end);
    EXPECT_EQ(first, end) << "an empty line selects nothing";
}

TEST(TitleTextEditTest, PreeditReplacesTheSelectionWithItsCaretAndRange) {
    PreeditSplice s = splicePreedit("激熱", 3, 3, "げきあつ", 6);
    EXPECT_EQ(s.text, "激げきあつ熱");
    EXPECT_EQ(s.preeditFirst, 1);
    EXPECT_EQ(s.preeditEnd, 5);
    EXPECT_EQ(s.caret, 3) << "after げき";
    s = splicePreedit("激熱", 6, 0, "げき", 0);
    EXPECT_EQ(s.text, "げき") << "a selection in either order is replaced";
    EXPECT_EQ(s.preeditFirst, 0);
    EXPECT_EQ(s.preeditEnd, 2);

    // the underline covers the composition in the spliced text's layout
    const ta::LaidOutText layout = horizontalLayout();
    const TextCaretGeometry g(layout, "AB\nCDE");
    const std::vector<LayerSegment> lines = g.underline(1, 4);
    ASSERT_EQ(lines.size(), 2u);
    expectPoint(lines[0].a, 10, -3.8, "line 0 from B");
    expectPoint(lines[0].b, 20, -3.8, "to its end");
    expectPoint(lines[1].a, 0, -28.8, "line 1 from C");
    expectPoint(lines[1].b, 10, -28.8, "to D");
}

TEST(TitleTextEditTest, LettersRemovedMatchesSetText) {
    const ta::Json chars = {{"0", {{"rotation", 1}}}, {"2", {{"rotation", 2}}}, {"3", {{"rotation", 3}}}, {"7", {{"rotation", 4}}}};
    // red control: `>` instead of `>=` keeps key 3 of a 3-letter text
    EXPECT_EQ(lettersRemoved(chars, 3), 2u);
    EXPECT_EQ(lettersRemoved(chars, 8), 0u);
    EXPECT_EQ(lettersRemoved(ta::Json(), 0), 0u);

    // the same count as S80's set_text drops
    const ta::TitleArtParseResult parsed = ta::parseTitleArt(R"({"format": "jasperengine.titleart", "version": 1,
        "layers": [{"id": "00000001", "type": "text", "name": "Title", "text": "激熱ABCD"}]})");
    ASSERT_TRUE(parsed.ok) << parsed.error.toString();
    using jasperengine::editor::TitleArtOps;
    using jasperengine::editor::TitleOpsResult;
    const TitleOpsResult adjusted = TitleArtOps::apply(parsed.doc, ta::Json{{"op", "set"}, {"layer", "Title"},
        {"values", {{"chars.1.rotation", 5}, {"chars.3.rotation", 6}, {"chars.5.rotation", 7}}}});
    ASSERT_TRUE(adjusted.ok) << adjusted.error();
    const std::string newText = "激熱A";
    const TitleOpsResult edited = TitleArtOps::apply(adjusted.doc, ta::Json{{"op", "set_text"}, {"layer", "Title"}, {"text", newText}});
    ASSERT_TRUE(edited.ok) << edited.error();
    size_t dropped = 0;
    for (const auto& change : edited.changes) dropped += change.path.rfind("chars.", 0) == 0 && change.to.is_null();
    EXPECT_EQ(dropped, 2u);
    EXPECT_EQ(lettersRemoved(adjusted.doc.layers()[0]["chars"], codepointCount(newText)), dropped);
}

// ---- the editor in headless ImGui frames ----

TEST(TitleTextEditTest, TypingChangesTheTextAndThePlaceholderIsReplaced) {
    Harness ui;
    TitleTextEdit edit;
    ui.start(edit, "Win");
    ASSERT_TRUE(edit.hasKeyboard()) << "the invisible field takes the keyboard";
    edit.typeText("激熱");
    EXPECT_EQ(ui.frame(edit), TitleTextEdit::Result::Changed);
    EXPECT_EQ(edit.text(), "Win激熱") << "the caret starts at the end";
    EXPECT_EQ(edit.originalText(), "Win");
    EXPECT_EQ(edit.caret(), 5);
    EXPECT_EQ(ui.press(edit, ImGuiKey_Enter), TitleTextEdit::Result::Changed);
    EXPECT_EQ(edit.text(), "Win激熱\n") << "Enter is a new line";
    edit.end();

    // red control: ignoring selectAll appends to the placeholder ("TextHi")
    TitleTextEdit added;
    ui.start(added, "Text", true);
    ASSERT_TRUE(added.hasKeyboard());
    added.typeText("Hi");
    ui.frame(added);
    EXPECT_EQ(added.text(), "Hi");
}

TEST(TitleTextEditTest, CompositionShowsInThePreviewOnlyUntilCommitted) {
    Harness ui;
    TitleTextEdit edit;
    ui.start(edit, "激熱");
    edit.compose("げきあつ", 6);
    EXPECT_EQ(ui.frame(edit), TitleTextEdit::Result::Changed) << "the preview changed: the window restages";
    EXPECT_TRUE(edit.composing());
    EXPECT_EQ(edit.text(), "激熱") << "a composition never edits the text";
    EXPECT_EQ(edit.previewText(), "激熱げきあつ");
    int first = 0, end = 0;
    edit.selection(first, end);
    EXPECT_EQ(first, end);

    edit.commitComposition("激熱");
    EXPECT_EQ(ui.frame(edit), TitleTextEdit::Result::Changed);
    EXPECT_FALSE(edit.composing());
    EXPECT_EQ(edit.text(), "激熱激熱");
    EXPECT_EQ(edit.previewText(), edit.text());
}

TEST(TitleTextEditTest, EnterAndEscDuringCompositionBelongToTheIme) {
    Harness ui;
    TitleTextEdit edit;
    ui.start(edit, "Win");
    edit.compose("げき", 6);
    ui.frame(edit);
    // red control: without the composing gate Ctrl+Enter commits; without the key lock Enter
    // reaches the field and adds a new line
    EXPECT_EQ(ui.press(edit, ImGuiKey_Enter, true), TitleTextEdit::Result::None);
    EXPECT_EQ(ui.press(edit, ImGuiKey_Enter), TitleTextEdit::Result::None);
    EXPECT_EQ(ui.press(edit, ImGuiKey_Escape), TitleTextEdit::Result::None);
    EXPECT_EQ(edit.text(), "Win");
    EXPECT_TRUE(edit.active());
    EXPECT_TRUE(edit.composing());
}

TEST(TitleTextEditTest, CtrlEnterCommitsAndEscCancels) {
    Harness ui;
    TitleTextEdit edit;
    ui.start(edit, "Win");
    edit.typeText("ner");
    ui.frame(edit);
    ASSERT_EQ(edit.text(), "Winner");
    EXPECT_EQ(ui.press(edit, ImGuiKey_Enter, true), TitleTextEdit::Result::Commit);
    EXPECT_EQ(edit.text(), "Winner") << "no new line";
    edit.end();

    TitleTextEdit other;
    ui.start(other, "Win");
    ASSERT_TRUE(other.hasKeyboard());
    other.typeText("ner");
    ui.frame(other);
    EXPECT_EQ(ui.press(other, ImGuiKey_Escape), TitleTextEdit::Result::Cancel);
    EXPECT_EQ(other.originalText(), "Win") << "what the window restores";
    other.end();
    EXPECT_FALSE(other.active());
    EXPECT_FALSE(ImePreedit::inlineWanted());
}

TEST(TitleTextEditTest, ClicksPlaceTheCaretSelectWordsAndPointTheIme) {
    Harness ui;
    const ta::LaidOutText layout = horizontalLayout();
    ui.layout = &layout;
    TitleTextEdit edit;
    ui.start(edit, "AB\nCDE");
    int first = 0, end = 0;

    edit.clickAt(ui.screen(6, 0), false, false);
    ui.frame(edit);
    EXPECT_EQ(edit.caret(), 1);
    edit.selection(first, end);
    EXPECT_EQ(first, 1);
    EXPECT_EQ(end, 1);

    edit.clickAt(ui.screen(22, -20), false, true);
    ui.frame(edit);
    edit.selection(first, end);
    EXPECT_EQ(first, 3) << "double-click on E selects CDE";
    EXPECT_EQ(end, 6);

    edit.clickAt(ui.screen(6, 0), true, false);
    ui.frame(edit);
    edit.selection(first, end);
    EXPECT_EQ(first, 1) << "Shift+click keeps the anchor";
    EXPECT_EQ(end, 3);

    // candidate windows open at the on-canvas caret: line 0's end (x 420), its box top (y 300 - 15)
    // (red control: without the override ImGui's own position at the invisible field stays)
    edit.clickAt(ui.screen(16, 0), false, false);
    ui.frame(edit);
    const ImGuiPlatformImeData& ime = ImGui::GetCurrentContext()->PlatformImeData;
    EXPECT_TRUE(ime.WantVisible);
    EXPECT_NEAR(ime.InputPos.x, 420.0f, 1e-3f);
    EXPECT_NEAR(ime.InputPos.y, 285.0f, 1e-3f);
    EXPECT_NEAR(ime.InputLineHeight, 20.0f, 1e-3f);
    EXPECT_TRUE(ImePreedit::inlineWanted()) << "the backends suppress their composition window";
}
