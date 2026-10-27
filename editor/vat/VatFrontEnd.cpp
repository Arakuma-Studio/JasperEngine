// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "vat/VatFrontEnd.h"

#include "Project.h"
#include "animation/ClipConverters.h"
#include "animation/ClipEditorUtil.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/CreateVatCmd.h"
#include "command/type/WriteFilesCmd.h"
#include "util/AssetDatabase.h"
#include "util/FileUtils.h"
#include "util/Util.h"

#include "animation/ModelSubClips.h"
#include "pose/PoseAnimationIO.h"

#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::vat;

namespace jasperengine::editor::vat{

namespace{

    VatIssue invalid(const std::string& message, const std::string& fix){
        VatIssue issue;
        issue.kind = "invalid_source";
        issue.error = true;
        issue.message = message;
        issue.fix = fix;
        return issue;
    }

    std::string lower(std::string text){
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        return text;
    }

    std::string extensionOf(const std::string& path){
        return lower(FileUtils::pathToUtf8(FileUtils::pathFromUtf8(path).extension()));
    }

    bool fileThere(const fs::path& path){
        std::error_code ec;
        return fs::exists(path, ec);
    }

    std::string fileSafe(const std::string& name){
        std::string out;
        for (char c : name){
            out += (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') ? '_' : c;
        }
        while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
        return out.empty() ? std::string("vertex animation") : out;
    }

    const char* ACCEPTED = "give a model (.gltf/.glb/.fbx, \"hero.gltf#Walk\" for one clip), .anim files with on (a model, prefab or "
                           "GameObject), a .poseanim, a GameObject, or a Houdini VAT 3 / OpenVAT export (its folder, mesh, .mat or "
                           "-remap_info.json)";

}

bool projectRelativeFile(Project* project, const std::string& given, bool mustExist, std::string& relative, std::string& error){
    if (given.empty()){
        error = "an empty path";
        return false;
    }
    const fs::path path = FileUtils::pathFromUtf8(given);
    const fs::path absolute = (path.is_absolute() ? path : project->getProjectPath() / path).lexically_normal();
    const fs::path rel = absolute.lexically_relative(project->getProjectPath().lexically_normal());
    if (rel.empty() || *rel.begin() == ".."){
        error = "'" + given + "' is outside the project";
        return false;
    }
    relative = FileUtils::pathToGenericUtf8(rel);
    if (mustExist && !fileThere(absolute)){
        error = "'" + relative + "' does not exist";
        return false;
    }
    return true;
}

bool routeVatSource(Project* project, const std::vector<std::string>& files, const std::string& on, bool allowOutside,
                    VatSourceRoute& out, VatIssue& error){
    out = VatSourceRoute();
    if (!project){
        error = invalid("no project is open", "open a project");
        return false;
    }
    if (files.empty()){
        error = invalid("no source given", ACCEPTED);
        return false;
    }
    // each entry: a file, with a "#Clip" suffix on model files
    std::vector<std::string> relatives;
    std::vector<std::string> clipNames;
    for (const std::string& given : files){
        std::string path = given, clip;
        const fs::path direct = FileUtils::pathFromUtf8(given);
        const fs::path directAbsolute = direct.is_absolute() ? direct : project->getProjectPath() / direct;
        const size_t hash = given.rfind('#');
        if (!fileThere(directAbsolute) && hash != std::string::npos && hash > 0){
            path = given.substr(0, hash);
            clip = given.substr(hash + 1);
            if (!anim::isModelFilePath(path)){
                error = invalid("'" + given + "': only model files take a #Clip suffix", ACCEPTED);
                return false;
            }
            if (clip.empty()){
                error = invalid("'" + given + "' names no clip after '#'", "write model#ClipName, e.g. assets/models/hero.gltf#Walk");
                return false;
            }
        }
        std::string relative, why;
        if (!projectRelativeFile(project, path, true, relative, why)){
            // an export outside the project (the CLI)
            const fs::path p = FileUtils::pathFromUtf8(path);
            if (allowOutside && files.size() == 1 && p.is_absolute() && fileThere(p)){
                const VatImportDetection detection = detectVatImport(path);
                if (detection.found()){
                    out.import = true;
                    out.importPath = FileUtils::pathToUtf8(p.lexically_normal());
                    out.detection = detection;
                    return true;
                }
            }
            error = invalid("source: " + why, ACCEPTED);
            return false;
        }
        relatives.push_back(relative);
        clipNames.push_back(clip);
    }

    // what the files are
    const std::string first = extensionOf(relatives[0]);
    auto all = [&](const std::string& extension){
        for (const std::string& r : relatives){
            if (extensionOf(r) != extension) return false;
        }
        return true;
    };
    const fs::path firstAbsolute = (project->getProjectPath() / FileUtils::pathFromUtf8(relatives[0])).lexically_normal();
    std::error_code ec;
    const bool folder = fs::is_directory(firstAbsolute, ec);

    if (!folder && anim::isModelFilePath(relatives[0])){
        // a model, unless it is a Houdini / OpenVAT export's mesh (its data textures are next to it:
        // the importers take any mesh, so a mesh alone is a model)
        const VatImportDetection detection = detectVatImport(FileUtils::pathToUtf8(firstAbsolute));
        const VatImportFiles& found = detection.files;
        const bool exportData = !found.position.empty() || !found.rotation.empty() || !found.lookup.empty() || !found.vat.empty() ||
                                !found.remapInfo.empty();
        if (detection.found() && exportData && relatives.size() == 1 && clipNames[0].empty()){
            out.import = true;
            out.importPath = FileUtils::pathToUtf8(firstAbsolute);
            out.detection = detection;
            return true;
        }
        for (size_t i = 0; i < relatives.size(); i++){
            if (relatives[i] != relatives[0]){
                error = invalid("the source lists two models ('" + relatives[0] + "', '" + relatives[i] + "')",
                                "bake one model at a time (list its clips as model#Clip or in clips)");
                return false;
            }
            if (clipNames[i].empty() && relatives.size() > 1){
                error = invalid("'" + relatives[i] + "' is listed with and without a #Clip", "list every clip as model#Clip, or the model once");
                return false;
            }
        }
        if (lower(relatives[0]).size() > 8 && lower(relatives[0]).rfind(".vat.glb") == lower(relatives[0]).size() - 8){
            error = invalid("'" + relatives[0] + "' is a baked Vertex Animation mesh, not a source",
                            "rebake it with rebake: its .vat, or bake from the original model");
            return false;
        }
        if (!on.empty()){
            error = invalid("on is for .anim and .poseanim sources ('" + relatives[0] + "' is a model with its own clips)",
                            "leave on out, or give the .anim files as source");
            return false;
        }
        out.source.kind = VatSourceKind::Model;
        out.source.path = relatives[0];
        for (const std::string& clip : clipNames){
            if (!clip.empty() && std::find(out.clips.begin(), out.clips.end(), clip) == out.clips.end()) out.clips.push_back(clip);
        }
        return true;
    }
    if (!folder && all(".anim")){
        out.source.kind = VatSourceKind::Anim;
        out.source.anims = relatives;
        if (on.empty()){
            out.needsTarget = true;
        }else{
            std::string relative, why;
            if (!projectRelativeFile(project, on, true, relative, why)){
                error = invalid("on: " + why, "give the model or prefab the clips play on (or a GameObject)");
                return false;
            }
            out.source.path = relative;
        }
        return true;
    }
    if (!folder && first == ".poseanim"){
        if (relatives.size() != 1){
            error = invalid("a pose animation source is one .poseanim (" + std::to_string(relatives.size()) + " given)",
                            "bake one pose animation at a time");
            return false;
        }
        out.source.kind = VatSourceKind::Pose;
        out.source.anims = relatives;
        std::string model = on;
        if (model.empty()){
            std::string text, why;
            pose::PoseAnimationData data;
            std::vector<std::string> warnings;
            if (clipui::readWholeFile(firstAbsolute, text) && pose::parsePoseAnimation(text, data, warnings, &why) && !data.previewModel.empty()){
                const fs::path file = project->resolveAssetPath(FileUtils::pathFromUtf8(data.previewModel)).lexically_normal();
                model = FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(file));
            }
        }
        if (model.empty()){
            out.needsTarget = true;
            return true;
        }
        std::string relative, why;
        if (!projectRelativeFile(project, model, true, relative, why)){
            error = invalid("on: " + why, "give the model the pose animation plays on (or a GameObject)");
            return false;
        }
        out.source.path = relative;
        return true;
    }
    if (!folder && (first == ".anim" || first == ".poseanim")){
        error = invalid("the source mixes .anim / .poseanim files with other files", "list only .anim files (with on), or one .poseanim");
        return false;
    }
    if (relatives.size() == 1){
        if (Util::isVertexAnimationFile(relatives[0])){
            error = invalid("'" + relatives[0] + "' is a Vertex Animation already", "pass it as rebake to bake it again from its recipe");
            return false;
        }
        if (first == ".prefab"){
            error = invalid("a prefab is a target, not a source", "give the .anim files as source and the prefab as on");
            return false;
        }
        const VatImportDetection detection = detectVatImport(FileUtils::pathToUtf8(firstAbsolute));
        if (detection.found()){
            out.import = true;
            out.importPath = FileUtils::pathToUtf8(firstAbsolute);
            out.detection = detection;
            return true;
        }
        error = invalid("'" + relatives[0] + "' is not a bake source" + (detection.summary.empty() ? std::string() : " (" + detection.summary + ")"),
                        ACCEPTED);
        return false;
    }
    error = invalid("the source lists " + std::to_string(relatives.size()) + " files that are not .anim clips", ACCEPTED);
    return false;
}

bool readVatAsset(Project* project, const std::string& vatPath, VatData& out, std::string* error){
    const fs::path given = FileUtils::pathFromUtf8(vatPath);
    const fs::path absolute = given.is_absolute() ? given : project->getProjectPath() / given;
    std::string text;
    if (!clipui::readWholeFile(absolute, text)){
        if (error) *error = "can't read '" + vatPath + "'";
        return false;
    }
    VatParseResult parsed = parseVat(text);
    if (!parsed.ok()){
        if (error) *error = parsed.error;
        return false;
    }
    out = *parsed.data;
    return true;
}

bool isImportRecipe(const VatRecipe& recipe){
    return !recipe.sources.empty() && (recipe.sources[0].kind == "houdini_vat3" || recipe.sources[0].kind == "openvat");
}

namespace{

    // the baker window's import options (vat/VatImportWrite.h) for a front end's plan
    VatImportWriteOptions writeOptions(const VatImportPlan& plan){
        VatImportWriteOptions options;
        options.output = plan.output;
        options.overwrite = plan.overwrite;
        options.place = plan.place;
        if (plan.place == VatPlace::Crowd) options.crowd = crowdPlacement(plan.crowd);
        return options;
    }

}

bool planVatImport(Project* project, const VatImportResult& result, const VatImportDetection& detection, const VatImportPlan& options,
                   std::string& vatPath, bool& rewrite, std::string& error){
    return plannedImportOutput(project, result, detection, writeOptions(options), vatPath, rewrite, error);
}

bool writeImportedVat(Project* project, VatImportResult& result, const VatImportDetection& detection, const VatImportPlan& options,
                      VatWriteResult& out, std::string& error){
    out = VatWriteResult();
    if (options.place == VatPlace::Replace){
        error = "place replace needs a GameObject source (an import has none): use beside or crowd";
        return false;
    }
    if (options.place == VatPlace::Crowd){
        const std::string problem = crowdPlacementProblem(options.crowd);
        if (!problem.empty()){
            error = problem;
            return false;
        }
    }
    // writeVatImport places into the selected scene
    if (options.place != VatPlace::None && options.sceneId != 0 && project->getScene(options.sceneId)){
        project->setSelectedSceneId(options.sceneId);
    }
    return writeVatImport(project, result, detection, writeOptions(options), out, error);
}

bool reimportVat(Project* project, const std::string& vatPath, VatWriteResult& out, std::string& error, VatImportReport* report){
    VatData old;
    if (!readVatAsset(project, vatPath, old, &error)) return false;
    VatImportSpec spec;
    if (!importSpecFromRecipe(old.recipe, FileUtils::pathToUtf8(project->getProjectPath()), spec, error)) return false;
    VatImportResult result = importVat(spec);
    if (report) *report = result.report;
    if (!result.ok()){
        error = vatImportIssuesText(result.errors);
        return false;
    }
    const VatImportDetection detection = detectVatImport(spec.path);
    VatImportPlan options;
    if (!projectRelativeFile(project, vatPath, true, options.output, error)) return false;
    // the same export: rewritten in place, GUID and hand edits kept (the existing-file rule)
    options.overwrite = true;
    return writeImportedVat(project, result, detection, options, out, error);
}

}
