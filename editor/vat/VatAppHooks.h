// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (vertex animation spec 4.5, 5.1-5.3)
// - Purpose: what the Inspector (.vat asset view, Vertex Animator section) and the Project window
//   ask of the Vertex Animation Baker without linking it (the AnimationClipInspectorHooks /
//   PoseInspectorHooks pattern). The baker-window task sets them at startup.
// - Owner: W5 (struct, every call site: InspectorWindowVat.cpp, ProjectWindow.cpp); the
//   baker-window agent sets openBaker / rebake / checkStaleness (VatBaker's checkVatStaleness).
// - Thread-safety: main thread; set once at startup.
// - Invariants: every hook is optional. Unset: the Inspector's Rebake / Open in Baker buttons are
//   disabled with a tooltip saying the baker is not available, the staleness banner is not drawn,
//   and the Project window's "Bake Vertex Animation..." / "Import as Vertex Animation..." entries
//   are hidden. Hooks report their own errors and never open modal popups synchronously.

#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor{

    class Project;

    // what checkStaleness found (VatBaker's checkVatStaleness): the sources of the recipe that
    // changed since the bake or are gone
    struct VatStalenessInfo{
        bool checked = false;           // false: the recipe could not be checked (`message` says why)
        bool stale = false;             // a source changed or is missing
        std::vector<std::string> changed;   // project-relative sources whose hash differs
        std::vector<std::string> missing;   // sources not found
        std::string message;            // a sentence for the banner ("Source changed since baking: hero.gltf")
    };

    // What "Open in Baker" / the Project window entries prefill the window with
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
        // The staleness of a .vat (absolute path) against its recipe's source hashes. Called at
        // most once per revision of the file and once a second (the Inspector caches it)
        std::function<VatStalenessInfo(Project* project, const std::filesystem::path& vatPath)> checkStaleness;
    };

    // the editor-wide hooks (set once at startup by the tasks that own them)
    inline VatAppHooks& vatAppHooks(){
        static VatAppHooks hooks;
        return hooks;
    }

}
