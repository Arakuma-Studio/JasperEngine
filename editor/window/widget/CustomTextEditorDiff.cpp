// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The code editor's version control layer (docs/superpowers/specs/2026-11-04-code-editor-git-design.md
// 6.3): change markers in the gutter, the same marks on the scrollbar, the diff popup and Revert
// Change. The baseline comes from CodeEditor (vcs::TextBaselines); this file knows nothing of version
// control.

#include "CustomTextEditor.h"

#include "Backend.h"
#include "App.h"
#include "Theme.h"
#include "external/IconsFontAwesome6.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace jasperengine::editor {

// a named namespace: the editor's unity build puts CustomTextEditor.cpp's helpers beside these
namespace textdiff {

using Clock = std::chrono::steady_clock;

// Seconds on a marker to open its popup, and off both to close it
constexpr double DIFF_PEEK_DELAY = 0.15;
constexpr double DIFF_PEEK_GRACE = 0.3;

// VS Code's diff tints; the changed part of a line gets a second one
constexpr ImU32 DIFF_REMOVED_LINE_BG = IM_COL32(255, 0, 0, 51);
constexpr ImU32 DIFF_ADDED_LINE_BG = IM_COL32(155, 185, 85, 51);
constexpr ImU32 DIFF_REMOVED_TEXT_BG = IM_COL32(255, 0, 0, 51);
constexpr ImU32 DIFF_ADDED_TEXT_BG = IM_COL32(156, 204, 44, 51);

bool continuationByte(unsigned char c) {
    return (c & 0xC0) == 0x80;
}

// Bytes between the common prefix and suffix of two lines, cut at characters
void changedBytes(const std::string& before, const std::string& after, int& start, int& beforeEnd, int& afterEnd) {
    auto insideCharacter = [&](size_t i) {
        return (i < before.size() && continuationByte(before[i])) || (i < after.size() && continuationByte(after[i]));
    };

    size_t prefix = 0;
    while (prefix < before.size() && prefix < after.size() && before[prefix] == after[prefix]) prefix++;
    while (prefix > 0 && insideCharacter(prefix)) prefix--;

    size_t suffix = 0;
    while (suffix < before.size() - prefix && suffix < after.size() - prefix &&
           before[before.size() - 1 - suffix] == after[after.size() - 1 - suffix]) {
        suffix++;
    }
    while (suffix > 0 && continuationByte(before[before.size() - suffix])) suffix--;

    start = static_cast<int>(prefix);
    beforeEnd = static_cast<int>(before.size() - suffix);
    afterEnd = static_cast<int>(after.size() - suffix);
}

// Modified lines get a striped bar, so the three kinds differ in shape as well as colour
void drawHatchedBar(ImDrawList* drawList, const ImVec2& min, const ImVec2& max, ImU32 color) {
    // Only the visible part, a block can span thousands of lines
    const float top = std::max(min.y, drawList->GetClipRectMin().y);
    const float bottom = std::min(max.y, drawList->GetClipRectMax().y);
    if (top >= bottom) return;

    const float width = max.x - min.x;
    const float thickness = width * 0.5f;

    // Antialiasing would blur stripes this thin into a flat tint
    const ImDrawListFlags flags = drawList->Flags;
    drawList->Flags &= ~ImDrawListFlags_AntiAliasedFill;
    drawList->PushClipRect(ImVec2(min.x, top), ImVec2(max.x, bottom), true);

    // Anchored to the block, so they scroll with the text
    for (float y = min.y + std::floor((top - min.y) / width) * width - 2.0f * width; y < bottom; y += width) {
        drawList->AddQuadFilled(ImVec2(min.x, y + width), ImVec2(max.x, y),
                                ImVec2(max.x, y + thickness), ImVec2(min.x, y + width + thickness), color);
    }

    drawList->PopClipRect();
    drawList->Flags = flags;
}

std::string lineRange(int start, int end) {
    return end - start == 1 ? "line " + std::to_string(start + 1)
                            : "lines " + std::to_string(start + 1) + "-" + std::to_string(end);
}

} // namespace textdiff

void CustomTextEditor::SetDiffBase(std::shared_ptr<const std::vector<std::string>> baseLines, const std::string& revision) {
    if (!baseLines) {
        ClearDiffBase();
        return;
    }
    diffBase = std::move(baseLines);
    diffRevision = revision;
    // the baseline's own multi-line state, so a removed line inside a block comment reads as one
    diffBaseTokens.assign(diffBase->size(), {});
    OpenToken open;
    for (size_t i = 0; i < diffBase->size(); i++) {
        tokenizeText((*diffBase)[i], open, diffBaseTokens[i]);
    }
    diffDirty = true;
    diffNextTime = {};
    closeDiffPeek();
}

void CustomTextEditor::ClearDiffBase() {
    diffBase.reset();
    diffBaseTokens.clear();
    diffChanges.clear();
    diffDirty = false;
    diffVersion++;
    diffHoverChange = -1;
    closeDiffPeek();
}

const std::vector<LineChange>& CustomTextEditor::GetDiffChanges() {
    updateDiff(true);
    return diffChanges;
}

void CustomTextEditor::updateDiff(bool force) {
    if (!diffDirty || !diffBase) return;

    const textdiff::Clock::time_point now = textdiff::Clock::now();
    if (!force && now < diffNextTime) {
        // the loop may be idling while the wait runs out
        Backend::getApp().requestFrame();
        return;
    }

    diffChanges = LineDiff::compute(*diffBase, lines);
    diffVersion++;
    diffDirty = false;
    // a slow diff runs at most 5% of the time
    const textdiff::Clock::time_point end = textdiff::Clock::now();
    diffNextTime = end + (end - now) * 20;
}

int CustomTextEditor::GetDiffChangeAt(int line) {
    updateDiff(true);
    const int lineCount = static_cast<int>(lines.size());
    for (int i = 0; i < static_cast<int>(diffChanges.size()); i++) {
        const LineChange& change = diffChanges[i];
        if (change.modifiedStart == change.modifiedEnd) {
            // marked on the edge above modifiedStart (the last line's bottom at the end)
            const int marked = std::min(change.modifiedStart, lineCount - 1);
            if (line == marked) return i;
        } else if (line >= change.modifiedStart && line < change.modifiedEnd) {
            return i;
        }
    }
    return -1;
}

bool CustomTextEditor::RevertDiffChange(int index) {
    updateDiff(true);
    if (!diffBase || index < 0 || index >= static_cast<int>(diffChanges.size())) return false;
    const LineChange change = diffChanges[index];
    const std::vector<std::string>& base = *diffBase;
    if (change.modifiedEnd > static_cast<int>(lines.size()) || change.originalEnd > static_cast<int>(base.size())) return false;

    if (showParamHint) closeParamHint();
    CloseAutoComplete();

    std::vector<std::string> reverted(lines.begin(), lines.begin() + change.modifiedStart);
    reverted.insert(reverted.end(), base.begin() + change.originalStart, base.begin() + change.originalEnd);
    reverted.insert(reverted.end(), lines.begin() + change.modifiedEnd, lines.end());
    // a document always has a line
    if (reverted.empty()) reverted.emplace_back();

    addUndoRecord(false);
    lines = std::move(reverted);

    // Carets below the change follow their text, those inside go to its first line
    const int shift = (change.originalEnd - change.originalStart) - (change.modifiedEnd - change.modifiedStart);
    auto follow = [&](TextPosition& pos) {
        if (pos.line >= change.modifiedEnd) {
            pos.line += shift;
        } else if (pos.line >= change.modifiedStart) {
            pos = TextPosition(change.modifiedStart, 0);
        }
    };
    for (Cursor& cursor : cursors) {
        follow(cursor.position);
        follow(cursor.selection.start);
        follow(cursor.selection.end);
    }
    ensureValidCursors();
    mergeCursors();

    tokenizeAll();
    finalizeUndoRecord();

    closeDiffPeek();
    pendingFocus = true;
    return true;
}

void CustomTextEditor::goToChange(int index) {
    if (index < 0 || index >= static_cast<int>(diffChanges.size())) return;
    const int line = std::min(diffChanges[index].modifiedStart, static_cast<int>(lines.size()) - 1);
    cursors.clear();
    Cursor cursor;
    cursor.position = TextPosition(line, 0);
    cursor.selection.start = cursor.position;
    cursor.selection.end = cursor.position;
    cursors.push_back(cursor);
    primaryCursor = 0;
    pendingScrollToCursor = true;
}

bool CustomTextEditor::GoToNextChange() {
    updateDiff(true);
    if (diffChanges.empty()) return false;
    const int line = cursors.empty() ? -1 : cursors[primaryCursor].position.line;
    int next = 0;
    for (int i = 0; i < static_cast<int>(diffChanges.size()); i++) {
        if (diffChanges[i].modifiedStart > line) {
            next = i;
            break;
        }
    }
    goToChange(next);
    return true;
}

bool CustomTextEditor::GoToPreviousChange() {
    updateDiff(true);
    if (diffChanges.empty()) return false;
    const int line = cursors.empty() ? 0 : cursors[primaryCursor].position.line;
    int previous = static_cast<int>(diffChanges.size()) - 1;
    for (int i = static_cast<int>(diffChanges.size()) - 1; i >= 0; i--) {
        if (diffChanges[i].modifiedStart < line) {
            previous = i;
            break;
        }
    }
    goToChange(previous);
    return true;
}

TokenType CustomTextEditor::GetTokenTypeAt(int line, int column) const {
    if (line < 0 || line >= static_cast<int>(lineTokens.size())) return TokenType::Default;
    for (const Token& token : lineTokens[line]) {
        if (column >= token.start && column < token.start + token.length) return token.type;
    }
    return TokenType::Default;
}

TokenType CustomTextEditor::GetDiffBaseTokenTypeAt(int line, int column) const {
    if (line < 0 || line >= static_cast<int>(diffBaseTokens.size())) return TokenType::Default;
    for (const Token& token : diffBaseTokens[line]) {
        if (column >= token.start && column < token.start + token.length) return token.type;
    }
    return TokenType::Default;
}

void CustomTextEditor::closeDiffPeek() {
    diffPeekChange = -1;
    diffPeekPinned = false;
    diffPeekHovered = false;
    diffPeekLeaveTime = -1.0;
}

// ---------------------------------------------------------------- drawing

void CustomTextEditor::renderLineTokens(ImDrawList* drawList, ImVec2 pos, const std::string& line, const std::vector<Token>& tokens) const {
    const ImU32 defaultColor = ImGui::ColorConvertFloat4ToU32(palette[static_cast<int>(TokenType::Default)]);
    const char* text = line.c_str();

    int lastEnd = 0;
    for (const Token& token : tokens) {
        if (token.start > lastEnd) {
            drawList->AddText(pos, defaultColor, text + lastEnd, text + token.start);
            pos.x += measureText(text + lastEnd, text + token.start);
        }

        lastEnd = token.start + token.length;
        drawList->AddText(pos, ImGui::ColorConvertFloat4ToU32(palette[static_cast<int>(token.type)]), text + token.start, text + lastEnd);
        pos.x += measureText(text + token.start, text + lastEnd);
    }

    if (lastEnd < static_cast<int>(line.size())) {
        drawList->AddText(pos, defaultColor, text + lastEnd, text + line.size());
    }
}

float CustomTextEditor::diffGutterX(const ImVec2& origin) const {
    return std::round(origin.x + leftMargin + lineNumberDigits * charWidth + leftMargin * 0.5f);
}

int CustomTextEditor::diffChangeAt(const ImVec2& point, const ImVec2& origin) const {
    if (diffChanges.empty()) return -1;
    const float x = diffGutterX(origin);
    const float barWidth = std::round(Theme::dpi(3.0f));
    // the bar alone is hard to hit
    const float reach = Theme::dpi(3.0f);
    if (point.x < x - reach || point.x > x + barWidth + reach) return -1;

    const float triangleSize = std::round(Theme::dpi(4.0f));
    for (int i = 0; i < static_cast<int>(diffChanges.size()); i++) {
        const LineChange& change = diffChanges[i];
        float top = origin.y + change.modifiedStart * lineHeight;
        float bottom = origin.y + change.modifiedEnd * lineHeight;
        if (change.modifiedStart == change.modifiedEnd) {
            const float y = std::max(top, origin.y + triangleSize);
            top = y - triangleSize;
            bottom = y + triangleSize;
        }
        if (point.y >= top && point.y < bottom) return i;
    }
    return -1;
}

void CustomTextEditor::renderDiffGutter(ImDrawList* drawList, const ImVec2& origin, int startLine, int endLine) {
    if (diffChanges.empty()) return;

    // the first change reaching the visible lines (changes are in order)
    auto first = std::lower_bound(diffChanges.begin(), diffChanges.end(), startLine,
                                  [](const LineChange& change, int line) { return change.modifiedEnd < line; });

    // VS Code's 3 px bars and 4 px triangles, twice as large under the mouse
    const float x = diffGutterX(origin);
    for (auto it = first; it != diffChanges.end(); ++it) {
        const LineChange& change = *it;
        if (change.modifiedStart > endLine + 1) break;
        const int index = static_cast<int>(it - diffChanges.begin());

        const float scale = (index == diffHoverChange || index == diffPeekChange) ? 2.0f : 1.0f;
        const float top = origin.y + change.modifiedStart * lineHeight;

        if (change.modifiedStart == change.modifiedEnd) {
            // on the edge where lines were removed
            const float size = std::round(Theme::dpi(4.0f) * scale);
            const float y = std::max(top, origin.y + size);
            drawList->AddTriangleFilled(ImVec2(x, y - size), ImVec2(x + size, y), ImVec2(x, y + size), diffColors.deleted);
            continue;
        }

        const ImVec2 min(x, top);
        const ImVec2 max(x + std::round(Theme::dpi(3.0f) * scale), origin.y + change.modifiedEnd * lineHeight);
        if (change.originalStart == change.originalEnd) {
            drawList->AddRectFilled(min, max, diffColors.added);
        } else {
            textdiff::drawHatchedBar(drawList, min, max, diffColors.modified);
        }
    }
}

void CustomTextEditor::renderDiffOverview() {
    if (diffChanges.empty()) return;

    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (!window->ScrollbarY) return;

    // the scrollbar doubles as an overview ruler, the changes in its left third
    const ImRect bar = ImGui::GetWindowScrollbarRect(window, ImGuiAxis_Y);
    const float contentHeight = window->ContentSize.y + window->WindowPadding.y * 2.0f;
    if (contentHeight <= 0.0f || bar.GetHeight() <= 0.0f) return;

    const float scale = bar.GetHeight() / contentHeight;
    const float laneEnd = bar.Min.x + std::round(bar.GetWidth() / 3.0f);
    const float minHeight = Theme::dpi(2.0f);

    // the content clip rect leaves the scrollbar out
    ImRect clip = bar;
    clip.ClipWithFull(window->OuterRectClipped);
    if (clip.GetWidth() <= 0.0f || clip.GetHeight() <= 0.0f) return;

    ImDrawList* drawList = window->DrawList;
    drawList->PushClipRect(clip.Min, clip.Max, false);
    for (const LineChange& change : diffChanges) {
        float top = bar.Min.y + change.modifiedStart * lineHeight * scale;
        float bottom = bar.Min.y + change.modifiedEnd * lineHeight * scale;
        if (bottom - top < minHeight) {
            const float center = (top + bottom) * 0.5f;
            top = center - minHeight * 0.5f;
            bottom = center + minHeight * 0.5f;
        }

        const ImU32 kind = change.kind() == LineChange::Kind::Added ? diffColors.added
                         : change.kind() == LineChange::Kind::Deleted ? diffColors.deleted : diffColors.modified;
        // 60% opaque, as in VS Code
        const ImU32 color = (kind & ~IM_COL32_A_MASK) | (0x99u << IM_COL32_A_SHIFT);
        drawList->AddRectFilled(ImVec2(bar.Min.x, top), ImVec2(laneEnd, bottom), color);
    }
    drawList->PopClipRect();
}

void CustomTextEditor::updateDiffPeek(const ImVec2& origin) {
    diffMarkerClicked = false;
    // an edit that moved the change closes its popup
    if (diffPeekChange >= 0 && (diffPeekChange >= static_cast<int>(diffChanges.size()) || diffChanges[diffPeekChange] != diffPeekKey)) {
        closeDiffPeek();
    }

    const double now = ImGui::GetTime();
    const bool overEditor = ImGui::IsWindowHovered() && !suggestionsHovered && !showContextMenu;
    const int hovered = overEditor ? diffChangeAt(ImGui::GetIO().MousePos, origin) : -1;
    if (hovered >= 0) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
    }

    if (hovered != diffHoverChange) {
        diffHoverChange = hovered;
        diffHoverStart = now;
    }

    // a click on a marker opens its popup at once and keeps it until Esc or a click elsewhere
    if (hovered >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        diffMarkerClicked = true;
        diffPeekChange = hovered;
        diffPeekKey = diffChanges[hovered];
        diffPeekVersion = 0;
        diffPeekPinned = true;
        diffPeekLeaveTime = -1.0;
        return;
    }
    if (diffPeekPinned) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !diffPeekHovered) closeDiffPeek();
        return;
    }

    // a mouse just crossing the gutter opens nothing
    const bool dragging = isDragging || isDraggingText || isMiddleDragging || ImGui::IsAnyMouseDown();
    if (hovered >= 0 && hovered != diffPeekChange && !dragging && now - diffHoverStart >= textdiff::DIFF_PEEK_DELAY) {
        diffPeekChange = hovered;
        diffPeekKey = diffChanges[hovered];
        diffPeekVersion = 0;
        diffPeekHovered = false;
        diffPeekLeaveTime = -1.0;
    }

    // open while the mouse is on the marker or the popup, with time to go from one to the other
    if (diffPeekChange >= 0) {
        if (hovered == diffPeekChange || diffPeekHovered) {
            diffPeekLeaveTime = -1.0;
        } else if (diffPeekLeaveTime < 0.0) {
            diffPeekLeaveTime = now;
        } else if (now - diffPeekLeaveTime > textdiff::DIFF_PEEK_GRACE) {
            closeDiffPeek();
        }
    }

    // the loop may be idling while a wait runs out
    if ((hovered >= 0 && hovered != diffPeekChange) || (diffPeekChange >= 0 && diffPeekLeaveTime >= 0.0)) {
        Backend::getApp().requestFrame();
    }
}

void CustomTextEditor::buildDiffPeekRows(const LineChange& change) {
    diffPeekRows.clear();
    const std::vector<std::string>& base = *diffBase;

    for (int i = change.originalStart; i < change.originalEnd && i < static_cast<int>(base.size()); i++) {
        DiffPeekRow row;
        row.text = base[i];
        if (i < static_cast<int>(diffBaseTokens.size())) row.tokens = diffBaseTokens[i];
        diffPeekRows.push_back(std::move(row));
    }
    const int removed = static_cast<int>(diffPeekRows.size());

    for (int i = change.modifiedStart; i < change.modifiedEnd && i < static_cast<int>(lines.size()); i++) {
        DiffPeekRow row;
        row.added = true;
        row.text = lines[i];
        if (i < static_cast<int>(lineTokens.size())) row.tokens = lineTokens[i];
        diffPeekRows.push_back(std::move(row));
    }
    const int added = static_cast<int>(diffPeekRows.size()) - removed;

    // paired lines mark what changed between them
    for (int i = 0; i < std::min(removed, added); i++) {
        DiffPeekRow& before = diffPeekRows[i];
        DiffPeekRow& after = diffPeekRows[removed + i];
        textdiff::changedBytes(before.text, after.text, before.changedStart, before.changedEnd, after.changedEnd);
        after.changedStart = before.changedStart;
    }

    diffPeekTextWidth = 0.0f;
    for (const DiffPeekRow& row : diffPeekRows) {
        diffPeekTextWidth = std::max(diffPeekTextWidth, measureText(row.text.c_str(), row.text.c_str() + row.text.size()));
    }
}

void CustomTextEditor::renderDiffPeek(const ImVec2& origin) {
    if (diffPeekChange < 0 || diffPeekChange >= static_cast<int>(diffChanges.size()) || !diffBase) {
        diffPeekHovered = false;
        return;
    }

    const int changeIndex = diffPeekChange;
    const LineChange change = diffChanges[changeIndex];
    if (diffPeekVersion != diffVersion) {
        buildDiffPeekRows(change);
        diffPeekVersion = diffVersion;
    }

    const int removed = change.originalEnd - change.originalStart;
    const int added = change.modifiedEnd - change.modifiedStart;
    std::string title;
    if (removed == 0) {
        title = "Added " + textdiff::lineRange(change.modifiedStart, change.modifiedEnd);
    } else if (added == 0) {
        title = "Removed " + std::to_string(removed) + (removed == 1 ? " line" : " lines");
    } else {
        title = "Changed " + textdiff::lineRange(change.modifiedStart, change.modifiedEnd);
    }
    const bool several = diffChanges.size() > 1;

    const ImRect editorRect = ImGui::GetCurrentWindow()->InnerRect;

    // whole pixels: ImGui truncates window sizes and a lost fraction shows scrollbars
    const float spacing = std::round(Theme::dpi(6.0f));
    const float spacingY = std::round(Theme::dpi(3.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(spacing, spacingY));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing, spacingY));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Theme::dpi(4.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, std::round(Theme::dpi(10.0f)));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.3f, 0.3f, 0.3f, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, backgroundColor);

    const ImGuiStyle& style = ImGui::GetStyle();

    ImGui::PushFont(ImGui::GetIO().FontDefault);
    const float headerHeight = std::ceil(ImGui::GetFrameHeight());
    auto buttonWidth = [&](const char* label) { return ImGui::CalcTextSize(label).x + 4.0f + style.ItemSpacing.x; };
    float buttonsWidth = buttonWidth(ICON_FA_ROTATE_LEFT);
    if (several) buttonsWidth += buttonWidth(ICON_FA_ARROW_UP) + buttonWidth(ICON_FA_ARROW_DOWN);
    if (diffPeekPinned) buttonsWidth += buttonWidth(ICON_FA_XMARK);
    const float headerWidth = ImGui::CalcTextSize(title.c_str()).x + style.ItemSpacing.x * 4.0f + buttonsWidth;
    ImGui::PopFont();

    const float signWidth = charWidth * 2.0f;
    const float contentWidth = std::ceil(signWidth + diffPeekTextWidth + charWidth);
    const float contentHeight = diffPeekRows.size() * lineHeight;
    const float maxBodyWidth = std::floor(std::max(Theme::dpi(240.0f), editorRect.GetWidth() - Theme::dpi(8.0f)) - spacing * 2.0f);

    float bodyHeight = std::min(contentHeight, std::floor(std::max(lineHeight * 3.0f, editorRect.GetHeight() * 0.45f)));
    float bodyWidth = std::ceil(std::max(contentWidth + (contentHeight > bodyHeight ? style.ScrollbarSize : 0.0f), headerWidth));
    if (bodyWidth > maxBodyWidth) {
        bodyWidth = maxBodyWidth;
        bodyHeight += style.ScrollbarSize;
    }
    const float width = bodyWidth + spacing * 2.0f;
    const float height = headerHeight + bodyHeight + spacingY * 3.0f;

    float top = origin.y + change.modifiedStart * lineHeight;
    float bottom = origin.y + change.modifiedEnd * lineHeight;
    if (added == 0) {
        const float triangleSize = std::round(Theme::dpi(4.0f));
        const float y = std::max(top, origin.y + triangleSize);
        top = y - triangleSize;
        bottom = y + triangleSize;
    }

    // below the change, above it when there is no room
    ImVec2 pos(diffGutterX(origin), bottom);
    if (pos.y + height > editorRect.Max.y) {
        pos.y = top - height;
        if (pos.y < editorRect.Min.y) pos.y = std::max(editorRect.Min.y, editorRect.Max.y - height);
    }
    pos.x = std::max(editorRect.Min.x, std::min(pos.x, editorRect.Max.x - width));

    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(ImVec2(width, height));

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking;

    // one per editor, two can show side by side
    char windowName[48];
    std::snprintf(windowName, sizeof(windowName), "##DiffPeek%p", static_cast<void*>(this));

    enum class Action { None, Revert, Previous, Next, Close } action = Action::None;
    if (ImGui::Begin(windowName, nullptr, flags)) {
        diffPeekHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

        ImGui::PushFont(ImGui::GetIO().FontDefault);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(title.c_str());

        // the reset arrow of the Inspector, and the arrows of VS Code's peek
        ImGui::SameLine(ImGui::GetContentRegionMax().x - buttonsWidth + style.ItemSpacing.x);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, style.FramePadding.y));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, style.Colors[ImGuiCol_TextDisabled]);
        if (several) {
            if (ImGui::Button(ICON_FA_ARROW_UP "##diff_previous")) action = Action::Previous;
            ImGui::SetItemTooltip("Previous Change (Shift+Alt+F5)");
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_ARROW_DOWN "##diff_next")) action = Action::Next;
            ImGui::SetItemTooltip("Next Change (Alt+F5)");
            ImGui::SameLine();
        }
        if (ImGui::Button(ICON_FA_ROTATE_LEFT "##diff_revert")) action = Action::Revert;
        ImGui::SetItemTooltip("Revert Change (back to %s)", diffRevision.empty() ? "the baseline" : diffRevision.c_str());
        if (diffPeekPinned) {
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_XMARK "##diff_close")) action = Action::Close;
            ImGui::SetItemTooltip("Close (Esc)");
        }
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
        ImGui::PopFont();

        if (ImGui::BeginChild("##DiffPeekBody", ImVec2(bodyWidth, bodyHeight), ImGuiChildFlags_None,
                              ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoNav)) {
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            const ImVec2 start = ImGui::GetCursorScreenPos();
            const float rowWidth = std::max(contentWidth, ImGui::GetContentRegionAvail().x);
            ImGui::Dummy(ImVec2(contentWidth, contentHeight));

            // removed lines first, then what replaced them; only the visible rows
            const int first = std::max(0, static_cast<int>(ImGui::GetScrollY() / lineHeight));
            const int last = std::min(static_cast<int>(diffPeekRows.size()), first + static_cast<int>(bodyHeight / lineHeight) + 2);
            for (int i = first; i < last; i++) {
                const DiffPeekRow& row = diffPeekRows[i];
                const float y = start.y + i * lineHeight;
                const float textX = start.x + signWidth;

                drawList->AddRectFilled(ImVec2(start.x, y), ImVec2(start.x + rowWidth, y + lineHeight),
                                        row.added ? textdiff::DIFF_ADDED_LINE_BG : textdiff::DIFF_REMOVED_LINE_BG);
                if (row.changedEnd > row.changedStart) {
                    const char* text = row.text.c_str();
                    drawList->AddRectFilled(ImVec2(textX + measureText(text, text + row.changedStart), y),
                                            ImVec2(textX + measureText(text, text + row.changedEnd), y + lineHeight),
                                            row.added ? textdiff::DIFF_ADDED_TEXT_BG : textdiff::DIFF_REMOVED_TEXT_BG);
                }
                drawList->AddText(ImVec2(start.x + charWidth * 0.5f, y + textOffsetY),
                                  row.added ? diffColors.added : diffColors.deleted, row.added ? "+" : "-");
                renderLineTokens(drawList, ImVec2(textX, y + textOffsetY), row.text, row.tokens);
            }
        }
        ImGui::EndChild();

        // a click here gives the keyboard back to the editor
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsAnyMouseDown()) {
            pendingFocus = true;
        }
    }
    ImGui::End();

    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(4);

    const int count = static_cast<int>(diffChanges.size());
    switch (action) {
        case Action::Revert:
            RevertDiffChange(changeIndex);
            break;
        case Action::Previous:
        case Action::Next: {
            const int other = (changeIndex + (action == Action::Next ? 1 : count - 1)) % count;
            goToChange(other);
            diffPeekChange = other;
            diffPeekKey = diffChanges[other];
            diffPeekVersion = 0;
            diffPeekPinned = true;
            break;
        }
        case Action::Close:
            closeDiffPeek();
            break;
        case Action::None:
            break;
    }
}

} // namespace jasperengine::editor
