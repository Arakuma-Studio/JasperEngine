// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SpriteNormalsSession.h"

#include "Catalog.h"
#include "Platform.h"
#include "Project.h"
#include "Scene.h"
#include "System.h"
#include "component/Light2DComponent.h"
#include "component/MeshComponent.h"
#include "command/type/WriteFilesCmd.h"
#include "component/SpriteComponent.h"
#include "component/TilemapComponent.h"
#include "component/Transform.h"
#include "mcp/McpValues.h"
#include "sprite/SpriteSheetBuilder.h"
#include "sprite/SpriteSheetPool.h"
#include "sprite/SpriteSheetSources.h"
#include "texsynth/SynthesisRecipe.h"
#include "texsynth/SynthesisSession.h"
#include "util/Ambient2D.h"
#include "util/EntityPath.h"
#include "util/ProjectChecks.h"
#include "util/ProjectFileWrites.h"
#include "util/SafeWrite.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <system_error>
#include <thread>
#include <utility>
#include "util/FileUtils.h"

namespace fs = std::filesystem;
using nlohmann::json;

namespace jasperengine::editor::spritenormals {

    namespace {

        std::string lower(std::string text) {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        std::string extensionOf(const std::string& path) { return lower(jasperengine::editor::FileUtils::pathToUtf8(fs::u8path(path).extension())); }

        void add(std::vector<Problem>& problems, const char* kind, const std::string& field, const std::string& message, const std::string& fix = "") {
            problems.push_back({kind, field, message, fix});
        }

        // Recipes record paths as JSON text: a file name that is not UTF-8 can't be recorded
        std::string notUtf8(const std::string& path) {
            return texsynth::printableUtf8(path) + ": the file name is not valid UTF-8, so the recipe could not record it; rename the file";
        }

        std::string trimmed(double v) {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%.2f", v);
            std::string text = buffer;
            while (!text.empty() && text.back() == '0') text.pop_back();
            if (!text.empty() && text.back() == '.') text.pop_back();
            return text;
        }

        std::string fileName(const std::string& path) {
            const std::string name = jasperengine::editor::FileUtils::pathToUtf8(fs::u8path(path).filename());
            return name.empty() ? path : name;
        }

        bool readFile(const fs::path& path, std::vector<uint8_t>& bytes) {
            std::ifstream file(path, std::ios::binary);
            if (!file) return false;
            bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            return true;
        }

        // The project the engine resolves paths for: the editor and the CLI both install an
        // editor::Platform for theirs (where a scene's material links live); nullptr without one,
        // and without the engine at all (the CLI without --sprite: no System is installed)
        Project* editorProject() {
            if (!System::hasInstance()) return nullptr;
            if (auto* platform = dynamic_cast<editor::Platform*>(&System::instance())) return platform->getProject();
            return nullptr;
        }

        // `path` without a trailing separator, lexically normal
        fs::path normalDir(const fs::path& path) {
            fs::path p = path.lexically_normal();
            if (!p.has_filename() && p.has_parent_path()) p = p.parent_path();
            return p;
        }

        // `path` is `dir` or below it (lexically)
        using safewrite::isAtOrBelow;

        // A stored texture path (relative to the project's assets directory, as Texture keeps
        // it) made absolute: the engine resolves it through the Platform's asset path
        fs::path textureFile(const std::string& stored) {
            if (stored.empty()) return fs::path();
            const fs::path path = fs::u8path(stored);
            if (path.is_absolute() || !System::hasInstance()) return path.lexically_normal();
            return (fs::u8path(System::instance().getAssetPath()) / path).lexically_normal();
        }

        // Where project-relative paths of the scene's textures are relative to, without a
        // projectDir: the editor's project, else the Platform's asset path (the project
        // directory with the default assets directory ".")
        fs::path sceneProjectDir() {
            if (Project* project = editorProject()) return project->getProjectPath();
            return System::hasInstance() ? fs::u8path(System::instance().getAssetPath()) : fs::path();
        }

        // The assets directory outputs must be in: `assets` when it lies at or below projectDir,
        // else projectDir itself. `assets` named through another spelling of projectDir (a link to
        // the project) is that directory under projectDir's spelling.
        fs::path assetsRootOf(const fs::path& projectDir, const fs::path& assets) {
            if (assets.empty() || projectDir.empty()) return normalDir(projectDir);
            if (isAtOrBelow(assets, projectDir)) return normalDir(assets);
            // the ancestor of `assets` that is projectDir once links are resolved: the rest of
            // `assets` below projectDir as named
            const fs::path realProject = resolvedPath(projectDir);
            fs::path tail;
            for (fs::path a = normalDir(assets); a.has_relative_path(); a = a.parent_path()) {
                if (resolvedPath(a) == realProject) return normalDir(tail.empty() ? projectDir : projectDir / tail);
                tail = tail.empty() ? a.filename() : a.filename() / tail;
            }
            return normalDir(projectDir);
        }

        // ... the Platform's asset path (the project's assets directory); without the engine: the
        // assets directory projectDir's project.yaml names
        fs::path assetsRoot(const fs::path& projectDir) {
            return assetsRootOf(projectDir, System::hasInstance() ? fs::u8path(System::instance().getAssetPath())
                                                                  : ProjectChecks::assetsRootFromProjectFile(projectDir));
        }

        // An output's escape once links are resolved: "" when `abs` (lexically inside `assets`)
        // stays inside the assets directory and out of hidden directories of the project, else
        // the problem's message. `shownPath` as the caller names the output.
        std::string resolvedEscape(const fs::path& projectDir, const fs::path& assets, const fs::path& abs, const std::string& shownPath) {
            const fs::path real = resolvedPath(abs);
            const fs::path realAssets = resolvedPath(assets);
            if (real.empty()) {
                return shownPath + " can't be resolved: a symbolic link on its way loops or can't be read.";
            }
            if (!isAtOrBelow(real, realAssets)) {
                return shownPath + " resolves outside the project's assets directory (" + projectRelative(projectDir, assets) +
                       "): a symbolic link on its way leads to " + texsynth::printableUtf8(jasperengine::editor::FileUtils::pathToGenericUtf8(real)) + ".";
            }
            // (the assets directory itself may be a link out of the project: it is a trusted root,
            // resolved once; only links below it are checked)
            // a link into a hidden directory of the project (.jasperengine, .git ...)
            if (!projectDir.empty()) {
                const fs::path back = real.lexically_relative(resolvedPath(projectDir));
                if (!back.empty() && *back.begin() != "..") {
                    for (auto it = back.begin(); it != back.end() && std::next(it) != back.end(); ++it) {
                        const std::string part = jasperengine::editor::FileUtils::pathToUtf8(*it);
                        if (!part.empty() && part[0] == '.' && part != "." && part != "..") {
                            return shownPath + " resolves into the hidden directory " + texsynth::printableUtf8(part) +
                                   " through a symbolic link.";
                        }
                    }
                }
            }
            return std::string();
        }

        Scene* mutableScene(const Scene* scene) { return const_cast<Scene*>(scene); }

        // A mesh's submeshes; a Sprite's first exists before its first update sets the count
        unsigned submeshCount(Scene* scene, Entity entity, const MeshComponent& mesh) {
            return std::max(mesh.numSubmeshes, scene->findComponent<SpriteComponent>(entity) ? 1u : 0u);
        }

        struct SpriteInfo {
            fs::path texture;                // absolute
            std::string stored;
            std::vector<Cell> cells;
            Problem problem;                 // kind empty when fine
        };

        // The texture of `submesh` (its base colour) and, for submesh 0, the Sprite's frames
        SpriteInfo spriteInfo(const Scene* scene, Entity entity, unsigned submesh) {
            SpriteInfo info;
            auto refuse = [&](const char* kind, const std::string& message, const std::string& fix) {
                info.problem = {kind, "entity", message, fix};
                return info;
            };
            if (!scene || entity == NULL_ENTITY || !scene->isEntityCreated(entity)) {
                return refuse("not_a_sprite", "The GameObject " + std::to_string(entity) + " is not in the scene.", "Pick a Sprite GameObject of the open scene.");
            }
            Scene* s = mutableScene(scene);
            const std::string name = s->getEntityName(entity);
            MeshComponent* mesh = s->findComponent<MeshComponent>(entity);
            const unsigned count = mesh ? submeshCount(s, entity, *mesh) : 0;
            if (count == 0) {
                return refuse("not_a_sprite", "\"" + name + "\" has no mesh to hold a texture.", "Pick a Sprite (or a mesh) GameObject.");
            }
            if (submesh >= count) {
                return refuse("not_a_sprite", "\"" + name + "\" has " + std::to_string(count) + " submesh(es); there is no submesh " +
                                                  std::to_string(submesh) + ".",
                              "Pick a submesh from 0 to " + std::to_string(count - 1) + ".");
            }
            info.stored = mesh->submeshes[submesh].material.baseColorTexture.getPath();
            // a texture sheet sprite not drawn yet (a scene loaded without an update: the CLI) has
            // no mesh texture: its texture is the sheet's
            if (SpriteComponent* sheetSprite = submesh == 0 ? s->findComponent<SpriteComponent>(entity) : nullptr;
                info.stored.empty() && sheetSprite && SpriteSheetSources::isTexturePath(sheetSprite->sheet.path)) {
                info.stored = sheetSprite->sheet.path;
            }
            if (info.stored.empty()) {
                return refuse("invalid_source", "\"" + name + "\" has no base colour texture.", "Give the Sprite a texture first.");
            }
            info.texture = textureFile(info.stored);
            SpriteComponent* sprite = submesh == 0 ? s->findComponent<SpriteComponent>(entity) : nullptr;
            if (sprite && !sprite->sheet.empty()) {
                // a sheet sprite (S44/S45): the frames of the texture sheet it draws, read from the
                // texture's Sprite Mode (the CLI has no editor answering the pool). Generated pages
                // (an atlas page, an .aseprite import) are rebuilt, so a map made from one would not
                // stay paired with it
                const std::string sheetPath = sprite->sheet.path;
                const bool textureSheet = SpriteSheetSources::isTexturePath(sheetPath);
                const std::string drawn = mesh->submeshes[submesh].material.baseColorTexture.getPath();
                const bool onPage = !drawn.empty() && jasperengine::editor::FileUtils::pathFromUtf8(drawn).lexically_normal() !=
                                                          jasperengine::editor::FileUtils::pathFromUtf8(sheetPath).lexically_normal();
                if (!textureSheet || onPage) {
                    return refuse("invalid_source",
                                  "\"" + name + "\" draws " + (textureSheet ? sheetPath + " from a Sprite Atlas page" : "the generated pages of " + sheetPath) +
                                      ": a normal map of a page would not stay paired with it.",
                                  textureSheet ? "Remove " + sheetPath + " from its atlas (edit_sprite_atlas remove) or turn the atlas's Include in Build off, then generate."
                                               : "Generate the map from the source image (source: its path) and pair it in the editor that made the sheet.");
                }
                info.stored = sheetPath;
                info.texture = textureFile(sheetPath);
                const sprite::TextureSheetInfo sheetInfo = sprite::textureSheetSettings(info.texture);
                const sprite::TextureSheetBuild built = sprite::buildTextureSheet(info.texture, sheetPath, sheetInfo.settings);
                for (const SpriteFrame& f : built.sheet.frames) {
                    Rect r = f.rect;
                    Cell cell;
                    cell.x = static_cast<int>(std::lround(r.getX()));
                    cell.y = static_cast<int>(std::lround(r.getY()));
                    cell.width = static_cast<int>(std::lround(r.getWidth()));
                    cell.height = static_cast<int>(std::lround(r.getHeight()));
                    if (cell.width > 0 && cell.height > 0) info.cells.push_back(cell);
                }
                return info;
            }
            if (sprite) {
                int w = 0, h = 0;
                bool sized = false;
                for (unsigned i = 0; i < sprite->numFramesRect; i++) {
                    Rect r = sprite->framesRect[i].rect;
                    if (r.getWidth() <= 0.0f || r.getHeight() <= 0.0f) continue;
                    if (r.isNormalized()) {
                        // frames given as fractions of the texture
                        if (!sized) sized = texsynth::probeImage(info.texture, w, h);
                        if (!sized) continue;
                        r = Rect(r.getX() * w, r.getY() * h, r.getWidth() * w, r.getHeight() * h);
                    }
                    Cell cell;
                    cell.x = static_cast<int>(std::lround(r.getX()));
                    cell.y = static_cast<int>(std::lround(r.getY()));
                    cell.width = static_cast<int>(std::lround(r.getWidth()));
                    cell.height = static_cast<int>(std::lround(r.getHeight()));
                    info.cells.push_back(cell);
                }
            }
            return info;
        }

        // How a spec names a Sprite's texture as its source (plannedOutputs and checkSpec/prepare
        // alike): project-relative, or outside a project (empty projectDir) the texture's own
        // absolute path, since a relative one would be read from the working directory
        std::string spriteSourcePath(const fs::path& projectDir, const SpriteInfo& info) {
            return projectDir.empty() ? jasperengine::editor::FileUtils::pathToGenericUtf8(info.texture) : projectRelative(projectDir, info.texture);
        }

        // ---- running workers (SpriteNormalsRun::cancelAll): the texsynth SynthesisRun pattern,
        // a registry every worker thread holds a copy of, so a thread outliving the editor's
        // statics (exit) still locks and notifies a live object
        struct RunRegistry {
            std::mutex mutex;
            std::condition_variable done;
            std::vector<std::weak_ptr<SpriteNormalsRun>> runs;
            int active = 0;
            std::atomic<bool> shuttingDown{false};
        };

        std::shared_ptr<RunRegistry> registry() {
            static const std::shared_ptr<RunRegistry> instance = std::make_shared<RunRegistry>();
            return instance;
        }

        Result failedResult(const std::string& message) {
            Result result;
            result.status = Status::Failed;
            result.errors.push_back({"failed", "", message, "Try again; report it if it persists."});
            return result;
        }

        const char* sourceWord(SourceKind kind) {
            return kind == SourceKind::Gif ? "GIF" : kind == SourceKind::Apng ? "APNG" : "image";
        }

    }

    // ------------------------------------------------------------------ spec

    std::string Spec::fingerprint() const {
        json j;
        j["source"] = source;
        j["sprite"] = {sceneId, static_cast<uint64_t>(entity), submesh};
        j["settings"] = settingsToJson(settings);
        json frameCells = json::array();
        for (const Cell& c : frames.cells) frameCells.push_back({c.x, c.y, c.width, c.height});
        const Grid& g = frames.grid;
        j["frames"] = {static_cast<int>(frames.mode), static_cast<int>(frames.from),
                       {g.tileWidth, g.tileHeight, g.columns, g.rows, g.count, g.offsetX, g.offsetY, g.paddingX, g.paddingY}, frameCells};
        // the paint (only when there is some: unpainted specs keep their Phase 2 fingerprint)
        if (!paint.empty() || paintMode != PaintMode::Keep) j["paint"] = {toString(paintMode), paintOpsToJson(paint)};
        // where the files go is not part of the result (outputs, overwrite). CBOR: compared,
        // never shown, and it never throws on a path that is not valid UTF-8
        const std::vector<uint8_t> bytes = json::to_cbor(j);
        return std::string(bytes.begin(), bytes.end());
    }

    // ------------------------------------------------------------------ paths

    bool resolveProjectFile(const fs::path& projectDir, const std::string& rel, fs::path& absolute, std::string& error) {
        if (rel.empty()) {
            error = "no path given";
            return false;
        }
        const fs::path given = fs::u8path(rel);
        if (projectDir.empty()) {
            // outside a project (the CLI without --project): as given, from the working directory
            std::error_code ec;
            absolute = fs::absolute(given, ec).lexically_normal();
            if (ec) absolute = given.lexically_normal();
            return true;
        }
        if (given.is_absolute() || given.has_root_name() || given.has_root_directory()) {
            error = "\"" + texsynth::printableUtf8(rel) + "\" is absolute: give a project-relative path (e.g. \"assets/hero.png\")";
            return false;
        }
        const fs::path normal = (projectDir / given).lexically_normal();
        const fs::path back = normal.lexically_relative(projectDir.lexically_normal());
        if (back.empty() || *back.begin() == ".." || back == ".") {
            error = "\"" + texsynth::printableUtf8(rel) + "\" is outside the project";
            return false;
        }
        // a hidden directory (.jasperengine is the editor's own, .git ...)
        for (auto it = back.begin(); it != back.end(); ++it) {
            if (std::next(it) == back.end()) break;
            const std::string part = jasperengine::editor::FileUtils::pathToUtf8(*it);
            if (!part.empty() && part[0] == '.') {
                error = "\"" + texsynth::printableUtf8(rel) + "\" is inside the hidden directory " + texsynth::printableUtf8(part);
                return false;
            }
        }
        absolute = normal;
        return true;
    }

    std::string projectRelative(const fs::path& projectDir, const fs::path& absolute) {
        if (projectDir.empty()) return jasperengine::editor::FileUtils::pathToGenericUtf8(absolute.lexically_normal());
        const fs::path rel = absolute.lexically_normal().lexically_relative(projectDir.lexically_normal());
        if (rel.empty() || *rel.begin() == ".." || rel.is_absolute()) return jasperengine::editor::FileUtils::pathToGenericUtf8(absolute.lexically_normal());
        return jasperengine::editor::FileUtils::pathToGenericUtf8(rel);
    }

    std::vector<PlannedOutput> defaultOutputs(const std::string& sourcePath, SourceKind kind, bool height, bool framesJson) {
        const fs::path source = fs::u8path(sourcePath);
        const std::string stem = jasperengine::editor::FileUtils::pathToUtf8(source.stem());
        auto sibling = [&](const std::string& name) { return jasperengine::editor::FileUtils::pathToGenericUtf8((source.parent_path() / fs::u8path(name))); };
        std::vector<PlannedOutput> out;
        if (kind == SourceKind::Gif || kind == SourceKind::Apng) {
            out.push_back({"normal", sibling(stem + "_sheet_normal.png")});
            out.push_back({"atlas", sibling(stem + "_sheet.png")});
            if (height) out.push_back({"height", sibling(stem + "_sheet_height.png")});
            if (framesJson) out.push_back({"frames_json", sibling(stem + "_sheet.frames.json")});
        } else {
            out.push_back({"normal", sibling(stem + "_normal.png")});
            if (height) out.push_back({"height", sibling(stem + "_height.png")});
            if (framesJson) out.push_back({"frames_json", sibling(stem + "_normal.frames.json")});
        }
        return out;
    }

    std::vector<PlannedOutput> plannedOutputs(const fs::path& projectDir, const Spec& spec, SourceKind kind) {
        std::string source = spec.source;
        if (source.empty() && spec.hasSprite()) {
            // the Sprite's texture, from the editor's open scene
            if (Project* project = editorProject()) {
                if (SceneProject* scene = project->getScene(spec.sceneId)) {
                    const SpriteInfo info = spriteInfo(scene->scene, spec.entity, spec.submesh);
                    if (info.problem.kind.empty()) source = spriteSourcePath(projectDir, info);
                }
            }
        }
        const bool animated = kind == SourceKind::Gif || kind == SourceKind::Apng;
        std::vector<PlannedOutput> defaults =
            source.empty() ? std::vector<PlannedOutput>() : defaultOutputs(source, kind, spec.settings.height, spec.output.framesJson);
        auto defaultFor = [&](const char* role) {
            for (const PlannedOutput& p : defaults) {
                if (p.role == role) return p.path;
            }
            return std::string();
        };
        std::vector<PlannedOutput> out;
        const std::string normal = spec.output.normal.empty() ? defaultFor("normal") : spec.output.normal;
        PlannedOutput normalOut{"normal", normal, std::nullopt};
        if (spec.paintMode == PaintMode::Keep && !normal.empty()) {
            // what keep would start from (a head read: the chunk's JSON, its image not decoded)
            fs::path abs;
            std::string error;
            std::error_code ec;
            if (resolveProjectFile(projectDir, normal, abs, error) && fs::is_regular_file(abs, ec)) normalOut.existingPaint = readPaintSummary(abs);
        }
        out.push_back(normalOut);
        std::string atlas;
        if (animated) {
            atlas = spec.output.atlas.empty() ? defaultFor("atlas") : spec.output.atlas;
            out.push_back({"atlas", atlas, std::nullopt});
        }
        if (spec.settings.height) out.push_back({"height", spec.output.height.empty() ? defaultFor("height") : spec.output.height, std::nullopt});
        if (spec.output.framesJson) {
            std::string path = spec.output.framesJsonPath;
            if (path.empty()) {
                // <atlas stem>.frames.json, else <normal stem>.frames.json
                const fs::path base = fs::u8path(animated ? atlas : normal);
                if (!base.empty()) path = jasperengine::editor::FileUtils::pathToGenericUtf8((base.parent_path() / fs::u8path(jasperengine::editor::FileUtils::pathToUtf8(base.stem()) + ".frames.json")));
            }
            out.push_back({"frames_json", path, std::nullopt});
        }
        return out;
    }

    // ------------------------------------------------------------------ frames

    bool framesFromSprite(const Scene* scene, Entity entity, unsigned submesh, std::string& texturePath, std::vector<Cell>& cells,
                          std::string& error) {
        const SpriteInfo info = spriteInfo(scene, entity, submesh);
        if (!info.problem.kind.empty()) {
            error = info.problem.message;
            return false;
        }
        texturePath = projectRelative(sceneProjectDir(), info.texture);
        cells = info.cells;
        return true;
    }

    // ------------------------------------------------------------------ check and prepare

    namespace {

        // The first two cells that overlap (identical duplicates are processed once: not an overlap)
        bool overlapping(const std::vector<Cell>& cells, size_t& a, size_t& b) {
            for (a = 0; a < cells.size(); a++) {
                for (b = a + 1; b < cells.size(); b++) {
                    const Cell& p = cells[a];
                    const Cell& q = cells[b];
                    if (p == q) continue;
                    if (p.x < q.x + q.width && q.x < p.x + p.width && p.y < q.y + q.height && q.y < p.y + p.height) return true;
                }
            }
            return false;
        }

        // checkSpec's work, keeping what it found for prepare()
        struct Checked {
            std::string sourcePath;          // project-relative (or as given outside a project)
            fs::path sourceAbsolute;
            SourceInfo info;
            bool sourceOk = false;
            std::vector<Cell> spriteCells;
            std::vector<PlannedOutput> outputs;
        };

        std::vector<Problem> check(const fs::path& projectDir, const Spec& spec, const Scene* scene, Checked& c) {
            std::vector<Problem> problems = validateSettings(spec.settings);
            std::error_code ec;

            // ---- paint (frames are checked against the cells by prepare)
            for (const Problem& p : validatePaintOps(spec.paint)) problems.push_back(p);
            if (spec.paintMode == PaintMode::Clear && !spec.paint.empty()) {
                add(problems, "invalid_paint", "paint",
                    "paint_mode \"clear\" drops the paint, so paint must be empty (got " + std::to_string(spec.paint.size()) + " operations).",
                    "Use paint_mode \"replace\" to start over with these operations.");
            }

            // ---- source
            std::string source = spec.source;
            if (spec.hasSprite()) {
                if (!scene) {
                    add(problems, "not_a_sprite", "entity", "The Sprite's scene is not open.", "Open the scene, or give the image path as the source.");
                } else {
                    const SpriteInfo info = spriteInfo(scene, spec.entity, spec.submesh);
                    if (!info.problem.kind.empty()) {
                        problems.push_back(info.problem);
                    } else {
                        source = spriteSourcePath(projectDir, info);
                        c.spriteCells = info.cells;
                    }
                }
            } else if (source.empty()) {
                add(problems, "invalid_source", "source", "No source image was given.", "Give a project-relative image path (e.g. assets/hero.png) or a Sprite GameObject.");
            }
            if (!source.empty()) {
                std::string error;
                if (!texsynth::isValidUtf8(source)) {
                    add(problems, "invalid_source", "source", notUtf8(source), "Rename the file (UTF-8 name).");
                } else if (!resolveProjectFile(projectDir, source, c.sourceAbsolute, error)) {
                    add(problems, "invalid_source", "source", error, "Use an image inside the project.");
                } else if (!fs::is_regular_file(c.sourceAbsolute, ec)) {
                    add(problems, "invalid_source", "source", source + " does not exist.", "Check the path (project-relative, e.g. assets/hero.png).");
                } else {
                    std::vector<Problem> probe;
                    if (probeSource(c.sourceAbsolute, c.info, probe)) {
                        c.sourceOk = true;
                    } else {
                        for (Problem& p : probe) {
                            if (p.field.empty()) p.field = "source";
                            problems.push_back(p);
                        }
                    }
                }
                c.sourcePath = projectDir.empty() ? source : projectRelative(projectDir, c.sourceAbsolute);
            }
            const bool animated = c.sourceOk && (c.info.kind == SourceKind::Gif || c.info.kind == SourceKind::Apng);

            // ---- frames
            if (c.sourceOk) {
                const std::string fromFile = std::string("The frames come from the ") + sourceWord(c.info.kind) + "; leave frames auto.";
                if (spec.frames.mode == FramesSpec::Mode::None) {
                    if (animated) {
                        add(problems, "frames_mismatch", "frames", "An animation's frames come from the file; leave frames auto.", "Leave frames auto.");
                    }
                } else if (spec.frames.mode == FramesSpec::Mode::Auto) {
                    size_t a = 0, b = 0;
                    if (!animated && overlapping(c.spriteCells, a, b)) {
                        add(problems, "frames_mismatch", "frames",
                            "The Sprite's frames " + std::to_string(a) + " and " + std::to_string(b) + " overlap; each pixel can belong to one frame only.",
                            "Use frames none, or reslice the sprite without overlap");
                    }
                } else if (spec.frames.mode == FramesSpec::Mode::Grid) {
                    if (animated) {
                        add(problems, "frames_mismatch", "frames", fromFile, "Leave frames auto.");
                    } else {
                        std::vector<Cell> cells;
                        std::string error;
                        if (!sliceGrid(spec.frames.grid, c.info.width, c.info.height, cells, error)) {
                            add(problems, "frames_mismatch", "frames", error, "Check tile_size (or columns and rows), offset and padding against the image size.");
                        }
                    }
                } else if (spec.frames.mode == FramesSpec::Mode::Cells) {
                    if (animated) {
                        add(problems, "frames_mismatch", "frames", fromFile, "Leave frames auto.");
                    } else if (spec.frames.cells.empty()) {
                        add(problems, "frames_mismatch", "frames.cells", "No frame rectangles were given.", "Give [x, y, w, h] pixel rects, or leave frames auto.");
                    } else if (static_cast<int>(spec.frames.cells.size()) > kMaxCells) {
                        add(problems, "frames_mismatch", "frames.cells",
                            std::to_string(spec.frames.cells.size()) + " frames were given; at most " + std::to_string(kMaxCells) + " are processed.",
                            "Give fewer frames.");
                    } else {
                        for (size_t i = 0; i < spec.frames.cells.size(); i++) {
                            const Cell& cell = spec.frames.cells[i];
                            if (cell.x < 0 || cell.y < 0 || cell.width < 1 || cell.height < 1 || cell.x + cell.width > c.info.width ||
                                cell.y + cell.height > c.info.height) {
                                add(problems, "frames_mismatch", "frames.cells[" + std::to_string(i) + "]",
                                    "Frame " + std::to_string(i) + " [" + std::to_string(cell.x) + ", " + std::to_string(cell.y) + ", " +
                                        std::to_string(cell.width) + ", " + std::to_string(cell.height) + "] is not inside the " +
                                        std::to_string(c.info.width) + "x" + std::to_string(c.info.height) + " image.",
                                    "Give pixel rects inside the image.");
                                break;
                            }
                        }
                        size_t a = 0, b = 0;
                        if (overlapping(spec.frames.cells, a, b)) {
                            const bool sprite = spec.frames.from == FramesFrom::Sprite;
                            add(problems, "frames_mismatch", "frames.cells",
                                std::string(sprite ? "The Sprite's frames " : "Frames ") + std::to_string(a) + " and " + std::to_string(b) +
                                    " overlap; each pixel can belong to one frame only.",
                                sprite ? "Use frames none, or reslice the sprite without overlap" : "Give frames that do not overlap.");
                        }
                    }
                }
            }

            // ---- outputs
            Spec resolved = spec;
            resolved.source = c.sourcePath;
            c.outputs = plannedOutputs(projectDir, resolved, animated ? c.info.kind : SourceKind::Image);
            std::set<std::string> written;
            const fs::path assets = projectDir.empty() ? fs::path() : assetsRoot(projectDir);
            // the source and the outputs compared with their links resolved (an alias is the same file)
            const std::string sourceKey = c.sourceAbsolute.empty() ? std::string() : jasperengine::editor::FileUtils::pathToGenericUtf8(resolvedPath(c.sourceAbsolute));
            for (const PlannedOutput& out : c.outputs) {
                const std::string field = "output." + out.role;
                if (out.path.empty()) {
                    if (!source.empty()) add(problems, "output_invalid", field, "No " + out.role + " output path.", "Give the output path.");
                    continue;
                }
                if (!texsynth::isValidUtf8(out.path)) {
                    add(problems, "output_invalid", field, notUtf8(out.path), "Use a UTF-8 file name.");
                    continue;
                }
                const std::string ext = extensionOf(out.path);
                if (out.role == "frames_json" ? ext != ".json" : ext != ".png") {
                    add(problems, "output_invalid", field, "The " + out.role + " output must be a " + (out.role == "frames_json" ? ".json" : ".png") + " file: " + out.path,
                        "Give a path ending in " + std::string(out.role == "frames_json" ? ".json." : ".png."));
                    continue;
                }
                fs::path abs;
                std::string error;
                if (!resolveProjectFile(projectDir, out.path, abs, error)) {
                    add(problems, "output_invalid", field, error, "Write it inside the project's assets directory.");
                    continue;
                }
                if (!assets.empty() && !isAtOrBelow(abs, assets)) {
                    add(problems, "output_invalid", field,
                        out.path + " is outside the project's assets directory (" + projectRelative(projectDir, assets) + ").",
                        "Write it inside the assets directory.");
                    continue;
                }
                if (!assets.empty()) {
                    const std::string escape = resolvedEscape(projectDir, assets, abs, out.path);
                    if (!escape.empty()) {
                        add(problems, "output_invalid", field, escape, "Write it to a folder of the assets directory that is not a link to elsewhere.");
                        continue;
                    }
                }
                const std::string key = jasperengine::editor::FileUtils::pathToGenericUtf8(resolvedPath(abs));
                if (!written.insert(key).second) {
                    add(problems, "output_invalid", field, "Two outputs would be written to " + out.path + ".", "Give each output its own path.");
                    continue;
                }
                if (key == sourceKey) {
                    add(problems, "output_invalid", field, "The " + out.role + " output would replace the source " + out.path + ".", "Write it elsewhere.");
                    continue;
                }
                if (fs::is_directory(abs, ec)) {
                    add(problems, "output_invalid", field, out.path + " is a directory.", "Give a file path.");
                    continue;
                }
                if (!spec.overwrite && fs::exists(abs, ec)) {
                    add(problems, "output_exists", field, out.path + " already exists.", "Pass overwrite, or another output path.");
                }
            }
            return problems;
        }

    }

    std::vector<Problem> checkSpec(const fs::path& projectDir, const Spec& spec, const Scene* scene) {
        Checked c;
        return check(projectDir, spec, scene, c);
    }

    namespace {

        std::string sizeText(int w, int h) { return std::to_string(w) + "x" + std::to_string(h); }

        // The cells a recipe recorded ([] for one whole image)
        std::vector<Cell> recipeCells(const json& recipe) {
            std::vector<Cell> cells;
            if (!recipe.contains("frames") || !recipe["frames"].is_object() || !recipe["frames"].contains("cells")) return cells;
            for (const json& c : recipe["frames"]["cells"]) cells.push_back({c[0].get<int>(), c[1].get<int>(), c[2].get<int>(), c[3].get<int>()});
            return cells;
        }

        // The paint layer of a prepared spec (2.5): the ops (their frames against the cells) and,
        // with Keep, the overlay of the planned normal output
        bool preparePaint(const Spec& spec, Prepared& out, std::vector<Problem>& problems) {
            const Request& request = out.request;
            for (const Problem& p : validatePaintOps(spec.paint, static_cast<int>(request.cells.size()))) problems.push_back(p);
            if (!problems.empty()) return false;
            out.paint = PaintLayer();
            out.paint.ops = spec.paint;
            if (spec.paintMode != PaintMode::Keep) return true;
            const fs::path normal = [&] {
                for (const PlannedOutput& o : out.outputs) {
                    fs::path abs;
                    std::string error;
                    if (o.role == "normal" && resolveProjectFile(out.projectDir, o.path, abs, error)) return abs;
                }
                return fs::path();
            }();
            std::error_code ec;
            if (normal.empty() || !fs::is_regular_file(normal, ec)) return true;
            const std::string rel = out.projectDir.empty() ? jasperengine::editor::FileUtils::pathToGenericUtf8(normal) : projectRelative(out.projectDir, normal);
            const std::string name = jasperengine::editor::FileUtils::pathToUtf8(normal.filename());
            std::string why;
            std::optional<PaintOverlay> kept = readPaint(normal, &why);
            if (!kept) {
                if (!why.empty()) {
                    add(out.paintWarnings, "paint_unreadable", "paint_mode",
                        name + "'s paint layer can't be read (" + why + "); it is dropped when this map is written.",
                        "Regenerate with paint_mode: \"clear\", or repaint.");
                }
                return true;
            }
            const int w = request.color.width, h = request.color.height;
            if (!kept->sized(w, h)) {
                add(problems, "paint_size_mismatch", "paint_mode",
                    name + " has " + std::to_string(kept->paintedCount()) + " painted pixels for a " + sizeText(kept->width, kept->height) +
                        " image; the new output is " + sizeText(w, h) + ".",
                    "Pass paint_mode: \"clear\" to drop the paint, or restore the source size.");
                return false;
            }
            out.paint.overlay = std::move(*kept);
            out.paintKeptFrom = rel;
            // the frames the paint was made on
            std::string error;
            const std::optional<json> recipe = readRecipeHead(normal, &error);
            if (recipe && validateRecipe(*recipe, error) && recipeCells(*recipe) != request.cells) {
                add(out.paintWarnings, "paint_layout_changed", "frames",
                    name + "'s frames changed since it was painted (" + std::to_string(recipeCells(*recipe).size()) + " cells then, " +
                        std::to_string(request.cells.size()) + " now); painted pixels may now land on other frames.",
                    "Check the painted pixels (the magenta outline), or pass paint_mode: \"clear\" to drop the paint.");
            }
            return true;
        }

    }

    bool prepare(const fs::path& projectDir, const Spec& spec, const Scene* scene, Prepared& out, std::vector<Problem>& problems) {
        return preparePlan(projectDir, spec, scene, out, problems) && prepareLoad(out, problems);
    }

    bool preparePlan(const fs::path& projectDir, const Spec& spec, const Scene* scene, Prepared& out, std::vector<Problem>& problems) {
        Checked c;
        problems = check(projectDir, spec, scene, c);
        if (!problems.empty()) return false;
        out = Prepared();
        out.projectDir = projectDir;
        out.spec = spec;
        out.sourcePath = c.sourcePath;
        out.sourceAbsolute = c.sourceAbsolute;
        out.outputs = c.outputs;
        out.plannedSpriteCells = c.spriteCells;
        if (spec.hasSprite()) out.spriteEntityPath = EntityPath::of(mutableScene(scene), spec.entity);
        return true;
    }

    bool prepareLoad(Prepared& out, std::vector<Problem>& problems) {
        problems.clear();
        const Spec& spec = out.spec;
        if (!loadSource(out.sourceAbsolute, out.source, problems)) {
            for (Problem& p : problems) {
                if (p.field.empty()) p.field = "source";
            }
            return false;
        }

        Request& request = out.request;
        request.color = out.source.color;
        request.settings = spec.settings;
        request.shapeNormals = true;   // the assessment's roughness and creases
        const bool animated = out.source.kind == SourceKind::Gif || out.source.kind == SourceKind::Apng;
        if (animated) {
            // the atlas only exists with its frames, whatever frames says
            request.cells = out.source.cells;
            request.sequence = true;
            out.framesFrom = out.source.kind == SourceKind::Gif ? FramesFrom::Gif : FramesFrom::Apng;
        } else if (spec.frames.mode == FramesSpec::Mode::Grid) {
            std::string error;
            if (!sliceGrid(spec.frames.grid, out.source.color.width, out.source.color.height, request.cells, error)) {
                problems.push_back({"frames_mismatch", "frames", error, "Check the grid against the image size."});
                return false;
            }
            out.framesFrom = FramesFrom::Given;
        } else if (spec.frames.mode == FramesSpec::Mode::Cells) {
            request.cells = spec.frames.cells;
            out.framesFrom = spec.frames.from == FramesFrom::Sprite ? FramesFrom::Sprite : FramesFrom::Given;
        } else if (spec.frames.mode == FramesSpec::Mode::Auto && !out.plannedSpriteCells.empty()) {
            request.cells = out.plannedSpriteCells;
            out.framesFrom = FramesFrom::Sprite;
        }
        // frames of one animation share one auto radius (Grid, Sprite frames and Cells alike)
        if (!animated && request.cells.size() >= 2) request.sequence = true;
        return preparePaint(spec, out, problems);
    }

    // ------------------------------------------------------------------ outputs

    namespace {

        // How a recipe (or the frames JSON) records `path`: project-relative inside a project,
        // else relative to the directory of the file carrying it ('/')
        std::string recordedPath(const Prepared& prepared, const fs::path& path, const fs::path& carrier) {
            if (!prepared.projectDir.empty()) return projectRelative(prepared.projectDir, path);
            const fs::path rel = path.lexically_normal().lexically_relative(carrier.parent_path().lexically_normal());
            return rel.empty() ? jasperengine::editor::FileUtils::pathToGenericUtf8(path.lexically_normal()) : jasperengine::editor::FileUtils::pathToGenericUtf8(rel);
        }

        fs::path plannedAbsolute(const Prepared& prepared, const std::string& role) {
            for (const PlannedOutput& out : prepared.outputs) {
                fs::path abs;
                std::string error;
                if (out.role == role && resolveProjectFile(prepared.projectDir, out.path, abs, error)) return abs;
            }
            return fs::path();
        }

        int distinct(const std::vector<Cell>& cells, bool columns) {
            std::set<int> values;
            for (const Cell& c : cells) values.insert(columns ? c.x : c.y);
            return static_cast<int>(values.size());
        }

    }

    json framesJson(const Prepared& prepared) {
        const Source& s = prepared.source;
        const bool animated = s.kind == SourceKind::Gif || s.kind == SourceKind::Apng;
        const std::vector<Cell>& cells = prepared.request.cells;
        json j = json::object();
        j["version"] = 1;
        fs::path carrier = plannedAbsolute(prepared, "frames_json");
        if (carrier.empty()) carrier = plannedAbsolute(prepared, "normal");
        j["source"] = recordedPath(prepared, prepared.sourceAbsolute, carrier);
        j["width"] = s.color.width;
        j["height"] = s.color.height;
        if (animated) {
            json delays = json::array(), ms = json::array();
            for (const Delay& d : s.delays) {
                delays.push_back(json::array({d.numerator, d.denominator}));
                ms.push_back(d.ms());
            }
            j["animation"] = {{"frame_width", s.frameWidth}, {"frame_height", s.frameHeight}, {"columns", s.columns}, {"rows", s.rows},
                              {"frame_count", s.frameCount}, {"delays_ms", delays}, {"frame_ms", ms}, {"loop", s.loopCount}};
        } else if (!cells.empty()) {
            // a still with frames: their layout, no timing
            j["animation"] = {{"frame_width", cells[0].width}, {"frame_height", cells[0].height}, {"columns", distinct(cells, true)},
                              {"rows", distinct(cells, false)}, {"frame_count", cells.size()}};
        } else {
            j["animation"] = nullptr;
        }
        j["settings"] = settingsToJson(prepared.request.settings);
        return j;
    }

    bool encodeOutputs(const Prepared& prepared, const Result& result, std::vector<OutputFile>& files, std::string& error, Problem* problem) {
        files.clear();
        if (problem) *problem = Problem();
        auto fail = [&](const char* kind, const std::string& why) {
            error = why;
            if (problem) *problem = {kind, "output", why, "Check the output paths, then try again."};
            return false;
        };
        if (result.status != Status::Ok) return fail("write_failed", "there is no finished result to write");
        const Source& source = prepared.source;
        const bool animated = source.kind == SourceKind::Gif || source.kind == SourceKind::Apng;
        RecipeInput in;
        in.sourceSha1 = source.sha1;
        in.sourceKind = source.kind;
        in.fromSprite = prepared.spec.hasSprite();
        in.spriteEntity = prepared.spriteEntityPath;
        in.spriteSubmesh = prepared.spec.hasSprite() ? prepared.spec.submesh : 0;
        in.framesFrom = prepared.framesFrom;
        in.cells = prepared.request.cells;
        if (animated) {
            in.columns = source.columns;
            in.rows = source.rows;
            in.delays = source.delays;
            in.loopCount = source.loopCount;
        }
        in.settings = prepared.request.settings;
        in.result = &result;
        const fs::path normalFile = plannedAbsolute(prepared, "normal");
        const fs::path atlasFile = plannedAbsolute(prepared, "atlas");
        const fs::path heightFile = plannedAbsolute(prepared, "height");
        for (const PlannedOutput& out : prepared.outputs) {
            OutputFile file;
            file.role = out.role;
            if (!resolveProjectFile(prepared.projectDir, out.path, file.absolute, error)) return fail("write_failed", error);
            file.path = prepared.projectDir.empty() ? out.path : projectRelative(prepared.projectDir, file.absolute);
            if (out.role == "frames_json") {
                file.bytes = texsynth::jsonTextLossy(framesJson(prepared), 2) + "\n";
                files.push_back(std::move(file));
                continue;
            }
            const Image* image = &result.normal;
            if (out.role == "atlas") image = &source.color;
            else if (out.role == "height") image = &result.height;
            if (image->empty()) return fail("write_failed", "the result has no " + out.role + " image");
            // paths as this PNG records them (outside a project: from its own directory)
            in.role = out.role;
            in.sourcePath = recordedPath(prepared, prepared.sourceAbsolute, file.absolute);
            in.normalOutput = normalFile.empty() ? std::string() : recordedPath(prepared, normalFile, file.absolute);
            in.atlasOutput = atlasFile.empty() ? std::string() : recordedPath(prepared, atlasFile, file.absolute);
            in.heightOutput = heightFile.empty() ? std::string() : recordedPath(prepared, heightFile, file.absolute);
            // the normal map carries the paint layer (never the atlas or the height map)
            const PaintOverlay* paint = out.role == "normal" && !prepared.paint.overlay.empty() ? &prepared.paint.overlay : nullptr;
            in.paintedPixels = paint ? static_cast<int>(paint->paintedCount()) : 0;
            std::vector<uint8_t> png;
            Problem why;
            if (!encodeWithRecipeAndPaint(*image, makeRecipe(in), paint, png, why)) {
                error = why.message;
                if (problem) *problem = why;
                return false;
            }
            file.bytes.assign(png.begin(), png.end());
            files.push_back(std::move(file));
        }
        return true;
    }

    bool outputsContained(Project* project, const std::vector<OutputFile>& files, Problem& problem) {
        if (!project || project->getProjectPath().empty()) return true;
        const fs::path root = project->getProjectPath();
        const fs::path assets = assetsRootOf(root, project->getAssetsPath());
        for (const OutputFile& file : files) {
            const std::string shown = file.path.empty() ? projectRelative(root, file.absolute) : file.path;
            const bool lexicallyInside = isAtOrBelow(file.absolute, assets);
            const std::string escape = lexicallyInside ? resolvedEscape(root, assets, file.absolute, shown)
                                                       : shown + " is outside the project's assets directory (" + projectRelative(root, assets) + ").";
            if (!escape.empty()) {
                problem = {"output_invalid", "output." + file.role, escape,
                           lexicallyInside ? "Write it to a folder of the assets directory that is not a link to elsewhere."
                                           : "Write it inside the assets directory."};
                return false;
            }
        }
        return true;
    }

    bool writeOutputsUndoable(Project* project, const std::vector<OutputFile>& files, std::string& error, Problem* problem) {
        if (!project || project->getProjectPath().empty()) {
            error = "No editor project is open to write into (outside the editor, write with writeOutputsDirect).";
            if (problem) *problem = {"write_failed", "output", error, "Open the project in the editor."};
            return false;
        }
        // checked again where they are written: a symbolic link made since the check must not
        // lead a write out of the assets directory
        Problem escape;
        if (!outputsContained(project, files, escape)) {
            error = escape.message;
            if (problem) *problem = escape;
            return false;
        }
        std::vector<WriteFilesCmd::Item> items;
        for (const OutputFile& file : files) items.push_back({file.absolute, file.bytes});
        // ... and before each redo and undo of the step (a link made in between: that step is not
        // applied; a refused redo or undo leaves the history where it is)
        std::vector<OutputFile> where;   // the paths only, not the bytes (the step keeps those)
        for (const OutputFile& file : files) where.push_back({file.role, file.path, file.absolute, std::string()});
        auto guard = [project, where](std::string& why) {
            Problem p;
            if (outputsContained(project, where, p)) return true;
            why = p.message;
            return false;
        };
        // ... and every write, removal and restore beneath the assets directory when it happens:
        // a link planted on the way between the check and the write is never followed
        if (!ProjectFileWrites::writeUndoable(project, std::move(items), error, nullptr, guard, outputsRoot(project))) {
            if (problem) *problem = {"write_failed", "output", error, "Check the path is writable, then try again."};
            return false;
        }
        // (the editor's refresh, ProjectFileWrites::refresh, has the watcher read them again)
        return true;
    }

    fs::path outputsRoot(Project* project) {
        if (!project || project->getProjectPath().empty()) return fs::path();
        return assetsRootOf(project->getProjectPath(), project->getAssetsPath());
    }

    bool writeOutputsDirect(const std::vector<OutputFile>& files, std::string& failedPath, std::string& error, const fs::path& beneath) {
        if (files.empty()) return true;
        std::vector<WriteFilesCmd::Item> items;
        for (const OutputFile& file : files) items.push_back({file.absolute, file.bytes});
        // WriteFilesCmd without a history: all or nothing (a failure puts back what was written
        // before it), every write and rollback beneath the root (util/SafeWrite.h)
        WriteFilesCmd command(std::move(items), nullptr);
        auto failure = std::make_shared<std::string>();
        command.reportFailureTo(failure);
        // beneath `beneath` (the watcher: the assets directory); the CLI (no containment): each
        // file beneath its own folder once resolved, still never through a link planted since
        if (!beneath.empty()) command.writeBeneath(beneath);
        else command.writeBeneathOwnFolders();
        if (command.execute()) return true;
        failedPath = command.getFailedPath().empty() ? jasperengine::editor::FileUtils::pathToUtf8(files.front().absolute) : jasperengine::editor::FileUtils::pathToUtf8(command.getFailedPath());
        error = failure->empty() ? std::string("the files could not be written") : *failure;
        const std::string putBack = "; nothing was changed (the files written before it were put back)";
        if (error.size() >= putBack.size() && error.compare(error.size() - putBack.size(), putBack.size(), putBack) == 0) {
            error = error.substr(0, error.size() - putBack.size()) + "; nothing was written";
        }
        return false;
    }

    // ------------------------------------------------------------------ recipes and staleness

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

    namespace {

        // A path a recipe recorded, back to a file: project-relative, or outside a project
        // relative to the directory of the PNG carrying the recipe
        fs::path recordedFile(const fs::path& projectDir, const fs::path& png, const std::string& recorded) {
            const fs::path path = fs::u8path(recorded);
            if (path.is_absolute()) return path.lexically_normal();
            if (projectDir.empty()) {
                std::error_code ec;
                const fs::path dir = fs::absolute(png, ec).parent_path();
                return ((ec ? png.parent_path() : dir) / path).lexically_normal();
            }
            return (projectDir / path).lexically_normal();
        }

        // ... as a spec path: project-relative, or (outside a project) the file itself
        std::string recordedRelative(const fs::path& projectDir, const fs::path& png, const json& value) {
            if (!value.is_string() || value.get<std::string>().empty()) return std::string();
            return projectRelative(projectDir, recordedFile(projectDir, png, value.get<std::string>()));
        }

    }

    Staleness checkStaleness(const fs::path& projectDir, const fs::path& png) {
        Staleness s;
        const std::string pngName = jasperengine::editor::FileUtils::pathToUtf8(png.filename());
        std::string error;
        RecipeReadError origin = RecipeReadError::None;
        const std::optional<json> recipe = readRecipeHead(png, &error, &origin);
        if (!recipe) {
            // the error's origin decides: a damaged recipe chunk is a garbled recipe; a file that
            // can't be read as a PNG has none
            s.state = origin == RecipeReadError::Chunk ? Staleness::State::Invalid
                      : origin == RecipeReadError::File ? Staleness::State::Unreadable
                                                        : Staleness::State::NoRecipe;
            s.message = s.state == Staleness::State::NoRecipe ? pngName + " was not made by the sprite normal generator (no recipe)."
                                                              : pngName + ": " + error;
            return s;
        }
        if (!validateRecipe(*recipe, error)) {
            s.state = Staleness::State::Invalid;
            s.message = pngName + "'s recipe is garbled: " + error;
            return s;
        }
        const json& source = (*recipe)["source"];
        s.sourcePath = source["path"].get<std::string>();
        s.sourceAbsolute = recordedFile(projectDir, png, s.sourcePath);
        const std::string name = fileName(s.sourcePath);
        std::vector<uint8_t> bytes;
        std::error_code ec;
        if (!fs::is_regular_file(s.sourceAbsolute, ec) || !readFile(s.sourceAbsolute, bytes)) {
            s.state = Staleness::State::Missing;
            s.message = name + " is gone (this map was made from it).";
            return s;
        }
        const std::string recorded = source.contains("sha1") ? source["sha1"].get<std::string>() : std::string();
        if (texsynth::sha1Hex(bytes.data(), bytes.size()) != recorded) {
            s.state = Staleness::State::Changed;
            s.message = name + " changed since this map was made.";
            return s;
        }
        s.state = Staleness::State::Fresh;
        s.message = pngName + " is up to date with " + name + ".";
        return s;
    }

    bool specFromRecipe(const json& recipe, const fs::path& png, const fs::path& projectDir, Spec& spec, std::vector<std::string>& notes,
                        std::string& error) {
        // every type read below is checked first: a garbled recipe is an error, never an exception
        std::string invalid;
        if (!validateRecipe(recipe, invalid)) {
            error = recipe.is_object() && recipe.contains("format") && recipe["format"] == kRecipeFormat ? "the recipe is garbled (" + invalid + ")"
                                                                                                         : "not a sprite normals recipe";
            return false;
        }
        Spec out;
        const json& source = recipe["source"];
        out.source = recordedRelative(projectDir, png, source["path"]);
        if (out.source.empty()) {
            error = "the recipe names no source";
            return false;
        }
        if (source.contains("submesh")) out.submesh = source["submesh"].get<unsigned>();
        if (source.contains("entity")) {
            // a GameObject is not a file: the map comes back from the Sprite's texture
            notes.push_back("It was made from the Sprite " + source["entity"].get<std::string>() + "; its texture " + out.source +
                            " is the source now (pick the GameObject again to use its current frames).");
        }
        // a recipe written before high-resolution support: its rules (and its profile default)
        const int64_t version = recipe["version"].is_number_integer() ? recipe["version"].get<int64_t>() : 1;
        if (version < 2) {
            out.settings.version = 1;
            out.settings.profile = Profile::Rounded;
        }
        if (recipe.contains("settings") && !settingsFromJson(recipe["settings"], out.settings, error)) {
            error = "the recipe's settings are garbled (" + error + ")";
            return false;
        }
        FramesFrom from = FramesFrom::Single;
        const json frames = recipe.contains("frames") ? recipe["frames"] : json::object();
        if (frames.contains("from")) parseFramesFrom(frames["from"].get<std::string>(), from);
        if (from == FramesFrom::Sprite || from == FramesFrom::Given) {
            out.frames.mode = FramesSpec::Mode::Cells;
            out.frames.from = from;
            if (frames.contains("cells")) {
                for (const json& c : frames["cells"]) out.frames.cells.push_back({c[0].get<int>(), c[1].get<int>(), c[2].get<int>(), c[3].get<int>()});
            }
            if (out.frames.cells.empty()) out.frames.mode = FramesSpec::Mode::Auto;
        }
        // the outputs are the set written together, written again in place
        const json outputs = recipe.contains("outputs") ? recipe["outputs"] : json::object();
        out.output.normal = recordedRelative(projectDir, png, outputs.contains("normal") ? outputs["normal"] : json());
        out.output.atlas = recordedRelative(projectDir, png, outputs.contains("atlas") ? outputs["atlas"] : json());
        out.output.height = recordedRelative(projectDir, png, outputs.contains("height") ? outputs["height"] : json());
        const std::string role = recipe.contains("role") ? recipe["role"].get<std::string>() : std::string("normal");
        const std::string self = projectDir.empty() ? jasperengine::editor::FileUtils::pathToGenericUtf8(png) : projectRelative(projectDir, png);
        if (role == "normal" && out.output.normal.empty()) out.output.normal = self;
        if (role == "atlas" && out.output.atlas.empty()) out.output.atlas = self;
        if (role == "height" && out.output.height.empty()) out.output.height = self;
        out.overwrite = true;
        spec = out;
        return true;
    }

    // ------------------------------------------------------------------ the scene

    LightCheck lightCheck(const Scene* scene) {
        LightCheck check;
        if (!scene) return check;
        Scene* s = mutableScene(scene);
        check.ambientIntensity = s->getAmbientLight2DIntensity();
        check.ambientBrightness = ambientLight2DBrightness(*s);
        auto lights = s->getComponentArray<Light2DComponent>();
        const size_t count = lights ? lights->size() : 0;
        for (size_t i = 0; i < count; i++) {
            const Light2DComponent* light = lights->findComponentFromIndex(i);
            if (!light || !light->enabled) continue;
            check.lights++;
            if (light->height > 0.0f) check.lightsWithHeight++;
        }
        return check;
    }

    int framePixels(const std::vector<Cell>& cells, int width, int height) {
        if (cells.empty()) return std::max(width, height);
        int side = 0;
        for (const Cell& c : cells) side = std::max({side, c.width, c.height});
        return side;
    }

    LowLights lowLights(const Scene* scene, Entity entity, int framePixels) {
        LowLights out;
        if (!scene) return out;
        Scene* s = mutableScene(scene);
        const bool hasEntity = entity != NULL_ENTITY && s->isEntityCreated(entity);
        Vector3 scale(1.0f, 1.0f, 1.0f);
        if (const Transform* t = hasEntity ? s->findComponent<Transform>(entity) : nullptr) scale = t->worldScale;
        const float sx = std::fabs(scale.x), sy = std::fabs(scale.y);
        const SpriteComponent* sprite = hasEntity ? s->findComponent<SpriteComponent>(entity) : nullptr;
        if (sprite && sprite->width > 0 && sprite->height > 0) {
            const Vector2 size = spriteWorldSize(*sprite);
            out.spriteSize = std::max(size.x * sx, size.y * sy);
        } else {
            const TilemapComponent* tilemap = hasEntity ? s->findComponent<TilemapComponent>(entity) : nullptr;
            const float ppu = resolvePixelsPerUnit(sprite ? sprite->pixelsPerUnit : tilemap ? tilemap->pixelsPerUnit : 0.0f);
            out.spriteSize = static_cast<float>(framePixels) / ppu * std::max(sx, sy);
        }
        if (!(out.spriteSize > 0.0f) || !std::isfinite(out.spriteSize)) return out;
        // whole units for a big sprite, hundredths for one of a few units (a high Pixels Per Unit)
        const float step = out.spriteSize >= 10.0f ? 1.0f : 0.01f;
        out.raiseTo = std::ceil(out.spriteSize / step - 1e-3f) * step;
        auto lights = s->getComponentArray<Light2DComponent>();
        for (size_t i = 0; lights && i < lights->size(); i++) {
            const Light2DComponent* light = lights->findComponentFromIndex(i);
            if (!light || !light->enabled || !(light->height > 0.0f) || light->height >= 0.5f * out.spriteSize) continue;
            out.lights.push_back(lights->getEntity(i));
        }
        return out;
    }

    std::vector<Problem> sceneWarnings(const Scene* scene, Entity entity, const Settings& settings, int normalWidth, int normalHeight,
                                       const std::vector<Delay>& delays, bool willApply, unsigned submesh, int framePixels) {
        std::vector<Problem> warnings;
        int zero = 0;
        for (const Delay& d : delays) {
            if (std::round(d.ms()) < 10.0) zero++;   // as Delay::playbackMs() replaces them
        }
        if (scene) {
            const LightCheck lights = lightCheck(scene);
            if (lights.lights == 0) {
                add(warnings, "no_light", "", "This scene has no Light 2D.", "Create a Light 2D (new lights get Height 64).");
            } else if (lights.lightsWithHeight == 0) {
                add(warnings, "no_light_height", "", "No Light 2D in this scene has a height above 0, so normal maps don't change the lighting.",
                    "Set a Light 2D's Height (64 is a good start).");
            }
            const LowLights low = lowLights(scene, entity, framePixels > 0 ? framePixels : std::max(normalWidth, normalHeight));
            if (!low.lights.empty()) {
                const size_t named = std::min<size_t>(low.lights.size(), 3);
                std::string names;
                for (size_t i = 0; i < named; i++) names += (i ? ", \"" : "\"") + scene->getEntityName(low.lights[i]) + "\"";
                if (low.lights.size() > named) names += " and " + std::to_string(low.lights.size() - named) + " more";
                const bool one = low.lights.size() == 1;
                add(warnings, "light_low", "",
                    std::string(one ? "Light 2D " : "Light 2Ds ") + names + (one ? " sits" : " sit") + " lower than half the sprite's larger side (" +
                        trimmed(low.spriteSize) + " world units), so " + (one ? "it lights" : "they light") + " it from the side only.",
                    std::string("Set ") + (one ? "its" : "their") + " Height to at least " + trimmed(low.raiseTo) + " (the sprite's larger side).");
            }
            if (lights.ambientBrightness >= kBrightAmbient2D) {
                add(warnings, "ambient_high", "",
                    "The scene's 2D ambient brightness is " + trimmed(lights.ambientBrightness) +
                        " (intensity times its brightest colour channel), which washes out normal maps.",
                    "Darken Ambient Light 2D: colour (" + trimmed(kDarkAmbient2DColor[0]) + ", " + trimmed(kDarkAmbient2DColor[1]) + ", " +
                        trimmed(kDarkAmbient2DColor[2]) + ") at intensity 1, or a lower intensity.");
            }
        }
        Scene* s = mutableScene(scene);
        MeshComponent* mesh = scene && entity != NULL_ENTITY && scene->isEntityCreated(entity) ? s->findComponent<MeshComponent>(entity) : nullptr;
        if (mesh && submesh < submeshCount(s, entity, *mesh)) {
            const Material& material = mesh->submeshes[submesh].material;
            const Texture& base = material.baseColorTexture;
            int w = 0, h = 0;
            // applying an animation makes its atlas the base texture (and a still's mismatch is refused by apply)
            const bool replacesTexture = willApply && !delays.empty();
            if (!replacesTexture && !base.getPath().empty() && texsynth::probeImage(textureFile(base.getPath()), w, h) &&
                (w != normalWidth || h != normalHeight)) {
                add(warnings, "size_mismatch", "",
                    "The sprite's texture is " + std::to_string(w) + "x" + std::to_string(h) + " but the normal map is " + std::to_string(normalWidth) +
                        "x" + std::to_string(normalHeight) + "; they must match pixel for pixel.",
                    "Generate from the sprite's own texture (source: the GameObject).");
            }
            // apply copies the base texture's sampling: only a map set some other way can differ
            const Texture& normal = material.normalTexture;
            if (!willApply && !normal.getPath().empty() &&
                (normal.getMinFilter() != base.getMinFilter() || normal.getMagFilter() != base.getMagFilter() || normal.getWrapU() != base.getWrapU() ||
                 normal.getWrapV() != base.getWrapV())) {
                add(warnings, "filter_mismatch", "", "The Normal Texture is filtered differently from the Base Texture (pixels will smear at edges).",
                    "Apply the map (it copies the filter and wrap), or give the Normal Texture the Base Texture's filter and wrap.");
            }
            // material links live in the editor's project
            if (Project* project = editorProject()) {
                for (SceneProject& sp : project->getScenes()) {
                    if (sp.scene != scene) continue;
                    const std::string linked = project->getMaterialFilePath(sp.id, entity, submesh);
                    if (!linked.empty()) {
                        add(warnings, "material_linked", "", "The sprite's material comes from " + linked + "; the scene's copy changed, the file did not.",
                            "Save the material from the Inspector to keep the map in " + linked + ".");
                    }
                }
            }
            if (!material.shader.empty()) {
                const std::shared_ptr<const ShaderManifest> manifest = mcp::McpValues::findManifest(material.shader);
                if (manifest && !Catalog::shaderReadsMaterialNormalMap(manifest.get(), material.shaderFeatures)) {
                    add(warnings, "shader_ignores_normals", "", "The sprite's material shader " + material.shader + " does not sample a normal map.",
                        "Use the built-in sprite material or library:sprite.");
                }
            }
        }
        if (scene && entity != NULL_ENTITY && delays.size() > ARRAY_INDEX_LIMIT) {
            add(warnings, "too_many_frames", "",
                "The animation has " + std::to_string(delays.size()) + " frames; a Sprite holds at most " + std::to_string(ARRAY_INDEX_LIMIT) + ".",
                "Trim the animation, or use the atlas with your own frames.");
        }
        if (scene && entity != NULL_ENTITY && settings.directx) {
            add(warnings, "directx_in_engine", "", "The map uses DirectX green (down); this engine reads OpenGL (green up), so lights look upside down.",
                "Turn directx off for this engine's sprites.");
        }
        if (zero > 0) {
            add(warnings, "zero_delay", "", std::to_string(zero) + " frames have a delay under 10 ms; they play at 100 ms, as browsers do.",
                "Set the Sprite Animation's frame times if another rate is meant.");
        }
        return warnings;
    }

    // ------------------------------------------------------------------ generation

    Result generatePrepared(Prepared& prepared, const Control& control) {
        Result result = generatePainted(prepared.request, prepared.paint, control);
        result.warnings.insert(result.warnings.begin(), prepared.paintWarnings.begin(), prepared.paintWarnings.end());
        return result;
    }

    // ------------------------------------------------------------------ SpriteNormalsRun

    std::shared_ptr<SpriteNormalsRun> SpriteNormalsRun::start(Prepared prepared, int threads, Finished onFinished) {
        std::shared_ptr<SpriteNormalsRun> run(new SpriteNormalsRun());
        run->prep = std::move(prepared);
        if (threads > 0) run->prep.request.threads = threads;
        run->startGeneration = texsynth::projectGeneration();
        const std::shared_ptr<RunRegistry> shared = registry();
        {
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->runs.erase(std::remove_if(shared->runs.begin(), shared->runs.end(), [](const std::weak_ptr<SpriteNormalsRun>& w) { return w.expired(); }),
                               shared->runs.end());
            shared->runs.push_back(run);
            shared->active++;
        }
        auto finish = [run, onFinished, shared](Result result) {
            run->res = std::move(result);
            run->done = true;
            if (onFinished && !shared->shuttingDown) {
                try {
                    onFinished(*run);
                } catch (...) {
                    // a callback's failure must not take the editor down from a worker thread
                }
            }
            {
                std::lock_guard<std::mutex> lock(shared->mutex);
                shared->active--;
            }
            shared->done.notify_all();
        };
        try {
            std::thread([run, finish]() {
                Result result;
                try {
                    Control control;
                    control.cancel = &run->cancelFlag;
                    control.onProgress = [run](const Progress& p) {
                        std::lock_guard<std::mutex> lock(run->mutex);
                        run->last = p;
                    };
                    // generate() reports out of memory as Status::Failed; anything else is caught here
                    result = generatePrepared(run->prep, control);
                } catch (const std::exception& e) {
                    result = failedResult(std::string("The generation failed: ") + e.what() + ".");
                } catch (...) {
                    result = failedResult("The generation failed.");
                }
                finish(std::move(result));
            }).detach();
        } catch (const std::system_error& e) {
            // no thread: the run ends at once, failed (its callback on this thread)
            finish(failedResult(std::string("Could not start a worker thread: ") + e.what() + "."));
        }
        return run;
    }

    void SpriteNormalsRun::cancelRunning() {
        const std::shared_ptr<RunRegistry> shared = registry();
        std::lock_guard<std::mutex> lock(shared->mutex);
        for (const std::weak_ptr<SpriteNormalsRun>& w : shared->runs) {
            if (std::shared_ptr<SpriteNormalsRun> run = w.lock()) run->cancel();
        }
    }

    bool SpriteNormalsRun::cancelAll(double timeoutSeconds) {
        const std::shared_ptr<RunRegistry> shared = registry();
        shared->shuttingDown = true;
        cancelRunning();
        // generate() checks the flag before each island or cell: runs end quickly
        std::unique_lock<std::mutex> lock(shared->mutex);
        return shared->done.wait_for(lock, std::chrono::duration<double>(timeoutSeconds), [&shared]() { return shared->active == 0; });
    }

    Progress SpriteNormalsRun::progress() const {
        std::lock_guard<std::mutex> lock(mutex);
        return last;
    }

}
