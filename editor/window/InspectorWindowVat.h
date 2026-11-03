// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The pure helpers of the Inspector's Vertex Animation parts (InspectorWindowVat.cpp: the Vertex
// Animator section, the .vat asset view and the Create Crowd dialog; vertex animation spec 3.3,
// 3.6, 4.5, 5.2, 5.4), kept out of InspectorWindow so the editor tests check them without ImGui.
// Owner: W5. Thread-safety: pure, except vertexAnimatorFileCmd (main thread: reads the project).

#pragma once

#include "animation/VertexAnimation.h"
#include "ecs/Entity.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor{
    class Project;
    class MultiPropertyCmd;
}

namespace jasperengine::editor::inspectorvat{

    // The command of the Vertex Animator's Animation field (one GameObject or a multi-selection):
    // `file` (a .vat or its .vat.glb, absolute or asset-relative; "" clears) set on every entity of
    // the scene with its GUID (the GUID follows the path: a stale one would win on the next load),
    // one undo step. Null with `error` when the file is not a .vat inside the assets folder.
    MultiPropertyCmd* vertexAnimatorFileCmd(Project* project, uint32_t sceneId, const std::vector<Entity>& entities,
                                            const std::string& file, std::string& error);

    // "812 B", "14.2 KB", "1.73 MB"
    std::string formatBytes(uint64_t bytes);
    // "Vertices" / "Bones"
    const char* modeLabel(jasperengine::vat::VatMode mode);
    // "RGBA16F" / "RGBA32F"
    const char* formatLabel(jasperengine::vat::VatPrecision precision);
    // the header lines: "Vertices · 5,120 vertices", "2048 × 37 RGBA16F · 1.73 MB",
    // "Error: max 0.21 mm, mean 0.04 mm"
    std::string modeLine(const jasperengine::vat::VatData& data);
    std::string textureLine(const jasperengine::vat::VatData& data);
    std::string errorLine(const jasperengine::vat::VatData& data);
    // payload bytes of one clip (its rows of the texture)
    uint64_t clipBytes(const jasperengine::vat::VatData& data, const jasperengine::vat::VatClip& clip);

    // The Model filename a mesh_mismatch fix sets: the .vat's mesh next to it, in the
    // component's (asset-relative) form; "" when `loadError` is not a mesh_mismatch
    std::string meshMismatchFix(const std::string& loadError, const std::string& animation, const std::string& meshName);
    // the button's label: "Use hero.vat.glb"
    std::string meshFixLabel(const std::string& meshPath);

    // The device-limit warning of a payload `height` rows tall (spec 2.4): "" when it fits or
    // the limit is unknown (0)
    std::string deviceLimitWarning(int height, int maxTextureSize2D, const std::string& asset);

    // A clip's time to show and set (seconds within the clip, 0..length) for a local time `tau`
    // (unwrapped, as vatTrackTau gives it): the clip's wrap applied
    float clipTimeAt(const jasperengine::vat::VatClip& clip, double tau);
    // The local time (unwrapped) the scrubber sets for clip time `time` while the clip is at `tau`:
    // in the same cycle for loop and ping-pong clips (a ping-pong's forward leg), `time` itself
    // for once / clamp-forever clips
    double scrubTarget(const jasperengine::vat::VatClip& clip, double tau, float time);
    // The local times the instances get when the scrubber moves the entity's clip from `from` to
    // `to` (each keeps its offset, so a crowd stays out of step)
    std::vector<double> shiftedTimes(const std::vector<double>& instanceTaus, double from, double to);

    // "Loop", "Once", "Ping Pong", "Clamp Forever" (the .vat wraps: never Default)
    const char* wrapLabel(ClipWrapMode wrap);
    // an event's args as typed in the events list ("left, 1, true"); parseEventArgs reads the
    // same back (numbers, true/false, quoted or bare strings); false with a message when the text
    // does not read
    std::string eventArgsText(const std::vector<AnimationEventArg>& args);
    bool parseEventArgs(const std::string& text, std::vector<AnimationEventArg>& out, std::string* error);
    // a Lua identifier (an event's function)
    bool isLuaName(const std::string& name);

    // The staleness banner's line for one checkVatStaleness issue: "Source changed since baking:
    // 'models/hero.gltf' changed since 'vat/hero.vat' was baked. Rebake ..." for the source kinds,
    // "<message>. <fix>" for the others
    std::string stalenessText(const std::string& kind, const std::string& message, const std::string& fix);

    // "Removed clips keep their rows until the next Rebake (37 unused frames, 812 KB)"; "" when the
    // clips use every frame
    std::string removedClipsNote(const jasperengine::vat::VatData& data);

}
