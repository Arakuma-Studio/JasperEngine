// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SpriteNormalsCli.h"
#include "util/UiItemLocator.h"
#include "window/widget/InputTextContextMenu.h"

#include "SpriteMedia.h"
#include "SpriteNormals.h"
#include "SpriteNormalsPaint.h"
#include "SpriteNormalsPreview.h"
#include "SpriteNormalsRecipe.h"
#include "SpriteNormalsSession.h"

#include "AppSettings.h"
#include "EditorHost.h"
#include "JasperEngine.h"
#include "Out.h"
#include "Platform.h"
#include "Project.h"
#include "System.h"
#include "cli/CommandLine.h"
#include "pool/ShaderPool.h"
#include "shader/ShaderBuilder.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/TilemapComponent.h"
#include "subsystem/MeshSystem.h"
#include "texsynth/SynthesisRecipe.h"
#include "util/EntityPath.h"
#include "util/ProjectChecks.h"

#include "json.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <set>
#include <sstream>
#include <vector>
#include "util/FileUtils.h"

#if defined(_WIN32)
    #include <fcntl.h>
    #include <io.h>
#else
    #include <fcntl.h>
    #include <unistd.h>
#endif

namespace fs = std::filesystem;
using nlohmann::json;

namespace jasperengine::editor::spritenormals {

    namespace {

        const char* const PREFIX = "generate-sprite-normals: ";

        struct Options {
            std::vector<fs::path> sources;       // absolute (symlinks resolved)
            std::string sprite;                  // --sprite <GameObject>
            std::string scene;
            fs::path project;
            Preset preset = Preset::Default;
            json overrides = json::object();     // settings flags, as settingsFromJson reads them
            std::vector<Region> regions;
            bool regionsGiven = false;
            bool directx = false;
            // frames
            bool gridGiven = false;
            Grid grid;
            bool cellsGiven = false;
            std::vector<Cell> cells;
            bool noFrames = false;
            // output
            std::string output;                  // as given
            std::string outputDir;
            std::string atlas;
            bool heightMap = false;
            bool framesJson = false;
            std::string previewOut;
            // paint (Phase 5): --paint-json ops (the MCP `paint` shape) and --paint-mode
            std::vector<PaintOp> paint;
            std::vector<Problem> paintProblems;   // invalid ops: reported as invalid_paint (exit 2)
            bool paintGiven = false;
            PaintMode paintMode = PaintMode::Keep;
            bool overwrite = false;
            bool dryRun = false;
            int threads = 0;
            std::string format = "text";
            bool quiet = false;
            bool help = false;
        };

        // The run's errors and exit code
        struct Run {
            const Options* o = nullptr;
            std::ostream* err = nullptr;
            json errors = json::array();
            int code = 0;

            int fail(int exit, const std::string& kind, const std::string& message, const std::string& fix, const std::string& source = "") {
                json e = {{"kind", kind}, {"message", message}, {"fix", fix}};
                if (!source.empty()) e["source"] = source;
                errors.push_back(e);
                *err << PREFIX << kind << ": " << message << (fix.empty() ? std::string() : " (fix: " + fix + ")") << "\n";
                code = std::max(code, exit);
                return exit;
            }
        };

        // ---------------------------------------------------------------- parsing

        bool parseFloatText(const std::string& text, double& value) {
            if (text.empty()) return false;
            char* end = nullptr;
            const double v = std::strtod(text.c_str(), &end);
            if (*end != 0 || !std::isfinite(v)) return false;
            value = v;
            return true;
        }

        bool parseIntText(const std::string& text, int& value) {
            if (text.empty()) return false;
            char* end = nullptr;
            const long v = std::strtol(text.c_str(), &end, 10);
            if (*end != 0 || v < 0 || v > 1000000L) return false;
            value = static_cast<int>(v);
            return true;
        }

        // "x,y"
        bool parsePair(const std::string& text, int& x, int& y) {
            const size_t comma = text.find(',');
            return comma != std::string::npos && parseIntText(text.substr(0, comma), x) && parseIntText(text.substr(comma + 1), y);
        }

        // "WxH" or "N"
        bool parseSize(const std::string& text, int& w, int& h) {
            const size_t x = text.find_first_of("xX");
            if (x == std::string::npos) {
                if (!parseIntText(text, w)) return false;
                h = w;
            } else if (!parseIntText(text.substr(0, x), w) || !parseIntText(text.substr(x + 1), h)) {
                return false;
            }
            return w > 0 && h > 0;
        }

        // colors=#rrggbb[,#rrggbb...][;tolerance=x][;detail=x][;lift=px][;flat]
        bool parseRegion(const std::string& text, Region& region, std::string& error) {
            std::stringstream parts(text);
            std::string part;
            bool colors = false;
            while (std::getline(parts, part, ';')) {
                if (part.empty()) continue;
                if (part == "flat") {
                    region.flat = true;
                    continue;
                }
                const size_t eq = part.find('=');
                const std::string key = part.substr(0, eq);
                const std::string value = eq == std::string::npos ? std::string() : part.substr(eq + 1);
                if (key == "colors") {
                    std::stringstream list(value);
                    std::string c;
                    while (std::getline(list, c, ',')) {
                        uint32_t rgb = 0;
                        if (!parseColor(c, rgb)) {
                            error = "--region colour '" + c + "' is not #rrggbb";
                            return false;
                        }
                        region.colors.push_back(rgb);
                    }
                    colors = !region.colors.empty();
                } else if (key == "tolerance" || key == "detail" || key == "lift") {
                    double v = 0.0;
                    if (!parseFloatText(value, v)) {
                        error = "--region " + key + " '" + value + "' is not a number";
                        return false;
                    }
                    (key == "tolerance" ? region.tolerance : key == "detail" ? region.detail : region.lift) = static_cast<float>(v);
                } else {
                    error = "--region has an unknown part '" + part + "' (colors=, tolerance=, detail=, lift=, flat)";
                    return false;
                }
            }
            if (!colors) {
                error = "--region needs colors=#rrggbb[,#rrggbb...]";
                return false;
            }
            return true;
        }

        bool readCells(const std::string& value, std::vector<Cell>& cells, std::string& error) {
            std::string text;
            if (value == "-") {
                text.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
            } else {
                std::ifstream file(CommandLine::absoluteArgPath(fs::u8path(value)), std::ios::binary);
                if (!file) {
                    error = "--cells-json: can't read " + value;
                    return false;
                }
                text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            }
            const json j = json::parse(text, nullptr, false);
            // a recipe's frames.cells, or a whole frames object / recipe holding them
            const json* list = &j;
            if (j.is_object() && j.contains("cells")) list = &j["cells"];
            else if (j.is_object() && j.contains("frames") && j["frames"].is_object() && j["frames"].contains("cells")) list = &j["frames"]["cells"];
            if (!list->is_array() || list->empty()) {
                error = "--cells-json must hold a JSON array [[x, y, w, h], ...]";
                return false;
            }
            for (const json& c : *list) {
                bool ok = c.is_array() && c.size() == 4;
                for (size_t k = 0; ok && k < 4; k++) ok = c[k].is_number_integer() && c[k].get<int>() >= 0;
                if (!ok) {
                    error = "--cells-json: " + c.dump() + " is not [x, y, w, h] in pixels";
                    return false;
                }
                cells.push_back({c[0].get<int>(), c[1].get<int>(), c[2].get<int>(), c[3].get<int>()});
            }
            return true;
        }

        // --paint-json: a file, "-" (stdin), or inline JSON when it starts with '['
        bool readPaintJson(const std::string& value, Options& o, std::string& error) {
            std::string text;
            if (!value.empty() && value[0] == '[') {
                text = value;
            } else if (value == "-") {
                text.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
            } else {
                std::ifstream file(CommandLine::absoluteArgPath(fs::u8path(value)), std::ios::binary);
                if (!file) {
                    error = "--paint-json: can't read " + value;
                    return false;
                }
                text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            }
            const json j = json::parse(text, nullptr, false);
            if (j.is_discarded()) {
                error = "--paint-json is not JSON (a JSON array of paint operations, as generate_sprite_normals' paint)";
                return false;
            }
            o.paintGiven = true;
            paintOpsFromJson(j, o.paint, o.paintProblems);
            return true;
        }

        bool parseArgs(int argc, char** argv, Options& o, std::string& error) {
            for (int i = 1; i < argc; i++) {
                const std::string arg = argv[i];
                std::string value;
                auto need = [&]() {
                    if (i + 1 >= argc) {
                        error = "missing value for " + arg;
                        return false;
                    }
                    value = argv[++i];
                    return true;
                };
                auto number = [&](const char* key) {
                    if (!need()) return false;
                    double v = 0.0;
                    if (!parseFloatText(value, v)) {
                        error = "invalid " + arg + " value '" + value + "' (a number)";
                        return false;
                    }
                    o.overrides[key] = v;
                    return true;
                };
                auto gridInt = [&](int& target) {
                    if (!need()) return false;
                    if (!parseIntText(value, target)) {
                        error = "invalid " + arg + " value '" + value + "' (a whole number >= 0)";
                        return false;
                    }
                    o.gridGiven = true;
                    return true;
                };
                auto gridPair = [&](int& x, int& y) {
                    if (!need()) return false;
                    if (!parsePair(value, x, y)) {
                        error = "invalid " + arg + " value '" + value + "' (x,y pixels)";
                        return false;
                    }
                    o.gridGiven = true;
                    return true;
                };
                if (arg == "-h" || arg == "--help") {
                    o.help = true;
                } else if (arg == "-s" || arg == "--source") {
                    if (!need()) return false;
                    o.sources.push_back(fs::u8path(value));
                } else if (arg == "--sprite") {
                    if (!need()) return false;
                    o.sprite = value;
                } else if (arg == "--scene") {
                    if (!need()) return false;
                    o.scene = value;
                } else if (arg == "-p" || arg == "--project") {
                    if (!need()) return false;
                    o.project = CommandLine::projectArgPath(fs::u8path(value));
                } else if (arg == "--preset") {
                    if (!need()) return false;
                    if (!parsePreset(value, o.preset)) {
                        error = "unknown preset '" + value + "' (" + presetNames() + ")";
                        return false;
                    }
                } else if (arg == "--strength") {
                    if (!number("strength")) return false;
                } else if (arg == "--detail") {
                    if (!number("detail")) return false;
                } else if (arg == "--smoothing") {
                    if (!number("smoothing")) return false;
                } else if (arg == "--volume") {
                    if (!number("volume")) return false;
                } else if (arg == "--softness") {
                    if (!number("softness")) return false;
                } else if (arg == "--alpha-threshold") {
                    if (!number("alpha_threshold")) return false;
                } else if (arg == "--radius") {
                    if (!need()) return false;
                    double v = 0.0;
                    if (value == "auto") o.overrides["radius"] = "auto";
                    else if (parseFloatText(value, v)) o.overrides["radius"] = v;
                    else {
                        error = "invalid --radius value '" + value + "' (auto or pixels)";
                        return false;
                    }
                } else if (arg == "--profile") {
                    if (!need()) return false;
                    Profile p;
                    if (!parseProfile(value, p)) {
                        error = "unknown profile '" + value + "' (auto, rounded, dome, bevel, inflate)";
                        return false;
                    }
                    o.overrides["profile"] = value;
                } else if (arg == "--resolution-scale") {
                    if (!need()) return false;
                    ResolutionScale r;
                    if (!parseResolutionScale(value, r)) {
                        error = "unknown resolution scale '" + value + "' (auto, off)";
                        return false;
                    }
                    o.overrides["resolution_scale"] = value;
                } else if (arg == "--soft-edge") {
                    if (!need()) return false;
                    SoftEdge e;
                    if (!parseSoftEdge(value, e)) {
                        error = "unknown soft edge '" + value + "' (auto, solid)";
                        return false;
                    }
                    o.overrides["soft_edge"] = value;
                } else if (arg == "--settings-version") {
                    if (!need()) return false;
                    if (value != "1" && value != "2") {
                        error = "invalid --settings-version value '" + value + "' (1: as before high-resolution support, 2)";
                        return false;
                    }
                    o.overrides["version"] = value == "1" ? 1 : 2;
                } else if (arg == "--edges") {
                    if (!need()) return false;
                    Edges e;
                    if (!parseEdges(value, e)) {
                        error = "unknown edges '" + value + "' (transparent, wrap, extend)";
                        return false;
                    }
                    o.overrides["edges"] = value;
                } else if (arg == "--ignore-outline") {
                    o.overrides["ignore_outline"] = true;
                } else if (arg == "--quantize" || arg == "--quantize-tilts") {
                    // Toon steps: whole numbers (the ranges are validateSettings')
                    if (!need()) return false;
                    int v = 0;
                    if (!parseIntText(value, v)) {
                        error = "invalid " + arg + " value '" + value + "' (" + (arg == "--quantize" ? "0 (off), 4, 6, 8, 12 or 16" : "1..4") + ")";
                        return false;
                    }
                    o.overrides["quantize"][arg == "--quantize" ? "directions" : "tilts"] = v;
                } else if (arg == "--quantize-flat" || arg == "--quantize-rotation") {
                    if (!need()) return false;
                    double v = 0.0;
                    if (!parseFloatText(value, v)) {
                        error = "invalid " + arg + " value '" + value + "' (degrees)";
                        return false;
                    }
                    o.overrides["quantize"][arg == "--quantize-flat" ? "flat_deg" : "rotation_deg"] = v;
                } else if (arg == "--region") {
                    if (!need()) return false;
                    Region region;
                    if (!parseRegion(value, region, error)) return false;
                    o.regions.push_back(region);
                    o.regionsGiven = true;
                } else if (arg == "--directx") {
                    o.directx = true;
                } else if (arg == "--frame-size") {
                    if (!need()) return false;
                    if (!parseSize(value, o.grid.tileWidth, o.grid.tileHeight)) {
                        error = "invalid --frame-size value '" + value + "' (WxH or N pixels)";
                        return false;
                    }
                    o.gridGiven = true;
                } else if (arg == "--columns") {
                    if (!gridInt(o.grid.columns)) return false;
                } else if (arg == "--rows") {
                    if (!gridInt(o.grid.rows)) return false;
                } else if (arg == "--count") {
                    if (!gridInt(o.grid.count)) return false;
                } else if (arg == "--offset") {
                    if (!gridPair(o.grid.offsetX, o.grid.offsetY)) return false;
                } else if (arg == "--padding") {
                    if (!gridPair(o.grid.paddingX, o.grid.paddingY)) return false;
                } else if (arg == "--cells-json") {
                    if (!need()) return false;
                    if (!readCells(value, o.cells, error)) return false;
                    o.cellsGiven = true;
                } else if (arg == "--no-frames") {
                    o.noFrames = true;
                } else if (arg == "-o" || arg == "--output") {
                    if (!need()) return false;
                    o.output = value;
                } else if (arg == "--output-dir") {
                    if (!need()) return false;
                    o.outputDir = value;
                } else if (arg == "--atlas") {
                    if (!need()) return false;
                    o.atlas = value;
                } else if (arg == "--height-map") {
                    o.heightMap = true;
                } else if (arg == "--frames-json") {
                    o.framesJson = true;
                } else if (arg == "--paint-json") {
                    if (!need() || !readPaintJson(value, o, error)) return false;
                } else if (arg == "--paint-mode") {
                    if (!need()) return false;
                    if (!parsePaintMode(value, o.paintMode)) {
                        error = "unknown --paint-mode '" + value + "' (keep, replace, clear)";
                        return false;
                    }
                } else if (arg == "--preview-out") {
                    if (!need()) return false;
                    o.previewOut = value;
                } else if (arg == "--overwrite") {
                    o.overwrite = true;
                } else if (arg == "--dry-run") {
                    o.dryRun = true;
                } else if (arg == "--threads") {
                    if (!need()) return false;
                    if (!parseIntText(value, o.threads)) {
                        error = "invalid --threads value '" + value + "' (0 = all cores)";
                        return false;
                    }
                } else if (arg == "--format") {
                    if (!need()) return false;
                    if (value != "text" && value != "json") {
                        error = "--format must be text or json";
                        return false;
                    }
                    o.format = value;
                } else if (arg == "-q" || arg == "--quiet") {
                    o.quiet = true;
                } else {
                    error = "unknown option '" + arg + "'";
                    return false;
                }
            }
            if (o.help) return true;
            if (o.sources.empty() && o.sprite.empty()) {
                error = "give --source <image> (or --project <dir> --sprite <GameObject>)";
                return false;
            }
            if (!o.sprite.empty() && o.project.empty()) {
                error = "--sprite needs --project (the project whose scene holds the GameObject)";
                return false;
            }
            if (!o.sprite.empty() && !o.sources.empty()) {
                error = "--sprite and --source can't be combined (the Sprite's texture is the source)";
                return false;
            }
            const size_t count = o.sources.size() + (o.sprite.empty() ? 0 : 1);
            if (count > 1 && !o.output.empty()) {
                error = "-o/--output names one normal map: with several sources use --output-dir";
                return false;
            }
            if (count > 1 && !o.previewOut.empty()) {
                error = "--preview-out takes one source";
                return false;
            }
            if (count > 1 && !o.atlas.empty()) {
                error = "--atlas takes one source";
                return false;
            }
            if (count > 1 && o.paintGiven) {
                error = "--paint-json paints one normal map: give one source per run";
                return false;
            }
            if (!o.output.empty() && !o.outputDir.empty()) {
                error = "-o/--output and --output-dir can't be combined";
                return false;
            }
            if (o.cellsGiven && (o.gridGiven || o.noFrames)) {
                error = "--cells-json can't be combined with the grid options (--frame-size, --columns, --rows, --count, --offset, --padding) or --no-frames";
                return false;
            }
            if (o.noFrames && o.gridGiven) {
                error = "--no-frames can't be combined with the grid options";
                return false;
            }
            if (o.gridGiven && !(o.grid.tileWidth > 0) && !(o.grid.columns > 0 && o.grid.rows > 0)) {
                error = "a frame grid needs --frame-size, or --columns and --rows";
                return false;
            }
            return true;
        }

        // ---------------------------------------------------------------- the engine (--sprite only)

        struct EngineRuntimeGuard {
            ~EngineRuntimeGuard() {
                ::jasperengine::ShaderPool::setShaderBuilder(nullptr);
                ShaderBuilder::requestShutdown();
                ::jasperengine::Engine::systemShutdown();
            }
        };

        struct EditorHostOverrideGuard {
            EditorHost* previousHost = nullptr;
            explicit EditorHostOverrideGuard(EditorHost* host): previousHost(&getEditorHost()) { setEditorHost(host); }
            ~EditorHostOverrideGuard() { setEditorHost(previousHost); }
        };

        // The engine logs to stdout: while it runs, stdout goes to stderr (nowhere with -q), so
        // stdout carries only the result
        class StdoutToStderr {
        public:
            explicit StdoutToStderr(bool discard) {
                std::cout.flush();
                std::fflush(stdout);
#if defined(_WIN32)
                saved = _dup(1);
                const int target = discard ? _open("NUL", _O_WRONLY) : _dup(2);
                if (target >= 0) {
                    _dup2(target, 1);
                    _close(target);
                }
#else
                saved = dup(1);
                const int target = discard ? open("/dev/null", O_WRONLY) : dup(2);
                if (target >= 0) {
                    dup2(target, 1);
                    close(target);
                }
#endif
            }
            ~StdoutToStderr() {
                std::cout.flush();
                std::fflush(stdout);
                if (saved < 0) return;
#if defined(_WIN32)
                _dup2(saved, 1);
                _close(saved);
#else
                dup2(saved, 1);
                close(saved);
#endif
            }
            StdoutToStderr(const StdoutToStderr&) = delete;
            StdoutToStderr& operator=(const StdoutToStderr&) = delete;

        private:
            int saved = -1;
        };

        // --sprite: the project loaded with the headless engine, the scene opened, the GameObject
        // found; `body` runs with them (the whole generation: the Specs name the GameObject, so
        // recipes record kind sprite and its entity) and the engine is shut down when it returns.
        // The only engine start of the command; stdout goes to stderr meanwhile.
        int withSprite(int argc, char** argv, const Options& o, Run& state, const std::function<int(SceneProject&, Entity)>& body) {
            StdoutToStderr redirect(o.quiet);
            EditorHostOverrideGuard hostOverride(nullptr);
            Out::setConsoleWindow(nullptr);
            AppSettings::initialize();
            Project project;
            auto platform = std::make_unique<Platform>(&project);
            ::jasperengine::Engine::systemInit(argc, argv, platform.get());
            EngineRuntimeGuard engineRuntime;
            ::jasperengine::Engine::pauseGameEvents(true);
            ::jasperengine::Engine::setAsyncLoading(false);
            const std::string fix = "give a Sprite, Polygon or Tilemap GameObject of the scene (path or unique name) and its --scene";
            // reading changes no project file
            project.getAssetDatabase().setReadOnly(true);
            // nor does it take the one-editor lock (R10): it reads the scene and writes only the
            // normal map images, which an editor that has the project open imports as any other
            // new file (the CLI runs beside it, as `check` does)
            project.setSkipProjectLock(true);
            if (!project.loadProject(o.project, false)) return state.fail(2, "usage", "the project could not be loaded: " + jasperengine::editor::FileUtils::pathToUtf8(o.project), "give a project directory or its project.yaml");
            ::jasperengine::Engine::setAsyncLoading(false);
            SceneProject* found = nullptr;
            std::string names;
            for (SceneProject& sp : project.getScenes()) {
                if (sp.filepath.empty()) continue;
                names += (names.empty() ? "" : ", ") + sp.name + " (" + jasperengine::editor::FileUtils::pathToGenericUtf8(sp.filepath) + ")";
                if (o.scene.empty() ? sp.id == project.getStartSceneId()
                                    : (sp.name == o.scene || jasperengine::editor::FileUtils::pathToUtf8(sp.filepath.filename()) == o.scene ||
                                       jasperengine::editor::FileUtils::pathToGenericUtf8(sp.filepath) == o.scene ||
                                       CommandLine::absoluteArgPath(fs::u8path(o.scene)) ==
                                           CommandLine::absoluteArgPath(sp.filepath.is_absolute() ? sp.filepath : project.getProjectPath() / sp.filepath))) {
                    found = &sp;
                }
            }
            if (!found) {
                return state.fail(2, "usage", (o.scene.empty() ? std::string("the project has no start scene") : "no scene '" + o.scene + "'") + " (scenes: " +
                                                  (names.empty() ? std::string("none") : names) + ")",
                                  fix);
            }
            const uint32_t id = found->id;
            if (!found->scene && !project.loadScene(found->filepath, false, false, true)) return state.fail(2, "usage", "the scene " + found->name + " could not be loaded", fix);
            ::jasperengine::Engine::setAsyncLoading(false);
            SceneProject* sp = project.getScene(id);
            if (!sp || !sp->scene) return state.fail(2, "usage", "the scene could not be opened", fix);
            Entity entity = EntityPath::find(*sp, o.sprite);
            if (entity == NULL_ENTITY) {
                const std::vector<Entity> named = EntityPath::findByName(sp->scene, sp->entities, o.sprite);
                if (named.size() != 1) {
                    return state.fail(2, "usage", named.empty() ? "no GameObject '" + o.sprite + "' in scene " + sp->name
                                                                : "several GameObjects are named '" + o.sprite + "' in scene " + sp->name + ": give its path",
                                      fix);
                }
                entity = named[0];
            }
            return body(*sp, entity);
        }

        // ---------------------------------------------------------------- paths

        bool below(const fs::path& path, const fs::path& dir) {
            const fs::path rel = path.lexically_normal().lexically_relative(dir.lexically_normal());
            return !rel.empty() && *rel.begin() != ".." && !rel.is_absolute();
        }

        // A path argument made absolute (against the current directory): as named when it lies in
        // the --project (lexically: a symbolic link in it, its assets directory one too, keeps its
        // project path; the session's checks resolve links themselves), else canonical
        fs::path argPath(const Options& o, const fs::path& path) {
            if (!o.project.empty() && !path.empty()) {
                fs::path lexical = (path.is_absolute() ? path : CommandLine::invocationDirectory() / path).lexically_normal();
                if (!lexical.has_filename() && lexical.has_relative_path()) lexical = lexical.parent_path();
                if (below(lexical, o.project)) return lexical;
            }
            return CommandLine::absoluteArgPath(path);
        }

        // An input argument: absolute, relative to the current directory, else (with a project)
        // to the project
        fs::path inputPath(const Options& o, const fs::path& given) {
            if (given.is_absolute()) return argPath(o, given);
            const fs::path fromCwd = argPath(o, given);
            std::error_code ec;
            if (fs::exists(fromCwd, ec) || o.project.empty()) return fromCwd;
            const fs::path fromProject = argPath(o, o.project / given);
            return fs::exists(fromProject, ec) ? fromProject : fromCwd;
        }

        // An output argument: absolute, relative to the current directory (when it is inside the
        // project, or without one), else to the project
        fs::path outputPath(const Options& o, const std::string& given) {
            const fs::path path = fs::u8path(given);
            if (path.is_absolute() || o.project.empty() || below(CommandLine::invocationDirectory(), o.project)) return argPath(o, path);
            return argPath(o, o.project / path);
        }

        // As the user would type it back: project-relative with --project, else relative to the
        // current directory when below it, else absolute
        std::string shown(const Options& o, const fs::path& absolute) {
            if (absolute.empty()) return std::string();
            if (!o.project.empty() && below(absolute, o.project)) return jasperengine::editor::FileUtils::pathToGenericUtf8(absolute.lexically_normal().lexically_relative(o.project));
            if (o.project.empty() && below(absolute, CommandLine::invocationDirectory())) {
                return jasperengine::editor::FileUtils::pathToGenericUtf8(absolute.lexically_normal().lexically_relative(CommandLine::invocationDirectory()));
            }
            return jasperengine::editor::FileUtils::pathToGenericUtf8(absolute.lexically_normal());
        }

        // A spec path: project-relative with a project, else absolute
        std::string specPath(const Options& o, const fs::path& absolute) {
            return o.project.empty() ? jasperengine::editor::FileUtils::pathToGenericUtf8(absolute.lexically_normal()) : projectRelative(o.project, absolute);
        }

        fs::path specAbsolute(const Options& o, const std::string& path) {
            const fs::path p = fs::u8path(path);
            return o.project.empty() || p.is_absolute() ? p.lexically_normal() : (o.project / p).lexically_normal();
        }

        // links resolved, a dangling one too (resolvedPath); a loop: the path as named
        fs::path canonicalPath(const fs::path& path) {
            const fs::path c = resolvedPath(path);
            return c.empty() ? path.lexically_normal() : c;
        }

        int exitFor(const std::string& kind) {
            return kind == "output_exists" || kind == "too_large" || kind == "failed" || kind == "write_failed" || kind == "cancelled" ||
                           kind == "paint_size_mismatch" || kind == "paint_too_large"
                       ? 1
                       : 2;
        }

        // ---------------------------------------------------------------- results

        std::string fixed(double v, int digits) {
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%.*f", digits, v);
            return buffer;
        }

        double rounded(double v, int digits) {
            const double scale = std::pow(10.0, digits);
            return std::round(v * scale) / scale;
        }

        int distinctCount(const std::vector<Cell>& cells, bool columns) {
            std::set<int> values;
            for (const Cell& c : cells) values.insert(columns ? c.x : c.y);
            return static_cast<int>(values.size());
        }

        // One source of the batch
        struct Item {
            fs::path source;                 // absolute
            std::string label;               // shown()
            bool fromSprite = false;          // spec names the GameObject (sceneId, entity, submesh)
            bool spriteHasFrames = false;     // ... and its frames are the cells (submesh 0)
            std::string entityPath;
            SourceInfo info;
            Spec spec;
            Prepared prepared;
            Result result;
            std::vector<OutputFile> files;
            std::vector<uint8_t> preview;
            fs::path previewPath;
            std::vector<Problem> warnings;
            json data;
        };

        // --quantize-tilts / -flat / -rotation given while Toon steps resolve to off: they do nothing
        std::optional<Problem> quantizeOffWarning(const Options& o, const Settings& s) {
            if (s.quantize.on() || !o.overrides.contains("quantize")) return std::nullopt;
            std::vector<std::string> flags;
            for (const auto& [key, flag] : {std::pair<const char*, const char*>{"tilts", "--quantize-tilts"}, {"flat_deg", "--quantize-flat"},
                                            {"rotation_deg", "--quantize-rotation"}}) {
                if (o.overrides["quantize"].contains(key)) flags.push_back(flag);
            }
            if (flags.empty()) return std::nullopt;
            std::string names;
            for (size_t i = 0; i < flags.size(); i++) names += (i == 0 ? "" : (i + 1 == flags.size() ? " and " : ", ")) + flags[i];
            return Problem{"quantize_off", "quantize",
                           "Toon steps are off: " + names + (flags.size() == 1 ? " has" : " have") + " no effect; add --quantize N",
                           "add --quantize 8 (or 4, 6, 12, 16), or drop " + names};
        }

        // ", toon steps N" for the summary line when Toon steps are on and the preset is not toon
        std::string toonStepsText(const Settings& s) {
            return s.quantize.on() && s.preset != Preset::Toon ? ", toon steps " + std::to_string(s.quantize.directions) : std::string();
        }

        json resultJson(const Options& o, Item& item) {
            const Prepared& p = item.prepared;
            const Result& r = item.result;
            const Settings& s = p.request.settings;
            const std::vector<Cell>& cells = p.request.cells;
            const bool animated = p.source.kind == SourceKind::Gif || p.source.kind == SourceKind::Apng;
            json j = json::object();
            j["source"] = item.label;
            j["kind"] = item.fromSprite ? "sprite" : toString(p.source.kind);
            if (item.fromSprite) j["entity"] = item.entityPath;
            j["width"] = p.source.fileWidth;
            j["height"] = p.source.fileHeight;
            const size_t count = cells.empty() ? 1 : cells.size();
            json frames = {{"from", toString(p.framesFrom)}, {"count", count}};
            if (animated) {
                frames["columns"] = p.source.columns;
                frames["rows"] = p.source.rows;
                frames["frame_width"] = p.source.frameWidth;
                frames["frame_height"] = p.source.frameHeight;
                json ms = json::array();
                for (const Delay& d : p.source.delays) ms.push_back(d.playbackMs());
                frames["frame_ms"] = ms;
                frames["loop"] = p.source.loopCount;
            } else if (!cells.empty()) {
                frames["columns"] = distinctCount(cells, true);
                frames["rows"] = distinctCount(cells, false);
                frames["frame_width"] = cells[0].width;
                frames["frame_height"] = cells[0].height;
            } else {
                frames["columns"] = 1;
                frames["rows"] = 1;
                frames["frame_width"] = p.source.color.width;
                frames["frame_height"] = p.source.color.height;
            }
            j["frames"] = frames;
            j["settings"] = settingsToJson(s);
            if (r.radiusPerIsland) {
                j["radius_resolved"] = "per-island";
                j["radius_range"] = json::array({rounded(r.radiusMin, 2), rounded(r.radiusMax, 2)});
            } else {
                j["radius_resolved"] = rounded(r.radiusMin, 2);
            }
            j["scale_range"] = json::array({rounded(r.scaleMin, 3), rounded(r.scaleMax, 3)});
            json profiles = json::array();
            for (Profile pr : {Profile::Rounded, Profile::Dome, Profile::Bevel, Profile::Inflate}) {
                if (r.profilesUsed & (1u << static_cast<int>(pr))) profiles.push_back(toString(pr));
            }
            j["profile_resolved"] = profiles;
            if (r.outlineColor) j["outline"] = colorText(*r.outlineColor);
            const Metrics m = measure(r.normal, cells, s.directx, s.alphaThreshold, &p.paint.overlay, r.shape.empty() ? nullptr : &r.shape, o.threads);
            const Assessment a = assess(s, m, static_cast<int>(cells.size()), &r);
            j["metrics"] = {{"coverage_pct", rounded(m.coveragePct, 1)}, {"flat_pct", rounded(m.flatPct, 1)}, {"steep_pct", rounded(m.steepPct, 1)},
                            {"mean_tilt_deg", rounded(m.meanTiltDeg, 1)}, {"noise_pct", rounded(m.noisePct, 1)}, {"lean_deg", rounded(m.leanDeg, 1)},
                            {"soft_edge_pct", rounded(m.softEdgePct, 1)}, {"frames_touching_edges", m.framesTouchingEdges},
                            {"has_transparency", m.hasTransparency}, {"opaque_pixels", m.opaquePixels}, {"painted_pct", rounded(m.paintedPct, 1)},
                            {"roughness_deg", rounded(m.roughnessDeg, 2)}, {"crease_pct", m.creaseMeasured ? json(rounded(m.creasePct, 2)) : json(nullptr)}};
            // the paint layer (Phase 5): where it came from, what each op selected and changed
            json ops = json::array();
            for (size_t i = 0; i < p.paint.ops.size(); i++) {
                const PaintStats::Op counts = i < p.paint.stats.ops.size() ? p.paint.stats.ops[i] : PaintStats::Op();
                ops.push_back({{"op", toString(p.paint.ops[i].kind)}, {"selected", counts.selected}, {"changed", counts.changed}});
            }
            j["paint"] = {{"mode", toString(p.spec.paintMode)},
                          {"kept_from", p.paintKeptFrom.empty() ? json(nullptr) : json(shown(o, specAbsolute(o, p.paintKeptFrom)))},
                          {"ops", ops},
                          {"painted_pixels", p.paint.stats.painted},
                          {"hidden_pixels", p.paint.stats.hidden},
                          {"painted_pct", rounded(m.paintedPct, 1)}};
            j["assessment"] = a.lines;
            j["hint"] = a.hint.empty() ? std::string("Looks fine.") : a.hint;
            item.warnings = a.warnings;
            item.warnings.insert(item.warnings.end(), r.warnings.begin(), r.warnings.end());
            for (const Problem& w : sceneWarnings(nullptr, NULL_ENTITY, s, r.normal.width, r.normal.height, p.source.delays, false)) item.warnings.push_back(w);
            if (const std::optional<Problem> off = quantizeOffWarning(o, s)) item.warnings.push_back(*off);
            json warnings = json::array();
            for (const Problem& w : item.warnings) warnings.push_back({{"kind", w.kind}, {"message", w.message}, {"fix", w.fix}});
            j["warnings"] = warnings;
            json outputs = {{"normal", nullptr}, {"atlas", nullptr}, {"height", nullptr}, {"frames_json", nullptr}, {"preview", nullptr}};
            for (const PlannedOutput& out : p.outputs) outputs[out.role] = shown(o, specAbsolute(o, out.path));
            if (!item.previewPath.empty()) outputs["preview"] = shown(o, item.previewPath);
            j["outputs"] = outputs;
            j["seconds"] = rounded(r.seconds, 3);
            return j;
        }

        // ---------------------------------------------------------------- the command

        int pipeline(const Options& o, Run& state, json& results, const Settings& settings, std::vector<Item>& items, const Scene* scene);

        int run(int argc, char** argv, Options& o, Run& state, json& results) {
            // settings: the preset, the flags over it (every surface resolves them the same way)
            Settings settings = presetSettings(o.preset);
            std::string error;
            json overrides = o.overrides;
            if (!settingsFromJson(overrides, settings, error)) return state.fail(2, "usage", error, "see --help");
            if (o.regionsGiven) settings.regions = o.regions;
            settings.preset = o.preset;
            settings.directx = o.directx;
            settings.height = o.heightMap;
            const std::vector<Problem> invalid = validateSettings(settings);
            if (!invalid.empty()) {
                for (const Problem& p : invalid) {
                    // "alpha_threshold must be ..." -> "--alpha-threshold must be ..." (the flag's name)
                    std::string message = p.message;
                    for (const auto& [field, flag] : {std::pair<const char*, const char*>{"quantize.directions", "quantize"}, {"quantize.tilts", "quantize-tilts"},
                                                      {"quantize.flat_deg", "quantize-flat"}, {"quantize.rotation_deg", "quantize-rotation"}}) {
                        if (p.field == field && message.rfind(field, 0) == 0) message = flag + message.substr(std::strlen(field));
                    }
                    const size_t space = message.find(' ');
                    std::replace(message.begin(), message.begin() + static_cast<std::ptrdiff_t>(space == std::string::npos ? 0 : space), '_', '-');
                    state.fail(2, "usage", (p.field.rfind("regions", 0) == 0 ? "--region " : "--") + message, p.fix.empty() ? "see --help" : p.fix);
                }
                return 2;
            }
            if (!o.project.empty()) {
                std::error_code ec;
                if (!fs::is_regular_file(o.project / "project.yaml", ec)) return state.fail(2, "usage", "no project.yaml in " + jasperengine::editor::FileUtils::pathToUtf8(o.project), "give a project directory or its project.yaml");
            }
            // invalid paint operations: usage errors naming paint[i].<field>
            if (!o.paintProblems.empty()) {
                for (const Problem& p : o.paintProblems) state.fail(2, p.kind, p.message, p.fix);
                return 2;
            }
            if (o.paintMode == PaintMode::Clear && !o.paint.empty()) {
                return state.fail(2, "invalid_paint", "--paint-mode clear drops the paint, so --paint-json must be empty (got " + std::to_string(o.paint.size()) + " operations)",
                                  "use --paint-mode replace to start over with these operations");
            }

            // ---- the sources: image files (no engine at all), or --sprite's GameObject inside one
            // headless engine run: one job per distinct colour texture (a Tilemap: per tileset)
            if (o.sprite.empty()) {
                std::vector<Item> items;
                for (const fs::path& given : o.sources) {
                    Item item;
                    item.source = inputPath(o, given);
                    items.push_back(std::move(item));
                }
                return pipeline(o, state, results, settings, items, nullptr);
            }
            return withSprite(argc, argv, o, state, [&](SceneProject& sp, Entity entity) {
                Scene* scene = sp.scene;
                MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
                const bool tilemap = scene->findComponent<TilemapComponent>(entity) != nullptr;
                const unsigned count = tilemap && mesh ? std::min<unsigned>(mesh->numSubmeshes, MAX_SUBMESHES) : 1;
                std::vector<Item> items;
                std::set<fs::path> textures;
                std::string why;
                for (unsigned i = 0; i < count; i++) {
                    std::string texture;
                    std::vector<Cell> cells;
                    if (!framesFromSprite(scene, entity, i, texture, cells, why)) continue;
                    const fs::path absolute = (o.project / fs::u8path(texture)).lexically_normal();
                    if (!textures.insert(absolute).second) continue;
                    Item item;
                    item.source = absolute;
                    item.fromSprite = true;
                    item.spriteHasFrames = !cells.empty();
                    item.entityPath = EntityPath::of(sp, entity);
                    item.spec.sceneId = sp.id;
                    item.spec.entity = entity;
                    item.spec.submesh = i;
                    items.push_back(std::move(item));
                }
                if (items.empty()) {
                    return state.fail(2, "usage", "'" + o.sprite + "' can't be the source: " + (why.empty() ? std::string("it has no colour texture") : why),
                                      "give it a texture file first, or pass the image with --source");
                }
                if (items.size() > 1 && (!o.output.empty() || !o.atlas.empty() || !o.previewOut.empty())) {
                    return state.fail(2, "usage", "'" + o.sprite + "' has " + std::to_string(items.size()) +
                                                      " tileset textures: -o, --atlas and --preview-out take one (one normal map per texture)",
                                      "leave them out (the default names), or use --output-dir");
                }
                return pipeline(o, state, results, settings, items, scene);
            });
        }

        // Preflight, generation and the all-or-nothing write of every item; `scene` the --sprite's
        int pipeline(const Options& o, Run& state, json& results, const Settings& settings, std::vector<Item>& items, const Scene* scene) {
            std::ostream& err = *state.err;
            std::string error;

            // ---- preflight: every problem of every source before anything is generated or written
            std::set<fs::path> inputs;
            for (Item& item : items) inputs.insert(canonicalPath(item.source));
            std::map<fs::path, std::string> plannedBy;   // canonical output -> its source's label
            std::vector<std::string> existing;
            for (Item& item : items) {
                item.label = shown(o, item.source);
                std::vector<Problem> probe;
                std::error_code ec;
                if (!fs::is_regular_file(item.source, ec)) {
                    state.fail(2, "invalid_source", item.label + " does not exist", "give an image path (absolute, relative to the current directory, or to --project)", item.label);
                    continue;
                }
                if (!o.project.empty() && !below(item.source, o.project)) {
                    state.fail(2, "invalid_source", item.label + " is outside the project " + jasperengine::editor::FileUtils::pathToUtf8(o.project), "give an image inside the project, or leave --project out", item.label);
                    continue;
                }
                if (!probeSource(item.source, item.info, probe)) {
                    for (const Problem& p : probe) state.fail(exitFor(p.kind), p.kind, p.message, p.fix, item.label);
                    continue;
                }
                const bool animated = item.info.kind == SourceKind::Gif || item.info.kind == SourceKind::Apng;
                Spec& spec = item.spec;
                // a GameObject source names its texture through the scene (spec.source stays empty)
                const std::string sourcePath = specPath(o, item.source);
                if (!item.fromSprite) spec.source = sourcePath;
                spec.settings = settings;
                spec.overwrite = o.overwrite;
                spec.paint = o.paint;
                spec.paintMode = o.paintMode;
                spec.frames = FramesSpec();
                if (o.noFrames) {
                    spec.frames.mode = FramesSpec::Mode::None;
                } else if (o.gridGiven) {
                    spec.frames.mode = FramesSpec::Mode::Grid;
                    spec.frames.grid = o.grid;
                } else if (o.cellsGiven) {
                    spec.frames.mode = FramesSpec::Mode::Cells;
                    spec.frames.cells = o.cells;
                    spec.frames.from = FramesFrom::Given;
                }
                // (frames auto on a GameObject: the session takes the Sprite's frames, from: "sprite")
                const bool hasCells = spec.frames.mode == FramesSpec::Mode::Grid || spec.frames.mode == FramesSpec::Mode::Cells ||
                                      (spec.frames.mode == FramesSpec::Mode::Auto && item.spriteHasFrames);
                if (o.framesJson && !animated && !hasCells) {
                    state.fail(2, "usage", "--frames-json is for animations and frame grids: " + item.label + " is one whole image",
                               "give --frame-size (or --columns and --rows, or --cells-json), or leave --frames-json out", item.label);
                    continue;
                }
                // the outputs: -o / --atlas, --output-dir, else the default names beside the source
                spec.output.framesJson = o.framesJson;
                if (!o.output.empty()) spec.output.normal = specPath(o, outputPath(o, o.output));
                if (!o.atlas.empty()) spec.output.atlas = specPath(o, outputPath(o, o.atlas));
                if (!o.outputDir.empty()) {
                    const fs::path dir = outputPath(o, o.outputDir);
                    for (const PlannedOutput& d : defaultOutputs(sourcePath, item.info.kind, settings.height, o.framesJson)) {
                        const std::string path = specPath(o, dir / fs::u8path(d.path).filename());
                        if (d.role == "normal") spec.output.normal = path;
                        if (d.role == "atlas" && o.atlas.empty()) spec.output.atlas = path;
                        if (d.role == "height") spec.output.height = path;
                        if (d.role == "frames_json") spec.output.framesJsonPath = path;
                    }
                }
                if (!o.previewOut.empty()) item.previewPath = outputPath(o, o.previewOut);
                const std::vector<PlannedOutput> outputs = plannedOutputs(o.project, spec, animated ? item.info.kind : SourceKind::Image);
                std::vector<Problem> problems = checkSpec(o.project, spec, scene);
                for (const Problem& p : problems) {
                    if (p.kind == "output_exists") {
                        // the planned path of that role (field "output.<role>"), as the user typed it
                        for (const PlannedOutput& out : outputs) {
                            if (p.field == "output." + out.role) existing.push_back(shown(o, specAbsolute(o, out.path)));
                        }
                        continue;
                    }
                    state.fail(exitFor(p.kind), p.kind, p.message, p.fix, item.label);
                }
                // across the batch: no output twice, none equal to an input (aliases resolved)
                std::vector<fs::path> planned;
                for (const PlannedOutput& out : outputs) planned.push_back(specAbsolute(o, out.path));
                if (!item.previewPath.empty()) planned.push_back(item.previewPath);
                for (const fs::path& out : planned) {
                    const fs::path key = canonicalPath(out);
                    if (inputs.count(key)) {
                        state.fail(2, "output_invalid", shown(o, out) + " is an input of this run (outputs never replace inputs, even with --overwrite)",
                                   "write it elsewhere", item.label);
                        continue;
                    }
                    auto [it, fresh] = plannedBy.emplace(key, item.label);
                    if (!fresh) {
                        state.fail(2, "output_invalid", shown(o, out) + " would be written for both " + it->second + " and " + item.label,
                                   "use separate output directories, or rename one source", item.label);
                    }
                }
                if (!item.previewPath.empty() && !o.overwrite && fs::exists(item.previewPath, ec)) existing.push_back(shown(o, item.previewPath));
            }
            // paint: one normal map per run (a Tilemap's tilesets are several)
            if (o.paintGiven && items.size() > 1) {
                return state.fail(2, "invalid_paint", "--paint-json paints one normal map, and this run makes " + std::to_string(items.size()) + " (one per tileset texture)",
                                  "one run per output: pass the tileset image with --source");
            }
            if (state.code == 2) return 2;
            if (!existing.empty()) {
                std::string list;
                for (const std::string& e : existing) list += (list.empty() ? "" : ", ") + e;
                return state.fail(1, "output_exists", "would replace " + list + " (nothing was written)",
                                  "pass --overwrite (inputs are never replaced), or other outputs");
            }
            if (state.code != 0) return state.code;

            // ---- generate every source (blocking; --threads never changes the output)
            for (Item& item : items) {
                std::vector<Problem> problems;
                if (!prepare(o.project, item.spec, scene, item.prepared, problems)) {
                    for (const Problem& p : problems) state.fail(exitFor(p.kind), p.kind, p.message, p.fix, item.label);
                    continue;
                }
                item.prepared.request.threads = o.threads;
                // the generation with the paint layer (kept from the existing map, the ops applied)
                item.result = generatePrepared(item.prepared);
                if (item.result.status != Status::Ok) {
                    for (const Problem& p : item.result.errors) state.fail(1, p.kind.empty() ? "failed" : p.kind, p.message, p.fix, item.label);
                    if (item.result.errors.empty()) state.fail(1, "failed", "the generation failed", "try again", item.label);
                    continue;
                }
                if (!item.previewPath.empty()) {
                    const Settings& s = item.prepared.request.settings;
                    const Image sheet = contactSheet(item.prepared.source.color, item.result.normal, item.prepared.request.cells, s.directx, s.alphaThreshold,
                                                     SheetOptions(), &item.prepared.paint.overlay);
                    if (!encodePng(sheet, item.preview, error)) state.fail(1, "failed", "the preview could not be encoded: " + error, "try again", item.label);
                }
            }
            if (state.code != 0) return state.code;

            // ---- write: every output of every source encoded first, then all or nothing
            std::vector<OutputFile> all;
            for (Item& item : items) {
                Problem problem;
                if (!encodeOutputs(item.prepared, item.result, item.files, error, &problem)) {
                    return state.fail(1, problem.kind.empty() ? "write_failed" : problem.kind, error, problem.fix.empty() ? "check the output paths" : problem.fix,
                                      item.label);
                }
                all.insert(all.end(), item.files.begin(), item.files.end());
                if (!item.previewPath.empty()) {
                    OutputFile preview;
                    preview.role = "preview";
                    preview.path = shown(o, item.previewPath);
                    preview.absolute = item.previewPath;
                    preview.bytes.assign(item.preview.begin(), item.preview.end());
                    all.push_back(std::move(preview));
                }
            }
            if (!o.dryRun) {
                std::string failedPath;
                if (!writeOutputsDirect(all, failedPath, error)) {
                    return state.fail(1, "write_failed", "could not write " + shown(o, fs::u8path(failedPath)) + ": " + error + "; nothing was written",
                                      "check that the output folders are writable and are not files");
                }
            }
            for (Item& item : items) {
                json j = resultJson(o, item);
                j["written"] = !o.dryRun;
                if (o.dryRun) {
                    json planned = json::array();
                    for (const OutputFile& f : item.files) planned.push_back(shown(o, f.absolute));
                    if (!item.previewPath.empty()) planned.push_back(shown(o, item.previewPath));
                    j["planned"] = planned;
                }
                if (!o.quiet && o.format == "text") {
                    std::string outputs;
                    for (const OutputFile& f : item.files) outputs += (outputs.empty() ? "" : ", ") + shown(o, f.absolute);
                    if (!item.previewPath.empty()) outputs += (outputs.empty() ? "" : ", ") + shown(o, item.previewPath);
                    const size_t frames = item.prepared.request.cells.empty() ? 1 : item.prepared.request.cells.size();
                    err << PREFIX << item.label << " -> " << (o.dryRun ? "(dry run, would write) " : "") << outputs << " (" << item.prepared.source.fileWidth << "x"
                        << item.prepared.source.fileHeight << ", " << frames << (frames == 1 ? " frame" : " frames") << ", preset " << toString(o.preset) << toonStepsText(item.prepared.request.settings) << ") in "
                        << fixed(item.result.seconds, 2) << "s\n";
                    const PaintLayer& paint = item.prepared.paint;
                    if (paint.active() || item.spec.paintMode != PaintMode::Keep) {
                        err << "paint: " << paint.ops.size() << (paint.ops.size() == 1 ? " op, " : " ops, ") << paint.stats.painted << " px painted (" << paint.stats.hidden
                            << " hidden)" << (item.prepared.paintKeptFrom.empty() ? std::string() : ", kept from " + shown(o, specAbsolute(o, item.prepared.paintKeptFrom)))
                            << "\n";
                    }
                    for (const Problem& w : item.warnings) err << "warning: " << w.kind << ": " << w.message << " (fix: " << w.fix << ")\n";
                }
                results.push_back(j);
            }
            return 0;
        }

    }

    void printGenerateSpriteNormalsUsage(const std::string& commandName, std::ostream& out) {
        out << "Usage:\n"
            << "  " << commandName << " generate-sprite-normals --source <image> [--source <image>...] [options]\n"
            << "  " << commandName << " generate-sprite-normals --project <dir> --sprite <GameObject> [--scene <scene>] [options]\n\n"
            << "Generates normal maps for sprites (pixel art to 4096 px painted art), sprite sheets, GIF/APNG\n"
            << "animations and tiles (a port of Sprite Fusion's sprite-normals, MIT). OpenGL convention (green up)\n"
            << "unless --directx. Every PNG written embeds its recipe (settings, source and SHA-1) as an iTXt chunk\n"
            << "named jasper.spritenormals.\n\n"
            << "Input:\n"
            << "  -s, --source <image>        PNG (APNG too), GIF, JPEG, TGA, BMP or PSD. Can repeat (a batch).\n"
            << "      --sprite <GameObject>   A Sprite, Polygon or Tilemap GameObject of the project (path or name): its\n"
            << "                              texture is the source and its frames the cells. Needs --project.\n"
            << "      --scene <scene>         The scene of --sprite (name or .scene path; default: the project's start scene).\n"
            << "  -p, --project <path>        Project directory or project.yaml: recipe paths are relative to it, and\n"
            << "                              outputs must be inside its assets directory (also once symbolic links\n"
            << "                              are resolved). Optional with --source.\n"
            << "Settings (defaults: the preset's):\n"
            << "      --preset <name>         soft, default (default), crisp, detailed, chunky, toon (Toon steps 8);\n"
            << "                              painted (painted / vector art), props (crates, panels), tile (wrap).\n"
            << "      --strength <x>          0..8.          --detail <x>      0..8.\n"
            << "      --smoothing <px>        0..32.         --volume <x>      0..8.\n"
            << "      --radius <auto|px>      auto (default: 0.3 x each island's thickness) or 0.5..512.\n"
            << "      --softness <px>         0..32 (1.15).  --alpha-threshold <x>  0..0.99 (0.03).\n"
            << "      --profile <p>           auto (default: rounded up to 128 px, inflate above), rounded, dome,\n"
            << "                              bevel, inflate.\n"
            << "      --edges <e>             transparent (default), wrap (seamless), extend (tiles).\n"
            << "      --resolution-scale <s>  auto (default): smoothing, softness and detail are for a 128 px sprite\n"
            << "                              and grow with it (x shorter side / 128); off: as given, in pixels.\n"
            << "      --soft-edge <s>         auto (default): above 128 px, soft pixels (alpha < 0.5) reaching the outside\n"
            << "                              are outside the shape (glows); solid: they count as shape (translucent art).\n"
            << "      --settings-version <n>  2 (default); 1 regenerates a map made before high-resolution support.\n"
            << "      --ignore-outline        Leave the detected outline colour out of the relief.\n"
            << "      --region <spec>         colors=#rrggbb[,#rrggbb...][;tolerance=x][;detail=x][;lift=px][;flat]. Can repeat (16).\n"
            << "      --quantize <n>          Toon steps: snap normals to n directions (0 off, 4, 6, 8, 12, 16) for\n"
            << "                              stepped, cel-shaded light.   --quantize-tilts <n>  1..4 rings (1).\n"
            << "      --quantize-flat <deg>   0..45 (15): flatter normals face the viewer.\n"
            << "      --quantize-rotation <deg>  0..360, 360 is 0 (0: one direction points right).\n"
            << "      --directx               Green down (Unreal). The engine reads OpenGL.\n"
            << "Frames (default: the animation's, the --sprite's, else the whole image):\n"
            << "      --frame-size <WxH|N>    Cell size.   --columns <n>  --rows <n>  --count <n>\n"
            << "      --offset <x,y>          --padding <x,y>      (as the Texture Slicer)\n"
            << "      --cells-json <file|->   Cells as a JSON array [[x,y,w,h],...] (a recipe's frames.cells; not with the\n"
            << "                              grid options; not for GIF/APNG).\n"
            << "      --no-frames             Whole image (not for GIF/APNG).\n"
            << "Output:\n"
            << "  -o, --output <png>          The normal map (one source only). Default: <stem>_normal.png beside the source\n"
            << "                              (<stem>_sheet_normal.png for an animation).\n"
            << "      --output-dir <dir>      Write every output there instead of beside its source.\n"
            << "      --atlas <png>           An animation's colour atlas (default <stem>_sheet.png).\n"
            << "      --height-map            Also write <stem>_height.png (16-bit grey).\n"
            << "      --frames-json           Also write <atlas stem>.frames.json for animations, <normal stem>.frames.json\n"
            << "                              otherwise (frame grids and cells).\n"
            << "      --preview-out <png>     Also write the lit contact sheet (one source only; painted pixels outlined\n"
            << "                              in magenta in the normal column).\n"
            << "Paint (one source only; corrections kept inside the normal map, re-applied on every generation):\n"
            << "      --paint-json <file|-|[...]>  A JSON array of operations (generate_sprite_normals' paint: angle,\n"
            << "                              flatten, smooth, erase by rect, pixels, colors or frame); - reads stdin,\n"
            << "                              a value starting with [ is the JSON itself.\n"
            << "      --paint-mode <mode>     keep (default: start from the existing map's paint), replace (from none),\n"
            << "                              clear (no paint written; no --paint-json).\n"
            << "      --overwrite             Replace existing outputs (inputs are never replaced).\n"
            << "      --dry-run               Generate and report, write nothing (not even --preview-out).\n"
            << "      --threads <n>           0 = all cores (default). Never changes the output.\n"
            << "      --format <format>       text (default) or json (one object on stdout).\n"
            << "  -q, --quiet                 No summary on stderr.\n"
            << "  -h, --help                  This help.\n\n"
            << "Paths are absolute, relative to the current directory, or (with --project) relative to the project.\n"
            << "Exit code: 0 written (or dry run done; warnings allowed), 1 generation or write failed, an output\n"
            << "exists without --overwrite, or the kept paint does not fit (paint_size_mismatch, paint_too_large;\n"
            << "nothing is written then), 2 usage error (invalid --paint-json operations too).\n";
    }

    int runGenerateSpriteNormals(int argc, char** argv, const std::string& commandName, std::ostream& out, std::ostream& err) {
        const auto started = std::chrono::steady_clock::now();
        Options o;
        Run state;
        state.o = &o;
        state.err = &err;
        // the format first, so a usage error is reported in it
        for (int i = 1; i + 1 < argc; i++) {
            if (std::strcmp(argv[i], "--format") == 0 && std::strcmp(argv[i + 1], "json") == 0) o.format = "json";
        }
        json results = json::array();
        auto finish = [&](int code) {
            if (o.format == "json") {
                const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
                const json result = {{"ok", code == 0}, {"dry_run", o.dryRun}, {"seconds", std::round(seconds * 100.0) / 100.0},
                                     {"results", results}, {"errors", state.errors}};
                out << result.dump(1, ' ', false, json::error_handler_t::replace) << "\n";
            }
            out.flush();
            err.flush();
            return code;
        };
        std::string error;
        if (!parseArgs(argc, argv, o, error)) {
            state.fail(2, "usage", error, "see " + commandName + " generate-sprite-normals --help");
            if (o.format != "json") {
                err << "\n";
                printGenerateSpriteNormalsUsage(commandName, err);
            }
            return finish(2);
        }
        if (o.help) {
            printGenerateSpriteNormalsUsage(commandName, out);
            return 0;
        }
        int code = 2;
        try {
            code = run(argc, argv, o, state, results);
        } catch (const std::exception& e) {
            code = state.fail(1, "failed", std::string("unexpected error: ") + e.what(), "report it");
        }
        return finish(code);
    }

}
