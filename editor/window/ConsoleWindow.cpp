// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ConsoleWindow.h"
#include "window/ProjectWindow.h"

#include "shortcuts/CommandCatalog.h"
#include <cstdio>
#include "external/IconsFontAwesome6.h"
#include "util/UIUtils.h"
#include "App.h"
#include "Out.h"
#include "AppSettings.h"
#include "Backend.h"
#include "Theme.h"
#include "Project.h"
#include "util/ExternalCodeEditor.h"
#include "util/FileUtils.h"
#include <cmath>

namespace {

    // The project-relative path a "[Hot Reload] Reloaded <path> (<kind>, <ms> ms)" or a
    // "[Hot Reload] <path>: <error> (kept the previous version)" line names ("" for others)
    std::string hotReloadAsset(const std::string& message) {
        // "[HTML UI] Patched ui/menu.html: ..." / "[HTML UI] Rebuilt ui/menu.html (2.0 ms)..." (S17 R37)
        static const std::string htmlPrefix = "[HTML UI] ";
        if (message.rfind(htmlPrefix, 0) == 0) {
            std::string rest = message.substr(htmlPrefix.size(), message.find('\n') - htmlPrefix.size());
            if (rest.rfind("Patched ", 0) == 0) {
                rest = rest.substr(8);
                const size_t colon = rest.find(": ");
                return colon == std::string::npos ? std::string() : rest.substr(0, colon);
            }
            if (rest.rfind("Rebuilt ", 0) == 0) {
                rest = rest.substr(8);
                const size_t paren = rest.find(" (");
                return paren == std::string::npos ? std::string() : rest.substr(0, paren);
            }
            return {};
        }
        static const std::string prefix = "[Hot Reload] ";
        if (message.rfind(prefix, 0) != 0) return {};
        std::string rest = message.substr(prefix.size());
        if (rest.rfind("Reloaded ", 0) == 0) {
            rest = rest.substr(9);
            const size_t paren = rest.rfind(" (");
            // "Reloaded 37 files: ..." names no single asset
            if (paren == std::string::npos || rest.find(" files: ") != std::string::npos) return {};
            return rest.substr(0, paren);
        }
        const size_t colon = rest.find(": ");
        if (colon == std::string::npos || rest.find("(kept the previous version)") == std::string::npos) return {};
        return rest.substr(0, colon);
    }

}
#include <algorithm>
#include <cstring>
#include <unordered_map>

#include "imgui_internal.h"

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

ConsoleWindow::ConsoleWindow() {
    storedLogLineCount = 0;
    lastPulledId = 0;
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

void ConsoleWindow::setOpen(bool open) {
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

bool ConsoleWindow::isOpen() const {
    return windowOpen;
}

void ConsoleWindow::setSearchText(const std::string& text) {
    std::snprintf(filter.InputBuf, sizeof(filter.InputBuf), "%s", text.c_str());
    filter.Build();
    needsRebuild = true;
}

void ConsoleWindow::requestFocus() {
    windowOpen = true;
    focusRequested = true;
}

void ConsoleWindow::clear() {
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

ConsoleWindow::Severity ConsoleWindow::severityOf(LogType type) {
    switch (type) {
        case LogType::Warning: return SEVERITY_WARNING;
        case LogType::Error:   return SEVERITY_ERROR_MESSAGE;
        default:               return SEVERITY_LOG;
    }
}

size_t ConsoleWindow::countRawLines(const std::string& message) {
    size_t lines = 1;
    for (char c : message) {
        if (c == '\n') {
            lines++;
        }
    }
    return lines;
}

size_t ConsoleWindow::trimLeadingRawLines(std::string& message, size_t linesToTrim) {
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

void ConsoleWindow::enforceStoredLineLimit() {
    if (storedLogLineCount <= MAX_STORED_LOG_LINES) {
        return;
    }

    while (storedLogLineCount > MAX_STORED_LOG_LINES && !logs.empty()) {
        const size_t excess = storedLogLineCount - MAX_STORED_LOG_LINES;
        const size_t frontLines = countRawLines(logs.front().message);

        if (frontLines <= excess) {
            storedLogLineCount -= frontLines;
            severityCounts[severityOf(logs.front().type)]--;
            logs.pop_front();
            continue;
        }

        // A partial trim keeps the entry, including its id and severity count.
        storedLogLineCount -= trimLeadingRawLines(logs.front().message, excess);
        break;
    }
}

void ConsoleWindow::recountSeverities() {
    for (int i = 0; i < SEVERITY_COUNT; i++) {
        severityCounts[i] = 0;
    }
    for (const LogData& log : logs) {
        severityCounts[severityOf(log.type)]++;
    }
}

void ConsoleWindow::pullLogs() {
    // everything logged since the last frame, from any thread (the log keeps the newest 20,000)
    std::vector<LogEntry> entries = Out::getLogsSince(lastPulledId);
    for (LogEntry& entry : entries) {
        lastPulledId = entry.id;
        addEntry(std::move(entry));
    }
}

void ConsoleWindow::addEntry(LogEntry&& entry) {
    LogData logEntry;
    logEntry.type = entry.type;
    logEntry.id = entry.id;
    logEntry.detail = entry.level <= LogLevel::Debug;
    logEntry.category = std::move(entry.category);
    logEntry.file = std::move(entry.file);
    logEntry.line = entry.line;
    logEntry.frame = entry.frame;
    logEntry.threadName = std::move(entry.threadName);
    logEntry.message = std::move(entry.message);
    logEntry.locations = findSourceLocations(logEntry.message);
    const LogType type = logEntry.type;
    if (seenCategories.insert(logEntry.category).second) needsRebuild = true;
    storedLogLineCount += countRawLines(logEntry.message);
    logs.push_back(std::move(logEntry));
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

namespace {

    SourceRoots consoleRoots(){
        SourceRoots roots;
        if (Project* project = Backend::getApp().getProject()) {
            roots.project = project->getProjectPath();
            roots.lua = project->getLuaPath();
            roots.assets = project->getAssetsPath();
        }
        if (!FileUtils::isEngineDirEphemeral()) roots.engine = FileUtils::getEngineDir();
        return roots;
    }

}

std::vector<SourceLocation> ConsoleWindow::linkTargets(const LogData& log) {
    std::vector<SourceLocation> targets;
    if (!log.file.empty() && log.line > 0) {
        SourceLocation source;
        source.path = log.file;
        source.line = static_cast<int>(log.line);
        targets.push_back(source);
    }
    for (const SourceLocation& location : log.locations) {
        if (!targets.empty() && targets[0].path == location.path && targets[0].line == location.line) continue;
        targets.push_back(location);
    }
    return targets;
}

void ConsoleWindow::openLocation(const SourceLocation& location, bool external) {
    const ResolvedSource resolved = resolveSourceLocation(location.path, consoleRoots());
    if (!resolved.exists) {
        Out::warning("%s not found", formatSourceLocation(location.path, location.line, location.column).c_str());
        return;
    }
    std::string error;
    const bool opened = external ? ExternalCodeEditor::openExternal(resolved.absolute, location.line, location.column, &error)
                                 : ExternalCodeEditor::open(resolved.absolute, location.line, location.column, true, &error);
    if (!opened) Out::warning("Could not open %s: %s", FileUtils::pathToUtf8(resolved.absolute).c_str(), error.c_str());
}

bool ConsoleWindow::openFirstLocation(const LogData& log, bool external) {
    const std::vector<SourceLocation> targets = linkTargets(log);
    if (targets.empty()) return false;
    const SourceRoots roots = consoleRoots();
    for (const SourceLocation& location : targets) {
        const ResolvedSource resolved = resolveSourceLocation(location.path, roots);
        if (resolved.exists && resolved.inProject) {
            openLocation(location, external);
            return true;
        }
    }
    openLocation(targets[0], external);
    return true;
}

std::string ConsoleWindow::originText(const LogData& log) const {
    std::string origin = log.category;
    if (!log.file.empty() && AppSettings::getLoggingSettings().showSource) {
        origin += "  " + log.file + ":" + std::to_string(log.line);
    }
    origin += "  frame " + std::to_string(log.frame);
    if (!log.threadName.empty()) origin += "  thread " + log.threadName;
    return origin;
}

ConsoleWindow::ViewState ConsoleWindow::viewState(size_t maxRows) {
    pullLogs();
    if (needsRebuild) rebuildRows();
    ViewState state;
    state.open = windowOpen;
    state.rowCount = rows.size();
    const size_t first = rows.size() > maxRows ? rows.size() - maxRows : 0;
    for (size_t i = first; i < rows.size(); i++) {
        const LogData& log = logs[rows[i].logIndex];
        state.rows.push_back(ViewRow{log.id, log.type, log.category, log.detail, firstLine(log.message), rows[i].count});
    }
    state.categories.assign(seenCategories.begin(), seenCategories.end());
    state.hiddenCategories.assign(hiddenCategories.begin(), hiddenCategories.end());
    if (const LogData* log = selectedLog()) {
        state.hasSelection = true;
        state.selectedId = log->id;
        state.selectedOrigin = originText(*log);
    }
    state.collapse = collapse;
    state.clearOnPlay = clearOnPlay;
    state.errorPause = errorPause;
    return state;
}

void ConsoleWindow::setHiddenCategories(const std::vector<std::string>& categories) {
    hiddenCategories = std::set<std::string>(categories.begin(), categories.end());
    needsRebuild = true;
}

bool ConsoleWindow::selectLog(uint64_t id) {
    pullLogs();
    if (needsRebuild) rebuildRows();
    for (const Row& row : rows) {
        if (logs[row.logIndex].id != id) continue;
        selectedLogId = id;
        hasSelection = true;
        scrollToSelection = true;
        autoScroll = false;
        return true;
    }
    return false;
}

bool ConsoleWindow::getLatestLog(LogType& type, std::string& message) const {
    if (logs.empty()) {
        return false;
    }
    type = logs.back().type;
    message = logs.back().message;
    return true;
}

bool ConsoleWindow::passTextFilter(const char* text) const {
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

void ConsoleWindow::rebuildRows() {
    rows.clear();
    // Collapse merges every identical message (same type, category and text) into the
    // row of its first occurrence, like Unity.
    std::unordered_map<std::string, size_t> collapsedRows;

    for (size_t i = 0; i < logs.size(); i++) {
        const LogData& log = logs[i];
        if (!severityFilters[severityOf(log.type)]) continue;
        if (!hiddenCategories.empty() && hiddenCategories.count(log.category)) continue;
        if (!passTextFilter(log.message.c_str())) continue;

        if (collapse) {
            // same type, category and text
            std::string key;
            key.reserve(log.message.size() + log.category.size() + 3);
            key.push_back(static_cast<char>('0' + static_cast<int>(log.type)));
            key.push_back('|');
            key += log.category;
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

const LogData* ConsoleWindow::selectedLog() const {
    if (!hasSelection || logs.empty()) return nullptr;
    // Keep the newest selection constant-time; older ids are ordered even after eviction.
    if (logs.back().id <= selectedLogId) {
        return logs.back().id == selectedLogId ? &logs.back() : nullptr;
    }
    const auto found = std::lower_bound(logs.rbegin(), logs.rend(), selectedLogId,
        [](const LogData& log, uint64_t id) { return log.id > id; });
    return found != logs.rend() && found->id == selectedLogId ? &(*found) : nullptr;
}

void ConsoleWindow::copyAllToClipboard() const {
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

void ConsoleWindow::updatePlayState() {
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
        lastIdBeforePlay = lastPulledId;
    }
    wasPlaying = playing;

    // An error while play mode is still starting pauses as soon as it runs.
    if (pendingErrorPause && (!playing || !errorPause)) {
        pendingErrorPause = false;
    }
    if (pendingErrorPause && project->isMainScenePlaying()) {
        pendingErrorPause = false;
        // pause() only acts on the play session's main scene
        for (SceneProject& scene : project->getScenes()) {
            if (scene.playState == ScenePlayState::PLAYING) {
                project->pause(scene.id);
            }
        }
    }
}

void ConsoleWindow::showToolbar() {
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
        // and what Out keeps for structured reads (the MCP console tools)
        Out::clearConsole();
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
    toolbarSeparator();
    if (toolbarButton(hiddenCategories.empty() ? "Category " ICON_FA_CARET_DOWN : "Category* " ICON_FA_CARET_DOWN,
                      !hiddenCategories.empty(), "Show messages of these log categories")) {
        ImGui::OpenPopup("##ConsoleCategories");
    }
    if (ImGui::BeginPopup("##ConsoleCategories")) {
        if (ImGui::MenuItem("Everything", nullptr, false, !hiddenCategories.empty())) {
            hiddenCategories.clear();
            needsRebuild = true;
        }
        if (ImGui::MenuItem("Nothing", nullptr, false, hiddenCategories.size() != seenCategories.size())) {
            hiddenCategories = seenCategories;
            needsRebuild = true;
        }
        ImGui::Separator();
        for (const std::string& category : seenCategories) {
            bool shown = hiddenCategories.count(category) == 0;
            if (ImGui::MenuItem(category.empty() ? "(none)" : category.c_str(), nullptr, &shown, true)) {
                if (shown) hiddenCategories.erase(category);
                else hiddenCategories.insert(category);
                needsRebuild = true;
            }
        }
        ImGui::EndPopup();
    }

    // Right side: search field, then the Log / Warning / Error counters.
    struct Counter { Severity severity; LogType iconType; const char* tooltip; };
    const Counter counters[] = {
        { SEVERITY_LOG, LogType::Info, "Show log messages" },
        { SEVERITY_WARNING, LogType::Warning, "Show warnings" },
        { SEVERITY_ERROR_MESSAGE, LogType::Error, "Show errors" },
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

void ConsoleWindow::showList(float height) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("##ConsoleList", ImVec2(0, height), ImGuiChildFlags_None, ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar();

    const ImGuiStyle& style = ImGui::GetStyle();
    const float rowHeight = std::floor(ImGui::GetTextLineHeight() + Theme::dpi(6.0f));
    const float textOffsetY = (rowHeight - ImGui::GetTextLineHeight()) * 0.5f;
    listFocused = ImGui::IsWindowFocused();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    int selectedRow = -1;
    if (hasSelection) {
        for (size_t i = 0; i < rows.size(); i++) {
            if (logs[rows[i].logIndex].id == selectedLogId) { selectedRow = static_cast<int>(i); break; }
        }
    }

    // Keyboard: arrows move the selection (Ctrl+C copying it is the catalog's console.copy)
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
    }

    const float width = ImGui::GetContentRegionAvail().x;
    const ImU32 altBg = ImGui::GetColorU32(Theme::Colors::RowBackgroundAlt);
    const ImU32 selBg = ImGui::GetColorU32(listFocused ? Theme::Colors::Selection : Theme::Colors::SelectionUnfocused);
    const ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 detailCol = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const ImU32 badgeBg = ImGui::GetColorU32(style.Colors[ImGuiCol_Button]);
    const float iconColumn = ImGui::GetFontSize() + Theme::dpi(10.0f);

    int clickedRow = -1;
    int doubleClickedRow = -1;
    int contextRow = -1;

    // Rows touch, so the clipper's step is exactly rowHeight.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, 0.0f));
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(rows.size()), rowHeight);
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
            const Row& row = rows[i];
            if (row.logIndex >= logs.size()) {
                // Logs trimmed mid-frame; the next rebuild drops this row.
                ImGui::Dummy(ImVec2(width, rowHeight));
                continue;
            }
            const LogData& log = logs[row.logIndex];
            ImVec2 min = ImGui::GetCursorScreenPos();
            ImVec2 max(min.x + width, min.y + rowHeight);

            ImGui::PushID(i);
            ImGui::InvisibleButton("##row", ImVec2(width, rowHeight));
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) clickedRow = i;
            // a double-click opens its first location in the project (click-to-source)
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) doubleClickedRow = i;
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
            // Debug and Trace rows are grey
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), textPos, log.detail ? detailCol : textCol, text.c_str(),
                        nullptr, 0.0f, &clip);
        }
    }
    clipper.End();
    ImGui::PopStyleVar();

    if (clickedRow >= 0 && rows[clickedRow].logIndex < logs.size()) {
        selectedLogId = logs[rows[clickedRow].logIndex].id;
        hasSelection = true;
        autoScroll = false;
        // a hot reload line pings its asset in the Project window (S05 R27)
        const std::string asset = hotReloadAsset(logs[rows[clickedRow].logIndex].message);
        if (!asset.empty() && Backend::getApp().getProjectWindow()) {
            Backend::getApp().getProjectWindow()->revealAsset(Backend::getApp().getProject()->getProjectPath() / asset);
        }
    }
    if (doubleClickedRow >= 0 && rows[doubleClickedRow].logIndex < logs.size()) {
        openFirstLocation(logs[rows[doubleClickedRow].logIndex], false);
    }
    if (contextRow >= 0 && rows[contextRow].logIndex < logs.size()) {
        selectedLogId = logs[rows[contextRow].logIndex].id;
        hasSelection = true;
        ImGui::OpenPopup("##ConsoleRowMenu");
    } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && ImGui::IsWindowHovered()) {
        ImGui::OpenPopup("##ConsoleRowMenu");
    }

    if (ImGui::BeginPopup("##ConsoleRowMenu")) {
        const LogData* log = selectedLog();
        if (ImGui::MenuItem(ICON_FA_COPY " Copy", CommandCatalog::editor().menuShortcutText("console.copy").c_str(), false, log != nullptr)) {
            ImGui::SetClipboardText(log->message.c_str());
        }
        if (ImGui::MenuItem(ICON_FA_CLIPBOARD_LIST " Copy All", nullptr, false, !rows.empty())) {
            copyAllToClipboard();
        }
        const std::vector<SourceLocation> targets = log ? linkTargets(*log) : std::vector<SourceLocation>();
        if (!targets.empty()) {
            ImGui::Separator();
            const std::string where = formatSourceLocation(targets[0].path, targets[0].line, targets[0].column);
            if (ImGui::MenuItem((ICON_FA_FILE_CODE " Open " + where).c_str())) {
                openFirstLocation(*log, false);
            }
            if (ImGui::MenuItem(ICON_FA_ARROW_UP_RIGHT_FROM_SQUARE " Open in External Editor")) {
                openFirstLocation(*log, true);
            }
            if (ImGui::MenuItem(ICON_FA_LOCATION_DOT " Copy Location")) {
                ImGui::SetClipboardText(where.c_str());
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_TRASH " Clear", nullptr, false, !logs.empty())) {
            Out::clearConsole();
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

void ConsoleWindow::showDetails() {
    const LogData* log = selectedLog();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("##ConsoleDetails", ImVec2(0, 0), ImGuiChildFlags_None);
    ImGui::PopStyleVar();
    if (log) {
        ImGui::PushID(static_cast<int>(log->id));
        // where it comes from: category, the call site (file:line), frame and thread
        const std::string origin = originText(*log);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + Theme::dpi(8.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextUnformatted(origin.c_str());
        ImGui::PopStyleColor();
        if (!log->file.empty() && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s:%u", log->file.c_str(), log->line);
        }
        // every location in the message is a link (click-to-source)
        const std::vector<SourceLocation> targets = linkTargets(*log);
        if (!targets.empty()) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + Theme::dpi(8.0f));
            const ImVec4 linkColor = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
            for (size_t i = 0; i < targets.size(); i++) {
                const std::string text = formatSourceLocation(targets[i].path, targets[i].line, targets[i].column);
                if (i > 0) {
                    ImGui::SameLine();
                    if (ImGui::GetContentRegionAvail().x < ImGui::CalcTextSize(text.c_str()).x) ImGui::NewLine();
                }
                ImGui::PushID(static_cast<int>(i));
                ImGui::PushStyleColor(ImGuiCol_Text, linkColor);
                ImGui::TextUnformatted(text.c_str());
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
                    ImGui::GetWindowDrawList()->AddLine(ImVec2(min.x, max.y), max, ImGui::GetColorU32(linkColor));
                    ImGui::SetTooltip("Open %s", text.c_str());
                }
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) openLocation(targets[i], false);
                ImGui::PopID();
            }
        }
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

void ConsoleWindow::show() {
    listFocused = false;
    pullLogs();
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
    const bool visible = ImGui::Begin(ConsoleWindow::WINDOW_NAME, &windowOpen, hasNotification ? ImGuiWindowFlags_UnsavedDocument : 0);
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

bool ConsoleWindow::hasSelectedMessage() const {
    return selectedLog() != nullptr;
}

void ConsoleWindow::copySelectedMessage() {
    if (const LogData* log = selectedLog()) ImGui::SetClipboardText(log->message.c_str());
}
