// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "vat/VatImportWrite.h"

#include "Project.h"
#include "animation/ClipConverters.h"
#include "animation/ClipEditorUtil.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/WriteFilesCmd.h"
#include "util/AssetDatabase.h"
#include "util/FileUtils.h"

#include <algorithm>
#include <filesystem>
#include <set>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::vat;

namespace jasperengine::editor::vat{

namespace{

    std::string lower(std::string text){
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        return text;
    }

    bool endsWith(const std::string& text, const std::string& suffix){
        return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    bool fileExists(const fs::path& path){
        std::error_code ec;
        return fs::is_regular_file(path, ec);
    }

    std::string fileSafe(const std::string& name){
        std::string out;
        for (char c : name){
            out += (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') ? '_' : c;
        }
        while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
        return out.empty() ? std::string("Imported") : out;
    }

    bool readVat(const fs::path& file, VatData& out){
        std::string text;
        if (!clipui::readWholeFile(file, text)) return false;
        VatParseResult parsed = parseVat(text);
        if (!parsed.ok()) return false;
        out = *parsed.data;
        return true;
    }

    fs::path absoluteOf(Project* project, const std::string& path){
        const fs::path given = FileUtils::pathFromUtf8(path);
        return (given.is_absolute() ? given : project->getProjectPath() / given).lexically_normal();
    }

    // an import of the same export (kind and path): the existing-file rule's "same source"
    bool sameImport(const VatData& old, const VatImportResult& result){
        if (old.recipe.sources.size() != 1 || result.data.recipe.sources.size() != 1) return false;
        const VatRecipeSource& a = old.recipe.sources[0];
        const VatRecipeSource& b = result.data.recipe.sources[0];
        return a.kind == b.kind && a.path == b.path;
    }

}

std::string defaultImportOutput(Project* project, const VatImportDetection& detection){
    const std::string stem = fileSafe(detection.assetName);
    if (project){
        const fs::path folder = FileUtils::pathFromUtf8(detection.folder).lexically_normal();
        if (!detection.folder.empty() && project->isInsideAssetsPath(folder)){
            const std::string relative = FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(folder));
            return relative + "/" + stem + VAT_EXTENSION;
        }
        const std::string assets = FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(project->getAssetsPath()));
        return (assets.empty() ? std::string() : assets + "/") + stem + VAT_EXTENSION;
    }
    return "assets/" + stem + VAT_EXTENSION;
}

bool plannedImportOutput(Project* project, const VatImportResult& result, const VatImportDetection& detection,
                         const VatImportWriteOptions& options, std::string& vatPath, bool& rewrite, std::string& error){
    rewrite = false;
    if (!project){
        error = "no project is open";
        return false;
    }
    if (!options.output.empty()){
        const fs::path absolute = absoluteOf(project, options.output);
        vatPath = FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(absolute));
        if (!endsWith(lower(vatPath), VAT_EXTENSION) || !project->isInsideAssetsPath(absolute)){
            error = "'" + vatPath + "' is not a .vat path inside the assets folder";
            return false;
        }
        if (fileExists(absolute) || fileExists(FileUtils::pathFromUtf8(meshPathFor(FileUtils::pathToUtf8(absolute))))){
            if (!options.overwrite){
                error = "'" + vatPath + "' exists: overwrite it, or choose another output";
                return false;
            }
            rewrite = true;
        }
        return true;
    }
    const std::string base = defaultImportOutput(project, detection);
    const std::string stemPath = base.substr(0, base.size() - std::char_traits<char>::length(VAT_EXTENSION));
    for (int n = 1;; n++){
        const std::string relative = n > 1 ? stemPath + " " + std::to_string(n) + VAT_EXTENSION : base;
        const fs::path absolute = absoluteOf(project, relative);
        const bool vatThere = fileExists(absolute);
        const bool meshThere = fileExists(FileUtils::pathFromUtf8(meshPathFor(FileUtils::pathToUtf8(absolute))));
        if (!vatThere && !meshThere){
            vatPath = relative;
            return true;
        }
        VatData old;
        if (vatThere && readVat(absolute, old) && sameImport(old, result)){
            vatPath = relative;
            rewrite = true;
            return true;
        }
    }
}

bool writeVatImport(Project* project, VatImportResult& result, const VatImportDetection& detection,
                    const VatImportWriteOptions& options, VatWriteResult& out, std::string& error){
    out = VatWriteResult();
    if (!result.ok()){
        error = "the import failed: " + vatImportIssuesText(result.errors);
        return false;
    }
    if (options.place == VatPlace::Replace){
        error = "an import has no GameObject to replace: place it beside or as files only";
        return false;
    }
    if (options.place == VatPlace::Crowd && !options.crowd){
        error = "crowd placement is not available";
        return false;
    }
    if (options.place != VatPlace::None && project){
        // refused before anything is written: the scene it would go to must take a Vertex Animation
        SceneProject* destination = project->getScene(project->getSelectedSceneId());
        if (destination && destination->sceneType != SceneType::SCENE_3D){
            error = "a Vertex Animation plays in a 3D scene: '" + destination->name + "' is 2D (import with place none, or select a 3D scene)";
            return false;
        }
    }
    std::string relative;
    bool rewrite = false;
    if (!plannedImportOutput(project, result, detection, options, relative, rewrite, error)) return false;
    const fs::path absolute = absoluteOf(project, relative);

    VatData& data = result.data;
    VatData old;
    const bool hasOld = rewrite && readVat(absolute, old);
    data.guid = hasOld && !old.guid.empty() ? old.guid : AssetDatabase::generateGuid();
    if (hasOld && sameImport(old, result)){
        // the hand edits a re-import keeps (spec 4.5)
        std::vector<std::string> names;
        std::vector<float> lengths;
        for (const VatClip& clip : data.clips){
            names.push_back(clip.name);
            lengths.push_back(clip.length());
        }
        const std::vector<int> match = matchClips(old.clips, names, lengths);
        std::set<std::string> used;
        for (size_t i = 0; i < data.clips.size(); i++){
            VatClip& clip = data.clips[i];
            if (match[i] < 0 || (size_t)match[i] >= old.clips.size()){
                used.insert(clip.name);
                continue;
            }
            const VatClip& previous = old.clips[(size_t)match[i]];
            if (!used.count(previous.name)) clip.name = previous.name;
            clip.events = previous.events;
            clip.speed = previous.speed;
            if (!clip.restNone) clip.interpolate = previous.interpolate;
            if ((previous.wrap == ClipWrapMode::Loop) == (clip.wrap == ClipWrapMode::Loop)) clip.wrap = previous.wrap;
            used.insert(clip.name);
        }
        data.unknownKeys = old.unknownKeys;
    }

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

    const uint32_t sceneId = project->getSelectedSceneId();
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene){
        error = "no open scene to place the result in";
        return false;
    }
    if (sceneProject->playState != ScenePlayState::STOPPED){
        error = "the scene is playing: stop play mode to place the result";
        return false;
    }
    VatPlacement placement;
    placement.mode = options.place == VatPlace::Crowd ? VatPlacement::Mode::Crowd : VatPlacement::Mode::Beside;
    placement.name = fileSafe(detection.assetName) + " (VAT)";
    placement.model = ClipConverters::assetRelative(project, FileUtils::pathFromUtf8(encoded.meshPath));
    placement.animation = ClipConverters::assetRelative(project, absolute);
    placement.guid = data.guid;
    placement.crowd = options.crowd;
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

}
