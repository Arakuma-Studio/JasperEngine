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
#include <set>

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

void keepVatEdits(VatData& fresh, const VatData& old){
    std::vector<std::string> names;
    std::vector<float> lengths;
    for (const VatClip& clip : fresh.clips){
        names.push_back(clip.name);
        lengths.push_back(clip.length());
    }
    const std::vector<int> match = matchClips(old.clips, names, lengths);
    std::set<std::string> used;
    for (size_t i = 0; i < fresh.clips.size(); i++){
        VatClip& clip = fresh.clips[i];
        if (i >= match.size() || match[i] < 0 || (size_t)match[i] >= old.clips.size()){
            used.insert(clip.name);
            continue;
        }
        const VatClip& previous = old.clips[(size_t)match[i]];
        if (!used.count(previous.name)) clip.name = previous.name;
        clip.events = previous.events;
        clip.speed = previous.speed;
        clip.interpolate = previous.interpolate;
        if ((previous.wrap == ClipWrapMode::Loop) == (clip.wrap == ClipWrapMode::Loop)) clip.wrap = previous.wrap;
        used.insert(clip.name);
    }
    for (VatSocket& socket : fresh.sockets){
        for (const VatSocket& previous : old.sockets){
            if (previous.path == socket.path) socket.name = previous.name;
        }
    }
    fresh.unknownKeys = old.unknownKeys;
}

bool planVatImport(Project* project, const VatImportResult& result, const VatImportDetection& detection,
                      const VatImportPlan& options, std::string& vatPath, bool& rewrite, std::string& error){
    rewrite = false;
    if (!result.ok() || result.data.recipe.sources.empty()){
        error = "the import failed";
        return false;
    }
    const VatRecipeSource& ours = result.data.recipe.sources[0];
    if (!options.output.empty()){
        std::string relative;
        if (!projectRelativeFile(project, options.output, false, relative, error)) return false;
        const fs::path absolute = (project->getProjectPath() / FileUtils::pathFromUtf8(relative)).lexically_normal();
        if (lower(relative).size() < 4 || lower(relative).rfind(".vat") != lower(relative).size() - 4 || !project->isInsideAssetsPath(absolute)){
            error = "'" + relative + "' is not a .vat path inside the assets folder";
            return false;
        }
        vatPath = relative;
        if (fileThere(absolute) || fileThere(FileUtils::pathFromUtf8(meshPathFor(FileUtils::pathToUtf8(absolute))))){
            if (!options.overwrite){
                error = "'" + relative + "' exists: pass overwrite, or choose another output";
                return false;
            }
            rewrite = true;
        }
        return true;
    }
    // next to the export when it is inside the assets, else in the assets folder
    std::string directory;
    if (!detection.folder.empty()){
        const fs::path folder = FileUtils::pathFromUtf8(detection.folder).lexically_normal();
        if (project->isInsideAssetsPath(folder)) directory = FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(folder));
    }
    if (directory.empty()) directory = FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(project->getAssetsPath()));
    const std::string stem = fileSafe(detection.assetName.empty() ? std::string("vertex animation") : detection.assetName);
    for (int n = 1;; n++){
        const std::string relative = (directory.empty() ? std::string() : directory + "/") + stem + (n > 1 ? " " + std::to_string(n) : std::string()) +
                                     VAT_EXTENSION;
        const fs::path absolute = (project->getProjectPath() / FileUtils::pathFromUtf8(relative)).lexically_normal();
        const bool vatThere = fileThere(absolute);
        const bool meshThere = fileThere(FileUtils::pathFromUtf8(meshPathFor(FileUtils::pathToUtf8(absolute))));
        if (!vatThere && !meshThere){
            vatPath = relative;
            return true;
        }
        VatData old;
        if (vatThere && readVatAsset(project, relative, old, nullptr) && old.recipe.sources.size() == 1 && sameRecipeSource(old.recipe.sources[0], ours)){
            vatPath = relative;
            rewrite = true;
            return true;
        }
    }
}

bool writeImportedVat(Project* project, VatImportResult& result, const VatImportDetection& detection, const VatImportPlan& options,
                    VatWriteResult& out, std::string& error){
    out = VatWriteResult();
    std::string relative;
    bool rewrite = false;
    if (!planVatImport(project, result, detection, options, relative, rewrite, error)) return false;
    if (options.place == VatPlace::Replace){
        error = "place replace needs a GameObject source (an import has none): use beside or crowd";
        return false;
    }
    const fs::path absolute = (project->getProjectPath() / FileUtils::pathFromUtf8(relative)).lexically_normal();
    VatData& data = result.data;
    VatData old;
    const bool hasOld = rewrite && readVatAsset(project, relative, old, nullptr);
    if (hasOld && !old.guid.empty()) data.guid = old.guid;
    if (data.guid.empty()) data.guid = AssetDatabase::generateGuid();
    const bool sameSource = hasOld && old.recipe.sources.size() == 1 && !data.recipe.sources.empty() &&
                            sameRecipeSource(old.recipe.sources[0], data.recipe.sources[0]);
    if (hasOld && (sameSource || options.keepGuidOf)) keepVatEdits(data, old);

    VatEncodedFiles encoded;
    if (!encodeVatImport(result, FileUtils::pathToUtf8(absolute), encoded, &error)) return false;
    std::vector<CreateVatCmd::File> files;
    files.push_back({absolute, encoded.vatText});
    files.push_back({FileUtils::pathFromUtf8(encoded.meshPath), std::string(encoded.glbBytes.begin(), encoded.glbBytes.end())});
    out.vat = relative;
    out.mesh = FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(FileUtils::pathFromUtf8(encoded.meshPath)));
    for (const CreateVatCmd::File& file : files) out.files.push_back(FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(file.path)));
    out.rewritten = rewrite;
    out.guid = data.guid;
    out.undoName = "Import Vertex Animation '" + relative + "'";

    if (options.place == VatPlace::None){
        std::vector<WriteFilesCmd::Item> items;
        for (CreateVatCmd::File& file : files) items.push_back({file.path, std::move(file.content)});
        Project* owner = project;
        auto failureText = std::make_shared<std::string>();
        auto* command = new WriteFilesCmd(std::move(items), [owner](const std::vector<fs::path>& paths){
            CreateVatCmd::notifyFiles(owner, paths);
        });
        command->reportFailureTo(failureText);
        if (!project->getProjectCommandHistory()->addCommand(command)){
            error = failureText->empty() ? std::string("the files could not be written") : *failureText;
            return false;
        }
        return true;
    }

    VatPlacement placement;
    placement.mode = options.place == VatPlace::Crowd ? VatPlacement::Mode::Crowd : VatPlacement::Mode::Beside;
    if (placement.mode == VatPlacement::Mode::Crowd){
        error = "crowd placement is not available yet: import with place beside, then make it a crowd";
        return false;
    }
    const uint32_t sceneId = options.sceneId != 0 ? options.sceneId : project->getSelectedSceneId();
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene){
        error = "no open scene to place the result in";
        return false;
    }
    if (sceneProject->playState != ScenePlayState::STOPPED){
        error = "the scene is playing: stop play mode to place the result";
        return false;
    }
    placement.name = (detection.assetName.empty() ? FileUtils::pathToUtf8(absolute.stem()) : detection.assetName) + " (VAT)";
    placement.model = ClipConverters::assetRelative(project, FileUtils::pathFromUtf8(encoded.meshPath));
    placement.animation = ClipConverters::assetRelative(project, absolute);
    placement.guid = data.guid;
    auto* command = new CreateVatCmd(project, sceneId, files, placement);
    auto failureText = std::make_shared<std::string>();
    command->reportFailureTo(failureText);
    if (!CommandHandle::get(sceneId)->addCommand(command)){
        error = failureText->empty() ? std::string("the import could not be placed") : *failureText;
        return false;
    }
    out.sceneId = sceneId;
    out.entity = command->getEntity();
    return true;
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
    std::string relative;
    if (!projectRelativeFile(project, vatPath, true, relative, error)) return false;
    options.output = relative;
    options.overwrite = true;
    options.keepGuidOf = true;
    return writeImportedVat(project, result, detection, options, out, error);
}

}
