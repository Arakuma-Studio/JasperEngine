// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S21-animation.md D13, R22, R32)
// - Purpose: the editor's working copy of one of the Animator's assets (.controller, .mask,
//   .montage), shared per path by everything that edits or shows it (the Animator window, the
//   Inspector's asset views, MCP). AnimationClipDocument's design: every edit is commit(): the
//   value is normalized, written as its canonical text through the project command history
//   (WriteFilesCmd: one named undo step; a non-empty merge key merges drags) and read back, so
//   data() is what the file holds. Undo, redo and outside changes (hot reload) reach documents
//   through filesChanged(), which reloads them and notifies listeners; every new revision
//   invalidates AnimatorAssetPool for the path, so running Animators rebind.
// - Thread-safety: main thread.
// - Invariants: one document per normalized absolute path; a file that does not parse keeps the
//   last good value and reports the error; the GUID of an existing file never changes on commit
//   (a new file gets one).

#pragma once

#include "animation/AnimatorAssetIO.h"
#include "animation/ControllerDef.h"

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine{
    struct AnimationPlayerComponent;
}

namespace jasperengine::editor{

    class Project;

    // "Create Animator Controller from Animation Player" (the Inspector, edit_animator_controller
    // from_player): a state per clip of `player` in the first layer (added when none), looping as
    // the clip wraps, laid out on a grid; the player's default clip the default state. Returns
    // the number of states added.
    size_t appendPlayerStates(anim::ControllerDef& def, const AnimationPlayerComponent& player);

    class AnimatorAssetDocument{
    public:
        enum class Kind { Controller, Mask, Montage };

        // the document of a path (absolute, project-relative or asset-relative); null + `error`
        // when it is not one of the three or does not load (missing, not valid)
        static std::shared_ptr<AnimatorAssetDocument> open(Project* project, const std::string& path, std::string* error);
        // a new file (refused when it exists) holding `kind`'s defaults: a controller with a
        // Base Layer, an empty mask, a montage of `clip`; one project undo step
        static std::shared_ptr<AnimatorAssetDocument> create(Project* project, const std::string& path, Kind kind,
                                                             const std::string& undoName, std::string* error);
        // reload the documents of these files (undo, redo, outside edits); listeners notified
        static void filesChanged(const std::vector<std::filesystem::path>& paths);
        static void closeAll();
        // the kind of a path by its extension; false when none
        static bool kindOf(const std::string& path, Kind& out);
        static const char* kindName(Kind kind);

        Kind kind() const { return assetKind; }
        // absolute; project-relative ("assets/anim/hero.controller"); asset-relative (the
        // component form, "anim/hero.controller")
        const std::filesystem::path& absolutePath() const { return filePath; }
        const std::string& projectPath() const { return relativePath; }
        const std::string& assetPath() const { return assetRelative; }
        uint64_t revision() const { return documentRevision; }
        const std::vector<ClipWarning>& warnings() const { return loadWarnings; }
        // the last reload's error (the file stopped parsing); empty when it holds `data`
        const std::string& loadError() const { return error; }
        const std::string& guid() const;

        const anim::ControllerDef& controller() const { return controllerData; }
        const anim::AvatarMaskDef& mask() const { return maskData; }
        const anim::MontageDef& montage() const { return montageData; }

        // writes the new value (normalized; the GUID kept) as one undo step named `undoName`;
        // false + `error` when the file cannot be written or changed on disk meanwhile
        bool commit(const anim::ControllerDef& data, const std::string& undoName, const std::string& mergeKey, std::string* error);
        bool commit(const anim::AvatarMaskDef& data, const std::string& undoName, const std::string& mergeKey, std::string* error);
        bool commit(const anim::MontageDef& data, const std::string& undoName, const std::string& mergeKey, std::string* error);
        // reads the file again (listeners notified when it changed)
        void reload();

        int addListener(std::function<void()> listener);
        void removeListener(int id);

    private:
        Project* project = nullptr;
        Kind assetKind = Kind::Controller;
        std::filesystem::path filePath;
        std::string relativePath;
        std::string assetRelative;
        std::string text;
        std::string error;
        uint64_t documentRevision = 0;
        std::vector<ClipWarning> loadWarnings;
        anim::ControllerDef controllerData;
        anim::AvatarMaskDef maskData;
        anim::MontageDef montageData;
        std::map<int, std::function<void()>> listeners;
        int nextListener = 1;
        uint64_t mergeEpoch = 0;

        bool load(const std::string& newText, std::string* error);
        bool write(const std::string& newText, const std::string& undoName, const std::string& mergeKey, std::string* error);
        void notify();
    };

}
