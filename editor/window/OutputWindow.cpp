// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "OutputWindow.h"
#include "external/IconsFontAwesome6.h"
#include "util/UIUtils.h"
#include "App.h"
#include "Backend.h"
#include "Theme.h"
#include <cstdarg>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <chrono>
#include <unordered_map>

#include "imgui_internal.h"

static float getElapsedSeconds() {
    static auto startTime = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<float>(now - startTime).count();
}

using namespace jasperengine::editor;

namespace {

const char* logIcon(LogType type) {
    switch (type) {
        case LogType::Warning: return ICON_FA_TRIANGLE_EXCLAMATION;
        case LogType::Error:   return ICON_FA_CIRCLE_EXCLAMATION;
        case LogType::Success: return ICON_FA_CIRCLE_CHECK;
        case LogType::Build:   return ICON_FA_HAMMER;
        case LogType::Info:
        default:               return ICON_FA_CIRCLE_INFO;
    }
}

ImVec4 logIconColor(LogType type) {
    switch (type) {
        case LogType::Warning: return Theme::Colors::WarningText;
        case LogType::Error:   return Theme::Colors::ErrorText;
        case LogType::Success: return ImVec4(0.45f, 0.80f, 0.45f, 1.00f);
        case LogType::Build:   return ImVec4(0.55f, 0.72f, 0.90f, 1.00f);
        case LogType::Info:
        default:               return Theme::Colors::LogIcon;
    }
}

const char* logTypeName(LogType type) {
    switch (type) {
        case LogType::Error:   return "Error";
        case LogType::Success: return "Success";
        case LogType::Warning: return "Warning";
        case LogType::Build:   return "Build";
        case LogType::Info:
        default:               return "Log";
    }
}

std::string firstLine(const std::string& message) {
    size_t end = message.find_first_of("\r\n");
    return end == std::string::npos ? message : message.substr(0, end);
}

// Flat Unity toolbar button; a toggle shows its on state with a pressed fill.
bool toolbarButton(const char* label, bool toggledOn = false, const char* tooltip = nullptr) {
    ImGui::PushStyleColor(ImGuiCol_Button, toggledOn ? Theme::Colors::ButtonActivated : ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toggledOn ? Theme::Colors::ButtonActivated : Theme::Colors::IconButtonHovered);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Theme::Colors::IconButtonActive);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    bool pressed = ImGui::Button(label);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("%s", tooltip);
    }
    return pressed;
}

void toolbarSeparator() {
    ImGui::SameLine();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float h = ImGui::GetFrameHeight();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(pos.x, pos.y + h * 0.15f), ImVec2(pos.x, pos.y + h * 0.85f),
                                        ImGui::GetColorU32(Theme::Colors::ToolbarBorder), Theme::dpi(1.0f));
    ImGui::Dummy(ImVec2(Theme::dpi(1.0f), h));
    ImGui::SameLine();
}

std::string countLabel(size_t count) {
    return count > 999 ? std::string("999+") : std::to_string(count);
}

}

OutputWindow::OutputWindow() {
    storedLogLineCount = 0;
    nextLogId = 1;
    needsRebuild = true;
    searchMatchCase = false;
    for (int i = 0; i < SEVERITY_COUNT; i++) {
        severityFilters[i] = true;
        severityCounts[i] = 0;
    }
    collapse = false;
    clearOnPlay = false;
    errorPause = false;
    selectedLogId = 0;
    hasSelection = false;
    scrollToSelection = false;
    autoScroll = true;
    detailsHeight = 90.0f;
    wasPlaying = false;
    lastIdBeforePlay = 0;
    pendingErrorPause = false;
    hasNotification = false;
    windowOpen = true;
    focusRequested = false;
    isWindowVisible = false;
    clear();
}

void OutputWindow::setOpen(bool open) {
    if (open) {
        if (!windowOpen) {
            focusRequested = true;
        }
        windowOpen = true;
        return;
    }

    windowOpen = false;
    focusRequested = false;
    isWindowVisible = false;
}

bool OutputWindow::isOpen() const {
    return windowOpen;
}

void OutputWindow::requestFocus() {
    windowOpen = true;
    focusRequested = true;
}

void OutputWindow::clear() {
    logs.clear();
    rows.clear();
    storedLogLineCount = 0;
    for (int i = 0; i < SEVERITY_COUNT; i++) {
        severityCounts[i] = 0;
    }
    hasSelection = false;
    selectedLogId = 0;
    needsRebuild = true;
    hasNotification = false;
    autoScroll = true;
}

OutputWindow::Severity OutputWindow::severityOf(LogType type) {
    switch (type) {
        case LogType::Warning: return SEVERITY_WARNING;
        case LogType::Error:   return SEVERITY_ERROR;
        default:               return SEVERITY_LOG;
    }
}

size_t OutputWindow::countRawLines(const std::string& message) {
    size_t lines = 1;
    for (char c : message) {
        if (c == '\n') {
            lines++;
        }
    }
    return lines;
}

size_t OutputWindow::trimLeadingRawLines(std::string& message, size_t linesToTrim) {
    size_t removed = 0;
    size_t eraseEnd = 0;

    while (removed < linesToTrim) {
        const size_t newline = message.find('\n', eraseEnd);
        removed++;
        if (newline == std::string::npos) {
            message.clear();
            return removed;
        }
        eraseEnd = newline + 1;
    }

    message.erase(0, eraseEnd);
    return removed;
}

bool OutputWindow::enforceStoredLineLimit() {
    if (storedLogLineCount <= MAX_STORED_LOG_LINES) {
        return false;
    }

    size_t eraseCount = 0;
    while (storedLogLineCount > MAX_STORED_LOG_LINES && eraseCount < logs.size()) {
        const size_t excess = storedLogLineCount - MAX_STORED_LOG_LINES;
        const size_t frontLines = countRawLines(logs[eraseCount].message);

        if (frontLines <= excess) {
            storedLogLineCount -= frontLines;
            eraseCount++;
            continue;
        }

        storedLogLineCount -= trimLeadingRawLines(logs[eraseCount].message, excess);
        break;
    }

    if (eraseCount > 0) {
        logs.erase(logs.begin(), logs.begin() + static_cast<std::ptrdiff_t>(eraseCount));
    }
    recountSeverities();
    return true;
}

void OutputWindow::recountSeverities() {
    for (int i = 0; i < SEVERITY_COUNT; i++) {
        severityCounts[i] = 0;
    }
    for (const LogData& log : logs) {
        severityCounts[severityOf(log.type)]++;
    }
}

void OutputWindow::addLog(LogType type, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char temp[4096];
    vsnprintf(temp, IM_ARRAYSIZE(temp), fmt, args);
    va_end(args);

    addLog(type, std::string(temp));
}

void OutputWindow::addLog(LogType type, const std::string& message) {
    LogData logEntry{type, message, getElapsedSeconds(), nextLogId++};
    logs.push_back(std::move(logEntry));
    storedLogLineCount += countRawLines(message);
    severityCounts[severityOf(type)]++;
    enforceStoredLineLimit();

    needsRebuild = true;
    if (!isWindowVisible) {
        hasNotification = true;
    }
    if (type == LogType::Error && errorPause) {
        pendingErrorPause = true;
    }
}

std::string OutputWindow::getRecentLogText(size_t maxEntries, bool onlyProblems) const {
    if (maxEntries == 0) return {};

    // Walk from the newest entry backwards, collecting up to maxEntries.
    std::vector<const LogData*> selected;
    for (auto it = logs.rbegin(); it != logs.rend() && selected.size() < maxEntries; ++it) {
        if (onlyProblems && it->type != LogType::Error && it->type != LogType::Warning && it->type != LogType::Build) {
            continue;
        }
        selected.push_back(&(*it));
    }

    // Emit oldest-first so the text reads in chronological order.
    std::string out;
    for (auto it = selected.rbegin(); it != selected.rend(); ++it) {
        out += "[";
        out += logTypeName((*it)->type);
        out += "] ";
        out += (*it)->message;
        out.push_back('\n');
    }
    return out;
}

bool OutputWindow::getLatestLog(LogType& type, std::string& message) const {
    if (logs.empty()) {
        return false;
    }
    type = logs.back().type;
    message = logs.back().message;
    return true;
}

bool OutputWindow::passTextFilter(const char* text) const {
    if (!filter.IsActive())
        return true;

    // Default (case-insensitive) -> use Dear ImGui implementation
    if (!searchMatchCase)
        return filter.PassFilter(text);

    // Case-sensitive path, same token rules as ImGuiTextFilter
    const char* text_begin = text;
    const char* text_end   = text + std::strlen(text);

    int include_count = 0;
    bool include_matched = false;

    for (int i = 0; i < filter.Filters.Size; ++i) {
        const ImGuiTextFilter::ImGuiTextRange& r = filter.Filters[i];
        if (r.empty()) continue;

        const char* f_begin = r.b;
        const char* f_end   = r.e;

        bool is_exclude = (*f_begin == '-');
        if (is_exclude) {
            f_begin++;
            if (f_begin >= f_end) continue; // empty exclude
        } else {
            include_count++;
        }

        const char* found = std::search(text_begin, text_end, f_begin, f_end);
        if (is_exclude) {
            if (found != text_end) return false;
        } else {
            if (found != text_end) include_matched = true;
        }
    }

    if (include_count > 0)
        return include_matched;

    return true;
}

void OutputWindow::rebuildRows() {
    rows.clear();
    // Collapse merges every identical message (same type and text) into the row
    // of its first occurrence, like Unity.
    std::unordered_map<std::string, size_t> collapsedRows;

    for (size_t i = 0; i < logs.size(); i++) {
        const LogData& log = logs[i];
        if (!severityFilters[severityOf(log.type)]) continue;
        if (!passTextFilter(log.message.c_str())) continue;

        if (collapse) {
            std::string key;
            key.reserve(log.message.size() + 2);
            key.push_back(static_cast<char>('0' + static_cast<int>(log.type)));
            key.push_back('|');
            key += log.message;
            auto it = collapsedRows.find(key);
            if (it != collapsedRows.end()) {
                rows[it->second].count++;
                continue;
            }
            collapsedRows.emplace(std::move(key), rows.size());
        }
        rows.push_back(Row{i, 1});
    }
    needsRebuild = false;
}

const LogData* OutputWindow::selectedLog() const {
    if (!hasSelection) return nullptr;
    for (auto it = logs.rbegin(); it != logs.rend(); ++it) {
        if (it->id == selectedLogId) return &(*it);
        if (it->id < selectedLogId) break;
    }
    return nullptr;
}

void OutputWindow::copyAllToClipboard() const {
    std::string out;
    for (const Row& row : rows) {
        const LogData& log = logs[row.logIndex];
        out += "[";
        out += logTypeName(log.type);
        out += "] ";
        out += log.message;
        out.push_back('\n');
    }
    if (!out.empty()) {
        ImGui::SetClipboardText(out.c_str());
    }
}

void OutputWindow::updatePlayState() {
    Project* project = Backend::getApp().getProject();
    const bool playing = project && project->isPlaySessionActive();

    if (playing && !wasPlaying && clearOnPlay) {
        // Messages logged since the last idle frame came from starting play
        // (e.g. "Scene started"), so only what came before is cleared.
        const uint64_t cutoff = lastIdBeforePlay;
        auto firstKept = std::find_if(logs.begin(), logs.end(), [cutoff](const LogData& l) { return l.id > cutoff; });
        logs.erase(logs.begin(), firstKept);
        storedLogLineCount = 0;
        for (const LogData& log : logs) storedLogLineCount += countRawLines(log.message);
        recountSeverities();
        if (!selectedLog()) hasSelection = false;
        needsRebuild = true;
        autoScroll = true;
    }
    if (!playing) {
        lastIdBeforePlay = nextLogId - 1;
    }
    wasPlaying = playing;

    if (pendingErrorPause) {
        pendingErrorPause = false;
        if (project && errorPause && project->isMainScenePlaying()) {
            // pause() only acts on the play session's main scene
            for (SceneProject& scene : project->getScenes()) {
                if (scene.playState == ScenePlayState::PLAYING) {
                    project->pause(scene.id);
                }
            }
        }
    }
}

void OutputWindow::showToolbar() {
    ImGuiStyle& style = ImGui::GetStyle();
    const float barPadY = Theme::dpi(2.0f);
    const float barPadX = Theme::dpi(4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Theme::dpi(6.0f), Theme::dpi(2.0f)));
    const float frameH = ImGui::GetFrameHeight();
    const float barH = frameH + barPadY * 2.0f;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 barMin = ImGui::GetCursorScreenPos();
    ImVec2 barMax = ImVec2(barMin.x + ImGui::GetContentRegionAvail().x, barMin.y + barH);
    dl->AddRectFilled(barMin, barMax, ImGui::GetColorU32(Theme::Colors::ToolbarBackground));
    dl->AddLine(ImVec2(barMin.x, barMax.y - 0.5f), ImVec2(barMax.x, barMax.y - 0.5f), ImGui::GetColorU32(Theme::Colors::ToolbarBorder), Theme::dpi(1.0f));

    ImGui::SetCursorScreenPos(ImVec2(barMin.x + barPadX, barMin.y + barPadY));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Theme::dpi(2.0f), style.ItemSpacing.y));

    if (toolbarButton("Clear", false, "Clear all messages")) {
        clear();
    }
    toolbarSeparator();
    if (toolbarButton("Collapse", collapse, "Merge identical messages into one row with a count")) {
        collapse = !collapse;
        needsRebuild = true;
    }
    ImGui::SameLine();
    if (toolbarButton("Clear on Play", clearOnPlay, "Clear the Console when entering Play mode")) {
        clearOnPlay = !clearOnPlay;
    }
    ImGui::SameLine();
    if (toolbarButton("Error Pause", errorPause, "Pause Play mode when an error is logged")) {
        errorPause = !errorPause;
    }

    // Right side: search field, then the Log / Warning / Error counters.
    struct Counter { Severity severity; LogType iconType; const char* tooltip; };
    const Counter counters[] = {
        { SEVERITY_LOG, LogType::Info, "Show log messages" },
        { SEVERITY_WARNING, LogType::Warning, "Show warnings" },
        { SEVERITY_ERROR, LogType::Error, "Show errors" },
    };
    const float iconGap = Theme::dpi(4.0f);
    std::string counterLabels[SEVERITY_COUNT];
    float counterWidths[SEVERITY_COUNT];
    float countersWidth = 0.0f;
    for (int i = 0; i < SEVERITY_COUNT; i++) {
        counterLabels[i] = countLabel(severityCounts[counters[i].severity]);
        counterWidths[i] = ImGui::CalcTextSize(logIcon(counters[i].iconType)).x + iconGap +
                           ImGui::CalcTextSize(counterLabels[i].c_str()).x + style.FramePadding.x * 2.0f;
        countersWidth += counterWidths[i] + Theme::dpi(2.0f);
    }

    const float rightEdge = barMax.x - barPadX;
    const float searchWidth = std::min(Theme::dpi(220.0f), std::max(Theme::dpi(90.0f), (rightEdge - barMin.x) * 0.3f));
    const float searchX = rightEdge - countersWidth - Theme::dpi(6.0f) - searchWidth;

    ImGui::SameLine();
    if (searchX > ImGui::GetCursorScreenPos().x) {
        ImGui::SetCursorScreenPos(ImVec2(searchX, barMin.y + barPadY));
    }
    bool prevMatchCase = searchMatchCase;
    if (UIUtils::searchInput("##console_search", "Search", filter.InputBuf, IM_ARRAYSIZE(filter.InputBuf), false, &searchMatchCase, searchWidth)) {
        filter.Build();
        needsRebuild = true;
    }
    if (prevMatchCase != searchMatchCase) {
        needsRebuild = true;
    }

    ImGui::SameLine(0.0f, Theme::dpi(6.0f));
    for (int i = 0; i < SEVERITY_COUNT; i++) {
        bool& enabled = severityFilters[counters[i].severity];
        ImGui::PushID(i);
        ImVec4 iconCol = logIconColor(counters[i].iconType);
        if (!enabled) iconCol.w *= 0.45f;
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::PushStyleColor(ImGuiCol_Button, enabled ? Theme::Colors::ButtonActivated : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, enabled ? Theme::Colors::ButtonActivated : Theme::Colors::IconButtonHovered);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Theme::Colors::IconButtonActive);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        if (ImGui::Button("##counter", ImVec2(counterWidths[i], frameH))) {
            enabled = !enabled;
            needsRebuild = true;
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip("%s", counters[i].tooltip);
        }
        const char* icon = logIcon(counters[i].iconType);
        ImVec2 textPos(pos.x + style.FramePadding.x, pos.y + (frameH - ImGui::GetTextLineHeight()) * 0.5f);
        dl->AddText(textPos, ImGui::GetColorU32(iconCol), icon);
        textPos.x += ImGui::CalcTextSize(icon).x + iconGap;
        dl->AddText(textPos, ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled), counterLabels[i].c_str());
        ImGui::PopID();
        if (i + 1 < SEVERITY_COUNT) ImGui::SameLine();
    }

    ImGui::PopStyleVar(2);
    ImGui::SetCursorScreenPos(ImVec2(barMin.x, barMax.y));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

void OutputWindow::showList(float height) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("##ConsoleList", ImVec2(0, height), ImGuiChildFlags_None, ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar();

    const ImGuiStyle& style = ImGui::GetStyle();
    const float rowHeight = std::floor(ImGui::GetTextLineHeight() + Theme::dpi(6.0f));
    const float textOffsetY = (rowHeight - ImGui::GetTextLineHeight()) * 0.5f;
    const bool listFocused = ImGui::IsWindowFocused();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    int selectedRow = -1;
    if (hasSelection) {
        for (size_t i = 0; i < rows.size(); i++) {
            if (logs[rows[i].logIndex].id == selectedLogId) { selectedRow = static_cast<int>(i); break; }
        }
    }

    // Keyboard: arrows move the selection, Ctrl+C copies it.
    if (listFocused && !rows.empty()) {
        int move = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) move = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) move = 1;
        if (move != 0) {
            int target = selectedRow < 0 ? (move > 0 ? 0 : static_cast<int>(rows.size()) - 1) : selectedRow + move;
            target = std::clamp(target, 0, static_cast<int>(rows.size()) - 1);
            selectedRow = target;
            selectedLogId = logs[rows[target].logIndex].id;
            hasSelection = true;
            scrollToSelection = true;
            autoScroll = false;
        }
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) {
            if (const LogData* log = selectedLog()) ImGui::SetClipboardText(log->message.c_str());
        }
    }

    const float width = ImGui::GetContentRegionAvail().x;
    const ImU32 altBg = ImGui::GetColorU32(Theme::Colors::RowBackgroundAlt);
    const ImU32 selBg = ImGui::GetColorU32(listFocused ? Theme::Colors::Selection : Theme::Colors::SelectionUnfocused);
    const ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 badgeBg = ImGui::GetColorU32(style.Colors[ImGuiCol_Button]);
    const float iconColumn = ImGui::GetFontSize() + Theme::dpi(10.0f);

    int clickedRow = -1;
    int contextRow = -1;

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(rows.size()), rowHeight);
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
            const Row& row = rows[i];
            const LogData& log = logs[row.logIndex];
            ImVec2 min = ImGui::GetCursorScreenPos();
            ImVec2 max(min.x + width, min.y + rowHeight);

            ImGui::PushID(i);
            ImGui::InvisibleButton("##row", ImVec2(width, rowHeight));
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) clickedRow = i;
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) contextRow = i;
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();

            if (i == selectedRow) {
                dl->AddRectFilled(min, max, selBg);
            } else if (hovered) {
                dl->AddRectFilled(min, max, ImGui::GetColorU32(Theme::Colors::IconButtonHovered));
            } else if (i % 2 == 1) {
                dl->AddRectFilled(min, max, altBg);
            }

            const char* icon = logIcon(log.type);
            ImVec2 iconSize = ImGui::CalcTextSize(icon);
            dl->AddText(ImVec2(min.x + (iconColumn - iconSize.x) * 0.5f, min.y + textOffsetY),
                        ImGui::GetColorU32(logIconColor(log.type)), icon);

            float textRight = max.x - Theme::dpi(6.0f);
            if (row.count > 1) {
                std::string badge = countLabel(static_cast<size_t>(row.count));
                ImVec2 bs = ImGui::CalcTextSize(badge.c_str());
                float padX = Theme::dpi(6.0f);
                ImVec2 bMax(max.x - Theme::dpi(6.0f), min.y + textOffsetY + bs.y);
                ImVec2 bMin(bMax.x - bs.x - padX * 2.0f, min.y + textOffsetY);
                dl->AddRectFilled(bMin, bMax, badgeBg, bs.y * 0.5f);
                dl->AddText(ImVec2(bMin.x + padX, bMin.y), textCol, badge.c_str());
                textRight = bMin.x - Theme::dpi(6.0f);
            }

            const std::string text = firstLine(log.message);
            ImVec2 textPos(min.x + iconColumn, min.y + textOffsetY);
            ImVec4 clip(textPos.x, min.y, textRight, max.y);
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), textPos, textCol, text.c_str(), nullptr, 0.0f, &clip);
        }
    }
    clipper.End();

    if (clickedRow >= 0) {
        selectedLogId = logs[rows[clickedRow].logIndex].id;
        hasSelection = true;
        autoScroll = false;
    }
    if (contextRow >= 0) {
        selectedLogId = logs[rows[contextRow].logIndex].id;
        hasSelection = true;
        ImGui::OpenPopup("##ConsoleRowMenu");
    } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && ImGui::IsWindowHovered()) {
        ImGui::OpenPopup("##ConsoleRowMenu");
    }

    if (ImGui::BeginPopup("##ConsoleRowMenu")) {
        const LogData* log = selectedLog();
        if (ImGui::MenuItem(ICON_FA_COPY " Copy", "Ctrl+C", false, log != nullptr)) {
            ImGui::SetClipboardText(log->message.c_str());
        }
        if (ImGui::MenuItem(ICON_FA_CLIPBOARD_LIST " Copy All", nullptr, false, !rows.empty())) {
            copyAllToClipboard();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_TRASH " Clear", nullptr, false, !logs.empty())) {
            clear();
        }
        ImGui::EndPopup();
    }

    // Keep the keyboard selection in view.
    if (scrollToSelection && selectedRow >= 0) {
        float y = selectedRow * rowHeight;
        float viewH = ImGui::GetWindowHeight();
        if (y < ImGui::GetScrollY()) ImGui::SetScrollY(y);
        else if (y + rowHeight > ImGui::GetScrollY() + viewH) ImGui::SetScrollY(y + rowHeight - viewH);
        scrollToSelection = false;
    }

    // Follow new messages while scrolled to the bottom; scrolling up stops it.
    const bool wheelUp = ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel > 0.0f;
    const ImGuiID scrollbarId = ImGui::GetWindowScrollbarID(ImGui::GetCurrentWindow(), ImGuiAxis_Y);
    const bool draggingScrollbar = ImGui::GetActiveID() == scrollbarId;
    if (wheelUp || draggingScrollbar) autoScroll = false;
    else if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) autoScroll = true;
    if (autoScroll) {
        // From this frame's rows: GetScrollMaxY() still has last frame's height.
        const float contentH = static_cast<float>(rows.size()) * rowHeight;
        ImGui::SetScrollY(std::max(0.0f, contentH - ImGui::GetWindowHeight()));
    }

    ImGui::EndChild();
}

void OutputWindow::showDetails() {
    const LogData* log = selectedLog();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("##ConsoleDetails", ImVec2(0, 0), ImGuiChildFlags_None);
    ImGui::PopStyleVar();
    if (log) {
        ImGui::PushID(static_cast<int>(log->id));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, Theme::dpi(ImVec2(8.0f, 6.0f)));
        // Read-only, so ImGui never writes into the buffer.
        ImGui::InputTextMultiline("##details", const_cast<char*>(log->message.c_str()), log->message.size() + 1,
                                  ImVec2(-FLT_MIN, -FLT_MIN), ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_WordWrap);
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void OutputWindow::show() {
    updatePlayState();

    if (!windowOpen) {
        isWindowVisible = false;
        return;
    }

    if (focusRequested) {
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }

    bool wasOpen = windowOpen;

    if (hasNotification) {
        App::pushTabNotificationStyle();
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const bool visible = ImGui::Begin(OutputWindow::WINDOW_NAME, &windowOpen, hasNotification ? ImGuiWindowFlags_UnsavedDocument : 0);
    ImGui::PopStyleVar();
    if (hasNotification) App::popTabNotificationStyle();
    if (!visible) {
        isWindowVisible = false;
        ImGui::End();
        if (wasOpen && !windowOpen) {
            setOpen(false);
        }
        return;
    }

    isWindowVisible = true;
    hasNotification = false;

    if (needsRebuild) {
        rebuildRows();
    }

    showToolbar();

    // List above, details pane below, split by a draggable bar.
    const float splitterH = Theme::dpi(4.0f);
    const float avail = ImGui::GetContentRegionAvail().y;
    const float minPane = ImGui::GetTextLineHeight() * 2.0f;
    float details = std::clamp(Theme::dpi(detailsHeight), minPane, std::max(minPane, avail - minPane - splitterH));
    const float listH = std::max(1.0f, avail - details - splitterH);

    showList(listH);

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImVec2 splitMin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##ConsoleSplitter", ImVec2(ImGui::GetContentRegionAvail().x, splitterH));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }
    if (ImGui::IsItemActive()) {
        detailsHeight = std::max(20.0f, detailsHeight - ImGui::GetIO().MouseDelta.y / Theme::dpiScale());
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(splitMin, ImVec2(splitMin.x + ImGui::GetItemRectSize().x, splitMin.y + splitterH),
                      ImGui::GetColorU32(Theme::Colors::ToolbarBorder));
    ImGui::PopStyleVar();

    showDetails();

    ImGui::End();

    if (wasOpen && !windowOpen) {
        setOpen(false);
    }
}
