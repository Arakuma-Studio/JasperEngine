// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Inspector's Model Import Settings (spec S53 R16-R18), shown while model files are selected
// in the Project window (Unity's Model Importer):
//   - a preview to orbit (drag), Shaded or Wireframe, the Source or the Cooked file, Force LOD;
//   - the Model and Animation tabs (Animation: the model's clips) and the platform tabs
//     (Default | Desktop | Mobile | Web) with Override for ...;
//   - Meshes (R8's settings, Read/Write with its reason), Level of Detail (a read-only LOD bar
//     and the levels' table), the before-and-after table of the shown platform, the per-platform
//     table, the warning and failure rows with their actions, Revert and Apply (one undo step);
//   - a multi-selection: mixed values as an em dash, totals, the largest models as rows; a
//     selection of models and textures offers Select Models and Select Textures.

#pragma once

#include "model/ModelAudit.h"
#include "model/ModelPreviewRaster.h"
#include "texture/Texture.h"
#include "window/ImporterFields.h"

#include "imgui.h"
#include "yaml-cpp/yaml.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    class ModelImporterView {
    public:
        explicit ModelImporterView(Project* project);
        ~ModelImporterView();

        // Draws the importer of the selected files (project-relative); false when none of them is
        // a model. `drawClips` draws the Animation tab (the model's clip table).
        bool draw(const std::vector<std::filesystem::path>& selection, const std::function<void(const std::string&)>& drawClips);
        bool hasPendingEdits() const { return !form.touched.empty(); }

    private:
        struct PreviewJob;

        Project* project;
        std::vector<std::string> assetPaths;        // asset-relative
        std::vector<YAML::Node> stored;             // each one's section (null: none)
        ImporterForm form;
        int tab = 0;                                // 0 Default, then Desktop, Mobile, Web
        int pageTab = 0;                            // 0 Model, 1 Animation
        std::string selectionKey;
        std::string promptedKey;
        std::vector<ModelAuditRow> rows;            // per selected model, for the shown family
        std::chrono::steady_clock::time_point rowsAt;
        int rowsFamily = -1;

        // preview
        float yaw = 0.6f, pitch = 0.35f;
        bool wireframe = false;
        bool showCooked = true;
        int forceLod = 0;
        std::string geometryKey, imageKey;
        std::shared_ptr<PreviewJob> geometryJob, imageJob;
        std::shared_ptr<ModelPreviewGeometry> geometry;
        std::string previewError;
        Texture previewTexture;
        int previewSerial = 0;

        void select(const std::vector<std::filesystem::path>& selection);
        ModelFamily shownFamily() const;
        ModelImportSettings workingSettings() const;
        void refreshRows(bool force);
        void apply();
        void revert();

        void drawPreview();
        void drawMeshes(const ResolvedModelSettings& resolved);
        void drawLevels(const ResolvedModelSettings& resolved);
        void drawBeforeAfter();
        void drawPlatformTable();
        void drawIssues();
        void drawMultiple();
    };

}
