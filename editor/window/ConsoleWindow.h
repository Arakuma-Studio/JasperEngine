// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <set>
#include <vector>
#include <string>
#include "imgui.h"
#include "log/LogCategory.h"
#include "util/SourceLocation.h"

namespace jasperengine::editor {
    enum class LogType {
        Info,
        Warning,
        Error,
        Success,
        Build
    };

    struct LogEntry;

    struct LogData {
        LogType type;
        // the counter and filter bucket (ConsoleWindow::Severity): a build line by its own level
        int severity = 0;
        std::string message;
        uint64_t id = 0;                     // the logger's record id (increasing)
        bool detail = false;                 // Debug or Trace: drawn grey
        std::string category;
        std::string file;                    // source, empty if none
        uint32_t line = 0;
        uint64_t frame = 0;
        std::string threadName;
        // the file locations in the message (tracebacks: all of them), parsed once when it
        // arrives; the logger's source (file, line) is not among them
        std::vector<SourceLocation> locations;
    };

    // The Console. Unity-style: a toolbar (Clear, Collapse, Clear on Play,
    // Error Pause, search, Log/Warning/Error counters that filter), one row per
    // message, and a details pane with the full text of the selected message.
    class ConsoleWindow {
    public:
        // Severity buckets the counters and filters work on. Info and Success are "Log"; a Build
        // line (compiler/shader output) goes by its level: a compiler error counts as an Error.
        enum Severity { SEVERITY_LOG = 0, SEVERITY_WARNING = 1, SEVERITY_ERROR_MESSAGE = 2, SEVERITY_COUNT = 3 };
        static Severity severityOf(LogType type, LogLevel level);
        // Opens a file location of a message (a build-tree copy maps back to its project file):
        // Preferences > External Tools > External Script Editor when external, else the Code Editor
        static void openLocation(const SourceLocation& location, bool external);

    private:

        struct Row {
            size_t logIndex;                 // entry shown (the first occurrence when collapsed)
            int count;                       // identical entries merged into this row
        };

        std::deque<LogData> logs;             // constant-time eviction of the oldest entry
        size_t storedLogLineCount;           // raw log lines kept, trimmed to MAX_STORED_LOG_LINES
        uint64_t lastPulledId;               // the newest record taken from the log (Out)
        size_t severityCounts[SEVERITY_COUNT];

        static constexpr size_t MAX_STORED_LOG_LINES = 10000;

        std::vector<Row> rows;               // filtered (and collapsed) view of logs
        bool needsRebuild;

        ImGuiTextFilter filter;
        bool searchMatchCase;
        bool severityFilters[SEVERITY_COUNT];
        // Category filter: every category seen, and those unticked
        std::set<std::string> seenCategories;
        std::set<std::string> hiddenCategories;

        bool collapse;
        bool clearOnPlay;
        bool errorPause;

        // Selection is kept by log id so it survives rebuilds and trimming.
        uint64_t selectedLogId;
        bool hasSelection;
        bool listFocused = false;   // the message list had the keyboard last frame
        bool scrollToSelection;

        bool autoScroll;                     // stick to the newest message
        float detailsHeight;                 // logical px, the pane under the list

        // Play-state tracking for Clear on Play and Error Pause.
        bool wasPlaying;
        uint64_t lastIdBeforePlay;           // newest id seen on the last frame not playing
        bool pendingErrorPause;

        // Tab notification
        bool hasNotification;
        bool windowOpen;
        bool focusRequested;
        bool isWindowVisible;

        static size_t countRawLines(const std::string& message);
        static size_t trimLeadingRawLines(std::string& message, size_t linesToTrim);
        void enforceStoredLineLimit();
        void recountSeverities();
        void rebuildRows();
        bool passTextFilter(const char* text) const;
        const LogData* selectedLog() const;

        void pullLogs();
        std::string originText(const LogData& log) const;
        void addEntry(LogEntry&& entry);
        void updatePlayState();
        void showToolbar();
        void showList(float height);
        void showDetails();
        void copyAllToClipboard() const;
        // Where a message's links go: the logger's source first, then its locations
        static std::vector<SourceLocation> linkTargets(const LogData& log);
        // Opens the first target in the project (any, when none is), preferred: Preferences >
        // External Tools > External Script Editor, else the built-in Code Editor. False when
        // the message names no file.
        static bool openFirstLocation(const LogData& log, bool external);

    public:
        static constexpr const char* WINDOW_NAME = "Console";

        ConsoleWindow();
        void clear();
        // Takes what was logged since the last frame from the log (Out), then draws
        void show();
        void setOpen(bool open);
        bool isOpen() const;
        // Opens the Console if needed and brings its tab to the front.
        void requestFocus();
        // The search field's text (the messages shown are those containing it)
        void setSearchText(const std::string& text);

        // The message list has focus (the catalog's Console context): Ctrl+C copies the
        // selected message (console.copy)
        bool isListFocused() const { return listFocused; }
        bool hasSelectedMessage() const;
        void copySelectedMessage();

        // Toolbar toggles (MCP preferences)
        bool isClearOnPlay() const { return clearOnPlay; }
        void setClearOnPlay(bool enabled) { clearOnPlay = enabled; }
        bool isErrorPause() const { return errorPause; }
        void setErrorPause(bool enabled) { errorPause = enabled; }

        // Newest message, for the status bar. False when the Console is empty.
        bool getLatestLog(LogType& type, std::string& message) const;

        // What the Console shows, for the MCP focus_window tool: its rows after the filters
        // (the newest maxRows of them), the categories of the Category filter and the details
        // pane's origin line (category, file:line, frame, thread) of the selected message
        struct ViewRow {
            uint64_t id;
            LogType type;
            std::string category;
            bool grey;                       // Debug and Trace rows
            std::string text;                // first line
            int count;                       // merged by Collapse
        };
        struct ViewState {
            bool open = false;
            size_t rowCount = 0;
            std::vector<ViewRow> rows;
            std::vector<std::string> categories;
            std::vector<std::string> hiddenCategories;
            bool hasSelection = false;
            uint64_t selectedId = 0;
            std::string selectedOrigin;
            bool collapse = false;
            bool clearOnPlay = false;
            bool errorPause = false;
        };
        ViewState viewState(size_t maxRows);
        // The Category filter as the menu sets it (unknown names are kept for later messages)
        void setHiddenCategories(const std::vector<std::string>& categories);
        void setCollapse(bool enabled) { collapse = enabled; needsRebuild = true; }
        // Selects a message by id, as a click does; false when it is not shown
        bool selectLog(uint64_t id);

    };
}
