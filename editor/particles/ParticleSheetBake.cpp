// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ParticleSheetBake.h"

#include "json.hpp"

#include "Project.h"
#include "particles/ParticleEffectCodec.h"
#include "util/FileUtils.h"

#include <cmath>
#include <algorithm>
#include <cstdio>
#include <filesystem>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    // a path the bake may write: inside the project's assets, new unless overwrite
    bool sheetTarget(Project* project, const std::string& relative, bool overwrite, fs::path& out, std::string& error){
        if (relative.empty() || fs::path(relative).is_absolute() || relative.find("..") != std::string::npos){
            error = "\"" + relative + "\" is not a project-relative path inside assets.";
            return false;
        }
        out = (project->getProjectPath() / FileUtils::pathFromUtf8(relative)).lexically_normal();
        if (!project->isInsideAssetsPath(out.string())){
            error = "\"" + relative + "\" is outside the assets folder (e.g. assets/fx/smoke_sheet).";
            return false;
        }
        std::error_code ec;
        if (!overwrite && fs::exists(out, ec)){
            error = "\"" + relative + "\" exists: pass overwrite (one undo step puts it back) or choose another output.";
            return false;
        }
        return true;
    }

    bool pngItem(const PoseImage& image, const fs::path& path, std::vector<WriteFilesCmd::Item>& items, std::string& error){
        std::vector<uint8_t> png;
        if (!image.encodePng(png)){
            error = "the PNG could not be encoded.";
            return false;
        }
        items.push_back({path, std::string(png.begin(), png.end())});
        return true;
    }

}

ParticlePreviewRequest ParticleSheetBake::sheetRequest(ParticlePreviewRequest base, int columns, int rows, int cellSize){
    base.frames = columns * rows;
    base.times.clear();
    base.transparentCells = true;
    base.labels = false;
    base.overdrawMap = false;
    base.cellSize = cellSize;
    base.reference = PoseImage();
    return base;
}

std::string ParticleSheetBake::validate(int columns, int rows, int cellSize){
    if (columns < 1 || columns > MAX_TILES || rows < 1 || rows > MAX_TILES) return "columns and rows are 1 to " + std::to_string(MAX_TILES) + ".";
    if (cellSize < 16 || cellSize > 1024) return "cell_size is 16 to 1024 pixels.";
    if (columns * cellSize > MAX_SHEET || rows * cellSize > MAX_SHEET) return "the sheet is over " + std::to_string(MAX_SHEET) + " pixels: fewer tiles or a smaller cell_size.";
    if (columns * rows > ParticleEffectPreview::MAX_FRAMES) return "at most " + std::to_string(ParticleEffectPreview::MAX_FRAMES) + " frames (columns x rows).";
    return std::string();
}

PoseImage ParticleSheetBake::composeSheet(const std::vector<PoseImage>& cells, int columns, int rows){
    if (cells.empty()) return PoseImage();
    const int w = cells.front().getWidth(), h = cells.front().getHeight();
    PoseImage sheet(columns * w, rows * h, PoseColor(0, 0, 0, 0));
    for (size_t k = 0; k < cells.size() && (int)k < columns * rows; k++){
        sheet.blit(cells[k], (int)(k % columns) * w, (int)(k / columns) * h);
    }
    return sheet;
}

particles::ParticleEffectDocument ParticleSheetBake::sheetEffect(const std::string& texture, int columns, int rows, double seconds, float worldSize,
                                                                const Vector3& center, bool looping, bool additive){
    particles::ParticleEffectDocument d;
    const float length = (float)std::max(seconds, 0.01);
    d.duration = length;
    d.looping = looping;
    d.maxParticles = 2;
    d.main.startLifetime = particles::MinMaxCurve(length);
    d.main.startSpeed = particles::MinMaxCurve(0.0f);
    d.main.startSize = particles::MinMaxCurve(worldSize);
    d.emission.rateOverTime = particles::MinMaxCurve(0.0f);
    particles::Burst burst;
    burst.time = 0.0f;
    burst.count = particles::MinMaxCurve(1.0f);
    burst.cycles = 1;
    d.emission.bursts.push_back(burst);
    // born where the run was framed, so the flipbook sits where the effect played
    d.shape.enabled = true;
    d.shape.shape = particles::ShapeType::Sphere;
    d.shape.radius = 0.0f;
    d.shape.position = center;
    d.textureSheetAnimation.present = true;
    d.textureSheetAnimation.enabled = true;
    d.textureSheetAnimation.tilesX = columns;
    d.textureSheetAnimation.tilesY = rows;
    d.renderer.texture = texture;
    d.renderer.blend = additive ? particles::ParticleBlend::Additive : particles::ParticleBlend::Alpha;
    return d;
}

bool ParticleSheetBake::sheetFiles(Project* project, const std::string& output, const PoseImage& sheet, const particles::ParticleEffectDocument& effect,
                                   bool overwrite, std::vector<WriteFilesCmd::Item>& items, std::vector<std::string>& written, std::string& error){
    fs::path png, particlesFile;
    if (!sheetTarget(project, output + ".png", overwrite, png, error)) return false;
    if (!sheetTarget(project, output + ".particles", overwrite, particlesFile, error)) return false;
    if (!pngItem(sheet, png, items, error)) return false;
    particles::ParticleEffectDocument document = effect;
    // the effect names its texture as the components do: assets-relative
    document.renderer.texture = project->normalizeToAssetsRelative(png).generic_string();
    items.push_back({particlesFile, particles::ParticleEffectCodec::write(document)});
    written.push_back(output + ".png");
    written.push_back(output + ".particles");
    return true;
}

std::string ParticleSheetBake::asepriteJson(const std::string& imageName, int columns, int rows, int cellWidth, int cellHeight, double seconds){
    using Ordered = nlohmann::ordered_json;
    const int n = columns * rows;
    std::string stem = imageName;
    const size_t dot = stem.rfind('.');
    if (dot != std::string::npos) stem.resize(dot);
    // every frame its equal part of the run (the cells are the middles of those parts)
    const int duration = std::max(1, (int)std::lround(seconds * 1000.0 / std::max(n, 1)));
    Ordered list = Ordered::object();
    for (int f = 0; f < n; f++){
        Ordered frame = Ordered::object();
        frame["frame"] = {{"x", (f % columns) * cellWidth}, {"y", (f / columns) * cellHeight}, {"w", cellWidth}, {"h", cellHeight}};
        frame["rotated"] = false;
        frame["trimmed"] = false;
        frame["spriteSourceSize"] = {{"x", 0}, {"y", 0}, {"w", cellWidth}, {"h", cellHeight}};
        frame["sourceSize"] = {{"w", cellWidth}, {"h", cellHeight}};
        frame["duration"] = duration;
        list[stem + " " + std::to_string(f) + ".aseprite"] = frame;
    }
    Ordered json = Ordered::object();
    json["frames"] = list;
    json["meta"] = {{"app", "JasperEngine (Bake Sprite Sheet)"}, {"version", "1"}, {"image", imageName}, {"format", "RGBA8888"},
                    {"size", {{"w", columns * cellWidth}, {"h", rows * cellHeight}}}, {"scale", "1"}, {"frameTags", Ordered::array()},
                    {"layers", Ordered::array()}, {"slices", Ordered::array()}};
    return json.dump(1, ' ') + "\n";
}

bool ParticleSheetBake::asepriteFile(Project* project, const std::string& output, int columns, int rows, int cellWidth, int cellHeight, double seconds,
                                     bool overwrite, std::vector<WriteFilesCmd::Item>& items, std::vector<std::string>& written, std::string& error){
    fs::path json;
    if (!sheetTarget(project, output + ".json", overwrite, json, error)) return false;
    items.push_back({json, asepriteJson(fs::path(output).filename().generic_string() + ".png", columns, rows, cellWidth, cellHeight, seconds)});
    written.push_back(output + ".json");
    return true;
}

bool ParticleSheetBake::sequenceFiles(Project* project, const std::string& folder, const std::string& name, const std::vector<PoseImage>& frames,
                                      bool overwrite, std::vector<WriteFilesCmd::Item>& items, std::vector<std::string>& written, std::string& error){
    for (size_t k = 0; k < frames.size(); k++){
        char file[32];
        std::snprintf(file, sizeof(file), "_%03u.png", (unsigned)k);
        const std::string relative = folder + "/" + name + file;
        fs::path path;
        if (!sheetTarget(project, relative, overwrite, path, error)) return false;
        if (!pngItem(frames[k], path, items, error)) return false;
        written.push_back(relative);
    }
    return true;
}
