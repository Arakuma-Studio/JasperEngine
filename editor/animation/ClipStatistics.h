// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (S74 R47)
// - Purpose: the rows of Window > Analysis > Animation Statistics: every clip of a project, each row
//   printed from analyzeClip (the one producer, so a row equals what check_animation says of the clip)
//   and the `optimize` record its file keeps. Also lists the project's clips for check_animation.
// - Thread-safety: listProjectClips and readClipStatRow run on any thread. Users and Loaded come from
//   the open scenes and the pool, and the window fills them on the main thread.
// - Editor only: nothing here compiles into a player.

#pragma once

#include "animation/ClipReport.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::anim{

    struct ProjectClip{
        std::string display;    // project-relative: "assets/anim/walk.anim", "assets/models/hero.glb#Walk"
        std::string reference;  // absolute, what the pool and the readers take
    };

    // Every .anim and every clip of every model under the project. A model that can't be listed (an
    // FBX not imported yet) adds a line to `notes`.
    std::vector<ProjectClip> listProjectClips(const std::filesystem::path& projectRoot, std::vector<std::string>& notes,
                                              const std::atomic<bool>* cancel = nullptr);

    struct ClipStatRow{
        ProjectClip clip;
        std::string name;               // the clip: the sub-clip's name, the file's stem
        std::string source;             // the file it lives in, by its name
        ClipReport report;
        std::optional<double> maxErrorMm;   // from the file's `optimize` record: what its last reduction measured
        size_t users = 0;
        bool loaded = false;
    };

    ClipStatRow readClipStatRow(const ProjectClip& clip);

    enum class ClipStatFilter{ All, Loaded, Unused, OverBudget };
    bool clipStatRowShown(const ClipStatRow& row, ClipStatFilter filter, uint64_t budgetBytes);

    struct ClipStatTotals{
        size_t clips = 0;
        uint64_t memoryBytes = 0;
        uint64_t loadedBytes = 0;
        size_t unusedClips = 0;
        uint64_t unusedBytes = 0;
    };
    ClipStatTotals clipStatTotals(const std::vector<ClipStatRow>& rows);

}
