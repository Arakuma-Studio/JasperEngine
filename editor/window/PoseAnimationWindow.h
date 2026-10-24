// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Window > Animation > Pose Animation (spec: pose animation phase 4, section 3): pick a
// character, duplicate-and-tweak key poses on a strip of live thumbnails, see and drag the
// timing to scale, pick a feel, humanize, preview exactly the frames that bake, bake in one
// click. Every action has a shortcut and is one project undo step (the .poseanim document).
// The window owns the PoseEditSession the viewport (phase 5) drives.

#pragma once

#include "Project.h"
#include "ecs/Entity.h"
#include "pose/PoseEditSession.h"
#include "pose/PoseTemplates.h"
#include "pose/PoseThumbnails.h"
#include "pose/PoseUiLogic.h"
#include "imgui.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::editor{

    class PoseAnimationWindow{
    public:
        static constexpr const char* WINDOW_NAME = "Pose Animation";
        static constexpr const char* TEMPLATES_TAB = "Templates";

        // Extra tabs after Pose, Feel and Humanize (phase 6 adds Templates): drawn inside the
        // window's tab bar with the window, whose session and actions they may use
        struct Tab{
            std::string name;
            std::function<void(PoseAnimationWindow& window)> draw;
            int order = 0;           // ascending, after the built-in tabs
        };
        static void addTab(Tab tab);

        explicit PoseAnimationWindow(Project* project);
        ~PoseAnimationWindow();

        void show();
        void setOpen(bool open);
        bool isOpen() const { return windowOpen; }
        bool isFocused() const { return windowFocused; }
        void requestFocus();

        PoseEditSession& getSession() { return session; }
        Project* getProject() const { return project; }

        // Binds a scene Model GameObject (the Character field). False with `error` when it is
        // not a usable humanoid; the window then shows the mapping popup's empty state for it.
        bool bindCharacter(uint32_t sceneId, Entity entity, std::string* error);
        // Opens a .poseanim (project-relative, assets-relative or absolute) in the window
        bool openAnimation(const std::string& path, std::string* error);
        // A new asset next to the bound model's file (one neutral pose, Snappy), opened
        bool createAnimation(std::string* error);
        // Opens the window on `path` (and binds `entity` of `sceneId` when given): Project window
        // double-click, the Pose Animator Inspector's Open button
        void openFromAsset(const std::string& path, uint32_t sceneId = 0, Entity entity = NULL_ENTITY);
        // shows a tab by name ("Pose", "Feel", "Humanize" or an added one) on the next draw
        void selectTab(const std::string& name);
        // opens the humanoid mapping popup for the Character field's model (Map Bones...)
        bool openMappingFor(std::string* error);

        // The shortcuts of spec 3.3. The window runs them while it has focus; the App runs them
        // for the Scene view while it poses this window's model.
        void handleShortcuts(bool fromSceneView);
        // Ctrl/Cmd+Z/Y go to the project history: the window or a posing Scene view has focus
        bool ownsUndo(bool sceneViewFocused) const;

        // editor lifecycle (App)
        void clearSceneState(uint32_t sceneId);   // a scene was closed
        void stopTransientPreviews();             // before a scene save / play mode
        void prepareForProjectSwitch();           // the project closes

        // A new .poseanim in `directory` (absolute, inside the project) previewing `previewModel`
        // (assets-relative, may be empty): the Project window's Create > Pose Animation.
        // Returns the project-relative path ("" + error on failure).
        static std::string createAsset(Project* project, const std::filesystem::path& directory,
                                       const std::string& baseName, const std::string& previewModel, std::string* error);

        // ---- the Templates tab (phase 6, section 4; PoseAnimationWindowTemplates.cpp) ----

        // A new animation from the template `name` at `path` (project-relative; "" = the save
        // dialog's default), previewing the character's model; it is opened and the Pose tab shown
        bool createFromTemplate(const std::string& name, const std::string& path, std::string* error);
        // The save dialog's default: <model dir>/<template name>.poseanim (numbered when taken);
        // "" without a character
        std::string templateAnimationPath(const std::string& name);
        // Replace Poses in Current Animation (the template's settings and poses, the preview
        // model kept) / Append Poses: one undo step
        bool applyTemplate(const std::string& name, PoseTemplateApply mode, std::string* error);
        // Save Current as Template...: the open animation as a project template (one undo step)
        bool saveAsTemplate(const std::string& name, const std::string& description, const std::string& category,
                            std::string* error);
        // Delete (project templates only, one undo step) and Show in Project
        bool deleteTemplate(const std::string& name, std::string* error);
        bool showTemplateInProject(const std::string& name, std::string* error);
        // the category combo ("" = all) and the search field
        void setTemplateFilter(const std::string& category, const std::string& search);

        // What the Templates tab showed in its last draw (MCP state)
        struct TemplatesState{
            bool shown = false;                 // drawn in the last frame (the tab or an empty state)
            std::vector<std::string> cards;     // the names after the filter, in order
            std::vector<std::string> user;      // the cards with the "user" badge
            int visible = 0;                    // cards on screen (not scrolled out of the tab)
            int thumbnails = 0;                 // cards with their rendered thumbnail
            bool hint = false;                  // "Bind a character to preview templates"
            std::string category;
            std::string search;
        };

        // What the window shows (MCP focus_window's state of this window)
        struct State{
            bool open = false;
            std::string character;              // the bound GameObject's name
            uint32_t sceneId = 0;
            Entity entity = NULL_ENTITY;
            std::string rig;                    // the status chip
            std::string animation;              // project-relative path
            int poseCount = 0;
            int primary = -1;
            std::vector<int> selection;
            std::string mode;                   // idle / posing / previewing
            float time = 0.0f;
            float length = 0.0f;
            bool playing = false;
            std::string tab;
            std::string status;                 // the last bake / error line
            std::string emptyState;             // the empty state shown, "" for none
            bool sceneKeys = false;             // the Scene view's keys and undo go to the window
            bool editable = false;              // not locked by a scene save or play mode
            std::string layout;                 // "stacked" or "columns"
            float thumbnailHeight = 116.0f;
            TemplatesState templates;
            std::string menu;                   // the context menu open: "card", "template:<name>" or ""
        };
        State getState();

        // The screen centre of a strip card (pose index) or a Templates card (template name) as
        // drawn on screen in the last frame; false when it was not (MCP test hook input)
        bool cardCenter(int index, ImVec2& out) const;
        bool templateCardCenter(const std::string& name, ImVec2& out) const;

        // selects one pose and poses it (a strip click; also used by MCP)
        void selectPose(int index);
        // the transport actions (also used by MCP)
        void play();
        void pause();
        void stop();
        void bake();

    private:
        Project* project;
        PoseEditSession session;
        PoseThumbnails thumbnails;
        PoseThumbnails liveThumbnails;          // the primary card while posing

        bool windowOpen = false;
        bool windowFocused = false;
        bool focusRequested = false;
        bool windowVisible = false;
        bool wasBound = false;
        // layout: the compact two-column layout and the strip's thumbnail height (logical px)
        bool compactLayout = false;
        float thumbHeight = 116.0f;
        // the dock node to grow once (first open), applied before the next Begin
        ImGuiID growNode = 0;
        float growHeight = 0.0f;

        // the Character field: also a model that could not be bound (not humanoid)
        uint32_t candidateScene = 0;
        Entity candidateEntity = NULL_ENTITY;
        std::string candidateError;
        std::shared_ptr<const PoseRigInfo> candidateInfo;

        std::string bakeName;
        std::string statusLine;
        bool statusError = false;
        double statusTime = 0.0;
        std::string emptyState;
        std::string activeTab;
        std::string requestedTab;

        // strip interaction
        int pressedCard = -1;
        ImVec2 pressPosition;
        bool draggingCards = false;
        int dropIndex = -1;
        int renameIndex = -1;
        bool renameFocus = false;
        char renameBuffer[128] = {};
        int contextPose = -1;
        bool cardMenuRequested = false;         // a card's right-click: drawCardMenu opens the menu
        // the cards on screen in the last draw (frame, name or index, centre)
        struct CardSpot{
            int frame = -1;
            int index = -1;
            std::string name;
            ImVec2 centre;
        };
        std::vector<CardSpot> cardSpots;
        void noteCardSpot(int index, const std::string& name);
        // the context menu drawn last (MCP state): "card" or "template:<name>", in frame menuShownFrame
        std::string menuShown;
        int menuShownFrame = -1;
        bool scrollToPrimary = false;
        bool focusTransitionFields = false;
        std::optional<pose::PoseKey> clipboard;

        // timing bar interaction
        poseui::TimingHit dragEdge;
        float dragStartValue = 0.0f;
        float dragStartX = 0.0f;
        double dragPixelsPerSecond = 0.0;
        float timingZoom = 1.0f;
        int dragSerial = 0;

        // edits that merge while a widget is held (sliders, drags)
        int editSerial = 0;
        bool editActive = false;

        // humanoid mapping popup
        bool mappingOpen = false;
        bool mappingRequested = false;
        uint32_t mappingScene = 0;
        Entity mappingEntity = NULL_ENTITY;
        std::shared_ptr<const PoseRigInfo> mappingInfo;
        std::array<std::optional<std::string>, pose::HUMAN_BONE_COUNT> mappingOverrides;
        char mappingFilter[64] = {};
        std::string mappingError;

        // open popup
        bool openPopupRequested = false;
        char openFilter[64] = {};
        std::vector<std::string> openCandidates;

        // pending open (Inspector hook / double-click before the first draw)
        std::string pendingOpenPath;
        uint32_t pendingOpenScene = 0;
        Entity pendingOpenEntity = NULL_ENTITY;

        // Templates tab: the list (PoseTemplates, re-read when the project's template files
        // change) with each template's middle pose realised on the bound rig for its thumbnail
        struct TemplateEntry{
            PoseTemplate tmpl;
            bool posed = false;
            pose::HumanPose middle;
            uint64_t hash = 0;
        };
        std::vector<TemplateEntry> templateEntries;
        bool templatesLoaded = false;
        std::string templateStamp;              // the project template files as last read
        double templateCheckTime = 0.0;
        std::weak_ptr<pose::PoseRig> templateRig;   // the rig the middle poses are realised on
        std::string templateCategory;
        char templateSearch[64] = {};
        std::string templateMenu;               // the card the context menu is open for
        bool templateMenuRequested = false;     // a card's right-click: drawTemplateMenu opens the menu
        std::string templateClicked;            // a card clicked this frame (handled after the grid)
        int templatesDrawnFrame = -1;
        TemplatesState templatesShown;
        // the save dialog of a card click
        bool templateCreateRequested = false;
        std::string templateCreateName;
        char templateCreatePath[512] = {};
        std::string templateCreateError;
        // the Delete confirmation
        bool templateDeleteRequested = false;
        std::string templateDeleteName;
        std::string templateDeleteError;
        // Save Current as Template...
        bool templateSaveRequested = false;
        char templateSaveName[PoseTemplates::MAX_NAME_LENGTH + 1] = {};
        char templateSaveDescription[256] = {};
        char templateSaveCategory[64] = {};
        std::string templateSaveError;

        void refreshTemplates(bool force);
        TemplateEntry* findTemplate(const std::string& name);
        void drawTemplatesTab();
        // true when the card shows its rendered thumbnail
        bool drawTemplateCard(TemplateEntry& entry, const ImVec2& pos, float cardWidth, float cardHeight);
        void drawTemplateMenu();
        void drawTemplatePopups();
        void openTemplateCreateDialog(const std::string& name);

        void drawSetupRow();
        void drawBody();
        void drawStatusAndGlobalEditTool();
        void drawSceneKeysHint();
        bool sceneKeysRouted() const;
        void drawCharacterField();
        void drawAnimationField(bool inlineRow);
        void drawRigChip();
        bool drawEmptyState();
        void drawTransport();
        void drawGlobalEditBar();
        void drawStrip();
        void drawCard(int index, const ImVec2& pos, float cardWidth, float cardHeight);
        void drawChip(int index, const ImVec2& pos, float chipWidth, float cardHeight);
        void drawCardMenu();
        void drawTimingBar();
        void drawTabs();
        void drawPoseTab();
        void drawFeelTab();
        void drawHumanizeTab();
        void drawJointAngles();
        void drawCurvePlot(const pose::Transition& transition, const ImVec2& size);
        void drawMappingPopup();
        void drawOpenPopup();
        void drawRenamePopup();

        // edits through the session (one undo step; mergeKey merges a held widget)
        bool commit(const pose::PoseAnimationData& data, const std::string& undoName, const std::string& mergeKey = "");
        bool commitEdit(const poseui::Edit& edit, const std::string& undoName, bool reordered = false);
        std::string widgetMergeKey(const char* field);
        void finishWidgetEdit();
        void setStatus(const std::string& text, bool error);

        // actions (menu, buttons, shortcuts)
        void actionAdd();
        void actionDuplicate(bool atEnd, bool mirrored);
        void actionMirror();
        void actionDelete();
        void actionCopy();
        void actionPaste();
        void actionMove(int direction);
        void actionMoveTo(int insertBefore);
        void actionResetNeutral();
        void actionGround();
        void actionCenter();
        void actionRename(int index);
        void actionAutoTiming();
        void actionResetCustom();
        void actionSelect(int index, bool additive, bool range);
        void actionStepFrame(int direction);
        void actionGlobalEdit();

        void useSelection();
        void openMappingPopup(uint32_t sceneId, Entity entity);
        void applyMapping();
        void refreshCandidate();
        void updateThumbnails();
        const std::vector<int>& selection() const;
        const pose::PoseAnimationData* data() const;
        // the document's data as an immutable snapshot (kept alive by its holders across commits)
        std::shared_ptr<const pose::PoseAnimationData> view();
        std::shared_ptr<const pose::PoseAnimationData> viewData;
        const PoseAnimationDocument* viewDocument = nullptr;
        uint64_t viewRevision = 0;
        std::string characterName() const;
        void collectOpenCandidates();
        void processPendingOpen();
    };

}
