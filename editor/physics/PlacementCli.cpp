// SPDX-License-Identifier: MIT

#include "PlacementCli.h"

#include "PlacementSession.h"

#include "AppSettings.h"
#include "EditorHost.h"
#include "JasperEngine.h"
#include "Out.h"
#include "Platform.h"
#include "Project.h"
#include "cli/CommandLine.h"
#include "physics/ConvexDecomposition.h"
#include "pool/ShaderPool.h"
#include "shader/ShaderBuilder.h"
#include "util/EntityPath.h"

#include "json.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <ostream>
#include <sstream>
#include <thread>
#include <vector>

#if defined(_WIN32)
    #include <fcntl.h>
    #include <io.h>
#else
    #include <fcntl.h>
    #include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace jasperengine::editor {

    namespace {

        struct Options {
            fs::path project;
            std::string scene;
            std::vector<std::string> entities;
            PlacementSpec spec;
            bool modeGiven = false;
            bool axisGiven = false;
            bool amountGiven = false;
            bool save = false;
            bool dryRun = false;
            std::string format = "text";
            bool quiet = false;
            bool help = false;
        };

        bool parseFloats(const std::string& text, float* values, size_t count) {
            size_t start = 0;
            for (size_t i = 0; i < count; i++) {
                const size_t comma = text.find(',', start);
                const bool last = i + 1 == count;
                if (last != (comma == std::string::npos)) return false;
                const std::string part = text.substr(start, last ? std::string::npos : comma - start);
                if (part.empty()) return false;
                char* end = nullptr;
                const double v = std::strtod(part.c_str(), &end);
                if (*end != 0 || !std::isfinite(v)) return false;
                values[i] = static_cast<float>(v);
                start = comma + 1;
            }
            return true;
        }

        bool parseVector3(const std::string& text, Vector3& out) {
            float v[3];
            if (!parseFloats(text, v, 3)) return false;
            out = Vector3(v[0], v[1], v[2]);
            return true;
        }

        bool parseSeed(const std::string& text, uint32_t& out) {
            if (text.empty() || !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) return false;
            errno = 0;
            char* end = nullptr;
            const unsigned long long v = std::strtoull(text.c_str(), &end, 10);
            if (errno == ERANGE || *end != 0 || v > 0xFFFFFFFFull) return false;
            out = static_cast<uint32_t>(v);
            return true;
        }

        bool parseArgs(int argc, char** argv, Options& o, std::string& error) {
            PlacementSpec& spec = o.spec;
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
                auto vector = [&](Vector3& target, const char* what) {
                    if (!need()) return false;
                    if (!parseVector3(value, target)) {
                        error = "invalid " + arg + " '" + value + "' (" + what + ")";
                        return false;
                    }
                    return true;
                };
                // a single number, at least `minimum`
                auto number = [&](float& target, float minimum, const char* what) {
                    if (!need()) return false;
                    float v = 0.0f;
                    if (!parseFloats(value, &v, 1) || v < minimum) {
                        error = "invalid " + arg + " '" + value + "' (" + what + ")";
                        return false;
                    }
                    target = v;
                    return true;
                };
                if (arg == "-h" || arg == "--help") {
                    o.help = true;
                } else if (arg == "-p" || arg == "--project") {
                    if (!need()) return false;
                    o.project = CommandLine::projectArgPath(fs::u8path(value));
                } else if (arg == "-s" || arg == "--scene") {
                    if (!need()) return false;
                    o.scene = value;
                } else if (arg == "-e" || arg == "--entity") {
                    if (!need()) return false;
                    o.entities.push_back(value);
                } else if (arg == "--mode") {
                    if (!need()) return false;
                    if (!parsePlacementMode(value.c_str(), spec.mode)) {
                        error = "unknown mode '" + value + "' (drop, separate, move, snap, gather, distribute)";
                        return false;
                    }
                    o.modeGiven = true;
                } else if (arg == "--target") {
                    if (!vector(spec.targetPosition, "x,y,z: where the group's centre goes, world space")) return false;
                    spec.hasTargetPosition = true;
                } else if (arg == "--delta") {
                    if (!vector(spec.targetDelta, "x,y,z: how far the group moves, world space")) return false;
                    spec.hasTargetDelta = true;
                } else if (arg == "--rotation-euler") {
                    Vector3 euler;
                    if (!vector(euler, "x,y,z degrees: the group's turn about its centre")) return false;
                    spec.targetRotation = Quaternion(euler.x, euler.y, euler.z);
                    spec.hasTargetRotation = true;
                } else if (arg == "--ray") {
                    if (!need()) return false;
                    float v[6];
                    if (!parseFloats(value, v, 6) || (v[3] == 0.0f && v[4] == 0.0f && v[5] == 0.0f)) {
                        error = "invalid --ray '" + value + "' (ox,oy,oz,dx,dy,dz: an origin and a non-zero direction)";
                        return false;
                    }
                    spec.snapRay = Ray(Vector3(v[0], v[1], v[2]), Vector3(v[3], v[4], v[5]));
                    spec.hasSnapRay = true;
                } else if (arg == "--axis") {
                    if (!need()) return false;
                    if (value != "x" && value != "y" && value != "z") {
                        error = "invalid --axis '" + value + "' (x, y or z)";
                        return false;
                    }
                    spec.axis = value[0] - 'x';
                    o.axisGiven = true;
                } else if (arg == "--amount") {
                    if (!number(spec.amount, 0.0f, "0..1: how far toward the centre")) return false;
                    if (spec.amount > 1.0f) {
                        error = "invalid --amount '" + value + "' (0..1: how far toward the centre)";
                        return false;
                    }
                    o.amountGiven = true;
                } else if (arg == "--gravity") {
                    if (!vector(spec.gravity, "x,y,z m/s^2")) return false;
                    spec.settings.gravity = spec.gravity;
                } else if (arg == "--no-settle") {
                    spec.settle = false;
                } else if (arg == "--seed") {
                    if (!need()) return false;
                    if (!parseSeed(value, spec.seed)) {
                        error = "invalid --seed '" + value + "' (an integer 0..4294967295)";
                        return false;
                    }
                } else if (arg == "--max-seconds") {
                    if (!number(spec.maxSeconds, 0.0f, "simulated seconds, more than 0")) return false;
                    if (spec.maxSeconds <= 0.0f) {
                        error = "invalid --max-seconds '" + value + "' (simulated seconds, more than 0)";
                        return false;
                    }
                } else if (arg == "--jitter") {
                    if (!need()) return false;
                    float v[2];
                    if (!parseFloats(value, v, 2) || v[0] < 0.0f || v[1] < 0.0f) {
                        error = "invalid --jitter '" + value + "' (POS,DEG: metres and degrees, not negative)";
                        return false;
                    }
                    spec.jitter.position = v[0];
                    spec.jitter.rotationDeg = v[1];
                } else if (arg == "--feel") {
                    if (!need()) return false;
                    if (!parsePlacementFeel(value.c_str(), spec.settings.feel)) {
                        error = "unknown feel '" + value + "' (precise, balanced, fast)";
                        return false;
                    }
                } else if (arg == "--colliders") {
                    if (!need()) return false;
                    if (!parsePlacementColliderPolicy(value.c_str(), spec.settings.colliders)) {
                        error = "unknown colliders '" + value + "' (auto, authored, generated, both)";
                        return false;
                    }
                } else if (arg == "--decompose") {
                    if (!need()) return false;
                    if (!parsePlacementDecomposition(value.c_str(), spec.settings.decomposition)) {
                        error = "unknown decompose '" + value + "' (never, cached, always)";
                        return false;
                    }
                } else if (arg == "--push-neighbours") {
                    spec.settings.pushNeighbours = true;
                } else if (arg == "--context-margin") {
                    if (!number(spec.contextMargin, 0.0f, "metres, not negative")) return false;
                    spec.settings.regionMargin = spec.contextMargin;
                } else if (arg == "--save") {
                    o.save = true;
                } else if (arg == "--dry-run") {
                    o.dryRun = true;
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
                    error = (!arg.empty() && arg[0] == '-' ? "unknown option " : "unexpected argument ") + arg;
                    return false;
                }
            }
            if (o.help) return true;
            if (o.project.empty()) {
                error = "--project is required";
                return false;
            }
            if (o.scene.empty()) {
                error = "--scene is required";
                return false;
            }
            if (o.entities.empty()) {
                error = "--entity is required (the GameObjects to place, by path; can repeat)";
                return false;
            }
            if (o.save && o.dryRun) {
                error = "--save and --dry-run exclude each other";
                return false;
            }
            const PlacementMode mode = spec.mode;
            const bool anyTarget = spec.hasTargetPosition || spec.hasTargetDelta || spec.hasTargetRotation;
            if (mode == PlacementMode::Move && !anyTarget) {
                error = "move mode needs --target, --delta or --rotation-euler";
                return false;
            }
            if (mode != PlacementMode::Move && anyTarget) {
                error = "--target, --delta and --rotation-euler are only used in move mode";
                return false;
            }
            if (spec.hasTargetPosition && spec.hasTargetDelta) {
                error = "--target and --delta both say where the group goes: give one";
                return false;
            }
            if (mode == PlacementMode::Snap && !spec.hasSnapRay) {
                error = "snap mode needs --ray";
                return false;
            }
            if (mode != PlacementMode::Snap && spec.hasSnapRay) {
                error = "--ray is only used in snap mode";
                return false;
            }
            if (mode == PlacementMode::Distribute && !o.axisGiven) {
                error = "distribute mode needs --axis";
                return false;
            }
            if (mode != PlacementMode::Distribute && o.axisGiven) {
                error = "--axis is only used in distribute mode";
                return false;
            }
            if (mode != PlacementMode::Gather && o.amountGiven) {
                error = "--amount is only used in gather mode";
                return false;
            }
            if (mode == PlacementMode::Separate && !spec.settle) {
                error = "--no-settle is not used in separate mode (it never settles under gravity)";
                return false;
            }
            return true;
        }

        // Restores the engine's state when the command returns (as the other headless commands)
        struct EngineRuntimeGuard {
            ~EngineRuntimeGuard() {
                ::jasperengine::ShaderPool::setShaderBuilder(nullptr);
                ShaderBuilder::requestShutdown();
                ::jasperengine::Engine::systemShutdown();
            }
        };

        struct EditorHostOverrideGuard {
            EditorHost* previousHost = nullptr;
            explicit EditorHostOverrideGuard(EditorHost* host): previousHost(&getEditorHost()) {
                setEditorHost(host);
            }
            ~EditorHostOverrideGuard() {
                setEditorHost(previousHost);
            }
        };

        // Pending background decompositions stop and the project's cache goes before the engine
        // shuts down
        struct DecompositionCacheGuard {
            ~DecompositionCacheGuard() {
                placementProjectSwitching();
            }
        };

        // The engine logs (Out's info and warnings) to stdout: while the project loads and the
        // simulation runs, stdout goes to stderr (to the null device with -q), so stdout carries
        // only the result
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

        std::string fixed(double v, int digits) {
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%.*f", digits, v);
            return buffer;
        }

        std::string sceneList(const Project& project) {
            std::string names;
            for (const SceneProject& sp : project.getScenes()) {
                if (sp.filepath.empty()) continue;
                names += (names.empty() ? "" : ", ") + sp.name + " (" + sp.filepath.generic_u8string() + ")";
            }
            return names.empty() ? std::string("none") : names;
        }

        // A scene of the project by name, file name or path (absolute, relative to the current
        // directory, else to the project)
        SceneProject* findScene(Project& project, const std::string& ref) {
            const fs::path refPath = fs::u8path(ref);
            std::error_code ec;
            fs::path asPath = CommandLine::absoluteArgPath(refPath);
            if (refPath.is_relative() && !fs::is_regular_file(asPath, ec)) {
                asPath = CommandLine::absoluteArgPath(project.getProjectPath() / refPath);
            }
            for (SceneProject& sp : project.getScenes()) {
                if (sp.filepath.empty()) continue;
                if (sp.name == ref || sp.filepath.filename().u8string() == ref ||
                    CommandLine::absoluteArgPath(project.getProjectPath() / sp.filepath) == asPath) {
                    return &sp;
                }
            }
            return nullptr;
        }

        // The per-run text summary (the JSON's essentials)
        void printText(const SceneProject& sp, const PlacementResult& r, bool saved, std::ostream& out) {
            size_t movers = 0, pushed = 0;
            for (const PlacementEntityResult& e : r.entities) (e.mover ? movers : pushed)++;
            out << "place-physics " << placementModeName(r.mode) << " on " << sp.name << ": " << movers << (movers == 1 ? " mover" : " movers");
            if (pushed) out << " (+" << pushed << " pushed)";
            out << ", " << r.contextBodies << " context " << (r.contextBodies == 1 ? "body" : "bodies") << ", " << r.steps << " steps ("
                << fixed(r.simSeconds, 2) << " s simulated), " << (r.allSettled ? "all settled" : "NOT all settled") << ", seed " << r.seed
                << ", state " << r.stateHash << "\n";
            for (const PlacementEntityResult& e : r.entities) {
                out << "  " << e.path << (e.mover ? "" : " (pushed)") << ": moved " << fixed(e.movedMeters, 3) << " m, turned "
                    << fixed(e.rotatedDegrees, 1) << " deg, ";
                if (e.fellOut) {
                    out << "fell out of the world";
                } else if (e.settled) {
                    out << "settled";
                } else {
                    out << "not settled";
                }
                if (!e.restingOnPaths.empty()) {
                    out << ", resting on ";
                    for (size_t i = 0; i < e.restingOnPaths.size(); i++) out << (i ? ", " : "") << e.restingOnPaths[i];
                }
                if (e.teleported) out << ", teleported";
                out << "\n";
                for (const std::string& warning : e.warnings) out << "      warning: " << warning << "\n";
            }
            EntityPath::Cache cache;
            for (const PlacementRefusal& refusal : r.refused) {
                out << "refused " << EntityPath::of(sp, refusal.entity, &cache) << ": " << refusal.reason << "\n";
            }
            for (const std::string& warning : r.warnings) out << "warning: " << warning << "\n";
            if (!r.hint.empty()) out << "hint: " << r.hint << "\n";
            if (saved) {
                out << "saved " << sp.filepath.generic_u8string() << "\n";
            } else {
                out << "dry run: " << sp.filepath.generic_u8string() << " not changed (--save applies the result and writes it)\n";
            }
        }

        // Loads the project and scene, runs, commits and saves. The result text in `report`.
        int runInProject(int argc, char** argv, Options& o, const std::string& prefix, std::string& report, std::ostream& err) {
            EditorHostOverrideGuard hostOverride(nullptr);
            Out::setConsoleWindow(nullptr);
            AppSettings::initialize();

            Project project;
            auto platform = std::make_unique<Platform>(&project);
            ::jasperengine::Engine::systemInit(argc, argv, platform.get());
            EngineRuntimeGuard engineRuntime;
            ::jasperengine::Engine::pauseGameEvents(true);
            ::jasperengine::Engine::setAsyncLoading(false);
            DecompositionCacheGuard decompositionCache;

            if (!project.loadProject(o.project, false)) {
                err << prefix << "the project could not be loaded: " << o.project.u8string() << "\n";
                return 1;
            }
            SceneProject* sp = findScene(project, o.scene);
            if (!sp) {
                err << prefix << "no scene '" << o.scene << "' in the project (scenes: " << sceneList(project) << ")\n";
                return 2;
            }
            const uint32_t sceneId = sp->id;
            if (!sp->scene && !project.loadScene(sp->filepath, false, false, true)) {
                err << prefix << "the scene could not be loaded: " << sp->filepath.generic_u8string() << "\n";
                return 1;
            }
            sp = project.getScene(sceneId);
            if (!sp || !sp->scene) {
                err << prefix << "the scene could not be loaded: " << o.scene << "\n";
                return 1;
            }

            // the movers: by path (else a unique name), each once, the top-most roots
            std::vector<Entity> selection;
            EntityPath::Cache paths;
            for (const std::string& ref : o.entities) {
                const std::vector<Entity> found = EntityPath::resolve(*sp, ref);
                if (found.empty()) {
                    err << prefix << "no GameObject '" << ref << "' in scene " << sp->name << " (give its path, e.g. Props/Crate)\n";
                    return 2;
                }
                if (found.size() > 1) {
                    err << prefix << "'" << ref << "' names " << found.size() << " GameObjects in scene " << sp->name << ": ";
                    for (size_t i = 0; i < found.size(); i++) err << (i ? ", " : "") << EntityPath::of(*sp, found[i], &paths);
                    err << " (give the path)\n";
                    return 2;
                }
                if (std::find(selection.begin(), selection.end(), found.front()) == selection.end()) selection.push_back(found.front());
            }
            PlacementSpec& spec = o.spec;
            spec.sceneId = sceneId;
            spec.entities = placementRoots(&project, sceneId, selection);

            const std::vector<std::string> problems = checkPlacementSpec(&project, spec);
            if (!problems.empty()) {
                for (const std::string& problem : problems) err << prefix << problem << "\n";
                return 2;
            }

            auto run = std::make_unique<PlacementRun>();
            if (!run->start(&project, spec)) {
                err << prefix << "the simulation could not start: " << run->result().error << "\n";
                return 1;
            }
            // --decompose always: every decomposition the movers asked for is computed first, then
            // a fresh run (a fresh world: the same as with a warm cache) picks them all up
            ConvexDecompositionCache* cache = placementDecompositionCache(&project);
            if (spec.settings.decomposition == PlacementDecomposition::ALWAYS && cache && cache->pendingCount() > 0) {
                if (!o.quiet) err << "place-physics: decomposing " << cache->pendingCount() << " mesh(es) into convex hulls\n";
                while (cache->pendingCount() > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                run = std::make_unique<PlacementRun>();
                if (!run->start(&project, spec)) {
                    err << prefix << "the simulation could not start: " << run->result().error << "\n";
                    return 1;
                }
            }
            if (!o.quiet) err << "place-physics: simulating " << placementModeName(spec.mode) << " (" << spec.entities.size() << " movers)\n";
            run->advance(0.0);
            const PlacementResult& result = run->result();
            if (!result.ok) {
                err << prefix << "the simulation failed: " << result.error << "\n";
                return 1;
            }

            bool saved = false;
            if (o.save) {
                std::string error;
                if (!commitPlacement(&project, result, error)) {
                    err << prefix << "the result could not be applied: " << error << "\n";
                    return 1;
                }
                const fs::path file = sp->filepath;
                if (!project.saveSceneToPath(sceneId, file, false)) {
                    err << prefix << "the scene could not be saved: " << file.generic_u8string() << "\n";
                    return 1;
                }
                saved = true;
            }

            if (o.format == "json") {
                report = placementResultJson(&project, result, saved).dump(1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
            } else {
                std::ostringstream text;
                printText(*sp, result, saved, text);
                report = text.str();
            }
            return result.allSettled ? 0 : 3;
        }

    }

    void printPlacePhysicsUsage(const std::string& commandName, std::ostream& out) {
        out << "Usage:\n"
            << "  " << commandName << " place-physics --project <path> --scene <scene> --entity <path> [--entity ...] [options]\n\n"
            << "Places GameObjects with an edit-time physics simulation (Physics Placement): drops them until they\n"
            << "rest, pushes overlapping ones apart, or moves, snaps, gathers or distributes them so they stop\n"
            << "against the scene around them. The same simulation and result as the place_with_physics MCP tool.\n"
            << "The scene's own physics never runs; nothing changes unless --save.\n\n"
            << "Options:\n"
            << "  -p, --project <path>        Project directory or project.yaml.\n"
            << "  -s, --scene <scene>         Scene name, file name or path (relative to the current directory, else\n"
            << "                              to the project).\n"
            << "  -e, --entity <path>         A GameObject to place, by hierarchy path (Props/Crate, Enemy#2) or a\n"
            << "                              unique name. Can repeat. A child of another one given moves with it.\n"
            << "      --mode <mode>           drop (default): gravity until resting; separate: push overlapping\n"
            << "                              objects apart, no gravity; move: toward --target/--delta/--rotation-euler\n"
            << "                              (the group rigidly), stopping at contacts; snap: onto the surface --ray\n"
            << "                              hits; gather: toward the centre by --amount; distribute: equal spacing\n"
            << "                              along --axis. Every mode but separate settles afterwards.\n"
            << "      --target <x,y,z>        Move: where the group's centre goes (world).\n"
            << "      --delta <x,y,z>         Move: how far the group goes (world).\n"
            << "      --rotation-euler <x,y,z>  Move: the group's turn about its centre, degrees.\n"
            << "      --ray <ox,oy,oz,dx,dy,dz>  Snap: the ray (origin, direction) whose hit the group lands on.\n"
            << "      --axis <x|y|z>          Distribute: the axis.\n"
            << "      --amount <a>            Gather: 0..1, how far toward the centre (default 1).\n"
            << "      --gravity <x,y,z>       Gravity, m/s^2 (default 0,-9.81,0).\n"
            << "      --no-settle             Stop after the gesture: no fall until resting afterwards.\n"
            << "      --seed <n>              Seed of --jitter (default 1). Same scene, arguments and editor build: the\n"
            << "                              same result and state hash.\n"
            << "      --max-seconds <s>       Simulated seconds at most (default 10).\n"
            << "      --jitter <pos,deg>      A random offset (metres) and turn (degrees) per object before the first\n"
            << "                              step (default 0,0).\n"
            << "      --feel <feel>           precise, balanced (default) or fast.\n"
            << "      --colliders <c>         auto (default): authored colliders, else a fitted primitive, else a convex\n"
            << "                              hull (decomposition when cached); authored; generated; both.\n"
            << "      --decompose <d>         never; cached (default): use convex decompositions already in the\n"
            << "                              project's cache; always: compute the missing ones first (waits).\n"
            << "      --push-neighbours       Unselected movable objects nearby can be knocked too.\n"
            << "      --context-margin <m>    The environment gathered around the objects, metres (default 2).\n"
            << "      --dry-run               Print the result, change nothing (the default).\n"
            << "      --save                  Apply the result (one undo step, as apply:true) and write the scene file.\n"
            << "      --format <format>       text (default): a summary per object; json: the place_with_physics result.\n"
            << "  -q, --quiet                 Only problems on stderr (no engine log, no progress).\n"
            << "  -h, --help                  Show this help.\n\n"
            << "Paths are absolute or relative to the current directory. The result goes to stdout; the engine's log\n"
            << "and progress to stderr.\n"
            << "Exit code: 0 every object settled, 1 failure (project, scene, simulation, apply or save), 2 usage\n"
            << "error (bad option or value, unknown scene or GameObject), 3 it ran but not every object came to rest\n"
            << "within --max-seconds (--save still writes the result).\n";
    }

    int runPlacePhysics(int argc, char** argv, const std::string& commandName, std::ostream& out, std::ostream& err) {
        const std::string prefix = commandName + " place-physics: ";
        Options o;
        std::string error;
        if (!parseArgs(argc, argv, o, error)) {
            err << prefix << error << "\n\n";
            printPlacePhysicsUsage(commandName, err);
            return 2;
        }
        if (o.help) {
            printPlacePhysicsUsage(commandName, out);
            return 0;
        }
        std::error_code ec;
        if (!fs::is_regular_file(o.project / "project.yaml", ec)) {
            err << prefix << "no project.yaml in " << o.project.u8string() << "\n";
            return 2;
        }
        std::string report;
        int status = 0;
        {
            out.flush();
            StdoutToStderr redirect(o.quiet);
            status = runInProject(argc, argv, o, prefix, report, err);
        }
        out << report;
        out.flush();
        return status;
    }

}
