// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/dialogue-graphs.md R19-R22, AC10; docs/dialogue.md)
// - Purpose: Window > Dialogue, the Graph Editor (GraphEditorWindow) of `.dialogue` files: they open
//   here from the Project window, focus_window and openGraph. A card per node: a line shows its
//   speaker, the first lines of its text in the window's Language (the source or a translation),
//   its mood and tag chips and a badge per translation (complete, missing, review); option rows
//   are its output ports. Entries and ends are coloured.
// - Toolbar: Language (the source or any translation's), Entry (frames it), Play (the preview from
//   the chosen entry).
// - Preview pane: the dialogue played in the editor without Play, through the walker
//   (dialogues::walk, the runtime's runner): the current line with its speaker, option buttons
//   (disabled ones greyed with their condition), Advance, Restart, Back (the last step taken back:
//   the walk replayed without it), a Variables table (start values, editable, and the values now),
//   Conditions per handler prefix (true / false / ask: an asked call is false and listed with
//   a button that takes it true), and the actions as they would fire (never run). The canvas marks
//   the current node and the path walked.
// - Inspector of the selected node: a line's Speaker (with the project's speakers), Text (ICU
//   checked, characters against Max Chars), Variants (Add, Remove, Up, Down; Variant Mode), Mood,
//   Tags, Voice, Options (Text, Condition, Action, Style, Once, When False; reorder keeps ids and
//   wires) and Line Action; branch, set and action nodes their expression field with completion of
//   variables, built-ins and declared prefixes. Each field edit is one undo step (a text edit also
//   turns its translations needs_review in that step: GraphEditCmd::makeStep).
// - Problems strip: the lint's rows, with Fix for fan-out (remove the extra wire), an undeclared
//   variable (declare it in the dialogue) and an undeclared prefix (declare it in Project Settings).
// - Thread-safety: main thread.

#pragma once

#include "window/GraphEditorWindow.h"

#include "dialogue/DialogueWalker.h"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace jasperengine::editor{

    struct DialogueSettings;

    class DialogueWindow : public GraphEditorWindow{
    public:
        static constexpr const char* WINDOW_NAME = "Dialogue";

        explicit DialogueWindow(Project* project);
        ~DialogueWindow() override;

        // the window App made (the DialogueRunner Inspector's Current Node); null when none
        static DialogueWindow* instance();
        // opens `path` (asset- or project-relative) and selects the node
        bool reveal(const std::string& path, const std::string& nodeId);

        // ---- the window's language: "" the source, else a translation's
        void setLanguage(const std::string& language) { language_ = language; }
        const std::string& getLanguage() const { return language_; }
        std::vector<std::string> languages() const;
        // what a line's card shows in the window's language
        std::string displayText(const graph::Node& node) const;
        // per translation language: "complete", "missing" or "review"
        std::vector<std::pair<std::string, std::string>> translationBadges(const graph::Node& node) const;

        // ---- line edits (the Inspector's; each one undo step)
        bool addOption(const std::string& nodeId, const std::string& text = std::string());
        bool removeOption(const std::string& nodeId, const std::string& optionId);
        bool moveOption(const std::string& nodeId, size_t from, size_t to);
        bool setOptionField(const std::string& nodeId, const std::string& optionId, const std::string& field, const graph::Json& value);
        bool addVariant(const std::string& nodeId, const std::string& text = std::string());
        bool removeVariant(const std::string& nodeId, const std::string& variantId);
        bool moveVariant(const std::string& nodeId, size_t from, size_t to);
        bool setVariantText(const std::string& nodeId, const std::string& variantId, const std::string& text);

        // ---- the preview
        void previewStart(const std::string& entry = std::string());
        bool previewAdvance();
        bool previewChoose(int index);
        bool previewBack();
        void previewRestart();
        bool previewRunning() const { return previewActive; }
        const dialogues::WalkResult& previewResult() const { return preview; }
        // the walk's start values (editable) and condition overrides by prefix: "true", "false" or "ask"
        void setPreviewVariable(const std::string& name, const jasperengine::dialogue::Value& value);
        void setConditionMode(const std::string& prefix, const std::string& mode);
        // an asked call taken true (the Conditions list's button)
        void answerCondition(const std::string& call, bool value);
        // the actions the walk would fire so far
        std::vector<std::string> previewActions() const;
        const std::string& previewEntry() const { return entry; }

    protected:
        bool nodeHasThumbnail(const graph::Node& node) const override;
        void drawNodeThumbnail(const graph::Node& node, ImDrawList* drawList, ImVec2 min, ImVec2 max) override;
        bool hasPreviewPane() const override { return true; }
        void drawPreviewPane() override;
        void drawToolbarExtras() override;
        void collectProblems(std::vector<Problem>& problems) const override;
        void documentChanged() override;
        void documentClosed() override;
        bool drawKindNodeInspector(const graph::Node& node) override;

    private:
        std::string language_;
        std::string entry;
        // translations of the open file: language -> key -> {text, status}
        struct Translation {
            std::string text;
            std::string status;
        };
        std::map<std::string, std::map<std::string, Translation>> translations;
        uint64_t translationsListener = 0;
        // the preview
        bool previewActive = false;
        std::vector<int> steps;
        std::map<std::string, jasperengine::dialogue::Value> startValues;
        std::map<std::string, std::string> conditionModes;      // prefix -> true | false | ask
        std::map<std::string, bool> answered;                    // a call -> its value
        dialogues::WalkResult preview;
        std::set<std::string> walked;
        // Inspector text buffers: field id -> text being typed
        std::map<std::string, std::string> buffers;
        // the lint rows that depend on the project (prefixes, variables, other dialogues), made again
        // when the graph or Project Settings > Dialogue change: the document's issues are its load's
        mutable std::vector<graph::Issue> projectIssues;
        mutable uint64_t projectIssuesRevision = 0;
        mutable std::shared_ptr<const DialogueSettings> projectIssuesSettings;

        void loadTranslations();
        void replay();
        bool applyOptions(const std::string& nodeId, const graph::Json& options, const std::vector<graph::GraphOp>& extra, const std::string& label);
        bool applyVariants(const std::string& nodeId, const graph::Json& variants, const std::string& label);
        std::string nextSubId(const graph::Node& node, const char* prefix, std::vector<graph::GraphOp>& ops) const;
        // a committed text field: true when the user finished an edit that changed it
        bool textField(const std::string& id, const char* label, const std::string& value, std::string& out, bool multiline = false);
        void expressionField(const graph::Node& node, const char* param, const char* label, bool statements);
        void drawLineInspector(const graph::Node& node);
        void drawOptionRows(const graph::Node& node);
        void drawVariantRows(const graph::Node& node);
    };

}
