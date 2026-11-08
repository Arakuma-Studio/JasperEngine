// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Window > 2D > Palette (docs/specs/S43-palettes.md R32, R10, R11, R33, R8): one .palette as a
// grid of swatches (`columns` wide), its ramps as strips under the grid, its variants as a table
// and how the selected texture uses it. The toolbar picks a project or built-in palette (a
// built-in is copied into the project on first use, R7), makes new ones, imports (a file,
// Lospec, a texture, a swatch library), exports, sorts, generates ramps and adds variants. Every
// edit is one undo step written to the file (the edit_palette op vocabulary, PaletteEdit.h); a
// colour dragged in the Color window is one step for the whole drag. The file is read again when
// it changes on disk (an undo, an agent, another editor). Network and image work runs on workers.

#include "command/type/WriteFilesCmd.h"
#include "jobs/JobSystem.h"
#include "pixelart/Palette.h"
#include "pixelart/PaletteAssets.h"
#include "pixelart/PaletteFormats.h"
#include "pixelart/PaletteUsage.h"
#include "pixelart/RampGenerator.h"
#include "pixelart/RgbaImage.h"
#include "texture/Texture.h"

#include "imgui.h"
#include "json.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class ApplyPaletteDialog;
    class CommandCatalog;
    class Project;
    class ProjectWindow;

    // An RGBA8 picture on the GPU for ImGui, Nearest-filtered (pixel art is shown at integer
    // zoom). A replaced texture is retired, not destroyed: the frame drawn with it may still be
    // in flight, so its owner destroys the retired ones at the start of its next frame.
    struct PaletteGpuImage {
        Texture texture;
        int width = 0, height = 0;

        bool valid() const { return width > 0 && !texture.empty(); }
        // false (and empty) when the picture is empty or the upload failed
        bool upload(const pixelart::RgbaImage& image, std::vector<Texture>& retired);
        void retire(std::vector<Texture>& retired);
        ImTextureID imguiId();
    };

    class PaletteWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Palette";
        static constexpr const char* EXTENSION = ".palette";

        PaletteWindow(Project* project, ProjectWindow* projectWindow, ApplyPaletteDialog* applyDialog);
        ~PaletteWindow();

        void show();
        // a .palette (absolute or project-relative); empty: the one shown last, else the
        // project's Default Palette, else its first palette
        void open(const std::filesystem::path& palette = {});
        void setOpen(bool open);
        bool isOpen() const;
        bool isFocused() const;
        // the catalog's Palette context: the window has the keyboard and no text field does
        bool takesKeys() const;
        void resetForProjectSwitch();
        // Delete, Duplicate, Move Left / Right of the selected swatches (the Palette context)
        void bindCommands(CommandCatalog& catalog);

        // ---- shared with the Project window, the Color window and the Apply Palette dialog

        // Files as one undo step of the project history named `stepName`; a non-empty mergeKey
        // merges the step into the previous one of the same key (a drag). Files inside the
        // project are written beneath it, others beneath their own folders.
        static bool writeFiles(Project* project, std::vector<WriteFilesCmd::Item> items, const std::string& stepName,
                               const std::string& mergeKey, std::string& error);
        // a .palette (any case of the extension)
        static bool isPaletteFile(const std::filesystem::path& path);
        // a palette format of another application: .gpl, .hex, .pal, .txt, .ase, .aseprite
        static bool isForeignPaletteFile(const std::filesystem::path& path);
        // Export ▸'s formats, in menu order, and their menu labels ("GIMP (.gpl)")
        static const std::vector<pixelart::PaletteFormat>& exportFormats();
        static const char* exportLabel(pixelart::PaletteFormat format);

        // `palette` as a new file "<baseName>.palette" in `directory` (absolute; " 2", " 3"...
        // when taken), one undo step; opened here when `openIt`. The file, empty on failure
        // (the reason in the console and the window's notice).
        std::filesystem::path createPaletteFile(const jasperengine::pixelart::Palette& palette, const std::filesystem::path& directory,
                                                const std::string& baseName, bool openIt = true);
        // Create ▸ Palette: "New Palette.palette" (black and white) in `directory`
        std::filesystem::path createNew(const std::filesystem::path& directory, bool openIt = true);
        // A foreign palette file or an image as a new .palette: beside it when it is under the
        // assets folder, else in Assets/Palettes. An image of more than 256 colours opens From
        // Texture on Reduce Colours instead; another file of more than 256 offers "Import the
        // First 256" in the window.
        void importFile(const std::filesystem::path& source);
        // Export ▸ <format>: a save dialog, then the bytes as one undo step
        void exportPalette(const std::filesystem::path& palette, pixelart::PaletteFormat format);
        // the Usage section on a palette and a texture (pinned; empty: the Project window's)
        void showUsage(const std::filesystem::path& palette, const std::filesystem::path& texture);

    private:
        Project* project;
        ProjectWindow* projectWindow;
        ApplyPaletteDialog* applyDialog;
        bool windowOpen = false;
        bool focusRequested = false;
        bool focused = false;
        bool firstOpenDone = false;

        // ---- the file shown
        std::filesystem::path file;          // absolute; empty: none
        jasperengine::pixelart::Palette palette;
        bool loaded = false;
        std::string loadError;
        std::vector<std::string> loadWarnings;
        std::string fileText;
        std::filesystem::file_time_type fileTime{};
        uintmax_t fileSize = 0;
        double lastCheck = -1.0;
        // one line under the toolbar: what the last action said (a refusal, a failed write)
        std::string notice;
        bool noticeIsError = false;
        // a foreign file of more than 256 colours waiting for "Import the First 256"
        std::filesystem::path tooManySource;

        // the project's palettes for the selector, listed when it opens
        std::vector<pixelart::ProjectPalette> projectPalettes;
        bool palettesListed = false;

        // ---- an edit is applied at the start of the next frame: the widgets of this one still
        // read the palette it was made from (a delete inside the grid's loop)
        struct Staged {
            jasperengine::pixelart::Palette next;
            std::string what;
            std::string mergeKey;
            bool setSelection = false;
            std::vector<int> selection;
        };
        std::optional<Staged> staged;
        // the palette edits build on: the staged one when there is one
        const jasperengine::pixelart::Palette& base() const;
        bool editOps(const nlohmann::json& ops, const std::string& what, const std::string& mergeKey = {},
                     std::optional<std::vector<int>> selectAfter = std::nullopt);
        void stage(jasperengine::pixelart::Palette next, const std::string& what, const std::string& mergeKey = {},
                   std::optional<std::vector<int>> selectAfter = std::nullopt);
        void applyStaged();
        bool writePalette(const jasperengine::pixelart::Palette& next, const std::string& what, const std::string& mergeKey);
        void setNotice(const std::string& text, bool error);
        void reload(bool force);
        void readFile();
        bool editable() const;
        std::string stepName(const std::string& what) const;
        std::string mergeKeyFor(const char* what, uint64_t serial) const;

        // ---- the grid
        std::vector<int> selection;          // sorted indices
        int anchor = -1;                     // Shift-click ranges start here
        int pendingSingleSelect = -1;        // a press on a selected swatch: select it alone on release, unless it dragged
        bool swatchDragged = false;
        int selectedRamp = -1;
        float swatchSize = 24.0f;
        bool alwaysShowIndices = false;
        bool sortIntoNewPalette = true;
        uint64_t dragSerial = 0;             // a new merge key after each drag ends
        bool isSelected(int index) const;
        void select(int index, bool shift, bool ctrl);
        void setSelection(std::vector<int> indices);
        void clampSelection();
        // the selection when it holds `index`, else just `index`
        std::vector<int> targetOf(int index) const;

        void deleteSelection();
        void duplicateSelection();
        void moveSelection(int direction);
        // the swatches `moved` (sorted) placed before old index `insertAt` (0..count)
        void reorder(const std::vector<int>& moved, int insertAt);
        void sortBy(int sort);

        // ---- the Color window on a colour (a swatch or a variant cell): every change written
        // under one merge key, so the whole session is one undo step
        struct PickerTarget {
            ImGuiID id = 0;
            int index = -1;
            int variant = -1;                // -1: the palette colour itself
            uint64_t serial = 0;
            bool clearOnCancel = false;      // a "same as base" cell: Esc makes it that again
        };
        PickerTarget picker;
        uint64_t pickerSerial = 0;
        void openPicker(int index, int variant);
        void pollPicker();

        // ---- rename popups (a colour, a ramp, a variant)
        enum class RenameKind { None, Color, Ramp, Variant };
        RenameKind renameKind = RenameKind::None;
        int renameIndex = -1;
        char renameBuffer[128] = {0};
        bool renameOpenRequested = false;
        void requestRename(RenameKind kind, int index, const std::string& current);
        void drawRenamePopup();
        int fitSteps = 8;

        // ---- Generate Ramp (R10), a window of its own
        // ---- Info: the fields follow the file while none of them is typed in
        char infoName[128] = {0};
        char infoAuthor[128] = {0};
        char infoDescription[512] = {0};
        std::string infoSyncedText;
        bool infoActive = false;

        bool rampDialogOpen = false;
        bool rampDialogFocus = false;
        pixelart::RampRecipe recipe;
        char rampName[64] = "Ramp 1";
        void openRampDialog();
        void drawRampDialog();

        // ---- Import ▸ From Lospec... (R8)
        struct LospecFetch;
        std::shared_ptr<LospecFetch> lospec;
        bool lospecOpen = false;
        bool lospecFocus = false;
        bool lospecOffline = false;
        char lospecInput[256] = {0};
        std::optional<jasperengine::pixelart::Palette> lospecResult;
        std::string lospecSlug;
        pixelart::PaletteError lospecError;
        void openLospec();
        void startLospecFetch();
        void pollLospec();
        void drawLospecDialog();

        // ---- Import ▸ From Texture... (unique colours, or Reduce Colours)
        struct TextureImport {
            std::optional<jasperengine::pixelart::Palette> palette;
            std::string error;
        };
        bool textureImportOpen = false;
        bool textureImportFocus = false;
        char textureImportPath[512] = {0};
        bool textureImportReduce = false;
        int textureImportColors = 16;
        jobs::Task<std::shared_ptr<TextureImport>> textureImportTask;
        std::filesystem::path textureImportSource;
        std::string textureImportError;
        void openTextureImport(const std::filesystem::path& texture, bool reduce);
        void drawTextureImportDialog();

        // ---- Usage (R33): the texture followed or pinned, measured on a worker
        struct UsageResult {
            pixelart::RgbaImage image;
            pixelart::PaletteUsageReport report;
            std::string error;
        };
        // reports by (image content hash, palette colours hash): the worker answers from it
        struct UsageCache;
        std::shared_ptr<UsageCache> usageCache;
        std::filesystem::path pinnedTexture;   // empty: follow the Project window's selection
        std::filesystem::path usageTexture;    // what the section shows now
        std::string usageKey;                  // texture, its time and size, the palette's colours
        std::string usageRunningKey;
        jobs::Task<std::shared_ptr<UsageResult>> usageTask;
        std::shared_ptr<UsageResult> usage;
        std::string usageShownKey;             // the key `usage` was measured for
        PaletteGpuImage usageImage;
        PaletteGpuImage usageMask;
        int hoveredOffColor = -1;               // in usage->report.offColors
        int maskColorShown = -1;
        jobs::Task<std::shared_ptr<pixelart::RgbaImage>> maskTask;
        int maskRunning = -1;
        std::vector<std::filesystem::path> textureChoices;
        bool usageFocus = false;
        void updateUsage();
        void drawUsage();

        std::vector<Texture> retired;

        // ---- drawing
        void drawToolbar();
        void drawSelector();
        void drawImportMenu();
        void drawExportMenu();
        void drawSortMenu();
        void drawGrid();
        void drawSwatchMenu(int index);
        void drawRamps();
        void drawVariants();
        void drawInfo();
        void handleWindowDrops();
        void listPalettes();
        void useBuiltin(const std::string& id);
        std::filesystem::path palettesFolder() const;
    };

}
