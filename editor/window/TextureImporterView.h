// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Inspector's Texture Import Settings (spec S14 R22), shown while texture files are
// selected in the Project window (Unity's Texture Importer):
//   - a preview with a checkerboard under alpha, R/G/B/A toggles, a mip slider and a Source /
//     Cooked split for the platform tab (cooked levels decoded on the CPU);
//   - the info line ("2048×2048 · BC7 · 12 mips · VRAM 5.3 MB · Disk 3.9 MB (source 7.1 MB) ·
//     PSNR 43.1 dB");
//   - the settings grouped (General / Advanced / Mip Maps) and a platform tab bar
//     (Default | Desktop | Mobile | Web | Android ETC2) with Override for ...;
//   - a per-platform table (format, VRAM, disk, PSNR, status), the warnings (R2 conflicts,
//     R6, maxSize clamping) and Apply / Revert: one undo step for every selected texture.

#pragma once

#include "texture/TextureCookQueue.h"
#include "texture/Texture.h"

#include "imgui.h"
#include "yaml-cpp/yaml.h"

#include <array>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    class TextureImporterView {
    public:
        explicit TextureImporterView(Project* project);
        ~TextureImporterView();

        // Draws the importer of the selected files (project-relative); false when none of them
        // is a texture source
        bool draw(const std::vector<std::filesystem::path>& selection);
        // the selection's edits not applied yet
        bool hasPendingEdits() const { return !touched.empty(); }

    private:
        struct PreviewImage {
            int width = 0, height = 0;
            std::vector<uint8_t> rgba;     // display-ready RGBA8
            std::string error;
        };
        struct PreviewJob;

        Project* project;
        std::vector<std::string> assetPaths;        // asset-relative
        std::vector<std::string> projectPaths;
        std::vector<YAML::Node> stored;             // each one's section (null: none yet)
        std::vector<TextureImportSettings> current; // as stored, or inferred
        std::string inferredReason;
        YAML::Node working;                         // the first one's section, with the edits
        std::set<std::string> touched;              // "maxSize", "platforms.mobile.quality"...
        std::set<std::string> mixed;                // keys whose values differ in the selection
        int tab = 0;                                // 0 Default, then the families shown
        std::string selectionKey;
        std::string promptedKey;                    // the selection the unapplied-edits prompt was shown for

        // preview
        bool showChannel[4] = {true, true, true, true};
        int mip = 0;
        float split = 0.5f;
        std::string sourceKey, cookedKey, compositeKey;
        std::shared_ptr<PreviewJob> sourceJob, cookedJob;
        PreviewImage source, cooked;
        Texture previewTexture;
        int previewSerial = 0;
        int previewWidth = 0, previewHeight = 0;

        void select(const std::vector<std::filesystem::path>& selection);
        std::vector<TextureFamily> shownFamilies() const;
        TextureFamily tabFamily() const;
        TextureImportSettings workingSettings() const;
        YAML::Node workingValue(const std::string& key) const;
        void setWorking(const std::string& key, const YAML::Node& value);
        void apply();
        void revert();

        void drawPreview(const TextureCookState* state);
        void drawGeneral(const ResolvedTextureSettings& resolved);
        void drawAdvanced(const ResolvedTextureSettings& resolved);
        void drawMipMaps(const ResolvedTextureSettings& resolved);
        void drawPlatform(const ResolvedTextureSettings& resolved);
        void drawPlatformTable();

        bool comboField(const char* label, const std::string& key, const std::vector<std::string>& names,
                        const std::vector<std::string>& labels, const std::string& shown, const char* tooltip);
        bool boolField(const char* label, const std::string& key, bool shown, const char* tooltip);

        void updatePreviewJobs(const TextureCookState* state);
        void composite();
    };

}
