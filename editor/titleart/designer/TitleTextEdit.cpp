// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleTextEdit.h"

#include "util/ImePreedit.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <initializer_list>
#include <utility>

using namespace jasperengine::editor::titledesigner;
using jasperengine::editor::ImePreedit;
namespace ta = jasperengine::titleart;

namespace {

    constexpr const char* FIELD_LABEL = "##title_text_edit";
    // small enough that the canvas keeps its mouse input, large enough for ImGui to lay it out
    constexpr float FIELD_SIZE = 1.0f;
    // a selected line break shows as a stub this fraction of the line's thickness
    constexpr double BREAK_STUB = 0.25;
    // the underline sits this fraction of the line's thickness inside its box
    constexpr double UNDERLINE_INSET = 0.06;

    bool continuation(unsigned char c) {
        return (c & 0xC0) == 0x80;
    }

    enum class CharClass { Space, Break, Punct, Hiragana, Katakana, Han, Word };

    CharClass classOf(uint32_t cp) {
        if (cp == '\n' || cp == '\r') return CharClass::Break;
        if (cp == ' ' || cp == '\t' || cp == 0x00A0 || cp == 0x3000 || (cp >= 0x2000 && cp <= 0x200B)) return CharClass::Space;
        if (cp >= 0x3040 && cp <= 0x309F) return CharClass::Hiragana;
        if (cp == 0x30A0 || cp == 0x30FB) return CharClass::Punct;   // ゠ ・ sit in the katakana block
        if ((cp >= 0x30A0 && cp <= 0x30FF) || (cp >= 0x31F0 && cp <= 0x31FF) || (cp >= 0xFF66 && cp <= 0xFF9F)) return CharClass::Katakana;
        if ((cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
            (cp >= 0x20000 && cp <= 0x3FFFF) || (cp >= 0x3005 && cp <= 0x3007)) return CharClass::Han;
        if (cp < 0x80) return (std::isalnum((int)cp) || cp == '_') ? CharClass::Word : CharClass::Punct;
        if ((cp >= 0x3000 && cp <= 0x303F) || (cp >= 0xFF01 && cp <= 0xFF0F) || (cp >= 0xFF1A && cp <= 0xFF20) ||
            (cp >= 0xFF3B && cp <= 0xFF40) || (cp >= 0xFF5B && cp <= 0xFF65) || (cp >= 0x2010 && cp <= 0x205E) ||
            (cp >= 0x00A1 && cp <= 0x00BF && cp != 0x00AA && cp != 0x00B5 && cp != 0x00BA)) return CharClass::Punct;
        return CharClass::Word;
    }

    int resizeField(ImGuiInputTextCallbackData* data) {
        if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
            auto* text = static_cast<std::string*>(data->UserData);
            text->resize((size_t)data->BufTextLen);
            data->Buf = text->data();
        }
        return 0;
    }

    void sorted(int& a, int& b) {
        if (a > b) std::swap(a, b);
    }

}

// ---- UTF-8 ----

std::vector<uint32_t> jasperengine::editor::titledesigner::decodeUtf8(const std::string& text) {
    std::vector<uint32_t> out;
    size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = (unsigned char)text[i];
        if (continuation(c)) {
            // a stray continuation byte: S80's count skips it, so it joins the previous code point
            i++;
            continue;
        }
        int extra = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        uint32_t cp = extra == 0 ? c : extra == 1 ? (c & 0x1Fu) : extra == 2 ? (c & 0x0Fu) : (c & 0x07u);
        size_t j = i + 1;
        int got = 0;
        while (extra > 0 && got < extra && j < text.size() && continuation((unsigned char)text[j])) {
            cp = (cp << 6) | ((unsigned char)text[j] & 0x3Fu);
            j++;
            got++;
        }
        if (extra < 0 || got < extra) cp = 0xFFFD;
        // continuation bytes past the sequence belong to it too (they start nothing)
        while (j < text.size() && continuation((unsigned char)text[j])) j++;
        out.push_back(cp);
        i = j;
    }
    return out;
}

size_t jasperengine::editor::titledesigner::codepointCount(const std::string& text) {
    size_t count = 0;
    for (unsigned char c : text) count += !continuation(c);
    return count;
}

int jasperengine::editor::titledesigner::byteToCodepoint(const std::string& text, int byte) {
    const int limit = std::clamp(byte, 0, (int)text.size());
    int count = 0;
    for (int i = 0; i < limit; i++) count += !continuation((unsigned char)text[i]);
    return count;
}

int jasperengine::editor::titledesigner::codepointToByte(const std::string& text, int codepoint) {
    if (codepoint <= 0) return 0;
    int count = 0;
    for (int i = 0; i < (int)text.size(); i++) {
        if (continuation((unsigned char)text[i])) continue;
        if (count == codepoint) return i;
        count++;
    }
    return (int)text.size();
}

// ---- words ----

void jasperengine::editor::titledesigner::wordAt(const std::vector<uint32_t>& codepoints, int index, int& first, int& end) {
    const int n = (int)codepoints.size();
    index = std::clamp(index, 0, n);
    first = end = index;
    if (n == 0) return;
    int i = index;
    // at the end of the text or of a line, the word before the caret
    if (i == n || (classOf(codepoints[i]) == CharClass::Break && i > 0 && classOf(codepoints[i - 1]) != CharClass::Break)) i--;
    const CharClass cls = classOf(codepoints[i]);
    if (cls == CharClass::Break) return;
    first = i;
    end = i + 1;
    while (first > 0 && classOf(codepoints[first - 1]) == cls) first--;
    while (end < n && classOf(codepoints[end]) == cls) end++;
}

// ---- preedit ----

PreeditSplice jasperengine::editor::titledesigner::splicePreedit(const std::string& text, int selectionStartByte, int selectionEndByte,
                                                                       const std::string& preedit, int preeditCaretByte) {
    int a = codepointToByte(text, byteToCodepoint(text, selectionStartByte));
    int b = codepointToByte(text, byteToCodepoint(text, selectionEndByte));
    sorted(a, b);
    PreeditSplice out;
    out.text = text.substr(0, (size_t)a) + preedit + text.substr((size_t)b);
    out.preeditFirst = byteToCodepoint(text, a);
    out.preeditEnd = out.preeditFirst + (int)codepointCount(preedit);
    out.caret = out.preeditFirst + byteToCodepoint(preedit, preeditCaretByte);
    return out;
}

// ---- letters ----

size_t jasperengine::editor::titledesigner::lettersRemoved(const Json& chars, size_t newLength) {
    if (!chars.is_object()) return 0;
    size_t removed = 0;
    for (auto it = chars.begin(); it != chars.end(); ++it) {
        const std::string& key = it.key();
        if (key.empty() || !std::all_of(key.begin(), key.end(), [](char c) { return c >= '0' && c <= '9'; })) continue;
        // a key longer than any text is past every length
        if (key.size() > 18 || std::stoull(key) >= newLength) removed++;
    }
    return removed;
}

// ---- geometry ----

TextCaretGeometry::TextCaretGeometry(const ta::LaidOutText& layout, const std::string& text) {
    isVertical = layout.vertical;
    const std::vector<uint32_t> cps = decodeUtf8(text);
    total = (int)cps.size();
    if (layout.lines.empty()) return;

    // S80's lines: split at '\n', a CR before it belongs to the break
    int start = 0;
    for (int i = 0; i <= total; i++) {
        if (i == total || cps[(size_t)i] == '\n') {
            Line line;
            line.start = start;
            line.end = (i > start && i < total && cps[(size_t)i - 1] == '\r') ? i - 1 : i;
            line.box = layout.lines[std::min(lines.size(), layout.lines.size() - 1)].box;
            lines.push_back(std::move(line));
            start = i + 1;
        }
    }
    for (const ta::LaidOutCluster& c : layout.clusters) {
        if (c.line >= lines.size()) continue;
        Cluster cluster;
        cluster.index = (int)c.index;
        cluster.count = std::max(1, (int)c.codepoints.size());
        cluster.pen = c.pen;
        cluster.advance = c.advance;
        lines[c.line].clusters.push_back(cluster);
    }
    for (Line& line : lines) {
        std::sort(line.clusters.begin(), line.clusters.end(), [](const Cluster& a, const Cluster& b) { return a.index < b.index; });
    }
}

int TextCaretGeometry::lineOf(int index) const {
    for (size_t i = 0; i < lines.size(); i++) {
        // the break characters after a line's end belong to that line
        const int next = i + 1 < lines.size() ? lines[i + 1].start : total + 1;
        if (index < next) return (int)i;
    }
    return lines.empty() ? 0 : (int)lines.size() - 1;
}

double TextCaretGeometry::along(const Line& line, int index) const {
    const double dir = isVertical ? -1.0 : 1.0;   // vertical text advances down (y up)
    const auto at = [&](const Cluster& c, double fraction) {
        return (isVertical ? c.pen.y : c.pen.x) + dir * c.advance * fraction;
    };
    if (line.clusters.empty()) {
        // an empty line: its box is the line's place (horizontal: zero wide at the alignment;
        // vertical: the column, starting at its top)
        return isVertical ? line.box.y + line.box.height : line.box.x + line.box.width / 2;
    }
    if (index <= line.clusters.front().index) return at(line.clusters.front(), 0.0);
    const Cluster* before = &line.clusters.front();
    for (const Cluster& c : line.clusters) {
        if (index >= c.index && index < c.index + c.count) return at(c, (double)(index - c.index) / c.count);
        if (c.index + c.count <= index) before = &c;
    }
    return at(*before, 1.0);
}

LayerSegment TextCaretGeometry::across(const Line& line, double at) const {
    if (isVertical) return {{line.box.x, at}, {line.box.x + line.box.width, at}};
    return {{at, line.box.y}, {at, line.box.y + line.box.height}};
}

LayerSegment TextCaretGeometry::caret(int index) const {
    if (lines.empty()) return {};
    index = std::clamp(index, 0, total);
    const Line& line = lines[(size_t)lineOf(index)];
    return across(line, along(line, std::min(index, line.end)));
}

std::vector<TitleRect> TextCaretGeometry::selection(int first, int end) const {
    std::vector<TitleRect> out;
    sorted(first, end);
    first = std::clamp(first, 0, total);
    end = std::clamp(end, 0, total);
    if (first == end) return out;
    for (size_t i = 0; i < lines.size(); i++) {
        const Line& line = lines[i];
        const int a = std::max(first, line.start), b = std::min(end, line.end);
        const bool breakSelected = i + 1 < lines.size() && first < lines[i + 1].start && end > line.end;
        if (a >= b && !breakSelected) continue;
        double p0 = a < b ? along(line, a) : along(line, line.end);
        double p1 = a < b ? along(line, b) : p0;
        if (breakSelected) {
            const double thickness = isVertical ? line.box.width : line.box.height;
            p1 += (isVertical ? -1.0 : 1.0) * thickness * BREAK_STUB;
        }
        if (p0 > p1) std::swap(p0, p1);
        if (isVertical) out.push_back({line.box.x, p0, line.box.width, p1 - p0});
        else out.push_back({p0, line.box.y, p1 - p0, line.box.height});
    }
    return out;
}

std::vector<LayerSegment> TextCaretGeometry::underline(int first, int end) const {
    std::vector<LayerSegment> out;
    sorted(first, end);
    for (const Line& line : lines) {
        const int a = std::max(first, line.start), b = std::min(end, line.end);
        if (a >= b) continue;
        const double p0 = along(line, a), p1 = along(line, b);
        if (isVertical) {
            const double x = line.box.x + line.box.width * (1.0 - UNDERLINE_INSET);
            out.push_back({{x, p0}, {x, p1}});
        } else {
            const double y = line.box.y + line.box.height * UNDERLINE_INSET;
            out.push_back({{p0, y}, {p1, y}});
        }
    }
    return out;
}

int TextCaretGeometry::hit(TitleVec2 point, bool character) const {
    if (lines.empty()) return 0;
    // the line under the point across the writing direction, else the nearest
    const double cross = isVertical ? point.x : point.y;
    size_t best = 0;
    double bestDistance = -1.0;
    for (size_t i = 0; i < lines.size(); i++) {
        const TitleRect& box = lines[i].box;
        const double lo = isVertical ? box.x : box.y;
        const double hi = lo + (isVertical ? box.width : box.height);
        const double distance = cross < lo ? lo - cross : cross > hi ? cross - hi : 0.0;
        if (bestDistance < 0.0 || distance < bestDistance) {
            best = i;
            bestDistance = distance;
        }
    }
    const Line& line = lines[best];
    const double u = isVertical ? point.y : point.x;
    for (const Cluster& c : line.clusters) {
        const double start = isVertical ? c.pen.y : c.pen.x;
        const double finish = isVertical ? start - c.advance : start + c.advance;
        if (isVertical ? u > finish : u < finish) {
            const double t = c.advance > 0.0 ? (isVertical ? start - u : u - start) / c.advance : 0.0;
            const int k = character ? std::clamp((int)std::floor(t * c.count), 0, c.count - 1)
                                    : std::clamp((int)std::lround(t * c.count), 0, c.count);
            return std::min(c.index + k, total);
        }
    }
    return line.end;
}

// ---- the editor ----

TitleTextEdit::~TitleTextEdit() {
    if (editing) ImePreedit::setInlineWanted(false);
}

void TitleTextEdit::begin(const std::string& layerId, const std::string& text, bool selectAll) {
    editing = true;
    keyboard = false;
    layer = layerId;
    original = text;
    current = text;
    lastPreview = text;
    // unique across instances too: two editors in one window never share a field
    static int nextSession = 0;
    session = ++nextSession;
    anchorByte = selectAll ? 0 : (int)current.size();
    cursorByte = (int)current.size();
    pushSelection = true;
    clicks.clear();
}

void TitleTextEdit::end() {
    if (!editing) return;
    editing = false;
    keyboard = false;
    clicks.clear();
    ImePreedit::setInlineWanted(false);
    // a composition left in the field is dropped with it
    ImePreedit::clear();
}

bool TitleTextEdit::composing() const {
    return editing && keyboard && ImePreedit::current().active;
}

std::string TitleTextEdit::previewText() const {
    if (!composing()) return current;
    const ImePreedit& preedit = ImePreedit::current();
    return splicePreedit(current, anchorByte, cursorByte, preedit.text, preedit.caretByte).text;
}

void TitleTextEdit::selection(int& first, int& end) const {
    first = byteToCodepoint(current, anchorByte);
    end = byteToCodepoint(current, cursorByte);
    if (composing()) first = end;
    sorted(first, end);
}

int TitleTextEdit::caret() const {
    return byteToCodepoint(current, cursorByte);
}

void TitleTextEdit::clickAt(ImVec2 screen, bool extend, bool word) {
    if (!editing || composing()) return;
    clicks.push_back({screen, extend, word});
}

void TitleTextEdit::resolveClicks(const TextCaretGeometry& geometry, const TextEditFrame& frame) {
    if (clicks.empty() || geometry.empty()) return;
    if (composing()) {
        clicks.clear();
        return;
    }
    const Affine2 docToLayer = frame.layerToDoc.inverse();
    const std::vector<uint32_t> cps = decodeUtf8(current);
    for (const Click& click : clicks) {
        const TitleVec2 point = docToLayer.apply(frame.xform.toDoc(click.screen));
        if (click.word) {
            int first = 0, end = 0;
            wordAt(cps, std::min(geometry.characterAt(point), (int)cps.size()), first, end);
            anchorByte = codepointToByte(current, first);
            cursorByte = codepointToByte(current, end);
        } else {
            cursorByte = codepointToByte(current, std::min(geometry.hitTest(point), (int)cps.size()));
            if (!click.extend) anchorByte = cursorByte;
        }
    }
    clicks.clear();
    pushSelection = true;
}

TitleTextEdit::Result TitleTextEdit::frame(const TextEditFrame& frame, ImDrawList* canvasDrawList) {
    if (!editing) return Result::None;
    ImGuiContext& g = *ImGui::GetCurrentContext();
    ImGuiIO& io = ImGui::GetIO();

    // the keys that end the edit, unless another widget has the keyboard
    const ImGuiID fieldId = [&] {
        ImGui::PushID(session);
        const ImGuiID id = ImGui::GetID(FIELD_LABEL);
        ImGui::PopID();
        return id;
    }();
    const ImGuiID activeBefore = ImGui::GetActiveID();
    const bool ownsKeys = activeBefore == fieldId || (activeBefore == 0 && keyboard);
    if (ownsKeys && composing()) {
        // the input method's keys while composing (R37); the backends keep them from ImGui, this
        // also keeps them from the field and from the window's shortcuts this frame
        for (ImGuiKey key : {ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Escape})
            ImGui::SetKeyOwner(key, ImGuiKeyOwner_Any, ImGuiInputFlags_LockThisFrame);
    } else if (ownsKeys) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) return Result::Cancel;
        if (io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)))
            return Result::Commit;
    }

    TextCaretGeometry geometry;
    if (frame.layout) geometry = TextCaretGeometry(*frame.layout, previewText());
    resolveClicks(geometry, frame);

    // keep the field focused: re-activate it once no other widget is active and the mouse is up
    // (a click on the canvas or a Properties row deactivates it). An activation landing on a
    // frame with a press would take that press from the canvas, so it waits.
    if (activeBefore == 0 && !ImGui::IsAnyMouseDown()) ImGui::ActivateItemByID(fieldId);
    if (g.NavActivateId == fieldId && ImGui::IsAnyMouseDown()) g.NavActivateId = g.NavActivateDownId = g.NavActivatePressedId = 0;

    // the field is no content of the canvas window: its layout leaves no trace (no scrollbars)
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImVec2 cursorBackup = window->DC.CursorPos;
    const ImVec2 cursorMaxBackup = window->DC.CursorMaxPos;
    const ImVec2 idealMaxBackup = window->DC.IdealMaxPos;
    ImGui::SetCursorScreenPos(frame.fieldPos);
    // transparent rather than style alpha 0: ImGui hides a child window begun at alpha 0, and the
    // multi-line field is one
    for (int i = 0; i < ImGuiCol_COUNT; i++) ImGui::PushStyleColor(i, IM_COL32(0, 0, 0, 0));
    ImGui::PushID(session);
    ImGui::InputTextMultiline(FIELD_LABEL, current.data(), current.capacity() + 1, ImVec2(FIELD_SIZE, FIELD_SIZE),
                              ImGuiInputTextFlags_CallbackResize, resizeField, &current);
    ImGui::PopID();
    ImGui::PopStyleColor(ImGuiCol_COUNT);
    window->DC.CursorPos = cursorBackup;
    window->DC.CursorMaxPos = cursorMaxBackup;
    window->DC.IdealMaxPos = idealMaxBackup;

    keyboard = ImGui::GetActiveID() == fieldId;
    ImePreedit::setInlineWanted(keyboard);

    // the field's state outlives its activity until another field takes it: selection goes in
    // there, and comes back from it
    ImGuiInputTextState* state = ImGui::GetInputTextState(fieldId);
    if (state) {
        if (pushSelection) {
            state->SetSelection(std::clamp(anchorByte, 0, state->TextLen), std::clamp(cursorByte, 0, state->TextLen));
            state->CursorAnimReset();
            pushSelection = false;
        }
        cursorByte = state->GetCursorPos();
        anchorByte = state->HasSelection() ? state->GetSelectionStart() : cursorByte;
    } else {
        pushSelection = true;
    }
    anchorByte = std::clamp(anchorByte, 0, (int)current.size());
    cursorByte = std::clamp(cursorByte, 0, (int)current.size());

    Result result = Result::None;
    const std::string preview = previewText();
    if (preview != lastPreview) {
        lastPreview = preview;
        result = Result::Changed;
    }

    if (geometry.empty()) return result;
    const auto toScreen = [&](TitleVec2 p) { return frame.xform.toScreen(frame.layerToDoc.apply(p)); };

    int first = 0, end = 0, caretIndex = 0;
    int underlineFirst = 0, underlineEnd = 0;
    if (composing()) {
        const ImePreedit& preedit = ImePreedit::current();
        const PreeditSplice splice = splicePreedit(current, anchorByte, cursorByte, preedit.text, preedit.caretByte);
        caretIndex = splice.caret;
        underlineFirst = splice.preeditFirst;
        underlineEnd = splice.preeditEnd;
    } else {
        selection(first, end);
        caretIndex = caret();
    }

    const LayerSegment caretSegment = geometry.caret(caretIndex);
    const ImVec2 caretA = toScreen(caretSegment.a), caretB = toScreen(caretSegment.b);
    if (canvasDrawList) {
        const ImU32 selectionColor = ImGui::GetColorU32(ImGuiCol_TextSelectedBg);
        for (const TitleRect& r : geometry.selection(first, end)) {
            canvasDrawList->AddQuadFilled(toScreen({r.x, r.y}), toScreen({r.x + r.width, r.y}),
                                          toScreen({r.x + r.width, r.y + r.height}), toScreen({r.x, r.y + r.height}), selectionColor);
        }
        // light over dark, so they read over any art
        const ImU32 dark = IM_COL32(0, 0, 0, 170), light = IM_COL32(255, 255, 255, 255);
        for (const LayerSegment& s : geometry.underline(underlineFirst, underlineEnd)) {
            const ImVec2 a = toScreen(s.a), b = toScreen(s.b);
            canvasDrawList->AddLine(a, b, dark, 4.0f);
            canvasDrawList->AddLine(a, b, light, 2.0f);
        }
        const bool blinkOn = !io.ConfigInputTextCursorBlink || !state || state->CursorAnim <= 0.0f || ImFmod(state->CursorAnim, 1.20f) <= 0.80f;
        if (blinkOn || composing()) {
            canvasDrawList->AddLine(caretA, caretB, dark, 3.5f);
            canvasDrawList->AddLine(caretA, caretB, light, 1.5f);
        }
    }

    // candidate windows open at the on-canvas caret (the field set its own position above)
    if (keyboard) {
        const ImVec2 lo(std::min(caretA.x, caretB.x), std::min(caretA.y, caretB.y));
        const ImVec2 hi(std::max(caretA.x, caretB.x), std::max(caretA.y, caretB.y));
        ImGuiPlatformImeData& ime = g.PlatformImeData;
        ime.WantVisible = true;
        ime.WantTextInput = true;
        ime.InputPos = lo;
        ime.InputLineHeight = std::max(hi.y - lo.y, ImGui::GetFontSize());
        ime.ViewportId = ImGui::GetWindowViewport()->ID;
    }
    return result;
}

void TitleTextEdit::typeText(const std::string& utf8) {
    ImGui::GetIO().AddInputCharactersUTF8(utf8.c_str());
}

void TitleTextEdit::compose(const std::string& preedit, int caretByte) {
    ImePreedit p;
    p.text = preedit;
    p.caretByte = caretByte;
    p.active = true;
    ImePreedit::set(p);
}

void TitleTextEdit::commitComposition(const std::string& utf8) {
    ImePreedit::clear();
    typeText(utf8);
}
