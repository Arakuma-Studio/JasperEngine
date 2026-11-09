// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PixelPresetCreate.h"

#include "Project.h"
#include "particles/ParticleEffectCodec.h"
#include "pixelart/PaletteAssets.h"
#include "pixelart/particles/PixelParticlePresets.h"
#include "util/FileUtils.h"

#include <filesystem>
#include <functional>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;

bool editor::pixelart::prepareProjectPixelPreset(Project& project, const std::string& name, const std::string& childDirectory,
                                                 ProjectPixelPreset& out, std::string& error){
    out = ProjectPixelPreset();
    if (pixelParticlePresetText(name).empty()){
        error = "no pixel preset '" + name + "'";
        return false;
    }
    // the project's copy of the built-in palette, written with the preset when there is none
    ResolvedPalette palette = resolvePaletteName(project, kPixelPresetPalette);
    if (!palette.ok){
        error = palette.error.message;
        return false;
    }
    if (palette.needsCopy)
        out.writes.push_back({project.getProjectPath() / FileUtils::pathFromUtf8(palette.path), palette.copyText});

    PixelPresetBuild build;
    build.pixelsPerUnit = std::max(project.getPixelsPerUnit(), 1);
    build.palette = palette.assetPath;
    std::set<std::string> written;
    std::function<std::string(const std::string&)> childPath;
    std::string childError;
    childPath = [&](const std::string& child) -> std::string {
        const std::string path = (childDirectory.empty() ? std::string() : childDirectory + "/") + child + ".particles";
        if (written.insert(path).second){
            out.children.push_back(path);
            const std::filesystem::path absolute = project.getAssetsPath() / FileUtils::pathFromUtf8(path);
            std::error_code ec;
            // a child already in the project is the user's: kept as it is
            if (!std::filesystem::exists(absolute, ec)){
                PixelPresetBuild childBuild = build;
                childBuild.childPath = childPath;
                particles::ParticleEffectDocument document;
                if (!buildPixelParticlePreset(child, childBuild, document, childError))
                    return path;
                out.writes.push_back({absolute, particles::ParticleEffectCodec::write(document)});
            }
        }
        return path;
    };
    build.childPath = childPath;
    if (!buildPixelParticlePreset(name, build, out.document, error))
        return false;
    if (!childError.empty()){
        error = childError;
        return false;
    }
    return true;
}
