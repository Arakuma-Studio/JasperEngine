// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PARTICLECHECKSPANEL_H
#define PARTICLECHECKSPANEL_H

// The Particle Effect window's Filmstrip and Checks (docs/specs/S91-effect-authoring.md R24): the
// same deterministic run and rows as preview_particle_effect (ParticleEffectPreview), a Fix button
// on a row whose fix is a single edit, and Record, which writes a PNG sequence or a sprite sheet
// with its effect as one undo step, to a path typed in the panel (no modal, no native dialog).
// A widget the window hosts in its slot: it owns its run and its textures. Main thread.

#include "ecs/Entity.h"
#include "particles/ParticleEffectPreview.h"
#include "texture/Texture.h"

#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    class ParticleChecksPanel {
    public:
        // one row as the panel shows it, from a report's checks and fixes
        struct Row {
            std::string id;
            std::string status;
            std::string measured;
            std::string threshold;
            std::string detail;     // limited_by, then fix_hint
            int fix = -1;           // index into the report's fixes: a single edit the Fix button applies
        };

        ParticleChecksPanel();
        ~ParticleChecksPanel();
        ParticleChecksPanel(const ParticleChecksPanel&) = delete;
        ParticleChecksPanel& operator=(const ParticleChecksPanel&) = delete;

        // what it checks: an asset (assets-relative), or a GameObject and its subtree
        void setAsset(const std::string& assetPath);
        void setSubtree(uint32_t sceneId, Entity root);
        // the window's preview seed and platform drive the run too
        void setSeed(uint32_t value) { seed = value; }
        void setPlatform(const std::string& value) { platform = value; }

        void draw(Project* project);

        // the request a run of the panel makes (the tool's defaults: 6 frames, three-quarter view,
        // dark background, the project's thresholds)
        bool buildRequest(Project* project, ParticlePreviewRequest& request, std::string& error) const;
        // starts a run (the Preview button); its cells and rows show when it ends
        bool start(Project* project, std::string& error);
        bool isRunning() const { return run && !run->isFinished(); }
        const qa::Json& getReport() const { return report; }

        static std::vector<Row> rowsOf(const qa::Json& report);

    private:
        std::string asset;
        uint32_t sceneId = 0;
        Entity root = NULL_ENTITY;
        uint32_t seed = 0;
        std::string platform = "mobile";
        int frames = 6;
        bool overdrawMap = false;

        std::unique_ptr<ParticleEffectPreview> run;
        qa::Json report;
        std::string message;
        std::vector<Row> rows;

        // the filmstrip as a texture (its pixels live as long as it does)
        std::shared_ptr<PoseImage> pixels;
        Texture filmstrip;
        int textureGeneration = 0;
        std::vector<Texture> retired;

        // Record
        int recordKind = 0;          // 0 sprite sheet, 1 sequence
        int columns = 4;
        int rowsCount = 4;
        int cellSize = 128;
        // the pixel view (S49): the base-pixel grid, cells at an integer scale; and Record's
        // Aseprite json-hash beside the sheet
        bool pixelView = false;
        int pixelScale = 4;
        bool aseprite = false;
        char recordPath[512] = {};
        std::unique_ptr<ParticleEffectPreview> recording;
        bool recordingSheet = true;
        std::string recordOutput;

        void finishRun(Project* project);
        std::string scope(Project* project) const;
        void finishRecording(Project* project);
        void applyFix(Project* project, int index);
        std::string defaultRecordPath(Project* project) const;
    };

}

#endif //PARTICLECHECKSPANEL_H
