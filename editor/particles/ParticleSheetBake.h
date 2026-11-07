// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PARTICLESHEETBAKE_H
#define PARTICLESHEETBAKE_H

#include "command/type/WriteFilesCmd.h"
#include "particles/ParticleEffect.h"
#include "particles/ParticleEffectPreview.h"
#include "pose/PoseImage.h"

#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    // Bake Sprite Sheet (docs/specs/S91-effect-authoring.md R12) and the Particle Effect window's
    // Record: an effect's run drawn into one PNG with straight alpha on a chosen grid, and an effect
    // that plays it with Texture Sheet Animation; or a numbered sequence of PNGs. The writes are one
    // undo step of the project (WriteFilesCmd), made by the caller.
    class ParticleSheetBake {
    public:
        static constexpr int MAX_TILES = 16;   // columns and rows each
        static constexpr int MAX_SHEET = 4096; // pixels each way

        // the preview request of a sheet: columns x rows cells, each in the middle of its part of the run, transparent
        static ParticlePreviewRequest sheetRequest(ParticlePreviewRequest base, int columns, int rows, int cellSize);
        // why columns, rows and cell size do not make a sheet (empty: they do)
        static std::string validate(int columns, int rows, int cellSize);

        // the cells, row by row from the top left (Texture Sheet Animation's order), no gaps
        static PoseImage composeSheet(const std::vector<PoseImage>& cells, int columns, int rows);
        // one particle the size of a cell's view, at the origin, playing the sheet over `seconds`
        // (again every `seconds` when looping), with the source's blend when every system adds
        static particles::ParticleEffectDocument sheetEffect(const std::string& texture, int columns, int rows, double seconds, float worldSize,
                                                             bool looping, bool additive);
        // <output>.png and <output>.particles (output: project-relative, without an extension)
        static bool sheetFiles(Project* project, const std::string& output, const PoseImage& sheet, const particles::ParticleEffectDocument& effect,
                               bool overwrite, std::vector<WriteFilesCmd::Item>& items, std::vector<std::string>& written, std::string& error);
        // <folder>/<name>_000.png, ... (folder: project-relative)
        static bool sequenceFiles(Project* project, const std::string& folder, const std::string& name, const std::vector<PoseImage>& frames,
                                  bool overwrite, std::vector<WriteFilesCmd::Item>& items, std::vector<std::string>& written, std::string& error);
    };

}

#endif //PARTICLESHEETBAKE_H
