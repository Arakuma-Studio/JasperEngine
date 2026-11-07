// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once
#include "jobs/JobSystem.h"
#include "imgui.h"
#include "widget/CustomTextEditor.h"
#include "Project.h"
#include "util/EntityPayload.h"
#include "util/ScriptEvents.h"
#include "vcs/TextBaselines.h"
#include <optional>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <filesystem>
#include <thread>
#include <mutex>
#include <atomic>

namespace fs = std::filesystem;

namespace jasperengine::editor {

    struct EditorInstance {
        std::unique_ptr<CustomTextEditor> editor;
        bool isOpen;
        fs::path filepath;
        SyntaxLanguage languageType;
        fs::file_time_type lastWriteTime;
        bool isModified;
        int savedUndoIndex;
        int propertyInsertUndoIndex; // undo index after a drag-drop insertion, -1 if none pending
        bool pendingWindowFocus; // focus the window when it is drawn next
        uint64_t baselineVersion = 0; // of the vcs::TextBaseline the change markers come from

        EditorInstance() : isOpen(true), languageType(SyntaxLanguage::None), isModified(false), savedUndoIndex(0), propertyInsertUndoIndex(-1), pendingWindowFocus(false) {}
    };

    class CodeEditor {
    private:
        struct PendingFileChange {
            fs::path filepath;
            fs::file_time_type newWriteTime;
        };

        Project* project;

        std::unordered_map<std::string, EditorInstance> editors;
        std::vector<PendingFileChange> changedFilesQueue;
        bool isFileChangePopupOpen;
        bool windowFocused;
        EditorInstance* lastFocused;

        // the project symbol parse, a Background job (it captures this editor)
        jobs::Task<void> symbolParseTask;
        std::atomic<bool> isParsingSymbols{false};
        std::mutex parsedSymbolsMutex;
        std::vector<CustomTextEditor::ProjectSymbol> newLuaSymbols;
        std::vector<CustomTextEditor::ProjectSymbol> newCppSymbols;
        std::atomic<bool> newSymbolsReady{false};

        ScriptEventScan eventMenuScan;

        // the open files' version control baselines (the change markers)
        vcs::TextBaselines baselines;
        void updateBaselines();
        // the status row's version control segment and its menu
        void showVcsSegment(EditorInstance& instance);
        // Added after the windows are drawn, it can open a file
        std::string pendingEventFile;
        size_t pendingEvent = 0;

        // The file changed on disk: an unmodified buffer reloads (true), a modified one gets
        // the Files Changed prompt
        bool reloadIfChangedOnDisk(EditorInstance& instance);
        bool loadFileContent(EditorInstance& instance);
        bool save(EditorInstance& instance);
        void handleFileChangePopup();
        std::string getWindowTitle(const EditorInstance& instance) const;
        void updateScriptProperties(const EditorInstance& instance, const std::string& inMemoryContent = "");

        void updateAllProjectSymbols();
        void applyParsedProjectSymbols();

        fs::path resolveFilepath(const fs::path& relPath) const;
        std::string toRelativePath(const std::string& filepath) const;
        void insertLuaEntityProperty(EditorInstance& instance, Entity entity, uint32_t entitySceneId);
        void insertCppEntityProperty(EditorInstance& instance, Entity entity, uint32_t entitySceneId);

        static std::string toCamelCase(const std::string& name);
        static std::string toDisplayName(const std::string& camelCase);
        static void offsetToLineCol(const std::string& text, size_t offset, int& line, int& col);

        static void applyFontZoom(int delta);
        void showSettingsButton();

        // Source of a header or header of a source, empty when there is none
        fs::path findCompanionFile(const fs::path& relPath) const;
        void getScriptEventFiles(const EditorInstance& instance, std::vector<fs::path>& paths, std::vector<std::string>& texts) const;
        void showEventsButton(const EditorInstance& instance);
        void addScriptEvent(const std::string& filepath, size_t event);

    public:
        // the script entries reading their properties from this file (a path relative to the project,
        // or absolute) take the file's, or the code being edited
        void updateScriptPropertiesForPath(const fs::path& relFilepath, const std::string& inMemoryContent = "");
        CodeEditor(Project* project);
        ~CodeEditor();

        bool isFocused() const;
        void closeAll();

        bool saveLastFocused();
        void saveAll();
        void undoLastFocused();
        void redoLastFocused();
        bool canUndoLastFocused() const;
        bool canRedoLastFocused() const;

        bool hasUnsavedChanges() const;
        bool hasLastFocusedUnsavedChanges() const;

        void openFile(const std::string& filepath, bool dockToCentral = false);
        void closeFile(const std::string& filepath);
        bool isFileOpen(const std::string& filepath) const;
        bool isFileModified(const std::string& filepath) const;
        bool handleFileRename(const fs::path& oldPath, const fs::path& newPath);
        // Save Project As continued the project in a copy at another folder: the tabs follow their
        // files there (a path kept outside the old folder stays), and take the copies' write times,
        // so the next save writes the copy, never the original
        void retargetProject(const fs::path& oldProjectPath);

        // ---- MCP (files edited by external agents) ----
        struct BufferState {
            std::string path;       // project-relative key
            bool modified = false;  // unsaved edits in the buffer
            int cursorLine = 0;     // 1-based
            int lineCount = 0;
            // version control: the baseline's state ("ready", "untracked", ...; "none" when off),
            // its revision and the buffer's changes against it
            std::string baselineState = "none";
            std::string baselineRevision;
            std::vector<LineChange> changes;
        };
        // Opens (or focuses) a file and puts the cursor at line/column (1-based, 0 = keep)
        void openFileAt(const std::string& filepath, int line, int column);
        std::vector<BufferState> getBufferStates() const;
        // Files changed on disk (the hot reload tells it, S05): their unmodified buffers
        // reload, modified ones get the "Files Changed" prompt and are listed in `conflicts`
        void onFilesChanged(const std::vector<fs::path>& files, std::vector<std::string>& reloaded, std::vector<std::string>& conflicts);
        // The same for every open buffer (a full sync)
        void syncWithDisk(std::vector<std::string>& reloaded, std::vector<std::string>& conflicts);
        bool saveFile(const std::string& filepath);
        // Discards the buffer's unsaved edits and loads the file from disk
        bool reloadFile(const std::string& filepath);
        // Revert Change on the change at a 1-based line (or the line a deletion is marked at): one
        // undo step in the buffer, unsaved
        bool revertChangeAt(const std::string& filepath, int line, std::string& error);
        // The buffer's text (unsaved edits included); none when the file is not open
        std::optional<std::string> bufferText(const std::string& filepath) const;

        void show();
    };
}
