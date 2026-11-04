// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The editor UI around .aseprite files that is not a panel (docs/specs/S44-aseprite-import.md
// R30-R32): the App's hooks the Inspector and the Project window call, and Open in Aseprite
// with its fallbacks. Main thread only.

#pragma once

#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::pixelart::aseprite {

    // Set by the App: the importer view and the Project window stay free of the window classes
    struct AsepriteEditorHooks {
        // Window > 2D > Sprite Preview on a file (asset-relative); unset: there is none to open
        std::function<void(const std::string& assetPath)> openSpritePreview;
    };
    AsepriteEditorHooks& asepriteEditorHooks();

    // Preferences > External Tools > Aseprite: the AppSettings external tool whose value is
    // settings.yaml's externalTools.aseprite.path (§6.8)
    inline constexpr const char* kAsepriteToolKey = "aseprite.path";
    std::string asepritePreference();

    // R32: the files (asset-relative or absolute) in Aseprite, detached. Detection runs off the
    // main thread (AsepriteLocator): until it answers, the request waits for updateAsepriteOpen.
    // When Aseprite is not found or does not start, the "Aseprite Not Found" dialog says why,
    // with Locate... (sets the preference and opens the files) and a link to aseprite.org; with
    // fallbackToPreview the first file also opens in the Sprite Preview.
    void openInAsepriteOrFallback(Project* project, const std::vector<std::string>& assetPaths, bool fallbackToPreview);

    // Every frame, inside the ImGui frame (App::show): a waiting request once detection is done
    // (the loop keeps drawing until then), and the dialog
    void updateAsepriteOpen();

}
