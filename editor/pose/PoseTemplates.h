// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Pose animation templates (spec: pose animation phase 6, section 2): starter animations that
// play on any humanoid, because poses are rig-independent joint angles.
//
// A template is a .poseanim file with a "template": {name, description, category} object after
// `version` and an empty preview_model (pose/PoseAnimationIO.h reads and writes it).
// - Built-in templates: editor/resources/pose-templates/*.poseanim, embedded in the editor binary
//   at configure time (CMakeLists.txt generates pose_templates.h), read-only.
// - User templates: <project>/pose-templates/*.poseanim (the folder is created on the first save).
//
// Names are looked up case-insensitively; a user template shadows the built-in template of the
// same name (list() then shows only the user one). The name inside the file is what counts, the
// file name is only derived from it when saving.
//
// Saving and deleting are steps of the project command history (Ctrl+Z, MCP undo with scope
// project): saving is a WriteFilesCmd, deleting a RemoveFilesCmd. Main thread only (the cache of
// the parsed built-ins is not locked).

#pragma once

#include "pose/PoseAnimationIO.h"
#include "pose/PoseTypes.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor{

    class Command;
    class CommandHistory;

    struct PoseTemplateInfo{
        std::string name;
        std::string description;
        std::string category;
        size_t poseCount = 0;
        bool loop = false;
        bool user = false;                  // a project template (else built-in)
        std::filesystem::path file;         // user: the absolute file; built-in: its resource file name
    };

    struct PoseTemplate{
        PoseTemplateInfo info;
        pose::PoseAnimationData data;       // previewModel is always empty
        std::vector<std::string> warnings;  // parse warnings of the file
    };

    // A built-in template file as embedded in the binary
    struct PoseTemplateResource{
        std::string fileName;               // "walk.poseanim"
        std::string text;
    };

    enum class PoseTemplateApply{
        Replace,    // the template's settings and poses replace the animation's (its preview model is kept)
        Append      // the template's poses are added after the animation's; its settings stay
    };

    class PoseTemplates{
    public:
        static constexpr const char* USER_FOLDER = "pose-templates";
        static constexpr size_t MAX_NAME_LENGTH = 64;

        // The embedded built-in files (PoseTemplatesBuiltin.cpp; the pose unit tests provide
        // their own, read from editor/resources/pose-templates)
        static std::vector<PoseTemplateResource> builtinResources();

        // The built-ins parsed once, sorted like list()
        static const std::vector<PoseTemplate>& builtins();

        // <project>/pose-templates
        static std::filesystem::path userDirectory(const std::filesystem::path& projectPath);

        // Built-in templates first, then the user templates; each group sorted by category, then
        // name (case-insensitive). Shadowed built-ins are left out. User files that are not
        // templates (no "template" object, unreadable) are skipped.
        static std::vector<PoseTemplateInfo> list(const std::filesystem::path& projectPath);

        // The template called `name` (user first). False with `error` (listing the names) when
        // there is none.
        static bool load(const std::filesystem::path& projectPath, const std::string& name, PoseTemplate& out,
                         std::string* error);

        // Non-empty after trimming, at most MAX_NAME_LENGTH bytes, no control characters
        static bool validName(const std::string& name, std::string* error);

        // The file a user template of that name is saved to: the existing user file with that
        // name, else <project>/pose-templates/<name with unsafe characters replaced>.poseanim
        static std::filesystem::path userFile(const std::filesystem::path& projectPath, const std::string& name);
        // A file name stem for `name`: trimmed, characters unsafe in file names replaced by '_'
        // (userFile's rule; the Templates tab names new animations with it)
        static std::string fileStem(const std::string& name);

        // The template file text of `data` (value ranges clamped, preview_model emptied)
        static std::string fileText(const pose::PoseAnimationData& data, const pose::PoseAnimationTemplateInfo& info);

        // `target` with the template applied (see PoseTemplateApply)
        static void apply(const pose::PoseAnimationData& templateData, pose::PoseAnimationData& target, PoseTemplateApply mode);

        // A new animation from a template: its settings and poses, `previewModel` as preview model
        static pose::PoseAnimationData newAnimation(const pose::PoseAnimationData& templateData, const std::string& previewModel);

        // The undoable commands (not executed). `afterChange` runs after execute, undo and redo
        // with the changed paths. Null with `error` when the name is invalid (save) or there is no
        // user template of that name (remove; built-ins cannot be removed).
        static Command* saveCommand(const std::filesystem::path& projectPath, const std::string& name,
                                    const std::string& description, const std::string& category,
                                    const pose::PoseAnimationData& data, std::string* error,
                                    std::filesystem::path* file = nullptr);
        static Command* removeCommand(const std::filesystem::path& projectPath, const std::string& name, std::string* error,
                                      std::filesystem::path* file = nullptr);

        // Runs the command through `history` (one undo step): false with `error` when it could
        // not be created or executed
        static bool save(CommandHistory& history, const std::filesystem::path& projectPath, const std::string& name,
                         const std::string& description, const std::string& category,
                         const pose::PoseAnimationData& data, std::string* error, std::filesystem::path* file = nullptr);
        static bool remove(CommandHistory& history, const std::filesystem::path& projectPath, const std::string& name,
                           std::string* error);

        // Called after a template file was written or removed (save, remove, their undo/redo):
        // the MCP server sets it to its file sync (asset database, Project window)
        static void setFilesChangedHook(std::function<void(const std::vector<std::filesystem::path>&)> hook);
    };

}
