// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from Pachislop src/io/project.ts (migrateProject, migrateLayer, migrateStyle2D,
// migrateStyle3D, migrateEffects, migrateDeepGlow), src/io/migrate/{generatorParams,
// effectDedupe, materialEffects, contentEffects, bands}.ts and the model defaults they fill
// (src/model/defaults.ts, bands.ts, companions.ts; src/generators/fromEffect.ts;
// src/effects/a/recipes.ts) by SafEight, used with the author's permission.

// Contract (docs/specs/S87-title-localisation-interchange.md R28, D11, §4.1)
// - Purpose: a Pachislop project of version 4 to 11 (`Project` JSON, the `project` member of a
//   `.pslot.json`) as a complete version 11 project, kept as Pachislop structures; the mapper
//   (PachislopMapper.h) turns that into a `.titleart`. One pass, literally Pachislop's: every
//   function fills members that are missing and keeps what is present (idempotent, so migrating
//   twice equals once), then the four ordered v7 steps (generator params, effect dedupe, material
//   effects, content effects) and the v11 bands conversion of a v4-v10 3D style.
// - Differences from the TypeScript, none of which changes what renders:
//   - new ids are minted deterministically (IdMint) instead of randomly;
//   - an effect a step makes (makeEffect, presetEffect, lookEffect) holds the params the step
//     sets; the rest take their defaults when the mapper converts the effect into S83's
//     definition, whose defaults are Pachislop's (S83 R51);
//   - `generatorFromPreset('sunburst')` (the ベタフラ burst fill) holds the burst colours and
//     colour mode only; S83's sunburst defaults give the rest.
// - The v4 per-character scale compensation needs font metrics: PachislopMigrateText.h runs it
//   after this pass with the imported fonts.
// - Thread-safety: pure functions.

#pragma once

#include "PachislopJson.h"

#include <functional>
#include <string>

namespace jasperengine::editor::pachislop {

    constexpr int PROJECT_VERSION = 11;

    // "This is a version 3 project; this version of the app opens version 4 to 11 projects only",
    // "Unsupported project version 12 (expected 4 to 11)": parseProjectFile's messages
    std::string unsupportedVersionMessage(const Json& version);
    bool isSupportedVersion(const Json& version);

    // a version 4-11 project as a complete version 11 one; false with `error` (Pachislop's
    // message) for anything else
    bool migrateProject(const Json& project, Json& out, std::string& error);

    // the field fillings of migrateProject, for tests and the look converter
    Json migrateStyle2D(const Json& style);
    // `legacyDocBlur` set: v4-v7 data in a document whose environment blur it is (v8 notes)
    Json migrateStyle3D(const Json& style, const double* legacyDocBlur);
    Json migrateEffects(const Json& effects);
    Json migrateDeepGlow(const Json& effect);

    // ---- the v11 bands conversion (src/io/migrate/bands.ts) ----

    bool isLegacyStyle3D(const Json& style);
    struct BandsConversion {
        Json style3d;
        // an old part (`side`, or ring / plate index) -> "face", "side" or a band index (>= 0)
        std::function<Json(const std::string& kind, int index)> part;
        // a letter's old overrides -> new ones
        std::function<Json(const Json& charAdjust)> charAdjust;
    };
    BandsConversion convertBands(const Json& legacy, bool orb, const Json& chars);

    // ---- the four v7 steps, in order ----

    Json generatorParams(const Json& project, IdMint& ids);
    Json dedupeEffects(const Json& effects, double globalLight, IdMint& ids);
    Json effectDedupe(const Json& project, IdMint& ids);
    Json migrateMaterialEffects(const Json& effects);
    Json materialEffects(const Json& project);
    Json contentEffects(const Json& project, IdMint& ids);

}
