// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The Unity AnimationClip importer (docs/specs/S94-rendering-anim-gaps.md F35): a Unity YAML
// `.anim` becomes a JasperEngine clip (JSON, AnimationClipIO). The runtime never reads YAML: this
// runs in the editor (Assets > Import Unity Animation..., MCP edit_animation create.unity).
// - Imported: m_PositionCurves, m_RotationCurves, m_EulerCurves and m_ScaleCurves; m_FloatCurves on
//   Transform (merged into vector tracks), GameObject m_IsActive and renderer, light, camera and
//   audio properties through PropertyBindings' Unity names (material._Color.r on a MeshRenderer);
//   keys with their Hermite slopes and Unity tangent modes; m_AnimationClipSettings (loop time,
//   start and stop, the root transform bake flags); events.
// - Curves keep Unity's shape: per-axis curves merge at the union of their key times with the
//   exact slope at an added key (a cubic split is still the same cubic); Euler curves become
//   quaternion keys at Unity's keys and every frame of the sample rate, with the Euler curve's
//   derivative as tangents. A key keeps its Unity tangent mode only when the engine's mode gives
//   the same slopes (else Free, slopes as stored).
// - mirrorX (default): Unity is left handed; S = diag(-1, 1, 1) maps it onto the engine (position
//   x negated, quaternion y and z negated), as the Unity particle importer does.
// - Humanoid muscle curves (Animator float curves) and bindings without an engine property are
//   listed in the report; a clip with nothing but muscle curves fails with an error naming it.
// - Pure (no Scene, no Project): tests and every caller share it.

#include "animation/AnimationClip.h"

#include "json.hpp"

#include <filesystem>
#include <string>

namespace jasperengine::editor::unity {

    struct AnimImportOptions {
        bool mirrorX = true;            // Unity's left-handed X mirrored onto the engine
        std::string source;             // how the report names the file ("Assets/Anim/Walk.anim")
    };

    struct AnimImportResult {
        bool ok = false;
        std::string error;
        ClipData clip;
        // {source, name, tracks, events, length, wrap, unmapped: [{binding, reason}],
        //  muscle_curves: {count, names}, notes: [{code, message}], root_motion?}
        nlohmann::json report;
    };

    // a Unity .anim's text (YAML with an AnimationClip document)
    AnimImportResult importAnimText(const std::string& text, const AnimImportOptions& options = {});
    AnimImportResult importAnimFile(const std::filesystem::path& file, const AnimImportOptions& options = {});

}
