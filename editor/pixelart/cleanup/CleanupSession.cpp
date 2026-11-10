// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CleanupSession.h"

#include "Project.h"
#include "Scene.h"
#include "asset/AssetMeta.h"
#include "asset/AssetMap.h"
#include "asset/AssetTypes.h"
#include "pixelart/PaletteAssets.h"
#include "pixelart/PaletteFormats.h"
#include "pixelart/PixelArtCheck.h"
#include "pixelart/PixelArtSettings.h"
#include "pixelart/aseprite/AsepriteDocument.h"
#include "pixelart/aseprite/AsepriteImporter.h"
#include "pixelart/cleanup/CleanupGrid.h"
#include "pixelart/cleanup/CleanupSample.h"
#include "pixelart/cleanup/CleanupSheetOutput.h"
#include "spritenormals/SpriteMedia.h"
#include "texsynth/SynthesisRecipe.h"
#include "util/AssetDatabase.h"
#include "util/FileUtils.h"
#include "util/ProjectChecks.h"
#include "util/ProjectFileWrites.h"
#include "util/SafeWrite.h"
#include "util/Sha256.h"

#include "jobs/JobSystem.h"

#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <system_error>

#ifndef JASPERENGINE_EDITOR_VERSION
#define JASPERENGINE_EDITOR_VERSION "dev"
#endif

namespace jasperengine::editor::pixelart::cleanup {

    namespace fs = std::filesystem;
    using jasperengine::pixelart::Palette;
    using nlohmann::json;

    namespace {

        constexpr const char* kRecipeChunk = "jasper.pixelcleanup";
        constexpr PixelRgba kMagenta{255, 0, 255, 255};
        constexpr PixelRgba kGridLine{0, 255, 255, 255};
        constexpr PixelRgba kFrameBox{255, 216, 0, 255};
        constexpr PixelRgba kLabelBack{0, 0, 0, 255};
        constexpr PixelRgba kLabelFront{255, 255, 255, 255};
        constexpr PixelRgba kPane{32, 32, 40, 255};

        // the FAILs a fix removes (R29): a native picture with only these is still `fixable`
        bool removableLint(const std::string& id) {
            return id == "off_palette" || id == "partial_alpha" || id == "transparent_background" || id == "colors";
        }

        std::string sessionExtension(const fs::path& path) {
            std::string extension = FileUtils::pathToUtf8(path.extension());
            std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            return extension;
        }

        bool isAsepritePath(const fs::path& path) {
            const std::string extension = sessionExtension(path);
            return extension == ".aseprite" || extension == ".ase";
        }

        bool readWholeFile(const fs::path& path, std::string& out) {
            std::ifstream in(path, std::ios::binary);
            if (!in) return false;
            out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            return true;
        }

        std::string sha256OfFile(const fs::path& path) {
            std::string bytes;
            if (!readWholeFile(path, bytes)) return {};
            Sha256 hash;
            hash.update(bytes);
            return hash.finishHex();
        }

        std::string sha256OfFrames(const std::vector<PixelImage>& frames) {
            Sha256 hash;
            for (const PixelImage& frame : frames) {
                const auto pixels = frame.pixels();
                hash.update(pixels.data(), pixels.size());
            }
            return hash.finishHex();
        }

        // What a request needs from a project: a live one, or the CLI's project.yaml
        struct ProjectView {
            Project* project = nullptr;
            fs::path dir;
            fs::path assets;
            std::string defaultPalette;
            PixelArtSettings settings;
            bool hasSettings = false;
        };

        ProjectView projectView(Project* project, const fs::path& given) {
            ProjectView view;
            view.project = project;
            if (project && !project->getProjectPath().empty()) {
                view.dir = project->getProjectPath();
                view.assets = spritenormals::outputsRoot(project);
                view.defaultPalette = defaultPalettePath(*project);
                view.settings = project->getPixelArtSettings();
                view.hasSettings = true;
                return view;
            }
            if (given.empty()) return view;
            view.dir = given;
            const fs::path assets = ProjectChecks::assetsRootFromProjectFile(given);
            view.assets = assets.empty() ? given : assets;
            try {
                const YAML::Node node = YAML::LoadFile(FileUtils::pathToUtf8(given / "project.yaml"));
                const YAML::Node palettes = node["palettes"];
                if (palettes.IsDefined() && palettes.IsMap() && palettes["default"].IsDefined() && palettes["default"].IsScalar()) {
                    view.defaultPalette = palettes["default"].as<std::string>();
                }
                const YAML::Node pixelArt = node["pixelArt"];
                if (pixelArt.IsDefined() && pixelArt.IsMap()) decodePixelArtSettings(pixelArt, view.settings);
                view.hasSettings = true;
            } catch (const YAML::Exception&) {
                // a project.yaml that does not parse has no settings to take: the defaults stand
            }
            return view;
        }

        // An S43 palette as the recipe holds it: opaque colours, its transparent entry left out
        RecipePalette recipePaletteOf(const Palette& palette) {
            RecipePalette out;
            out.name = palette.name.empty() ? std::string("Palette") : palette.name;
            std::vector<int> remap(palette.colors.size(), -1);
            for (int i = 0; i < (int)palette.colors.size(); i++) {
                if (i == palette.transparent || palette.colors[i].a == 0) continue;
                Rgba8 color = palette.colors[i];
                color.a = 255;
                remap[i] = (int)out.colors.size();
                out.colors.push_back(color);
            }
            for (uint16_t locked : palette.locked) {
                if (locked < remap.size() && remap[locked] >= 0) out.locked.push_back(remap[locked]);
            }
            return out;
        }

        bool readPaletteFile(const fs::path& file, Palette& out) {
            std::string text;
            if (!readWholeFile(file, text)) return false;
            std::vector<jasperengine::pixelart::Diagnostic> diagnostics;
            return jasperengine::pixelart::PaletteCodec::read(text, out, diagnostics);
        }

        // Fills an unresolved palette reference (a path or GUID) with the asset's colours, keeping
        // the reference: the recipe stores both, so a re-run without the asset still works (R28)
        bool resolveAssetPalette(const ProjectView& view, RecipePalette& palette, PixelProblem& problem) {
            const std::string fix = "name a .palette asset of the project, a built-in preset (sweetie-16, ...), or a list of \"#rrggbb\"";
            std::string name = palette.assetPath;
            if (name.empty() && !palette.assetGuid.empty()) {
                if (view.project) AssetMap::findPath(Guid::parse(palette.assetGuid), name);
                if (name.empty()) {
                    problem = {"palette_missing", "palette", "no palette asset with GUID " + palette.assetGuid, fix};
                    return false;
                }
            }
            if (view.project) {
                ResolvedPalette resolved = resolvePaletteName(*view.project, name);
                if (!resolved.ok) {
                    problem = {"palette_missing", "palette", resolved.error.message.empty() ? "palette " + name + " not found" : resolved.error.message, resolved.error.fix.empty() ? fix : resolved.error.fix};
                    return false;
                }
                RecipePalette found = recipePaletteOf(resolved.palette);
                palette.name = found.name;
                palette.colors = std::move(found.colors);
                if (palette.locked.empty()) palette.locked = std::move(found.locked);
                palette.assetPath = resolved.path;
                if (palette.assetGuid.empty() && !resolved.path.empty()) {
                    const Guid guid = AssetMap::findGuid(resolved.assetPath);
                    if (guid) palette.assetGuid = guid.str();
                }
                return true;
            }
            std::vector<fs::path> candidates = {fs::u8path(name)};
            if (!view.dir.empty()) {
                candidates.push_back(view.dir / fs::u8path(name));
                if (!view.assets.empty()) candidates.push_back(view.assets / fs::u8path(name));
            }
            std::error_code ec;
            for (const fs::path& candidate : candidates) {
                Palette loaded;
                if (!fs::is_regular_file(candidate, ec) || !readPaletteFile(candidate, loaded)) continue;
                RecipePalette found = recipePaletteOf(loaded);
                palette.name = found.name;
                palette.colors = std::move(found.colors);
                if (palette.locked.empty()) palette.locked = std::move(found.locked);
                return true;
            }
            problem = {"palette_missing", "palette", "palette " + name + " not found", fix};
            return false;
        }

        // R9: an absent palette takes the project's Default Palette, else auto; a named asset is
        // resolved to its colours. The recipe then needs nothing else to re-run.
        bool resolveRecipePalette(const ProjectView& view, Recipe& recipe, PixelProblem& problem) {
            if (recipe.paletteMode == PaletteMode::Default && !recipe.palette) {
                if (view.defaultPalette.empty()) {
                    recipe.paletteMode = PaletteMode::Auto;
                    if (!recipe.autoPalette) recipe.autoPalette = AutoPalette{};
                    return true;
                }
                RecipePalette palette;
                palette.assetPath = view.defaultPalette;
                recipe.palette = palette;
                recipe.paletteMode = PaletteMode::Fixed;
            }
            if (recipe.paletteMode == PaletteMode::Fixed && recipe.palette && recipe.palette->colors.empty()) {
                return resolveAssetPalette(view, *recipe.palette, problem);
            }
            return true;
        }

        void defaultPaletteWithoutProject(Recipe& recipe) {
            if (recipe.paletteMode == PaletteMode::Default && !recipe.palette) {
                recipe.paletteMode = PaletteMode::Auto;
                if (!recipe.autoPalette) recipe.autoPalette = AutoPalette{};
            }
        }

        // The backdrops `contrast` measures on (R26): given, else the project setting, else the
        // main scene's 2D camera clear colour, else none (the lint then SKIPs)
        void resolveBackdrops(const ProjectView& view, const std::vector<std::string>& given, std::vector<Rgba8>& out, std::string& source) {
            out.clear();
            const auto parse = [&](const std::vector<std::string>& texts) {
                for (const std::string& text : texts) {
                    Rgba8 color;
                    if (jasperengine::pixelart::parseHex(text, color)) out.push_back(color);
                }
            };
            if (!given.empty()) {
                parse(given);
                source = "options.backdrops";
                return;
            }
            if (view.hasSettings && !view.settings.backdrops.empty()) {
                parse(view.settings.backdrops);
                source = "project.pixel_art.backdrops";
                return;
            }
            if (view.project) {
                const SceneProject* main = view.project->getScene(view.project->getStartSceneId());
                if (main && main->scene && main->sceneType == SceneType::SCENE_2D) {
                    const Vector4 background = main->scene->getBackgroundColor();
                    const auto toByte = [](float v) { return (uint8_t)std::clamp((int)std::lround(v * 255.0f), 0, 255); };
                    out.push_back(Rgba8{toByte(background.x), toByte(background.y), toByte(background.z), 255});
                    source = "scene.camera_clear_color";
                }
            }
        }

        // ---- reading sources

        bool sourceToImage(const spritenormals::Image& color, PixelImage& out) {
            if (color.channels != 4 || color.bitDepth != 8 || color.empty()) return false;
            out = PixelImage(color.width, color.height);
            const auto pixels = out.editPixels();
            std::memcpy(pixels.data(), color.data.data(), pixels.size());
            return true;
        }

        PixelProblem tooLarge(const std::string& message) {
            return {"image_too_large", "source", message, "the limit is " + std::to_string(kMaxSourceSide) + " x " + std::to_string(kMaxSourceSide) + " and " + std::to_string(kMaxSourceFrames) + " frames: resize it or split it"};
        }

        PixelProblem sourceProblem(const std::string& given, const std::vector<PixelProblem>& reasons) {
            for (const PixelProblem& reason : reasons) {
                if (reason.kind == "too_large") return tooLarge(reason.message);
            }
            const std::string why = reasons.empty() ? std::string("it could not be read") : reasons.front().message;
            return {"source_missing", "source", given + ": " + why, "give an image (PNG, JPEG, BMP, TGA, PSD, GIF, APNG) or an .aseprite file inside the project"};
        }

        bool loadAsepriteFrames(const fs::path& path, const std::string& given, std::optional<int> frame, const std::string& layer, Prepared& out, std::vector<PixelProblem>& problems) {
            aseprite::ReadOptions options;
            options.provenance = false;
            aseprite::ReadResult read = aseprite::readFile(path, options);
            if (!read.ok()) {
                problems.push_back({"source_missing", "source", given + ": " + read.error.message, read.error.fix.empty() ? "give a readable .aseprite file" : read.error.fix});
                return false;
            }
            const aseprite::Document& document = *read.document;
            if (document.width > kMaxSourceSide || document.height > kMaxSourceSide || document.frameCount() > kMaxSourceFrames) {
                problems.push_back(tooLarge(given + " is " + std::to_string(document.width) + " x " + std::to_string(document.height) + " with " + std::to_string(document.frameCount()) + " frames"));
                return false;
            }
            aseprite::RenderOptions render;
            if (!layer.empty()) {
                render.soloLayer = document.findLayer(layer);
                if (render.soloLayer < 0) {
                    std::string names;
                    for (const aseprite::Layer& entry : document.layers) names += (names.empty() ? "" : ", ") + entry.name;
                    problems.push_back({"bad_recipe", "source.layer", "no layer \"" + layer + "\" in " + given, "layers: " + names});
                    return false;
                }
            }
            if (frame) {
                if (*frame < 1 || *frame > document.frameCount()) {
                    problems.push_back({"bad_recipe", "source.frame", "frame " + std::to_string(*frame) + " is not in " + given, "frames are 1-" + std::to_string(document.frameCount())});
                    return false;
                }
                out.frames.push_back(aseprite::renderFrame(document, *frame - 1, render));
                return true;
            }
            for (int i = 0; i < document.frameCount(); i++) {
                out.frames.push_back(aseprite::renderFrame(document, i, render));
                out.delaysMs.push_back(document.frameDurations[i]);
            }
            if (out.frames.size() < 2) {
                out.delaysMs.clear();
                return true;
            }
            // R22: one animation per tag when the tags partition the frames
            std::set<int> seen;
            bool partition = !document.tags.empty();
            for (const aseprite::Tag& tag : document.tags) {
                SheetAnimation animation = animationOf(tag.name, {});
                if (!isAnimationName(tag.name)) partition = false;
                for (int i = tag.from; i <= tag.to; i++) {
                    if (i < 0 || i >= (int)out.frames.size() || !seen.insert(i).second) partition = false;
                    animation.frames.push_back(i);
                }
                animation.loop = tag.direction == aseprite::TagDirection::PingPong ? Loop::PingPong : Loop::Forward;
                out.tagAnimations.push_back(std::move(animation));
            }
            if (!partition || seen.size() != out.frames.size()) out.tagAnimations.clear();
            return true;
        }

        bool loadSourceFrames(Prepared& out, std::vector<PixelProblem>& problems) {
            const fs::path& path = out.sourceAbsolute;
            std::error_code ec;
            if (!fs::is_regular_file(path, ec)) {
                problems.push_back({"source_missing", "source", "no file " + out.sourcePath, "give a path to an existing image or .aseprite file"});
                return false;
            }
            out.sourceSha256 = sha256OfFile(path);
            out.animationName = FileUtils::pathToUtf8(path.stem());
            if (isAsepritePath(path)) return loadAsepriteFrames(path, out.sourcePath, out.spec.frame, out.spec.layer, out, problems);
            spritenormals::Source source;
            std::vector<PixelProblem> reasons;
            if (!spritenormals::loadSource(path, source, reasons)) {
                problems.push_back(sourceProblem(out.sourcePath, reasons));
                return false;
            }
            if (source.fileWidth > kMaxSourceSide || source.fileHeight > kMaxSourceSide) {
                problems.push_back(tooLarge(out.sourcePath + " is " + std::to_string(source.fileWidth) + " x " + std::to_string(source.fileHeight)));
                return false;
            }
            PixelImage image;
            if (!sourceToImage(source.color, image)) {
                problems.push_back(sourceProblem(out.sourcePath, {}));
                return false;
            }
            const bool animated = source.kind != spritenormals::SourceKind::Image && source.frameCount > 1;
            if (!animated) {
                out.frames.push_back(std::move(image));
                return true;
            }
            if (source.frameCount > kMaxSourceFrames) {
                problems.push_back(tooLarge(out.sourcePath + " has " + std::to_string(source.frameCount) + " frames"));
                return false;
            }
            for (int i = 0; i < source.frameCount; i++) {
                const spritenormals::Cell& cell = source.cells[i];
                out.frames.push_back(image.crop({cell.x, cell.y, cell.width, cell.height}));
                out.delaysMs.push_back(source.delays[i].playbackMs());
            }
            return true;
        }

        // A Sprite GameObject's texture and, when it has more than one, its frames (R1)
        bool loadSpriteFrames(Project* project, const Spec& spec, Prepared& out, std::vector<PixelProblem>& problems) {
            SceneProject* scene = project ? project->getScene(spec.sceneId) : nullptr;
            if (!scene || !scene->scene) {
                problems.push_back({"source_missing", "entity", "the Sprite's scene is not open", "open the scene that holds the Sprite, or give a source path"});
                return false;
            }
            std::string texture, error;
            std::vector<spritenormals::Cell> cells;
            if (!spritenormals::framesFromSprite(scene->scene, spec.entity, 0, texture, cells, error)) {
                problems.push_back({"source_missing", "entity", error, "name a Sprite GameObject with a texture"});
                return false;
            }
            if (!spritenormals::resolveProjectFile(project->getProjectPath(), texture, out.sourceAbsolute, error)) {
                problems.push_back({"source_missing", "entity", error, "give the Sprite a texture inside the project"});
                return false;
            }
            out.sourcePath = texture;
            std::error_code ec;
            if (!fs::is_regular_file(out.sourceAbsolute, ec)) {
                problems.push_back({"source_missing", "entity", "no file " + texture, "give the Sprite a texture that exists"});
                return false;
            }
            out.sourceSha256 = sha256OfFile(out.sourceAbsolute);
            spritenormals::Source source;
            std::vector<PixelProblem> reasons;
            if (!spritenormals::loadSource(out.sourceAbsolute, source, reasons)) {
                problems.push_back(sourceProblem(texture, reasons));
                return false;
            }
            PixelImage image;
            if (!sourceToImage(source.color, image)) {
                problems.push_back(sourceProblem(texture, {}));
                return false;
            }
            out.animationName = FileUtils::pathToUtf8(fs::u8path(texture).stem());
            if (cells.size() < 2) {
                out.frames.push_back(std::move(image));
                return true;
            }
            if ((int)cells.size() > kMaxSourceFrames) {
                problems.push_back(tooLarge(texture + " has " + std::to_string(cells.size()) + " frames"));
                return false;
            }
            for (const spritenormals::Cell& cell : cells) out.frames.push_back(image.crop({cell.x, cell.y, cell.width, cell.height}));
            return true;
        }

        bool resolveInView(const ProjectView& view, const std::string& given, fs::path& absolute, std::string& shown, PixelProblem& problem) {
            shown = given;
            if (view.dir.empty()) {
                absolute = fs::u8path(given);
                return true;
            }
            std::string error;
            if (!spritenormals::resolveProjectFile(view.dir, given, absolute, error)) {
                problem = {"source_missing", "source", error, "give a project-relative path inside the project"};
                return false;
            }
            shown = spritenormals::projectRelative(view.dir, absolute);
            return true;
        }

        // ---- outputs (R31)

        // A sheet's JSON written by a cleanup: it carries meta.jasperengine.recipe (R31)
        bool atlasCarriesRecipe(const fs::path& file) {
            std::string text;
            if (!readWholeFile(file, text)) return false;
            try {
                const nlohmann::json json = nlohmann::json::parse(text);
                return json.contains("meta") && json["meta"].contains("jasperengine") && json["meta"]["jasperengine"].contains("recipe");
            } catch (const nlohmann::json::exception&) {
                return false;
            }
        }

        struct PlannedPath {
            std::string role;       // "png", "aseprite", "json", "meta"
            std::string path;       // project-relative, or as given
            fs::path absolute;
        };

        std::string joinPath(const std::string& directory, const std::string& name) {
            return directory.empty() ? name : directory + "/" + name;
        }

        std::string parentOf(const std::string& path) {
            const size_t slash = path.find_last_of('/');
            return slash == std::string::npos ? std::string() : path.substr(0, slash);
        }

        std::string stemOf(const std::string& path) {
            const size_t slash = path.find_last_of('/');
            const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
            const size_t dot = name.find_last_of('.');
            return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
        }

        fs::path absoluteFor(const fs::path& projectDir, const std::string& relative) {
            const fs::path path = fs::u8path(relative);
            return projectDir.empty() || path.is_absolute() ? path : projectDir / path;
        }

        // The planned output files: their roles and paths (no check made here)
        std::vector<PlannedPath> planPaths(const Prepared& prepared, OutputFormat format, bool sheet) {
            const OutputSpec& spec = prepared.spec.output;
            const std::string base = prepared.rerunOutput.empty() ? prepared.sourcePath : prepared.rerunOutput;
            const std::string dir = parentOf(base);
            const std::string stem = stemOf(prepared.sourcePath.empty() ? base : prepared.sourcePath);
            std::vector<PlannedPath> planned;
            const auto add = [&](const char* role, const std::string& path) {
                planned.push_back({role, path, absoluteFor(prepared.projectDir, path)});
            };
            if (format == OutputFormat::Aseprite) {
                std::string path = spec.aseprite;
                if (path.empty()) {
                    if (!prepared.rerunOutput.empty() && isAsepritePath(fs::u8path(prepared.rerunOutput))) {
                        path = prepared.rerunOutput;
                    } else if (sheet) {
                        path = joinPath(dir, stem + ".aseprite");
                        std::error_code ec;
                        if (fs::exists(absoluteFor(prepared.projectDir, path), ec)) path = joinPath(dir, stem + "_clean.aseprite");
                    } else {
                        path = joinPath(dir, stem + "_clean.aseprite");
                    }
                }
                add("aseprite", path);
                add("meta", FileUtils::pathToGenericUtf8(AssetTypes::metaPathFor(fs::u8path(path))));
                return planned;
            }
            std::string png = spec.png;
            if (png.empty()) png = !prepared.rerunOutput.empty() && !isAsepritePath(fs::u8path(prepared.rerunOutput)) ? prepared.rerunOutput : joinPath(dir, stem + "_clean.png");
            add("png", png);
            if (sheet) {
                const std::string json = spec.json.empty() ? joinPath(parentOf(png), stemOf(png) + ".json") : spec.json;
                add("json", json);
            }
            return planned;
        }

        // The format a run writes: the argument, an explicit path, the rerun's file, the recipe, else R23
        OutputFormat formatOf(const Prepared& prepared, bool sheet) {
            if (prepared.spec.format) return *prepared.spec.format;
            if (!prepared.spec.output.aseprite.empty()) return OutputFormat::Aseprite;
            if (!prepared.spec.output.png.empty()) return OutputFormat::Png;
            if (!prepared.rerunOutput.empty()) return isAsepritePath(fs::u8path(prepared.rerunOutput)) ? OutputFormat::Aseprite : OutputFormat::Png;
            if (prepared.recipe.format) return *prepared.recipe.format;
            return sheet ? OutputFormat::Aseprite : OutputFormat::Png;
        }

        bool samePath(const fs::path& a, const fs::path& b) {
            std::error_code ec;
            if (fs::exists(a, ec) && fs::exists(b, ec)) {
                if (fs::equivalent(a, b, ec)) return true;
            }
            return safewrite::resolvedPath(a) == safewrite::resolvedPath(b);
        }

        // ---- the sheet

        std::vector<PixelImage> commonCanvasFrames(const std::vector<AtlasClip>& clips) {
            int width = 0, height = 0;
            for (const AtlasClip& clip : clips) {
                for (const PixelArt& art : clip.frames) {
                    width = std::max(width, art.width);
                    height = std::max(height, art.height);
                }
            }
            std::vector<PixelImage> frames;
            for (const AtlasClip& clip : clips) {
                for (const PixelArt& art : clip.frames) {
                    PixelImage frame(width, height);
                    frame.blit(toRgba(art), 0, 0);
                    frames.push_back(std::move(frame));
                }
            }
            return frames;
        }

        void fillSheetResult(const Prepared& prepared, Result& result, const Recipe& recipe, const RgbaImage& image, AnimationOutcome&& animation) {
            result.report = reportOfSheet(image, recipe, animation, prepared.report);
            result.resolved = recipe;
            result.resolved.sheetMode = SheetMode::Given;
            result.resolved.sheet = animation.sheet;
            if (animation.palette) {
                result.resolved.paletteMode = PaletteMode::Fixed;
                result.resolved.palette = animation.palette;
                result.resolved.autoPalette.reset();
            }
            result.format = formatOf(prepared, true);
            const std::vector<AtlasClip> clips = atlasClips(animation);
            if (result.format == OutputFormat::Png) {
                if (!clips.empty()) result.frames.push_back(toRgba(pngSheet(clips).art));
            } else {
                result.frames = commonCanvasFrames(clips);
            }
            result.animation = std::move(animation);
        }

        // ---- the preview image: 3x5 labels, nearest scaling

        // A-Z from 0, digits from 26; '.', '|' and 'x' after them; rows top first, bit 2 = left column
        constexpr uint8_t kGlyphs[39][5] = {
            {2, 5, 7, 5, 5}, {6, 5, 6, 5, 6}, {3, 4, 4, 4, 3}, {6, 5, 5, 5, 6}, {7, 4, 6, 4, 7}, {7, 4, 6, 4, 4},
            {3, 4, 5, 5, 3}, {5, 5, 7, 5, 5}, {7, 2, 2, 2, 7}, {1, 1, 1, 5, 2}, {5, 5, 6, 5, 5}, {4, 4, 4, 4, 7},
            {5, 7, 7, 5, 5}, {6, 5, 5, 5, 5}, {2, 5, 5, 5, 2}, {6, 5, 6, 4, 4}, {2, 5, 5, 6, 3}, {6, 5, 6, 5, 5},
            {3, 4, 2, 1, 6}, {7, 2, 2, 2, 2}, {5, 5, 5, 5, 7}, {5, 5, 5, 5, 2}, {5, 5, 7, 7, 5}, {5, 5, 2, 5, 5},
            {5, 5, 2, 2, 2}, {7, 1, 2, 4, 7},
            // 0-9
            {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1},
            {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7},
            // '.', '|', ' '
            {0, 0, 0, 0, 2}, {2, 2, 2, 2, 2}, {0, 0, 0, 0, 0},
        };

        int glyphIndex(char c) {
            if (c >= 'A' && c <= 'Z') return c - 'A';
            if (c >= 'a' && c <= 'z') return c - 'a';
            if (c >= '0' && c <= '9') return 26 + (c - '0');
            if (c == '.') return 36;
            if (c == '|') return 37;
            return 38;
        }

        void fillBox(PixelImage& image, int x, int y, int w, int h, PixelRgba color) {
            for (int yy = std::max(0, y); yy < std::min(image.height(), y + h); yy++) {
                for (int xx = std::max(0, x); xx < std::min(image.width(), x + w); xx++) image.set(xx, yy, color);
            }
        }

        void drawLabel(PixelImage& image, const std::string& text, int x, int y, int scale = 2, PixelRgba back = kLabelBack, PixelRgba front = kLabelFront) {
            const int width = (int)text.size() * 4 * scale + scale, height = 7 * scale;
            fillBox(image, x, y, width, height, back);
            for (size_t i = 0; i < text.size(); i++) {
                const int glyph = glyphIndex(text[i]);
                for (int row = 0; row < 5; row++) {
                    for (int col = 0; col < 3; col++) {
                        if (!(kGlyphs[glyph][row] & (4 >> col))) continue;
                        fillBox(image, x + scale + (int)i * 4 * scale + col * scale, y + scale + row * scale, scale, scale, front);
                    }
                }
            }
        }

        void outlineBox(PixelImage& image, int x, int y, int w, int h, PixelRgba color, int thickness = 1) {
            fillBox(image, x, y, w, thickness, color);
            fillBox(image, x, y + h - thickness, w, thickness, color);
            fillBox(image, x, y, thickness, h, color);
            fillBox(image, x + w - thickness, y, thickness, h, color);
        }

        // Scales `src` by `scale` with nearest neighbour into a new w x h picture on `pane`
        PixelImage scaled(const PixelImage& src, double scale, int w, int h, PixelRgba pane = kPane) {
            PixelImage out(w, h);
            out.fill(pane);
            if (src.empty()) return out;
            for (int y = 0; y < h; y++) {
                const int sy = std::min((int)(y / scale), src.height() - 1);
                for (int x = 0; x < w; x++) {
                    const int sx = std::min((int)(x / scale), src.width() - 1);
                    const PixelRgba color = src.at(sx, sy);
                    if (color[3] > 0) out.set(x, y, color);
                }
            }
            return out;
        }

        std::string scaleText(double scale) {
            if (scale >= 1.0 && std::floor(scale) == scale) return std::to_string((int)scale) + "x";
            char text[16];
            std::snprintf(text, sizeof(text), "%.2fx", scale);
            return text;
        }

        // The factor showing a picture within `limit` (whole when it can grow, a fraction when it must shrink)
        double fitScale(int size, int limit) {
            if (size <= 0) return 1.0;
            if (size <= limit) return std::max(1, limit / size);
            return double(limit) / double(size);
        }

        // Grid lines over a picture drawn at `scale` (bent lines bent)
        void drawGrid(PixelImage& canvas, const Grid& grid, double scale, int dx, int dy, PixelRgba color) {
            const int w = canvas.width(), h = canvas.height();
            const auto line = [&](double from, double to, double at, bool vertical) {
                const int a = (int)std::round(from * scale), b = (int)std::round(to * scale), p = (int)std::round(at * scale);
                for (int i = std::min(a, b); i <= std::max(a, b); i++) {
                    if (vertical) {
                        if (p + dx >= 0 && p + dx < w && i + dy >= 0 && i + dy < h) canvas.set(p + dx, i + dy, color);
                    } else if (i + dx >= 0 && i + dx < w && p + dy >= 0 && p + dy < h) {
                        canvas.set(i + dx, p + dy, color);
                    }
                }
            };
            const auto bandEdges = [](const std::vector<double>& cuts, size_t band) {
                return std::pair<double, double>{cuts.empty() ? 0.0 : cuts[std::min(band, cuts.size() - 1)], cuts.empty() ? 0.0 : cuts[std::min(band + 1, cuts.size() - 1)]};
            };
            if (grid.bend) {
                for (size_t i = 0; i < grid.bend->xs.size(); i++) {
                    for (size_t j = 0; j + 1 < grid.ys.size(); j++) {
                        const auto [top, bottom] = bandEdges(grid.ys, j);
                        line(top, bottom, grid.bend->xs[i][std::min(j, grid.bend->xs[i].size() - 1)], true);
                    }
                }
                for (size_t i = 0; i < grid.bend->ys.size(); i++) {
                    for (size_t j = 0; j + 1 < grid.xs.size(); j++) {
                        const auto [left, right] = bandEdges(grid.xs, j);
                        line(left, right, grid.bend->ys[i][std::min(j, grid.bend->ys[i].size() - 1)], false);
                    }
                }
                return;
            }
            const double top = grid.ys.empty() ? 0.0 : grid.ys.front(), bottom = grid.ys.empty() ? 0.0 : grid.ys.back();
            const double left = grid.xs.empty() ? 0.0 : grid.xs.front(), right = grid.xs.empty() ? 0.0 : grid.xs.back();
            for (double x : grid.xs) line(top, bottom, x, true);
            for (double y : grid.ys) line(left, right, y, false);
        }

        // Source and result side by side, the result at a whole scale matched to the source's height
        PixelImage renderCompare(const Prepared& prepared, const Result& result, int maxWidth) {
            const PixelImage& source = prepared.frames.front();
            const PixelImage& fixed = result.frames.front();
            const int pane = (maxWidth - 4) / 2;
            double srcScale = fitScale(source.width(), pane);
            const int srcH = std::max(1, (int)std::round(source.height() * srcScale));
            srcScale = std::min(srcScale, double(srcH) / std::max(1, source.height()));
            const int srcW = std::min(pane, std::max(1, (int)std::round(source.width() * srcScale)));
            int resultScale = fixed.height() > 0 ? std::max(1, srcH / fixed.height()) : 1;
            if (fixed.width() * resultScale > pane) resultScale = std::max(1, pane / std::max(1, fixed.width()));
            const int resW = fixed.width() * resultScale, resH = fixed.height() * resultScale;
            const int width = srcW + 4 + std::min(pane, resW), height = std::max(srcH, resH);
            PixelImage canvas(std::max(width, 1), std::max(height, 1));
            canvas.fill(kPane);
            canvas.blit(scaled(source, srcScale, srcW, srcH), 0, 0);
            if (result.outcome) drawGrid(canvas, result.outcome->grid, srcScale, 0, 0, kGridLine);
            if (!fixed.empty()) canvas.blit(scaled(fixed, resultScale, resW, resH), srcW + 4, 0);
            drawLabel(canvas, "SOURCE " + scaleText(srcScale) + " | RESULT " + scaleText(resultScale), 0, std::max(0, height - 14));
            return canvas;
        }

        // A sheet: the source with each box labelled, then each clip as a strip
        PixelImage renderSheet(const Prepared& prepared, const Result& result, int maxWidth) {
            const AnimationOutcome& animation = *result.animation;
            const PixelImage& source = prepared.frames.front();
            const double srcScale = fitScale(source.width(), maxWidth);
            const int srcW = std::min(maxWidth, std::max(1, (int)std::round(source.width() * srcScale)));
            const int srcH = std::max(1, (int)std::round(source.height() * srcScale));
            const std::vector<AtlasClip> clips = atlasClips(animation);
            int stripScale = 1, strips = 0;
            for (const AtlasClip& clip : clips) {
                if (clip.frames.empty()) continue;
                strips += clip.frames.front().height + 2;
                const int across = clip.frames.front().width * (int)clip.frames.size() + (int)clip.frames.size();
                if (across > 0) stripScale = std::max(stripScale, maxWidth / across);
            }
            for (const AtlasClip& clip : clips) {
                if (clip.frames.empty()) continue;
                const int across = clip.frames.front().width * (int)clip.frames.size() + (int)clip.frames.size();
                if (across > 0) stripScale = std::min(stripScale, std::max(1, maxWidth / across));
            }
            PixelImage canvas(std::max(srcW, maxWidth), srcH + strips * stripScale + 14);
            canvas.fill(kPane);
            canvas.blit(scaled(source, srcScale, srcW, srcH), 0, 0);
            for (const FixedFrame& frame : animation.frames) {
                const int x = (int)std::round(frame.box.x * srcScale), y = (int)std::round(frame.box.y * srcScale);
                const int w = (int)std::round(frame.box.width * srcScale), h = (int)std::round(frame.box.height * srcScale);
                outlineBox(canvas, x, y, w, h, kFrameBox);
                drawLabel(canvas, "F" + std::to_string(frame.index + 1), x, y, 1);
            }
            int y = srcH + 2;
            for (const AtlasClip& clip : clips) {
                if (clip.frames.empty()) continue;
                int x = 0;
                for (const PixelArt& art : clip.frames) {
                    const PixelImage frame = toRgba(art);
                    canvas.blit(scaled(frame, stripScale, frame.width() * stripScale, frame.height() * stripScale), x, y);
                    x += (frame.width() + 1) * stripScale;
                }
                y += (clip.frames.front().height + 2) * stripScale;
            }
            drawLabel(canvas, "SOURCE " + scaleText(srcScale) + " | STRIPS " + scaleText(stripScale), 0, canvas.height() - 14);
            return canvas;
        }

        struct SpotPane {
            int columns = 1, cellW = 1, cellH = 1, scale = 1;
        };

        SpotPane layOutFrames(const std::vector<PixelImage>& frames, int maxWidth) {
            SpotPane pane;
            pane.columns = std::clamp((int)frames.size(), 1, 8);
            for (const PixelImage& frame : frames) {
                pane.cellW = std::max(pane.cellW, frame.width());
                pane.cellH = std::max(pane.cellH, frame.height());
            }
            const int across = (pane.cellW + 1) * pane.columns;
            pane.scale = std::max(1, maxWidth / std::max(1, across));
            return pane;
        }

        PixelImage drawFrames(const std::vector<PixelImage>& frames, const SpotPane& pane) {
            const int rows = ((int)frames.size() + pane.columns - 1) / pane.columns;
            PixelImage canvas((pane.cellW + 1) * pane.columns * pane.scale, (pane.cellH + 1) * rows * pane.scale + 14);
            canvas.fill(kPane);
            for (size_t i = 0; i < frames.size(); i++) {
                const int cx = (int)i % pane.columns * (pane.cellW + 1) * pane.scale;
                const int cy = (int)i / pane.columns * (pane.cellH + 1) * pane.scale;
                canvas.blit(scaled(frames[i], pane.scale, frames[i].width() * pane.scale, frames[i].height() * pane.scale), cx, cy);
            }
            return canvas;
        }

        void markSpot(PixelImage& canvas, const SpotPane& pane, int frame, int x, int y, const std::string& label) {
            const int index = std::max(0, frame - 1);
            const int cx = index % pane.columns * (pane.cellW + 1) * pane.scale;
            const int cy = index / pane.columns * (pane.cellH + 1) * pane.scale;
            outlineBox(canvas, cx + x * pane.scale, cy + y * pane.scale, pane.scale, pane.scale, kMagenta);
            if (!label.empty()) drawLabel(canvas, label, cx + (x + 1) * pane.scale, cy + y * pane.scale, 1, kLabelBack, kMagenta);
        }

        // Frames with a list of lint spots marked and numbered
        PixelImage renderSpots(const std::vector<PixelImage>& frames, const lint::LintReport& report, int maxWidth) {
            if (frames.empty()) return {};
            const SpotPane pane = layOutFrames(frames, maxWidth);
            PixelImage canvas = drawFrames(frames, pane);
            int number = 1;
            for (const lint::LintResult& entry : report.results) {
                for (const lint::LintSpot& spot : entry.at) markSpot(canvas, pane, spot.frame, spot.x, spot.y, "S" + std::to_string(number++));
            }
            drawLabel(canvas, "SPOTS " + scaleText(pane.scale), 0, canvas.height() - 14);
            return canvas;
        }

        PixelImage renderLost(const Result& result, int maxWidth) {
            if (result.frames.empty()) return {};
            const std::vector<PixelImage> shown = {result.frames.front()};
            const SpotPane pane = layOutFrames(shown, maxWidth);
            PixelImage canvas = drawFrames(shown, pane);
            int number = 1;
            for (const ReportLost& entry : result.report.lost) {
                const std::string label = "L" + std::to_string(number++);
                bool first = true;
                for (const std::array<int, 2>& cell : entry.at) {
                    markSpot(canvas, pane, 1, cell[0], cell[1], first ? label : std::string());
                    first = false;
                }
            }
            drawLabel(canvas, "LOST " + scaleText(pane.scale), 0, canvas.height() - 14);
            return canvas;
        }

    }

    // ---- prepare

    std::optional<Prepared> prepare(Project* project, const Spec& spec, std::vector<PixelProblem>& problems) {
        problems.clear();
        const ProjectView view = projectView(project, spec.projectDir);
        Prepared out;
        out.spec = spec;
        out.projectDir = view.dir;
        out.assetsDir = view.assets;
        Recipe recipe;
        if (spec.rerun) {
            if (spec.hasSprite()) {
                problems.push_back({"bad_recipe", "rerun", "rerun re-runs an output file, not a Sprite GameObject", "give source: <the output file>"});
                return std::nullopt;
            }
            if (!spec.recipe.is_null()) {
                problems.push_back({"bad_recipe", "recipe", "recipe and rerun are exclusive: a re-run uses the stored recipe", "drop recipe, or drop rerun"});
                return std::nullopt;
            }
            PixelProblem problem;
            std::string shown;
            if (!resolveInView(view, spec.source, out.rerunOutputAbsolute, shown, problem)) {
                problems.push_back(problem);
                return std::nullopt;
            }
            out.rerunOutput = shown;
            std::error_code ec;
            if (!fs::is_regular_file(out.rerunOutputAbsolute, ec)) {
                problems.push_back({"source_missing", "source", "no file " + shown, "give the path of an output written by fix_pixel_art"});
                return std::nullopt;
            }
            std::optional<Recipe> stored = readOutputRecipe(out.rerunOutputAbsolute, problem);
            if (!stored) {
                problems.push_back(problem);
                return std::nullopt;
            }
            recipe = std::move(*stored);
            out.recordedSha256 = recipe.source.sha256;
            // R31/R32: a re-run takes a new palette or sheet choice on top of the stored recipe
            if (!spec.palette.is_null()) {
                Recipe given;
                std::vector<PixelProblem> paletteProblems;
                if (!decodeRecipe(nlohmann::json{{"palette", spec.palette}}, given, paletteProblems)) {
                    problems = std::move(paletteProblems);
                    return std::nullopt;
                }
                if (!resolveRecipePalette(view, given, problem)) {
                    problems.push_back(problem);
                    return std::nullopt;
                }
                recipe.paletteMode = given.paletteMode;
                recipe.palette = given.palette;
                recipe.autoPalette = given.autoPalette;
            }
            if (spec.sheet == SheetArg::Off) {
                recipe.sheetMode = SheetMode::Off;
                recipe.sheet.reset();
            } else if (spec.sheet == SheetArg::On && recipe.sheetMode == SheetMode::Off) {
                recipe.sheetMode = SheetMode::Auto;
            }
            out.spec.frame = recipe.source.frame;
            out.spec.layer = recipe.source.layer;
            if (recipe.source.path.empty()) {
                problems.push_back({"source_missing", "source", shown + " records no source", "give the source image and a recipe instead of rerun"});
                return std::nullopt;
            }
            // recorded project-relative; a plain-file recipe is resolved next to the output, then as given
            const fs::path recorded = fs::u8path(recipe.source.path);
            if (!view.dir.empty()) {
                std::string error;
                if (!spritenormals::resolveProjectFile(view.dir, recipe.source.path, out.sourceAbsolute, error)) {
                    problems.push_back({"source_missing", "source", "the recorded source " + recipe.source.path + " is not usable: " + error, "give source and recipe instead of rerun"});
                    return std::nullopt;
                }
            } else {
                out.sourceAbsolute = recorded;
                if (recorded.is_relative() && fs::is_regular_file(out.rerunOutputAbsolute.parent_path() / recorded, ec)) {
                    out.sourceAbsolute = out.rerunOutputAbsolute.parent_path() / recorded;
                }
            }
            out.sourcePath = recipe.source.path;
            if (!fs::is_regular_file(out.sourceAbsolute, ec)) {
                problems.push_back({"source_missing", "source", "the recorded source " + recipe.source.path + " is gone", "put " + recipe.source.path + " back, or give a new source and recipe"});
                return std::nullopt;
            }
        } else {
            json recipeJson = spec.recipe.is_null() ? json::object() : spec.recipe;
            if (!recipeJson.is_object()) {
                problems.push_back({"bad_recipe", "recipe", "recipe must be an object, got " + recipeJson.dump(), "pass {} or partial recipe keys"});
                return std::nullopt;
            }
            if (!spec.palette.is_null()) recipeJson["palette"] = spec.palette;
            if (spec.sheet != SheetArg::Auto && !(recipeJson.contains("sheet") && recipeJson["sheet"].is_object())) {
                recipeJson["sheet"] = spec.sheet == SheetArg::Off ? "off" : "auto";
            }
            std::vector<PixelProblem> recipeProblems;
            if (!decodeRecipe(recipeJson, recipe, recipeProblems)) {
                problems = std::move(recipeProblems);
                return std::nullopt;
            }
            PixelProblem problem;
            if (!resolveRecipePalette(view, recipe, problem)) {
                problems.push_back(problem);
                return std::nullopt;
            }
            if (spec.hasSprite()) {
                if (!loadSpriteFrames(project, spec, out, problems)) return std::nullopt;
            } else {
                if (spec.source.empty()) {
                    problems.push_back({"source_missing", "source", "give a source image, an .aseprite file or a Sprite GameObject", "pass source or entity"});
                    return std::nullopt;
                }
                if (!resolveInView(view, spec.source, out.sourceAbsolute, out.sourcePath, problem)) {
                    problems.push_back(problem);
                    return std::nullopt;
                }
            }
        }
        if (!spec.hasSprite() && !loadSourceFrames(out, problems)) return std::nullopt;
        if (out.frames.empty()) {
            problems.push_back({"source_missing", "source", "no frames could be read from " + out.sourcePath, "give a readable image"});
            return std::nullopt;
        }
        recipe.source.path = out.sourcePath;
        recipe.source.sha256 = out.sourceSha256;
        recipe.source.frame = out.spec.frame;
        recipe.source.layer = out.spec.layer;
        recipe.engineVersion = JASPERENGINE_EDITOR_VERSION;
        out.recipe = std::move(recipe);
        out.report.failOnWarn = spec.failOnWarn;
        resolveBackdrops(view, {}, out.report.backdrops, out.report.backdropsSource);
        return out;
    }

    // ---- run

    Result run(const Prepared& prepared, RunCache* cacheArg, const std::atomic<bool>* cancel) {
        RunCache local;
        RunCache& cache = cacheArg ? *cacheArg : local;
        Result result;
        const auto cancelled = [&]() { return cancel && cancel->load(); };
        if (cancelled() || prepared.frames.empty()) {
            result.cancelled = cancelled();
            if (!result.cancelled) result.problem = {"source_missing", "source", "nothing to run on", "give a source"};
            return result;
        }
        Recipe recipe = prepared.recipe;
        defaultPaletteWithoutProject(recipe);
        const SheetArg sheetArg = prepared.spec.sheet;
        const bool animated = prepared.frames.size() > 1;
        const PixelImage& first = prepared.frames.front();

        if (animated) {
            // Real pixel art at its own size has no fake grid: detecting one there would invent a
            // cell. Read it one pixel per cell when every sampled frame is native (R6).
            if (recipe.grid.kind == GridKind::Auto) {
                const size_t step = std::max<size_t>(1, prepared.frames.size() / 8);
                bool native = true;
                for (size_t i = 0; i < prepared.frames.size() && native; i += step) {
                    const PixelImage& frame = prepared.frames[i];
                    if (frame.empty()) continue;
                    const std::optional<Outcome> found = runRecipe(frame, recipe);
                    const bool fewColors = artOfImage(frame, (int)recipe.alphaCutoff).art.palette.size() <= 256;
                    native = fewColors && isNative(frame, found);
                }
                if (native) {
                    recipe.grid.kind = GridKind::Cells;
                    recipe.grid.spec = {1, 1, 0, 0};
                    recipe.sample.inset = 0;
                }
            }
            std::optional<Sheet> given;
            if (recipe.sheetMode == SheetMode::Given && recipe.sheet) given = recipe.sheet;
            else if (!prepared.tagAnimations.empty()) {
                Sheet sheet;
                sheet.animations = prepared.tagAnimations;
                given = sheet;
            }
            AnimationOutcome animation = runFrames(prepared.frames, prepared.delaysMs, prepared.animationName, recipe, given, cache.animation);
            if (cancelled()) {
                result.cancelled = true;
                return result;
            }
            fillSheetResult(prepared, result, recipe, first, std::move(animation));
        } else if (recipe.sheetMode == SheetMode::Given && recipe.sheet) {
            AnimationOutcome animation = runSheet(first, recipe, *recipe.sheet, cache.animation);
            if (cancelled()) {
                result.cancelled = true;
                return result;
            }
            fillSheetResult(prepared, result, recipe, first, std::move(animation));
        } else if (recipe.sheetMode != SheetMode::Off) {
            std::vector<SheetFrame> frames = detectFrames(first, recipe, cache.animation);
            if (cancelled()) {
                result.cancelled = true;
                return result;
            }
            const bool slice = !frames.empty() && (sheetArg == SheetArg::On || isAnimationSheet(first, recipe, frames, cache.animation));
            if (slice) {
                Sheet sheet;
                sheet.frames = frames;
                std::vector<FrameBox> boxes;
                boxes.reserve(frames.size());
                for (const SheetFrame& frame : frames) boxes.push_back(frame.box);
                sheet.animations = groupFrames(boxes);
                if (recipe.sheet) {
                    sheet.anchor = recipe.sheet->anchor;
                    sheet.padding = recipe.sheet->padding;
                }
                AnimationOutcome animation = runSheet(first, recipe, sheet, cache.animation);
                if (cancelled()) {
                    result.cancelled = true;
                    return result;
                }
                fillSheetResult(prepared, result, recipe, first, std::move(animation));
            } else if (sheetArg == SheetArg::On) {
                result.warnings.push_back("sheet_not_found");
            }
        }

        if (!result.animation) {
            Checked checked = checkImage(first, recipe, prepared.report, &cache.pipeline);
            if (cancelled()) {
                result.cancelled = true;
                return result;
            }
            result.report = std::move(checked.report);
            result.resolved = std::move(checked.recipe);
            result.outcome = std::move(checked.outcome);
            result.format = formatOf(prepared, false);
            if (result.outcome) {
                result.frames.push_back(toRgba(result.outcome->exported));
                if (result.outcome->editsStale) result.warnings.push_back("edits_stale");
                if (result.outcome->cleanKeepStale) result.warnings.push_back("clean_keep_stale");
            }
        }

        for (const std::string& warning : prepared.warnings) result.warnings.push_back(warning);
        result.resolved.source.path = prepared.sourcePath;
        result.resolved.source.sha256 = prepared.sourceSha256;
        result.resolved.source.frame = prepared.spec.frame;
        result.resolved.source.layer = prepared.spec.layer;
        result.resolved.engineVersion = JASPERENGINE_EDITOR_VERSION;
        result.resolved.pipelineVersion = Recipe::kPipelineVersion;
        result.resolved.format = result.format;
        result.resolved.resultSha256 = result.frames.empty() ? std::string() : sha256OfFrames(result.frames);
        result.resolved.forced = (prepared.spec.force && result.report.verdict == Verdict::NeedsPerson) || (!prepared.rerunOutput.empty() && prepared.recipe.forced);
        if (!prepared.rerunOutput.empty()) result.stale.sourceChanged = prepared.recordedSha256 != prepared.sourceSha256;
        const std::vector<PlannedPath> planned = planPaths(prepared, result.format, result.animation.has_value());
        if (!planned.empty()) result.defaultOutput = planned.front().path;
        return result;
    }

    json toJson(const Prepared& prepared, const Result& result) {
        json out = reportToJson(result.report);
        if (out.contains("grid")) out["grid"]["follow"] = followName(prepared.recipe.follow);
        if (result.resolved.palette) {
            json colors = json::array();
            for (const Rgba8& color : result.resolved.palette->colors) colors.push_back(jasperengine::pixelart::toHex(color, false));
            json palette = out.value("palette", json::object());
            palette["colors"] = std::move(colors);
            palette["locked"] = result.resolved.palette->locked;
            out["palette"] = std::move(palette);
        }
        if (result.outcome) {
            int specks = 0;
            for (const std::vector<int>& group : result.outcome->specks) specks += (int)group.size();
            if (!result.outcome->specks.empty()) out["specks"] = {{"groups", result.outcome->specks.size()}, {"pixels", specks}};
            if (!result.outcome->backgroundFound.empty()) {
                json background = json::array();
                for (const Rgba8& color : result.outcome->backgroundFound) background.push_back(jasperengine::pixelart::toHex(color, false));
                out["background"] = std::move(background);
            }
        }
        if (result.animation && !result.animation->backgroundFound.empty()) {
            json background = json::array();
            for (const Rgba8& color : result.animation->backgroundFound) background.push_back(jasperengine::pixelart::toHex(color, false));
            out["background"] = std::move(background);
        }
        out["recipe"] = encodeRecipe(result.resolved);
        out["format"] = outputFormatName(result.format);
        out["default_output"] = result.defaultOutput;
        out["warnings"] = result.warnings;
        out["stale"] = {{"source_changed", result.stale.sourceChanged}};
        out["cancelled"] = result.cancelled;
        return out;
    }

    // ---- preview

    const char* previewKindName(PreviewKind kind) {
        switch (kind) {
            case PreviewKind::None: return "none";
            case PreviewKind::Compare: return "compare";
            case PreviewKind::Result: return "result";
            case PreviewKind::Grid: return "grid";
            case PreviewKind::Lost: return "lost";
            case PreviewKind::Lints: return "lints";
        }
        return "none";
    }

    bool parsePreviewKind(const std::string& text, PreviewKind& out) {
        for (PreviewKind kind : {PreviewKind::None, PreviewKind::Compare, PreviewKind::Result, PreviewKind::Grid, PreviewKind::Lost, PreviewKind::Lints}) {
            if (text == previewKindName(kind)) {
                out = kind;
                return true;
            }
        }
        return false;
    }

    PixelImage renderPreview(const Prepared& prepared, const Result& result, PreviewKind kind, int maxWidth) {
        if (kind == PreviewKind::None || prepared.frames.empty()) return {};
        const int width = std::clamp(maxWidth, kMinPreviewWidth, kMaxPreviewWidth);
        const PixelImage& source = prepared.frames.front();
        switch (kind) {
            case PreviewKind::Compare:
                if (result.animation) return renderSheet(prepared, result, width);
                if (result.frames.empty()) break;
                return renderCompare(prepared, result, width);
            case PreviewKind::Result:
                if (result.animation) return renderSheet(prepared, result, width);
                if (result.frames.empty()) break;
                {
                    const std::vector<PixelImage> shown = {result.frames.front()};
                    const SpotPane pane = layOutFrames(shown, width);
                    PixelImage canvas = drawFrames(shown, pane);
                    drawLabel(canvas, "RESULT " + scaleText(pane.scale), 0, canvas.height() - 14);
                    return canvas;
                }
            case PreviewKind::Grid: {
                const double scale = fitScale(source.width(), width);
                const int w = std::min(width, std::max(1, (int)std::round(source.width() * scale)));
                const int h = std::max(1, (int)std::round(source.height() * scale));
                PixelImage canvas = scaled(source, scale, w, h + 14);
                if (result.outcome) drawGrid(canvas, result.outcome->grid, scale, 0, 0, kGridLine);
                if (result.animation) {
                    for (const FixedFrame& frame : result.animation->frames) {
                        const int x = (int)std::round(frame.box.x * scale), y = (int)std::round(frame.box.y * scale);
                        outlineBox(canvas, x, y, (int)std::round(frame.box.width * scale), (int)std::round(frame.box.height * scale), kFrameBox);
                        drawLabel(canvas, "F" + std::to_string(frame.index + 1), x, y, 1);
                    }
                }
                drawLabel(canvas, "GRID " + scaleText(scale), 0, h);
                return canvas;
            }
            case PreviewKind::Lost:
                return renderLost(result, width);
            case PreviewKind::Lints:
                return renderSpots(result.frames, result.report.lints, width);
            case PreviewKind::None:
                break;
        }
        // a picture with no output yet still shows its source
        return scaled(source, 1.0, std::min(width, source.width()), source.height());
    }

    // ---- writing

    bool outputExtensionFits(const std::string& role, const std::string& path) {
        const std::string extension = sessionExtension(fs::u8path(path));
        if (role == "png") return extension == ".png";
        if (role == "aseprite") return extension == ".aseprite" || extension == ".ase";
        if (role == "json") return extension == ".json";
        return false;
    }

    bool encodeOutputs(const Prepared& prepared, const Result& result, std::vector<OutputFile>& files, PixelProblem& problem) {
        files.clear();
        const std::string shown = prepared.sourcePath.empty() ? prepared.spec.source : prepared.sourcePath;
        if (result.cancelled) {
            problem = {"cancelled", "", "the run was cancelled; nothing was written", "call again"};
            return false;
        }
        if (!result.problem.empty()) {
            problem = result.problem;
            return false;
        }
        const bool sheet = result.animation.has_value();
        if (result.report.verdict == Verdict::NeedsPerson && !prepared.spec.force && !prepared.spec.person) {
            std::string guards;
            for (const Guard& guard : result.report.guards) guards += (guards.empty() ? "" : ", ") + guard.id;
            problem = {"needs_person", "force", shown + " is needs_person" + (guards.empty() ? std::string() : " (" + guards + ")") + ": " + result.report.summary + " Nothing was written.",
                       "set recipe.grid or recipe.palette and preview again, or pass force: true to write it anyway"};
            return false;
        }
        if (result.frames.empty()) {
            problem = {"no_grid", "recipe.grid", "no grid was found in " + shown + ": there is nothing to write", "set recipe.grid: {\"kind\": \"cells\", \"cell_width\": 1-128} from the preview, or give another source"};
            return false;
        }

        const std::vector<PlannedPath> planned = planPaths(prepared, result.format, sheet);
        for (const PlannedPath& path : planned) {
            if (path.role == "meta") continue;
            if (!outputExtensionFits(path.role, path.path)) {
                problem = {"invalid_value", "output." + path.role, path.path + " is not a ." + (path.role == "aseprite" ? std::string("aseprite") : path.role) + " path; nothing was written",
                           "give output." + path.role + " a path ending in ." + (path.role == "aseprite" ? std::string("aseprite") : path.role) + " (never a .meta)"};
                return false;
            }
            if (samePath(path.absolute, prepared.sourceAbsolute)) {
                problem = {"output_is_source", "output." + path.role, path.path + " is the source: the source is never replaced", "write to a new path: omit output's paths for " + result.defaultOutput};
                return false;
            }
            if (!prepared.assetsDir.empty() && !safewrite::isInsideResolved(path.absolute, prepared.assetsDir)) {
                problem = {"output_outside_assets", "output." + path.role, path.path + " is outside the project's assets directory", "write it inside the assets directory (e.g. " + result.defaultOutput + ")"};
                return false;
            }
            std::error_code ec;
            const bool rerunTarget = !prepared.rerunOutput.empty() && samePath(path.absolute, prepared.rerunOutputAbsolute);
            // a re-run replaces only its own outputs: the file it re-runs, and the sheet's JSON beside it
            // when that JSON is the one this cleanup wrote (it carries the recipe); anything else needs overwrite
            const bool rerunOwn = rerunTarget || (!prepared.rerunOutput.empty() && path.role == "json" && atlasCarriesRecipe(path.absolute));
            if (fs::exists(path.absolute, ec)) {
                if (!prepared.spec.overwrite && !rerunOwn) {
                    problem = {"output_exists", "output." + path.role, path.path + " exists; nothing was written", "pass overwrite: true (one undo step puts it back), or another output path"};
                    return false;
                }
                if (rerunTarget && !prepared.spec.force && !prepared.recipe.resultSha256.empty()) {
                    const std::optional<std::string> pixels = outputPixelsSha256(path.absolute);
                    if (pixels && *pixels != prepared.recipe.resultSha256) {
                        problem = {"output_edited", "output." + path.role, path.path + " was edited after the cleanup; a re-run would throw those edits away. Nothing was written.",
                                   "give a new output path, or pass force: true to replace it"};
                        return false;
                    }
                }
            }
        }

        const std::string text = recipeText(result.resolved);
        const auto pathFor = [&](const char* role) -> const PlannedPath* {
            for (const PlannedPath& path : planned) {
                if (path.role == role) return &path;
            }
            return nullptr;
        };
        const auto pngBytes = [&](const PixelImage& image, std::string& out) {
            const std::string encoded = image.encodePng();
            std::vector<uint8_t> input(encoded.begin(), encoded.end()), withChunk;
            std::string error;
            if (!texsynth::insertRecipeChunk(input, text, withChunk, error, kRecipeChunk)) return false;
            out.assign(withChunk.begin(), withChunk.end());
            return true;
        };

        if (result.format == OutputFormat::Png) {
            const PlannedPath* png = pathFor("png");
            std::string bytes;
            if (!png || !pngBytes(result.frames.front(), bytes)) {
                problem = {"write_failed", "output.png", "the PNG could not be encoded", "try again"};
                return false;
            }
            files.push_back({"png", png->path, png->absolute, std::move(bytes)});
            if (sheet) {
                const PlannedPath* jsonPath = pathFor("json");
                const std::vector<AtlasClip> clips = atlasClips(*result.animation);
                const Atlas atlas = pngSheet(clips);
                SpritesheetInfo info;
                info.image = FileUtils::pathToUtf8(png->absolute.filename());
                info.clips = &clips;
                info.atlas = &atlas;
                info.recipe = encodeRecipe(result.resolved);
                files.push_back({"json", jsonPath->path, jsonPath->absolute, spritesheetJson(info)});
            }
        } else {
            const PlannedPath* ase = pathFor("aseprite");
            std::vector<AtlasClip> clips;
            if (sheet) {
                clips = atlasClips(*result.animation);
            } else {
                AtlasClip clip;
                clip.name = prepared.animationName.empty() ? "Cleanup" : prepared.animationName;
                clip.frames.push_back(result.outcome->exported);
                clip.durationsMs.push_back((int)std::lround(1000.0 / DEFAULT_FPS));
                clips.push_back(std::move(clip));
            }
            const aseprite::Document document = asepriteDocument(clips);
            std::vector<uint8_t> bytes;
            aseprite::CodecError error;
            if (!aseprite::write(document, bytes, &error)) {
                problem = {"write_failed", "output.aseprite", "the .aseprite could not be encoded: " + error.message, "try again"};
                return false;
            }
            files.push_back({"aseprite", ase->path, ase->absolute, std::string(bytes.begin(), bytes.end())});

            // the recipe lives in the .meta, written in the same step (R31)
            const PlannedPath* metaPath = pathFor("meta");
            AssetMeta meta;
            std::string existing;
            std::error_code ec;
            if (!(fs::exists(metaPath->absolute, ec) && readWholeFile(metaPath->absolute, existing) && AssetMeta::parse(existing, meta))) {
                meta = AssetMeta();
                // derived from the path, not random: the same run writes the same bytes (R40, R41)
                const fs::path relative = prepared.assetsDir.empty() ? ase->absolute.filename() : ase->absolute.lexically_relative(prepared.assetsDir);
                meta.guid = Guid::parse(AssetDatabase::legacyGuid(relative));
            }
            meta.setSection("pixelCleanup", "{recipe: " + json(text).dump() + "}");
            files.push_back({"meta", metaPath->path, metaPath->absolute, meta.serialize()});
        }
        return true;
    }

    bool writeUndoable(Project* project, const std::vector<OutputFile>& files, PixelProblem& problem) {
        if (!project || project->getProjectPath().empty()) {
            problem = {"write_failed", "output", "No editor project is open to write into.", "Open the project in the editor."};
            return false;
        }
        spritenormals::Problem escape;
        if (!spritenormals::outputsContained(project, files, escape)) {
            problem = {"output_outside_assets", escape.field, escape.message, escape.fix};
            return false;
        }
        std::vector<WriteFilesCmd::Item> items;
        std::vector<OutputFile> where;
        std::string stepFile;
        std::vector<std::string> asepritePaths;
        for (const OutputFile& file : files) {
            items.push_back({file.absolute, file.bytes});
            where.push_back({file.role, file.path, file.absolute, std::string()});
            if (stepFile.empty() || file.role == "png" || file.role == "aseprite") {
                if (file.role != "meta" && file.role != "json") stepFile = FileUtils::pathToUtf8(file.absolute.filename());
            }
            if (file.role == "aseprite") asepritePaths.push_back(file.path);
        }
        auto guard = [project, where](std::string& why) {
            spritenormals::Problem p;
            if (spritenormals::outputsContained(project, where, p)) return true;
            why = p.message;
            return false;
        };
        auto afterChange = [project, asepritePaths](const std::vector<fs::path>& changed) {
            ProjectFileWrites::filesWritten(project, changed);
            // S05's self-write suppression hides the editor's own writes from the watcher (R31)
            for (const std::string& path : asepritePaths) aseprite::AsepriteImporter::get().reimport(path);
        };
        std::string error;
        const std::string stepName = "Pixel Art Cleanup (" + stepFile + ")";
        if (!ProjectFileWrites::writeUndoable(project, std::move(items), error, afterChange, guard, spritenormals::outputsRoot(project), false, stepName)) {
            problem = {"write_failed", "output", error, "Check the path is writable, then try again."};
            return false;
        }
        return true;
    }

    bool writeDirect(const Prepared& prepared, const std::vector<OutputFile>& files, PixelProblem& problem) {
        std::string failed, error;
        if (!spritenormals::writeOutputsDirect(files, failed, error, prepared.assetsDir)) {
            problem = {"write_failed", "output", failed + ": " + error, "Check the path is writable, then try again."};
            return false;
        }
        return true;
    }

    // ---- recipes and staleness

    std::optional<Recipe> readOutputRecipe(const fs::path& output, PixelProblem& problem) {
        std::string text, error;
        if (isAsepritePath(output)) {
            std::string metaText;
            AssetMeta meta;
            if (!readWholeFile(AssetTypes::metaPathFor(output), metaText) || !AssetMeta::parse(metaText, meta) || !meta.hasSection("pixelCleanup")) {
                problem = {"bad_recipe", "source", FileUtils::pathToUtf8(output.filename()) + " carries no cleanup recipe (no pixelCleanup in its .meta)", "give an output written by fix_pixel_art"};
                return std::nullopt;
            }
            try {
                const YAML::Node node = YAML::Load(meta.getSection("pixelCleanup"));
                text = node["recipe"].as<std::string>();
            } catch (const YAML::Exception& e) {
                problem = {"bad_recipe", "source", std::string("the .meta's pixelCleanup could not be read: ") + e.what(), "give an output written by fix_pixel_art"};
                return std::nullopt;
            }
        } else {
            const std::optional<std::string> chunk = texsynth::readRecipeText(output, &error, kRecipeChunk);
            if (!chunk) {
                problem = {"bad_recipe", "source", FileUtils::pathToUtf8(output.filename()) + " carries no cleanup recipe" + (error.empty() ? "" : " (" + error + ")"), "give an output written by fix_pixel_art"};
                return std::nullopt;
            }
            text = *chunk;
        }
        Recipe recipe;
        std::vector<PixelProblem> problems;
        if (!decodeRecipeText(text, recipe, problems)) {
            problem = problems.empty() ? PixelProblem{"bad_recipe", "source", "the stored recipe could not be read", "give an output written by fix_pixel_art"} : problems.front();
            return std::nullopt;
        }
        return recipe;
    }

    const char* toString(Staleness::State state) {
        switch (state) {
            case Staleness::State::Fresh: return "fresh";
            case Staleness::State::Changed: return "changed";
            case Staleness::State::Missing: return "missing";
            case Staleness::State::NoRecipe: return "no_recipe";
            case Staleness::State::Invalid: return "invalid";
            case Staleness::State::Unreadable: return "unreadable";
        }
        return "invalid";
    }

    Staleness checkStaleness(const fs::path& projectDir, const fs::path& output) {
        Staleness out;
        std::error_code ec;
        if (!fs::is_regular_file(output, ec)) {
            out.state = Staleness::State::Unreadable;
            out.message = FileUtils::pathToUtf8(output.filename()) + " cannot be read";
            return out;
        }
        PixelProblem problem;
        std::optional<Recipe> recipe = readOutputRecipe(output, problem);
        if (!recipe) {
            out.state = problem.message.find("no cleanup recipe") != std::string::npos ? Staleness::State::NoRecipe : Staleness::State::Invalid;
            out.message = problem.message;
            return out;
        }
        out.sourcePath = recipe->source.path;
        out.forced = recipe->forced;
        const fs::path recorded = fs::u8path(recipe->source.path);
        out.sourceAbsolute = recorded.is_absolute() ? recorded : (projectDir.empty() ? output.parent_path() / recorded : projectDir / recorded);
        if (recipe->source.path.empty() || !fs::is_regular_file(out.sourceAbsolute, ec)) {
            out.state = Staleness::State::Missing;
            out.message = recipe->source.path + " is gone";
        } else if (sha256OfFile(out.sourceAbsolute) != recipe->source.sha256) {
            out.state = Staleness::State::Changed;
            out.message = recipe->source.path + " changed since this output was made";
        } else {
            out.state = Staleness::State::Fresh;
        }
        if (!recipe->resultSha256.empty()) {
            const std::optional<std::string> pixels = outputPixelsSha256(output);
            out.outputEdited = pixels && *pixels != recipe->resultSha256;
        }
        return out;
    }

    std::optional<std::string> outputPixelsSha256(const fs::path& output) {
        if (isAsepritePath(output)) {
            aseprite::ReadOptions options;
            options.provenance = false;
            const aseprite::ReadResult read = aseprite::readFile(output, options);
            if (!read.ok()) return std::nullopt;
            std::vector<PixelImage> frames;
            for (int i = 0; i < read.document->frameCount(); i++) frames.push_back(aseprite::renderFrame(*read.document, i));
            return sha256OfFrames(frames);
        }
        std::string error;
        std::optional<PixelImage> image = PixelImage::loadPng(output, &error);
        if (!image) return std::nullopt;
        return sha256OfFrames({*image});
    }

    // ---- the import step

    bool runImportStep(const PixelImage& source, const std::string& recipeText, PixelImage& out, Verdict& verdict, std::string& guard, std::string& error) {
        Recipe recipe;
        std::vector<PixelProblem> problems;
        if (!decodeRecipeText(recipeText, recipe, problems)) {
            error = problems.empty() ? std::string("bad recipe") : problems.front().message;
            return false;
        }
        defaultPaletteWithoutProject(recipe);
        Prepared prepared;
        prepared.frames = {source};
        prepared.recipe = std::move(recipe);
        prepared.spec.format = OutputFormat::Png;
        const Result result = run(prepared);
        if (!result.problem.empty()) {
            error = result.problem.message;
            return false;
        }
        verdict = result.report.verdict;
        guard = result.report.guards.empty() ? std::string() : result.report.guards.front().id;
        if (!result.frames.empty()) out = result.frames.front();
        return true;
    }

    // ---- linting

    namespace {

        bool frameNumbers(const json& given, int frameCount, std::vector<int>& out, std::vector<PixelProblem>& problems) {
            for (const json& item : given) {
                if (!item.is_number_integer() || item.get<int>() < 1 || item.get<int>() > frameCount) {
                    problems.push_back({"bad_recipe", "frames", "frame " + item.dump() + " is not in this source", "frames are 1-" + std::to_string(frameCount)});
                    return false;
                }
                out.push_back(item.get<int>() - 1);
            }
            return true;
        }

        std::vector<std::string> frameNameList(int count) {
            std::vector<std::string> names;
            names.reserve(count);
            for (int i = 0; i < count; i++) names.push_back(std::to_string(i + 1));
            return names;
        }

    }

    std::optional<LintPrepared> prepareLint(Project* project, const LintSpec& spec, std::vector<PixelProblem>& problems) {
        problems.clear();
        const ProjectView view = projectView(project, spec.projectDir);
        LintPrepared out;
        out.rules = spec.rules;
        out.failOnWarn = spec.failOnWarn;
        for (const std::string& id : spec.lints) {
            if (!lint::isLintId(id)) {
                problems.push_back({"bad_recipe", "lints", "\"" + id + "\" is not a lint", "use " + lint::lintIdList()});
                return std::nullopt;
            }
            out.only.push_back(id);
        }
        Prepared loader;
        loader.spec.layer = spec.layer;
        if (spec.hasSprite()) {
            Spec fake;
            fake.sceneId = spec.sceneId;
            fake.entity = spec.entity;
            if (!loadSpriteFrames(project, fake, loader, problems)) return std::nullopt;
            out.sourcePath = loader.sourcePath;
            out.sourceAbsolute = loader.sourceAbsolute;
        } else {
            if (spec.source.empty()) {
                problems.push_back({"source_missing", "source", "give an image, an .aseprite file or a Sprite GameObject", "pass source or entity"});
                return std::nullopt;
            }
            PixelProblem problem;
            if (!resolveInView(view, spec.source, loader.sourceAbsolute, loader.sourcePath, problem)) {
                problems.push_back(problem);
                return std::nullopt;
            }
            loader.projectDir = view.dir;
            if (!loadSourceFrames(loader, problems)) return std::nullopt;
            out.sourcePath = loader.sourcePath;
            out.sourceAbsolute = loader.sourceAbsolute;
        }

        // frames: a tag or numbers; loop defaults to the tag's direction, else true (R46)
        const bool aseprite = isAsepritePath(loader.sourceAbsolute);
        bool loop = true;
        std::vector<int> chosen;
        if (spec.frames.is_string()) {
            const std::string tag = spec.frames.get<std::string>();
            aseprite::ReadOptions options;
            options.provenance = false;
            const aseprite::ReadResult read = aseprite ? aseprite::readFile(loader.sourceAbsolute, options) : aseprite::ReadResult{};
            bool found = false;
            std::string names;
            if (read.ok()) {
                for (const aseprite::Tag& entry : read.document->tags) {
                    names += (names.empty() ? "" : ", ") + entry.name;
                    if (entry.name != tag) continue;
                    found = true;
                    for (int i = entry.from; i <= entry.to; i++) chosen.push_back(i);
                    if (!spec.loop) loop = entry.direction != aseprite::TagDirection::PingPong;
                }
            }
            if (!found) {
                problems.push_back({"bad_recipe", "frames", "no tag \"" + tag + "\" in " + loader.sourcePath, names.empty() ? "pass frame numbers, or an .aseprite with tags" : "tags: " + names});
                return std::nullopt;
            }
        } else if (spec.frames.is_array()) {
            if (!frameNumbers(spec.frames, (int)loader.frames.size(), chosen, problems)) return std::nullopt;
        } else if (!spec.frames.is_null()) {
            problems.push_back({"bad_recipe", "frames", "frames must be a tag name or a list of frame numbers", "e.g. \"walk\" or [1, 2, 3]"});
            return std::nullopt;
        }
        if (chosen.empty()) {
            for (int i = 0; i < (int)loader.frames.size(); i++) chosen.push_back(i);
        }
        for (int i : chosen) out.frames.push_back(loader.frames[i]);
        out.frameNames = frameNameList((int)out.frames.size());
        out.loop = spec.loop.value_or(loop);
        out.asepriteSource = aseprite;

        // the source's own palette applies when one is named; a recipe's colours when it carries one
        if (!spec.palette.is_null()) {
            Recipe paletteRecipe;
            std::vector<PixelProblem> paletteProblems;
            if (!decodeRecipe(json{{"palette", spec.palette}}, paletteRecipe, paletteProblems)) {
                problems = std::move(paletteProblems);
                return std::nullopt;
            }
            PixelProblem problem;
            if (!resolveRecipePalette(view, paletteRecipe, problem)) {
                problems.push_back(problem);
                return std::nullopt;
            }
            if (paletteRecipe.palette) {
                Palette palette;
                palette.name = paletteRecipe.palette->name;
                palette.colors = paletteRecipe.palette->colors;
                out.palette = std::move(palette);
            }
        }
        std::error_code ec;
        if (!out.sourceAbsolute.empty() && fs::is_regular_file(out.sourceAbsolute, ec)) {
            PixelProblem ignored;
            std::optional<Recipe> recipe = readOutputRecipe(out.sourceAbsolute, ignored);
            if (recipe) {
                out.forced = recipe->forced;
                if (!out.palette && recipe->palette && !recipe->palette->colors.empty()) {
                    Palette palette;
                    palette.name = recipe->palette->name;
                    palette.colors = recipe->palette->colors;
                    out.palette = std::move(palette);
                }
                out.outputRecipe = std::move(recipe);
            }
        }
        if (!spec.backdrops.empty() || view.hasSettings || view.project) {
            resolveBackdrops(view, spec.backdrops, out.backdrops, out.backdropsSource);
        }
        return out;
    }

    LintOutcome runLint(const LintPrepared& prepared) {
        LintOutcome out;
        out.frames = prepared.frames;
        out.forced = prepared.forced;
        lint::LintInput input;
        input.frames = out.frames;
        input.palette = prepared.palette ? &*prepared.palette : nullptr;
        input.backdrops = prepared.backdrops;
        if (!prepared.backdropsSource.empty()) input.backdropsSource = prepared.backdropsSource;
        input.loop = prepared.loop;
        input.rules = prepared.rules;
        lint::LintOptions options;
        options.only = prepared.only;
        out.report = lint::lint(input, options);

        // what the lints say by themselves
        Verdict verdict = Verdict::Clean;
        bool unfixable = false;
        for (const lint::LintResult& entry : out.report.results) {
            if (entry.status != lint::LintStatus::Fail) continue;
            if (removableLint(entry.id)) verdict = worse(verdict, Verdict::Fixable);
            else unfixable = true;
        }
        if (unfixable) verdict = Verdict::NeedsPerson;
        else if (verdict == Verdict::Clean && prepared.failOnWarn && out.report.warn > 0) verdict = Verdict::Fixable;

        // A flat picture also needs the grid judged: an AI sprite or a photo is not clean pixel
        // art, however few lints it happens to fail
        if (prepared.frames.size() == 1 && !prepared.asepriteSource && !prepared.frames.front().empty()) {
            Recipe recipe;
            defaultPaletteWithoutProject(recipe);
            if (prepared.palette) {
                recipe.paletteMode = PaletteMode::Fixed;
                RecipePalette palette;
                palette.name = prepared.palette->name;
                palette.colors = prepared.palette->colors;
                recipe.palette = std::move(palette);
                recipe.autoPalette.reset();
            }
            ReportOptions reportOptions;
            reportOptions.rules = prepared.rules;
            reportOptions.backdrops = prepared.backdrops;
            reportOptions.failOnWarn = prepared.failOnWarn;
            const Checked checked = checkImage(prepared.frames.front(), recipe, reportOptions);
            verdict = worse(verdict, checked.report.verdict);
        }
        out.verdict = verdict;
        out.summary = std::string(verdictName(verdict)) + ": " + std::to_string(out.report.fail) + " fail, " + std::to_string(out.report.warn) + " warn";
        return out;
    }

    json toJson(const LintPrepared& prepared, const LintOutcome& outcome) {
        json results = json::array();
        for (const lint::LintResult& entry : outcome.report.results) results.push_back(lint::toJson(entry));
        json out = {{"verdict", verdictName(outcome.verdict)},
                    {"results", std::move(results)},
                    {"counts", {{"pass", outcome.report.pass}, {"warn", outcome.report.warn}, {"fail", outcome.report.fail}, {"skip", outcome.report.skip}}},
                    {"frames", (int)prepared.frames.size()}};
        if (outcome.forced) out["forced"] = true;
        return out;
    }

    PixelImage renderLintImage(const LintPrepared&, const LintOutcome& outcome, int maxWidth) {
        return renderSpots(outcome.frames, outcome.report, std::clamp(maxWidth, kMinPreviewWidth, kMaxPreviewWidth));
    }

    // ---- wiring

    void installEditorJobs() {
        setParallelFor([](int count, const std::function<void(int)>& body) {
            jobs::parallelFor((size_t)count, [&body](size_t i) { body((int)i); });
        });
    }

    namespace {

        RuleResult cleanupOutputsRule(const PixelArtCheckInput& input, std::vector<Finding>& findings) {
            RuleResult result;
            result.id = "cleanup_outputs";
            std::error_code ec;
            if (!fs::is_directory(input.assetsRoot, ec)) return result;
            std::vector<fs::path> files;
            for (fs::recursive_directory_iterator it(input.assetsRoot, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
                if (ec) break;
                const fs::path& file = it->path();
                if (it->is_directory(ec) && file.filename().string().rfind('.', 0) == 0) {
                    it.disable_recursion_pending();
                    continue;
                }
                if (!it->is_regular_file(ec)) continue;
                const std::string extension = sessionExtension(file);
                if (extension == ".png" || extension == ".aseprite" || extension == ".ase") files.push_back(file);
            }
            std::sort(files.begin(), files.end());

            const auto worstStatus = [&](RuleStatus status) {
                if (status == RuleStatus::Fail || result.status == RuleStatus::Pass) result.status = status;
            };
            const auto addFinding = [&](const std::string& path, const std::string& issue, RuleStatus status, json measured, json expected, std::string message, std::string needsPerson) {
                Finding finding;
                finding.key = "cleanup_outputs:" + path + ":" + issue;
                finding.rule = "cleanup_outputs";
                finding.status = status;
                finding.target = {{"kind", "texture"}, {"path", path}};
                finding.measured = std::move(measured);
                finding.expected = std::move(expected);
                finding.message = std::move(message);
                finding.needsPerson = std::move(needsPerson);
                findings.push_back(std::move(finding));
                result.findings++;
                worstStatus(status);
            };

            // the project directory: the one holding project.yaml, as the recipes record paths under it
            fs::path projectRoot = input.assetsRoot;
            while (!fs::is_regular_file(projectRoot / "project.yaml", ec) && projectRoot.has_relative_path() && projectRoot != projectRoot.parent_path()) projectRoot = projectRoot.parent_path();
            for (const fs::path& file : files) {
                const std::string rel = FileUtils::pathToGenericUtf8(file.lexically_relative(input.assetsRoot));
                // fix_pixel_art takes project-relative paths
                const std::string projectRel = FileUtils::pathToGenericUtf8(file.lexically_relative(projectRoot));
                const fs::path metaPath = AssetTypes::metaPathFor(file);
                bool hasRecipe = false;
                std::string importRecipe;
                if (isAsepritePath(file)) {
                    std::string metaText;
                    AssetMeta meta;
                    hasRecipe = readWholeFile(metaPath, metaText) && AssetMeta::parse(metaText, meta) && meta.hasSection("pixelCleanup");
                } else {
                    std::string error;
                    hasRecipe = texsynth::readRecipeTextHead(file, &error, kRecipeChunk).has_value();
                    if (!hasRecipe) {
                        std::string metaText;
                        AssetMeta meta;
                        if (readWholeFile(metaPath, metaText) && AssetMeta::parse(metaText, meta) && meta.hasSection("import")) {
                            try {
                                const YAML::Node section = YAML::Load(meta.getSection("import"));
                                const YAML::Node pc = section["pixelCleanup"];
                                if (pc.IsDefined() && pc.IsMap() && pc["enabled"].IsDefined() && pc["enabled"].as<bool>()) {
                                    importRecipe = pc["recipe"].IsDefined() ? pc["recipe"].as<std::string>() : std::string("{}");
                                }
                            } catch (const YAML::Exception&) {
                                // an unreadable import section is the importer's to report, not this rule's
                            }
                        }
                    }
                }
                if (!hasRecipe && importRecipe.empty()) continue;
                result.checked++;

                if (hasRecipe) {
                    const Staleness staleness = checkStaleness(projectRoot, file);
                    if (staleness.state == Staleness::State::Changed || staleness.state == Staleness::State::Missing) {
                        addFinding(rel, "stale", RuleStatus::Warn, {{"source_changed", staleness.state == Staleness::State::Changed}}, {{"source_changed", false}},
                                   rel + ": " + staleness.message + ".",
                                   "re-run it: fix_pixel_art {source: \"" + projectRel + "\", rerun: true}, then the same with output: true");
                    }
                    if (staleness.forced) {
                        addFinding(rel, "forced", RuleStatus::Warn, {{"forced", true}}, {{"forced", false}},
                                   rel + " was written with force: its verdict was needs_person.",
                                   "a person must look at it, or run fix_pixel_art again with a better grid or palette");
                    }
                }

                LintPrepared prepared;
                if (!importRecipe.empty()) {
                    spritenormals::Source source;
                    std::vector<PixelProblem> reasons;
                    PixelImage image, out;
                    if (!spritenormals::loadSource(file, source, reasons) || !sourceToImage(source.color, image)) continue;
                    Verdict verdict = Verdict::Clean;
                    std::string guard, error;
                    if (!runImportStep(image, importRecipe, out, verdict, guard, error)) {
                        addFinding(rel, "recipe", RuleStatus::Fail, {{"recipe", "invalid"}}, {{"recipe", "valid"}}, rel + "'s import step cannot run: " + error, "fix the recipe in the Inspector");
                        continue;
                    }
                    if (verdict == Verdict::NeedsPerson) {
                        addFinding(rel, "verdict", RuleStatus::Fail, {{"guard", guard}}, {{"verdict", "fixable"}}, rel + "'s import step needs a person (" + guard + ").", "a person must look at it");
                    }
                    prepared.frames = {std::move(out)};
                } else {
                    Prepared loader;
                    loader.sourceAbsolute = file;
                    loader.sourcePath = rel;
                    std::vector<PixelProblem> problems;
                    if (!loadSourceFrames(loader, problems) || loader.frames.empty()) continue;
                    prepared.frames = std::move(loader.frames);
                    prepared.asepriteSource = isAsepritePath(file);
                    PixelProblem ignored;
                    if (const std::optional<Recipe> recipe = readOutputRecipe(file, ignored); recipe && recipe->palette && !recipe->palette->colors.empty()) {
                        Palette palette;
                        palette.name = recipe->palette->name;
                        palette.colors = recipe->palette->colors;
                        prepared.palette = std::move(palette);
                    }
                }
                prepared.only = {};
                const LintOutcome outcome = runLint(prepared);
                for (const lint::LintResult& entry : outcome.report.results) {
                    if (entry.status != lint::LintStatus::Fail) continue;
                    addFinding(rel, entry.id, RuleStatus::Fail, {{"measured", entry.measured}, {"id", entry.id}}, {{"threshold", entry.threshold}},
                               rel + " fails " + entry.id + ": " + entry.message, entry.fix.empty() ? "fix the art or re-run fix_pixel_art" : entry.fix);
                }
            }
            return result;
        }

    }

    void registerCleanupOutputsRule() {
        registerRule("cleanup_outputs", cleanupOutputsRule);
    }

}
