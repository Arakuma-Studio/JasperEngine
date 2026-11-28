// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// What a build ships (docs/specs/S98-editor-tooling-gaps.md REQ-8 to REQ-12, D3): Project Settings
// > Player > Build > Asset Stripping. Ship All Assets ships the assets folder as before; Referenced
// Only ships the closure, over the dependency index, of the build's scenes, project.yaml, every Lua
// and C++ source, the files their string literals name, and Always Include. Exclude From Build
// leaves its matches out under either mode and wins over Always Include. Lua sources, shader
// sources, scenes and data files always ship (only the file types below can be stripped).

#include "Project.h"

#include <set>
#include <string>
#include <vector>

namespace jasperengine::editor {

    // MCP and the Build Report: "ship_all", "referenced_only"
    const char* assetStrippingModeName(AssetStrippingMode mode);
    bool assetStrippingModeFromName(const std::string& name, AssetStrippingMode& mode);

    namespace buildglob {
        // A project-relative glob: '*' within one path segment, '**' across segments, '?' one
        // character. A glob without wildcards names that file, or everything below that folder.
        bool matches(const std::string& glob, const std::string& projectRelative);
        // The first of `globs` that matches, or nullptr
        const std::string* firstMatch(const std::vector<std::string>& globs, const std::string& projectRelative);
        // What the Project window adds for a selected file ("assets/ui/logo.png") or folder
        // ("assets/levels/**")
        std::string forSelection(const std::string& projectRelative, bool folder);
        // false with the fix in error: empty, absolute, "..", a backslash
        bool isValid(const std::string& glob, std::string& error);
    }

    // The settings with one glob added (on) or removed (off) from Always Include or Exclude From
    // Build: the Project window's toggles
    AssetStrippingSettings withBuildGlob(const AssetStrippingSettings& settings, bool exclude, const std::string& glob, bool on);

    class AssetStrippingPlan {
    public:
        // Main thread (the dependency index and its overlay of unsaved documents). Under Ship All
        // Assets it runs no closure (REQ-43).
        static AssetStrippingPlan make(Project& project);

        bool referencedOnly() const { return settings.mode == AssetStrippingMode::ReferencedOnly; }
        // "" when the file ships, else "unreferenced" or "excluded: <glob>"
        std::string leftOutReason(const std::string& projectRelative) const;
        // a reference found after the closure (an FBX import's linked textures)
        void keep(const std::string& projectRelative);
        const std::set<std::string>& referenced() const { return kept; }

        // The file types Referenced Only may leave out: textures, models, audio, video, fonts,
        // materials, prefabs, UI documents and stylesheets, animation and particle assets, sprite
        // and tile assets, Title Art, voxel volumes, .mesh assets
        static bool isStrippable(const std::string& projectRelative, const std::string& assetsDir);

    private:
        AssetStrippingSettings settings;
        std::string assetsDir;
        std::set<std::string> kept;
    };

}
