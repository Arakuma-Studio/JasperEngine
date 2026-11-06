// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "imgui.h"
#include "HtmlCssLexer.h"
#include "util/LineDiff.h"
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_set>
#include <functional>
#include <memory>
#include <chrono>

namespace jasperengine::editor {

    // Forward declarations
    class SemanticSuggestions;
    struct SuggestionItem;
    struct SuggestionContext;
    enum class SuggestionKind;

    enum class SyntaxLanguage {
        None,
        Cpp,
        Lua,
        CMake,
        Html,
        Css
    };

    // Token types for syntax highlighting
    enum class TokenType {
        Default,
        Keyword,
        Type,
        Number,
        String,
        Comment,
        MultiLineComment,
        Preprocessor,
        Identifier,
        Punctuation,
        Operator,
        Function,
        Count
    };

    struct Token {
        int start;
        int length;
        TokenType type;
    };

    // A comment or string that runs past the end of its line, and the text that closes it.
    // Html and Css keep the lexer's state instead, type then tells if it ends inside a comment or quote
    struct OpenToken {
        TokenType type = TokenType::Default;
        std::string end;
        HtmlCssLexer::State html;
    };

    struct TextPosition {
        int line;
        int column;

        TextPosition() : line(0), column(0) {}
        TextPosition(int l, int c) : line(l), column(c) {}

        bool operator==(const TextPosition& other) const {
            return line == other.line && column == other.column;
        }
        bool operator!=(const TextPosition& other) const {
            return !(*this == other);
        }
        bool operator<(const TextPosition& other) const {
            if (line != other.line) return line < other.line;
            return column < other.column;
        }
        bool operator<=(const TextPosition& other) const {
            return *this < other || *this == other;
        }
        bool operator>(const TextPosition& other) const {
            return !(*this <= other);
        }
        bool operator>=(const TextPosition& other) const {
            return !(*this < other);
        }
    };

    struct Selection {
        TextPosition start;
        TextPosition end;

        bool isEmpty() const { return start == end; }
        TextPosition getMin() const { return start < end ? start : end; }
        TextPosition getMax() const { return start > end ? start : end; }
    };

    struct Cursor {
        TextPosition position;
        Selection selection;
        int preferredColumn; // For up/down navigation

        Cursor() : preferredColumn(-1) {}
    };

    struct UndoRecord {
        std::string beforeText;
        std::string afterText;
        std::vector<Cursor> beforeCursors;
        std::vector<Cursor> afterCursors;
        std::chrono::steady_clock::time_point timestamp;
        bool isMerged;

        UndoRecord() : isMerged(false) {}
    };

    struct LanguageDefinition {
        std::unordered_set<std::string> keywords;
        std::unordered_set<std::string> types;
        std::unordered_set<std::string> builtinFunctions;
        std::string singleLineComment;
        std::string multiLineCommentStart;
        std::string multiLineCommentEnd;
        std::string preprocessorPrefix;
    };

    class CustomTextEditor {
    public:
        CustomTextEditor();
        ~CustomTextEditor();

        // Text operations
        void SetText(const std::string& text);
        std::string GetText() const;
        int GetLineCount() const { return static_cast<int>(lines.size()); }

        // Language and styling
        void SetLanguage(SyntaxLanguage lang);
        const char* GetLanguageName() const;

        // Cursor and selection
        void SetCursorPosition(int line, int column);
        void GetCursorPosition(int& line, int& column) const;
        // Column counted in characters instead of bytes, for display
        void GetCursorDisplayPosition(int& line, int& column) const;
        bool HasSelection() const;
        std::string GetSelectedText() const;
        void SelectAll();
        void ClearSelection();
        
        // Multi-cursor support
        void AddCursor(int line, int column);
        void ClearExtraCursors();

        // Editing operations
        void InsertText(const std::string& text, bool allowAutoIndent = true);
        // Replaces the text as one undo step
        void SetTextUndoable(const std::string& text);
        void DeleteSelection();
        void Backspace();
        void Delete();
        void DeleteWordLeft();
        void DeleteWordRight();

        // Undo/Redo
        void Undo(int steps = 1);
        void Redo(int steps = 1);
        bool CanUndo() const { return undoIndex > 0; }
        bool CanRedo() const { return undoIndex < static_cast<int>(undoBuffer.size()); }
        int GetUndoIndex() const { return undoIndex; }

        // Clipboard
        void Copy();
        void Cut();
        void Paste();

        // Find and Replace
        void SetSearchText(const std::string& text);
        bool FindNext();
        bool FindPrevious();
        bool ReplaceNext();
        void OpenFind();
        void CloseFind();
        int ReplaceAll(const std::string& find, const std::string& replace, bool caseSensitive = true);

        // Editor settings
        void SetTabSize(int size) { tabSize = size; }
        void SetAutoIndent(bool enable) { autoIndent = enable; }
        void SetHighlightCurrentLine(bool enable) { highlightCurrentLine = enable; }
        void SetMatchBrackets(bool enable) { matchBrackets = enable; }
        void SetLineHeightFactor(float factor) { lineHeightFactor = factor; } // relative to the pushed ImGui font size
        void RequestFocus() { pendingFocus = true; }
        void RequestScrollToCursor() { pendingScrollToCursor = true; }

        // Auto-complete
        void SetAutoComplete(bool enable) { autoComplete = enable; }
        void TriggerAutoComplete(bool manualInvoke = false);
        void CloseAutoComplete();

        struct ProjectSymbol {
            std::string name;
            SuggestionKind kind;
            std::string detail;
            std::string parentType; // Class or type this belongs to
            std::string typeInfo;   // Declared type, e.g. "Mesh" for "Mesh* box4"
        };

        // Project symbols (scanned from sibling files at runtime)
        void UpdateProjectSymbols(const std::vector<ProjectSymbol>& symbols);

        // Version control changes: gutter marks for the lines that differ from a baseline, the
        // same marks on the scrollbar, the old lines on hover and Revert Change
        // (docs/superpowers/specs/2026-11-04-code-editor-git-design.md 6.3)
        struct DiffColors {
            ImU32 added;
            ImU32 modified;
            ImU32 deleted;
        };
        void SetDiffColors(const DiffColors& colors) { diffColors = colors; }
        // The baseline's lines (shared, never copied) and its revision for tooltips ("HEAD")
        void SetDiffBase(std::shared_ptr<const std::vector<std::string>> baseLines, const std::string& revision);
        void ClearDiffBase();
        bool HasDiffBase() const { return diffBase != nullptr; }
        // The changes, brought up to date with the text first; empty without a baseline
        const std::vector<LineChange>& GetDiffChanges();
        // The changes as last drawn (no diff runs: a slow diff keeps its pace)
        const std::vector<LineChange>& GetDrawnDiffChanges() const { return diffChanges; }
        // The change covering a 0-based line, or the deletion marked at it; -1 for none
        int GetDiffChangeAt(int line);
        // Puts the baseline's lines of a change back as one undo step of its own
        bool RevertDiffChange(int index);
        // The caret to the next / previous change (wrapping); false when there is none
        bool GoToNextChange();
        bool GoToPreviousChange();
        // The change whose diff popup shows, -1 when closed
        int GetDiffPeekChange() const { return diffPeekChange; }
        // The highlight of the character at (line, column), Default between tokens
        TokenType GetTokenTypeAt(int line, int column) const;
        // The same for a line of the baseline, as the diff popup draws its removed lines
        TokenType GetDiffBaseTokenTypeAt(int line, int column) const;

        // Rendering
        void Render(const char* title, const ImVec2& size = ImVec2(0, 0), bool border = false);

        // Font zoom requests (Ctrl+= / Ctrl+- / Ctrl+0 / Ctrl+MouseWheel); delta is +1/-1 steps, 0 means reset
        using FontZoomCallback = std::function<void(int)>;
        void SetFontZoomCallback(FontZoomCallback callback) { onFontZoom = callback; }

    private:
        // Text data
        std::vector<std::string> lines;
        std::vector<std::vector<Token>> lineTokens;
        // Html and Css: the lexer state at the end of each line, the next line starts from it
        std::vector<HtmlCssLexer::State> lineEndStates;

        // Cursors and selection
        std::vector<Cursor> cursors;
        int primaryCursor;

        // Undo/Redo
        std::vector<UndoRecord> undoBuffer;
        int undoIndex;
        static const int maxUndoSteps = 1000;

        // Language support
        SyntaxLanguage language;
        LanguageDefinition languageDef;

        // Editor state
        int tabSize;
        bool autoIndent;
        bool highlightCurrentLine;
        bool matchBrackets;
        bool autoComplete;
        bool isDragging;
        bool isMiddleDragging;
        bool isDraggingText;
        bool cursorBlinkOn;
        int middleDragLine;
        float middleDragX;
        bool mayDragText;
        std::chrono::steady_clock::time_point lastClickTime;
        TextPosition lastClickPos;
        int clickCount;

        // Scroll state
        float scrollX;
        float scrollY;

        // Auto-complete state
        bool showAutoComplete;
        TextPosition autoCompleteAnchor;
        bool suggestionsHovered;

        // Parameter hint state
        bool showParamHint;
        std::vector<std::string> paramHintSignatures;  // All overload signatures
        int paramHintActiveParam;                       // Which param is active (0-based)
        int paramHintOverloadIndex;                     // Which overload is shown
        TextPosition paramHintAnchor;                   // Where the '(' is
        std::string paramHintFuncName;                  // Name of the function that triggered it

        // Search state
        std::string searchText;
        std::vector<TextPosition> searchResults;
        int currentSearchResult;
        bool showFindDialog;
        bool showReplaceInput;
        char findInputBuffer[256];
        char replaceInputBuffer[256];
        bool findCaseSensitive;
        bool pendingScrollToCursor = false;
        bool findRefocusInput = false;
        bool pendingFocus = false;
        bool findLastCaseSensitive = false;
        bool showContextMenu = false;
        ImVec2 contextMenuPos;

        // Visual settings
        ImVec4 palette[static_cast<int>(TokenType::Count)];
        ImVec4 backgroundColor;
        ImVec4 lineNumberColor;
        ImVec4 currentLineColor;
        ImVec4 selectionColor;
        ImVec4 cursorColor;
        ImVec4 matchingBracketColor;
        ImVec4 searchHighlightColor;

        // Character metrics
        float charWidth;
        float lineHeight;
        float lineHeightFactor;
        float textOffsetY; // centers glyphs vertically within lineHeight
        int lineNumberDigits;
        float lineNumberWidth;
        float leftMargin;
        float textStartX;

        // Code font captured in Render, so text measures the same outside it
        ImFont* measureFont;
        float measureFontSize;

        // Widest line in pixels, rebuilt only when the text or the font changes
        float maxLineWidth;
        bool maxLineWidthDirty;

        FontZoomCallback onFontZoom;

        // Changes against the baseline, recomputed after edits
        std::shared_ptr<const std::vector<std::string>> diffBase;
        std::vector<std::vector<Token>> diffBaseTokens;   // the baseline highlighted once, its own multi-line state
        std::string diffRevision;
        std::vector<LineChange> diffChanges;
        bool diffDirty = false;
        std::chrono::steady_clock::time_point diffNextTime{}; // a slow diff waits, so typing stays smooth
        uint64_t diffVersion = 1;
        DiffColors diffColors{IM_COL32(90, 200, 100, 255), IM_COL32(240, 176, 40, 255), IM_COL32(230, 80, 70, 255)};

        // The popup with the diff of the change under the mouse (or pinned by a click)
        struct DiffPeekRow {
            bool added = false;
            std::string text;
            std::vector<Token> tokens;
            int changedStart = 0;   // bytes that differ from the paired line
            int changedEnd = 0;
        };
        int diffHoverChange = -1;
        double diffHoverStart = 0.0;
        int diffPeekChange = -1;
        LineChange diffPeekKey;
        double diffPeekLeaveTime = -1.0;   // < 0 while the mouse is on the marker or the popup
        bool diffPeekHovered = false;
        bool diffPeekPinned = false;
        bool diffMarkerClicked = false;    // this frame's click went to a marker, not the text
        uint64_t diffPeekVersion = 0;      // diffVersion of the rows, 0 to rebuild them
        std::vector<DiffPeekRow> diffPeekRows;
        float diffPeekTextWidth = 0.0f;

        // Semantic suggestions engine
        std::unique_ptr<SemanticSuggestions> suggestions;
        std::vector<SuggestionItem> currentSuggestions;
        int suggestionIndex;
        bool scrollToSuggestion; // Flag to sync scroll with selection only on change

        // Internal methods
        void initializeLanguage();
        void initializePalette();
        void initializeSuggestions();
        void addEngineAPISuggestions();
        void updateSuggestions(bool manualInvoke = false);
        void applySuggestion();
        void renderSuggestions(const ImVec2& origin);
        bool isInCommentOrString(const TextPosition& pos) const;
        void triggerParamHint();
        void updateParamHint();
        void closeParamHint();
        void renderParamHint(const ImVec2& origin);
        SuggestionContext buildSuggestionContext() const;
        std::string inferTypeOfVariable(const std::string& varName, int currentLine) const;
        // Type of the expression ending at endColumn, for member completion and param hints
        std::string inferTypeBefore(int lineIndex, int endColumn) const;
        // Class or Lua table the cursor is inside, what "this" and "self" resolve to
        std::string findEnclosingType(int lineIndex) const;
        // open carries a comment or string from one line to the next
        void tokenizeText(const std::string& line, OpenToken& open, std::vector<Token>& tokens) const;
        // A comment or string at i that can span lines: returns the length of its opening text, 0 for none
        int multiLineTokenAt(const std::string& line, int i, OpenToken& token) const;
        // Where the open token ends on the line, scanning from i; open stays set when it runs on to the next line
        int scanOpenToken(const std::string& line, int i, OpenToken& open) const;
        void tokenizeAll();
        bool isHtmlOrCss() const { return language == SyntaxLanguage::Html || language == SyntaxLanguage::Css; }
        HtmlCssLexer::Language htmlCssLanguage() const;
        // Html and Css: the word starting at wordStart goes where a CSS property name does
        bool atCssPropertyName(const TextPosition& wordStart) const;
        // where an HTML word starts: an element name (after "<", "</"), an attribute name
        HtmlCssLexer::NamePosition htmlNamePosition(const TextPosition& wordStart) const;
        // Start of the word completion replaces; in Html and Css it spans hyphens (background-color)
        TextPosition findCompletionWordStart(const TextPosition& pos) const;
        TokenType classifyWord(const std::string& word) const;

        void setLinesFromText(const std::string& text);
        // Cursors sorted by edit position, so an edit never shifts one not yet applied
        std::vector<size_t> cursorEditOrder() const;

        void ensureValidCursors();
        void mergeCursors();
        void sortCursors();
        void scrollToCursor();
        TextPosition clampPosition(const TextPosition& pos) const;

        void handleKeyboardInput();
        void handleMouseInput();
        void handleTextInput();

        void moveCursor(Cursor& cursor, int deltaLine, int deltaCol, bool shift);
        void moveCursorWord(Cursor& cursor, int direction, bool shift);
        void moveCursorToLineStart(Cursor& cursor, bool shift);
        void moveCursorToLineEnd(Cursor& cursor, bool shift);

        // Returns where the text that followed the cursor ended up; removed gets how much whitespace auto-indent dropped from its start
        TextPosition insertTextAtCursor(Cursor& cursor, const std::string& text, bool allowAutoIndent, int* removed = nullptr);
        void deleteRange(const TextPosition& start, const TextPosition& end);
        void moveSelectedText(const TextPosition& destPos);
        std::string getRange(const TextPosition& start, const TextPosition& end) const;

        // mergeable: may join the previous record when it is recent (typing); a revert never does,
        // and nothing typed right after it joins the revert's
        void addUndoRecord(bool mergeable = true);
        void finalizeUndoRecord();

        int getLineIndent(int lineIndex) const;
        // Auto-indent: a line ending in a block opener indents the next one, a line starting with a closer is outdented
        OpenToken lineStartState(int lineIndex) const;
        bool opensBlock(const std::string& text, const std::vector<Token>& tokens) const;
        bool closesBlock(const std::string& text) const;
        int openingBracketLine(const TextPosition& closer) const;
        void reindentAfterTyping(int typedLength);

        TextPosition screenToText(const ImVec2& screenPos, const ImVec2& origin) const;
        ImVec2 textToScreen(const TextPosition& pos, const ImVec2& origin) const;

        void renderLineNumbers(ImDrawList* drawList, const ImVec2& origin, int startLine, int endLine);
        void renderText(ImDrawList* drawList, const ImVec2& origin, int startLine, int endLine);
        void renderLineTokens(ImDrawList* drawList, ImVec2 pos, const std::string& line, const std::vector<Token>& tokens) const;
        // force: now, whatever the time the last one took
        void updateDiff(bool force);
        float diffGutterX(const ImVec2& origin) const;
        // the change whose marker is under the point, -1 for none
        int diffChangeAt(const ImVec2& point, const ImVec2& origin) const;
        void renderDiffGutter(ImDrawList* drawList, const ImVec2& origin, int startLine, int endLine);
        void renderDiffOverview();
        void updateDiffPeek(const ImVec2& origin);
        void buildDiffPeekRows(const LineChange& change);
        void renderDiffPeek(const ImVec2& origin);
        void goToChange(int index);
        void closeDiffPeek();
        void renderSelections(ImDrawList* drawList, const ImVec2& origin, int startLine, int endLine);
        void renderCursors(ImDrawList* drawList, const ImVec2& origin);
        void renderMatchingBrackets(ImDrawList* drawList, const ImVec2& origin);
        void renderSearchHighlights(ImDrawList* drawList, const ImVec2& origin, int startLine, int endLine);
        void renderFindDialog(const ImVec2& editorPos, const ImVec2& editorSize);
        void renderContextMenu();

        void placeCursorAtClick(const TextPosition& clickPos);
        void columnSelect(const ImVec2& mousePos, const ImVec2& contentPos);

        char getCharAt(const TextPosition& pos) const;
        char findMatchingBracket(const TextPosition& pos, TextPosition& matchPos) const;
        bool isOpenBracket(char c) const;
        bool isCloseBracket(char c) const;
        char getMatchingBracket(char c) const;

        bool isWordChar(char c) const;
        int prevUtf8Column(int lineIndex, int column) const;
        int nextUtf8Column(int lineIndex, int column) const;
        int byteOffsetToVisualColumn(int lineIndex, int byteOffset) const;
        int visualColumnToByteOffset(int lineIndex, int visualColumn) const;
        float measureText(const char* begin, const char* end) const;
        float byteOffsetToPixelX(int lineIndex, int byteOffset) const;
        int pixelXToByteOffset(int lineIndex, float pixelX) const;
        float computeMaxLineWidth() const;
        TextPosition findWordStart(const TextPosition& pos) const;
        TextPosition findWordEnd(const TextPosition& pos) const;
        TextPosition findDeleteWordStart(const TextPosition& pos) const;
        TextPosition findDeleteWordEnd(const TextPosition& pos) const;

        void updateSearchResults();
    };

} // namespace jasperengine::editor
