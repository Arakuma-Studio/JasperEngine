// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "pixelart/PixelArtPreset.h"

#include "pixelart/PixelArtInput.h"
#include "pixelart/PixelArtRules.h"

#include <algorithm>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::pixelart;

namespace {

    // project settings, then scenes and their GameObjects, then textures, each by target
    int kindOrder(const FixChange& change){
        if (change.target == "project")
            return 0;
        if (change.target.find('#') != std::string::npos || change.field == "camera")
            return change.field == "camera" || change.field.rfind("pixelperfectcamera", 0) == 0 ? 1 : 3;
        return 2;
    }

}

const std::vector<PixelArtScreen>& jasperengine::editor::pixelart::presetBaseResolutions(){
    static const std::vector<PixelArtScreen> sizes = {{256, 144}, {320, 180}, {384, 216}, {480, 270}, {640, 360}};
    return sizes;
}

std::vector<FixChange> jasperengine::editor::pixelart::planPixelArtPreset(const PixelArtCheckInput& input, const PixelArtPresetOptions& options){
    std::vector<FixChange> out = rules::projectPresetChanges(input.project, options.baseWidth, options.baseHeight, options.scaling,
                                                              std::clamp(options.pixelsPerUnit, 1, 256), std::clamp(options.windowScale, 2, 8));
    std::vector<std::string> selected;
    if (options.addCameras) selected.push_back("integer_scale");
    if (options.convertTextures) selected.push_back("mipmaps_compression");
    if (options.pointReferences) selected.push_back("filtering");
    if (!selected.empty()){
        const CheckResult result = run(input, selected);
        std::vector<FixChange> others;
        for (const Finding& finding : result.findings){
            if (finding.options.empty())
                continue;
            // the preset adds cameras; it leaves a Stretch Fill camera to its owner
            if (finding.rule == "integer_scale" && finding.options.front().changes.front().field == "pixelperfectcamera.upscaleMode")
                continue;
            const std::vector<FixChange>& changes = finding.options.front().changes;
            others.insert(others.end(), changes.begin(), changes.end());
        }
        std::stable_sort(others.begin(), others.end(), [](const FixChange& a, const FixChange& b){
            const int ka = kindOrder(a), kb = kindOrder(b);
            if (ka != kb) return ka < kb;
            if (a.target != b.target) return a.target < b.target;
            return a.field < b.field;
        });
        out.insert(out.end(), others.begin(), others.end());
    }
    return out;
}

std::vector<FixChange> jasperengine::editor::pixelart::planPixelArtPreset(Project& project, const PixelArtPresetOptions& options){
    Scope scope;
    scope.assumeModeOn = true;
    std::vector<PixelProblem> errors;
    const PixelArtCheckInput input = inputFromProject(project, scope, errors);
    return planPixelArtPreset(input, options);
}

PresetCounts jasperengine::editor::pixelart::countPresetChanges(const std::vector<FixChange>& plan){
    PresetCounts counts;
    std::set<std::string> scenes, textures, references;
    for (const FixChange& change : plan){
        const int kind = kindOrder(change);
        if (kind == 0) counts.settings++;
        else if (kind == 1) scenes.insert(change.target.substr(0, change.target.find('#')));
        else if (kind == 2) textures.insert(change.target);
        else references.insert(change.target + ":" + change.field);
    }
    counts.scenes = (int)scenes.size();
    counts.textures = (int)textures.size();
    counts.references = (int)references.size();
    return counts;
}

bool jasperengine::editor::pixelart::applyPixelArtPreset(Project& project, const std::vector<FixChange>& plan, FixApplyReport& report,
                                                         std::string& error){
    return applyChanges(project, plan, "Apply Pixel Art Preset", report, error);
}
