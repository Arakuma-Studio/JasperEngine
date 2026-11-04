// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ReloadHandlers.h"

#include "Project.h"
#include "graph/GraphRegistry.h"
#include "util/ProjectChecks.h"
#include "util/Util.h"

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

std::string reload::rel(Project& project, const fs::path& path) {
    return ProjectChecks::projectRelative(project.getProjectPath(), path);
}

std::string reload::kindOf(const fs::path& path) {
    const std::string text = watch::pathToUtf8(path);
    const std::string ext = watch::lowerExtension(path);
    if (path.filename() == "project.yaml") return "project";
    // a .cube LUT is a texture of the post-processing (S13: the renderer caches it by path)
    if (Util::isImageFile(text) || Util::isColorLutFile(text)) return "texture";
    if (Util::isModelFile(text) || ext == ".fbx") return "model";
    if (Util::isMaterialFile(text)) return "material";
    if (Util::isParticleEffectFile(text)) return "particles";
    if (Util::isShaderFile(text)) return "shader";
    if (Util::isAudioFile(text)) return "audio";
    if (Util::isFontFile(text)) return "font";
    if (Util::isScriptFile(text) || ext == ".c" || ext == ".inl") return "script";
    if (Util::isHtmlFile(text) || Util::isCssFile(text) || ext == ".json" || ext == ".lottie") return "ui";
    // pose animations, the models' humanoid maps, animation clips, the Animator's assets,
    // Vertex Animations and Shake Presets: their documents and players
    if (Util::isPoseAnimationFile(text) || Util::isAnimationClipFile(text) || Util::isAnimatorAssetFile(text) || Util::isVertexAnimationFile(text) ||
        Util::isShakePresetFile(text) || ext == ".humanoid") {
        return "animation";
    }
    if (Util::isSceneFile(text)) return "scene";
    if (Util::isPrefabFile(text)) return "prefab";
    // any registered kind's files
    if (graph::GraphRegistry::get().kindForFile(text)) return "graph";
    if (ext == ".txt" || ext == ".md" || ext == ".yaml" || ext == ".yml" || ext == ".xml" || ext == ".csv" || ext == ".ini" || ext == ".cfg" ||
        ext == ".cmake" || ext == ".hlsl") {
        return "text";
    }
    return "";
}

bool reload::isEditorOwnedFile(const std::string& relativePath) {
    // the terrain maps the editor writes from its in-memory terrain (TerrainMapFileWriter):
    // reloading them would only undo edits (TerrainEditWindow's rule)
    const size_t slash = relativePath.find_last_of('/');
    if (slash == std::string::npos) return false;
    const std::string name = relativePath.substr(slash + 1);
    if (name.rfind("terrain_edit_", 0) != 0) return false;
    const std::string parent = relativePath.substr(0, slash);
    const size_t parentSlash = parent.find_last_of('/');
    return (parentSlash == std::string::npos ? parent : parent.substr(parentSlash + 1)) == "terrain_maps";
}
