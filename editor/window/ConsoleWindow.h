// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>
#include <string>
#include "imgui.h"

namespace jasperengine::editor {
    enum class LogType {
        Info,
        Warning,
        Error,
        Success,
        Build
    };

    struct LogData {
        LogType type;
        std::string message;
        uint64_t id = 0;                     // increasing sequence number
    };

    // The Console. Unity-style: a toolbar (Clear, Collapse, Clear on Play,
    // Error Pause, search, Log/Warning/Error counters that filter), one row per
    // message, and a details pane with the full text of the selected message.
    class ConsoleWindow {
    private:
        // Severity buckets the counters and filters work on. Info and Success
        // are "Log"; Build (compiler/shader output) folds into Log as well.
        enum Severity { SEVERITY_LOG = 0, SEVERITY_WARNING = 1, SEVERITY_ERROR_MESSAGE = 2, SEVERITY_COUNT = 3 };

        struct Row {
            size_t logIndex;                 // entry shown (the first occurrence when collapsed)
            int count;                       // identical entries merged into this row
        };

        std::deque<LogData> logs;             // constant-time eviction of the oldest entry
        size_t storedLogLineCount;           // raw log lines kept, trimmed to MAX_STORED_LOG_LINES
        uint64_t nextLogId;
        size_t severityCounts[SEVERITY_COUNT];

        static constexpr size_t MAX_STORED_LOG_LINES = 10000;

        std::vector<Row> rows;               // filtered (and collapsed) view of logs
        bool needsRebuild;

        ImGuiTextFilter filter;
        bool searchMatchCase;
        bool severityFilters[SEVERITY_COUNT];

        bool collapse;
        bool clearOnPlay;
        bool errorPause;

        // Selection is kept by log id so it survives rebuilds and trimming.
        uint64_t selectedLogId;
        bool hasSelection;
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
        static Severity severityOf(LogType type);
        void enforceStoredLineLimit();
        void recountSeverities();
        void rebuildRows();
        bool passTextFilter(const char* text) const;
        const LogData* selectedLog() const;

        void updatePlayState();
        void showToolbar();
        void showList(float height);
        void showDetails();
        void copyAllToClipboard() const;

    public:
        static constexpr const char* WINDOW_NAME = "Console";

        ConsoleWindow();
        void clear();
        void addLog(LogType type, const char* fmt, ...);
        void addLog(LogType type, const std::string& message);
        void show();
        void setOpen(bool open);
        bool isOpen() const;
        // Opens the Console if needed and brings its tab to the front.
        void requestFocus();

        // Toolbar toggles (MCP preferences)
        bool isClearOnPlay() const { return clearOnPlay; }
        void setClearOnPlay(bool enabled) { clearOnPlay = enabled; }
        bool isErrorPause() const { return errorPause; }
        void setErrorPause(bool enabled) { errorPause = enabled; }

        // Newest message, for the status bar. False when the Console is empty.
        bool getLatestLog(LogType& type, std::string& message) const;

    };
}
