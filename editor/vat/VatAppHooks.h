// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (vertex animation spec 4.5, 5.1-5.3)
// - Purpose: what the Inspector (.vat asset view) and the Project window ask of the Vertex
//   Animation Baker window without linking it (the AnimationClipInspectorHooks / PoseInspectorHooks
//   pattern). The staleness banner needs no hook: the Inspector calls checkVatStaleness
//   (vat/VatBakeSession.h) itself.
// - Owner: W5 (struct, every call site: InspectorWindowVat.cpp, ProjectWindow.cpp); the
//   baker-window task (W6) sets openBaker and rebake at startup.
// - Thread-safety: main thread; set once at startup.
// - Invariants: every hook is optional. Unset: the Inspector's Rebake / Open in Baker buttons are
//   disabled with a tooltip saying the baker is not available, and the Project window's
//   "Open in Vertex Animation Baker" / "Bake Vertex Animation..." / "Import as Vertex Animation..."
//   entries are hidden (a .vat double-click only shows it in the Inspector). Hooks report their
//   own errors and never open modal popups synchronously from the call sites.

#pragma once

#include <filesystem>
#include <functional>

namespace jasperengine::editor{

    class Project;

    // What the baker window opens with
    enum class VatBakerSource{
        VatAsset,       // a .vat: its recipe loaded, "Rebake" instead of "Bake" (double-click, Open in Baker)
        Bake,           // a model (.gltf/.glb/.fbx) or an .anim: "Bake Vertex Animation..."
        Import          // a Houdini VAT 3 / OpenVAT export file: "Import as Vertex Animation..."
    };

    struct VatAppHooks{
        // Opens the Vertex Animation Baker window prefilled with `path` (absolute)
        std::function<void(Project* project, const std::filesystem::path& path, VatBakerSource source)> openBaker;
        // Re-runs the .vat's recipe (absolute path) and rewrites the pair, keeping its GUID and the
        // hand edits (clip names, events, speeds, wraps), as one undo step; reports its own errors
        std::function<void(Project* project, const std::filesystem::path& vatPath)> rebake;
    };

    // the editor-wide hooks (set once at startup by the tasks that own them)
    inline VatAppHooks& vatAppHooks(){
        static VatAppHooks hooks;
        return hooks;
    }

}
