// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "vat/VatCli.h"
#include "window/widget/InputTextContextMenu.h"

#include "AppSettings.h"
#include "EditorHost.h"
#include "JasperEngine.h"
#include "Out.h"
#include "Platform.h"
#include "Project.h"
#include "cli/CommandLine.h"
#include "pool/ShaderPool.h"
#include "shader/ShaderBuilder.h"
#include "subsystem/MeshSystem.h"
#include "util/EntityPath.h"
#include "util/FileUtils.h"
#include "util/Util.h"
#include "vat/VatBakeSession.h"
#include "vat/VatFrontEnd.h"

#include "animation/AnimationClipIO.h"
#include "animation/VertexAnimation.h"

#include "json.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <ostream>
#include <set>
#include <sstream>
#include <vector>

#if defined(_WIN32)
    #include <fcntl.h>
    #include <io.h>
#else
    #include <fcntl.h>
    #include <unistd.h>
#endif

namespace fs = std::filesystem;
using Json = nlohmann::json;
using ::jasperengine::vat::VatClip;
using ::jasperengine::vat::VatData;
using ::jasperengine::vat::VatMode;

namespace jasperengine::editor::vat{

namespace{

    // ---------------------------------------------------------------- options

    struct Options{
        fs::path project;
        std::vector<std::string> sources;
        std::string on;
        std::string scene;
        std::vector<std::string> clips;
        std::vector<std::string> sockets;
        VatModeChoice mode = VatModeChoice::Auto;
        float fps = 0.0f;                   // 0: not given (bakes 30, imports the export's)
        bool normals = true;
        VatPrecisionChoice precision = VatPrecisionChoice::Auto;
        VatPreset preset = VatPreset::MobileWeb;
        std::string output;
        bool overwrite = false;
        std::string rebake;
        bool dryRun = false;
        std::string format = "text";
        float toleranceMm = 1.0f;           // the error budget (bakes and imports), > 0
        std::vector<VatClipOverride> clipOverrides;     // --clip-wrap / --clip-interpolate (bakes)
        std::string axis;                   // imports
        float axisScale = 0.0f;             // imports: engine units per metre of the target (0: the importer's)
        int upsample = 0;                   // imports: 0 = automatic
        std::string clipName;               // imports: the clip's name (Houdini; default the asset name)
        bool colors = true;                 // imports: the colour texel when the export has colours
        bool quiet = false;
        bool help = false;
        // the flags given (rebake refuses settings; imports and bakes refuse each other's)
        std::set<std::string> given;
    };

    std::vector<std::string> splitList(const std::string& text){
        std::vector<std::string> out;
        size_t start = 0;
        for (;;){
            const size_t comma = text.find(',', start);
            out.push_back(text.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        return out;
    }

    // a positive number (over `minimum` when exclusive), at most `maximum`
    bool positiveNumber(const std::string& value, double maximum, double& out){
        char* end = nullptr;
        out = std::strtod(value.c_str(), &end);
        return !value.empty() && *end == 0 && std::isfinite(out) && out > 0.0 && out <= maximum;
    }

    // the override of clip `name` (made when missing)
    VatClipOverride& clipOverride(Options& o, const std::string& name){
        for (VatClipOverride& override : o.clipOverrides){
            if (override.name == name) return override;
        }
        VatClipOverride override;
        override.name = name;
        o.clipOverrides.push_back(override);
        return o.clipOverrides.back();
    }

    // "NAME=VALUE" (the last '=' splits: clip names may hold one)
    bool clipSetting(const std::string& text, std::string& name, std::string& value){
        const size_t eq = text.rfind('=');
        if (eq == std::string::npos || eq == 0 || eq + 1 >= text.size()) return false;
        name = text.substr(0, eq);
        value = text.substr(eq + 1);
        return true;
    }

    bool parseArgs(int argc, char** argv, Options& o, std::string& error){
        for (int i = 1; i < argc; i++){
            const std::string arg = argv[i];
            std::string value;
            auto need = [&](){
                if (i + 1 >= argc){
                    error = "missing value for " + arg;
                    return false;
                }
                value = argv[++i];
                o.given.insert(arg);
                return true;
            };
            // a comma-separated list of non-empty names
            auto list = [&](std::vector<std::string>& target, const char* what){
                if (!need()) return false;
                const std::vector<std::string> items = splitList(value);
                for (const std::string& item : items){
                    if (item.empty()){
                        error = "invalid " + arg + " '" + value + "' (" + what + ", separated by commas)";
                        return false;
                    }
                    if (std::find(target.begin(), target.end(), item) == target.end()) target.push_back(item);
                }
                return true;
            };
            if (arg == "-h" || arg == "--help"){
                o.help = true;
            }else if (arg == "-p" || arg == "--project"){
                if (!need()) return false;
                o.project = CommandLine::projectArgPath(FileUtils::pathFromUtf8(value));
            }else if (arg == "-s" || arg == "--source"){
                if (!need()) return false;
                if (value.empty()){
                    error = "invalid --source '' (a file path, or a GameObject path with --scene)";
                    return false;
                }
                o.sources.push_back(value);
            }else if (arg == "--on"){
                if (!need()) return false;
                if (value.empty()){
                    error = "invalid --on '' (a model or prefab file, or a GameObject path with --scene)";
                    return false;
                }
                o.on = value;
            }else if (arg == "--scene"){
                if (!need()) return false;
                if (value.empty()){
                    error = "invalid --scene '' (a scene name, file name or path)";
                    return false;
                }
                o.scene = value;
            }else if (arg == "--clips"){
                if (!list(o.clips, "clip names")) return false;
            }else if (arg == "--sockets"){
                if (!list(o.sockets, "node paths below the source's root")) return false;
            }else if (arg == "--mode"){
                if (!need()) return false;
                if (!modeChoiceFromName(value, o.mode)){
                    error = "invalid --mode '" + value + "' (auto, vertices or bones)";
                    return false;
                }
            }else if (arg == "--fps"){
                if (!need()) return false;
                char* end = nullptr;
                const double v = std::strtod(value.c_str(), &end);
                if (value.empty() || *end != 0 || !std::isfinite(v) || v <= 0.0 || v > 240.0){
                    error = "invalid --fps '" + value + "' (samples per second, over 0 and at most 240)";
                    return false;
                }
                o.fps = static_cast<float>(v);
            }else if (arg == "--normals"){
                if (!need()) return false;
                if (value != "on" && value != "off"){
                    error = "invalid --normals '" + value + "' (on or off)";
                    return false;
                }
                o.normals = value == "on";
            }else if (arg == "--precision"){
                if (!need()) return false;
                if (!precisionChoiceFromName(value, o.precision)){
                    error = "invalid --precision '" + value + "' (auto, half or float)";
                    return false;
                }
            }else if (arg == "--preset"){
                if (!need()) return false;
                if (!presetFromName(value, o.preset)){
                    error = "invalid --preset '" + value + "' (mobile_web: 2048-wide payload for WebGL2/GLES3, or desktop)";
                    return false;
                }
            }else if (arg == "-o" || arg == "--output"){
                if (!need()) return false;
                if (value.empty()){
                    error = "invalid --output '' (a .vat path inside the assets folder)";
                    return false;
                }
                o.output = value;
            }else if (arg == "--overwrite"){
                o.overwrite = true;
                o.given.insert(arg);
            }else if (arg == "--rebake"){
                if (!need()) return false;
                if (value.empty()){
                    error = "invalid --rebake '' (a .vat file)";
                    return false;
                }
                o.rebake = value;
            }else if (arg == "--dry-run"){
                o.dryRun = true;
            }else if (arg == "--format"){
                if (!need()) return false;
                if (value != "text" && value != "json"){
                    error = "--format must be text or json";
                    return false;
                }
                o.format = value;
            }else if (arg == "--axis"){
                if (!need()) return false;
                if (value != "unity" && value != "unreal" && value != "houdini" && value != "zup"){
                    error = "invalid --axis '" + value + "' (the export's target engine: unity, unreal, houdini or zup)";
                    return false;
                }
                o.axis = value;
            }else if (arg == "--tolerance"){
                if (!need()) return false;
                double v = 0.0;
                if (!positiveNumber(value, 1000.0, v)){
                    error = "invalid --tolerance '" + value + "' (the error budget in mm, over 0 and at most 1000)";
                    return false;
                }
                o.toleranceMm = static_cast<float>(v);
            }else if (arg == "--clip-wrap"){
                if (!need()) return false;
                std::string name, wrapText;
                ClipWrapMode wrap = ClipWrapMode::Default;
                if (!clipSetting(value, name, wrapText) || !::jasperengine::anim::wrapFromName(wrapText, wrap) || wrap == ClipWrapMode::Default){
                    error = "invalid --clip-wrap '" + value + "' (NAME=WRAP, WRAP once, loop, ping_pong or clamp_forever)";
                    return false;
                }
                clipOverride(o, name).wrap = wrap;
            }else if (arg == "--clip-interpolate"){
                if (!need()) return false;
                std::string name, onOff;
                if (!clipSetting(value, name, onOff) || (onOff != "on" && onOff != "off")){
                    error = "invalid --clip-interpolate '" + value + "' (NAME=on or NAME=off)";
                    return false;
                }
                clipOverride(o, name).interpolate = onOff == "on";
            }else if (arg == "--axis-scale"){
                if (!need()) return false;
                double v = 0.0;
                if (!positiveNumber(value, 1e6, v)){
                    error = "invalid --axis-scale '" + value + "' (engine units per metre of the export's target, over 0)";
                    return false;
                }
                o.axisScale = static_cast<float>(v);
            }else if (arg == "--clip-name"){
                if (!need()) return false;
                if (value.empty()){
                    error = "invalid --clip-name '' (the imported clip's name)";
                    return false;
                }
                o.clipName = value;
            }else if (arg == "--colors"){
                if (!need()) return false;
                if (value != "on" && value != "off"){
                    error = "invalid --colors '" + value + "' (on or off)";
                    return false;
                }
                o.colors = value == "on";
            }else if (arg == "--upsample"){
                if (!need()) return false;
                char* end = nullptr;
                errno = 0;
                const long v = std::strtol(value.c_str(), &end, 10);
                if (value.empty() || *end != 0 || errno == ERANGE || v < 0 || v > 1024){
                    error = "invalid --upsample '" + value + "' (0 = automatic, or a factor 1..1024)";
                    return false;
                }
                o.upsample = static_cast<int>(v);
            }else if (arg == "-q" || arg == "--quiet"){
                o.quiet = true;
            }else{
                error = (!arg.empty() && arg[0] == '-' ? "unknown option " : "unexpected argument ") + arg;
                return false;
            }
        }
        if (o.help) return true;
        if (o.project.empty()){
            error = "--project is required";
            return false;
        }
        if (!o.rebake.empty()){
            for (const char* flag : {"--source", "--on", "--scene", "--clips", "--sockets", "--mode", "--fps", "--normals", "--precision",
                                     "--preset", "--output", "--overwrite", "--axis", "--upsample", "--tolerance", "--clip-wrap",
                                     "--clip-interpolate", "--axis-scale", "--clip-name", "--colors"}){
                if (o.given.count(flag) || (std::strcmp(flag, "--source") == 0 && o.given.count("-s")) ||
                    (std::strcmp(flag, "--output") == 0 && o.given.count("-o"))){
                    error = std::string("--rebake re-runs the .vat's recipe and writes over it (GUID and hand edits kept): leave out ") + flag +
                            " (to change settings, bake again from the source with --output <the .vat> --overwrite)";
                    return false;
                }
            }
            return true;
        }
        if (o.sources.empty()){
            error = "--source is required (a model, model#Clip, .anim files with --on, a .poseanim, a GameObject path with --scene, or a "
                    "Houdini VAT 3 / OpenVAT export), or --rebake <a .vat>";
            return false;
        }
        if (o.overwrite && o.output.empty()){
            error = "--overwrite needs --output (the default output never replaces another bake: it takes a free name)";
            return false;
        }
        return true;
    }

    // ---------------------------------------------------------------- engine guards (place-physics')

    struct EngineRuntimeGuard{
        ~EngineRuntimeGuard(){
            ::jasperengine::ShaderPool::setShaderBuilder(nullptr);
            ShaderBuilder::requestShutdown();
            ::jasperengine::Engine::systemShutdown();
        }
    };

    struct EditorHostOverrideGuard{
        EditorHost* previousHost = nullptr;
        explicit EditorHostOverrideGuard(EditorHost* host): previousHost(&getEditorHost()){
            setEditorHost(host);
        }
        ~EditorHostOverrideGuard(){
            setEditorHost(previousHost);
        }
    };

    // The engine logs to stdout: while it runs, stdout goes to stderr (to the null device with -q),
    // so stdout carries only the result
    class StdoutToStderr{
    public:
        explicit StdoutToStderr(bool discard){
            std::cout.flush();
            std::fflush(stdout);
#if defined(_WIN32)
            saved = _dup(1);
            const int target = discard ? _open("NUL", _O_WRONLY) : _dup(2);
            if (target >= 0){
                _dup2(target, 1);
                _close(target);
            }
#else
            saved = dup(1);
            const int target = discard ? open("/dev/null", O_WRONLY) : dup(2);
            if (target >= 0){
                dup2(target, 1);
                close(target);
            }
#endif
        }
        ~StdoutToStderr(){
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

    // ---------------------------------------------------------------- the run's result

    std::string fixed(double v, int digits){
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.*f", digits, v);
        return buffer;
    }

    // a fix's action as the flag that does it ("--fps 20"), "" when it has none here
    std::string fixFlag(const VatFix& fix){
        const Json value = Json::parse(fix.value, nullptr, false);
        if (value.is_discarded()) return std::string();
        auto text = [&](){ return value.is_string() ? value.get<std::string>() : value.dump(); };
        if (fix.setting == "fps" && value.is_number()) return "--fps " + text();
        if (fix.setting == "normals" && value.is_boolean()) return std::string("--normals ") + (value.get<bool>() ? "on" : "off");
        if ((fix.setting == "mode" || fix.setting == "preset" || fix.setting == "precision") && value.is_string()) return "--" + fix.setting + " " + text();
        if (fix.setting == "output" && value.is_string()) return "--output " + text();
        if (fix.setting == "overwrite" && value.is_boolean() && value.get<bool>()) return "--overwrite";
        if (fix.setting == "interpolate" && value.is_object() && value.contains("clip") && value["clip"].is_string() && value.contains("value") &&
            value["value"].is_boolean()){
            return "--clip-interpolate \"" + value["clip"].get<std::string>() + "=" + (value["value"].get<bool>() ? "on" : "off") + "\"";
        }
        if (fix.setting == "clips" && value.is_array()){
            std::string names;
            for (const Json& name : value){
                if (!name.is_string()) return std::string();
                names += (names.empty() ? "" : ",") + name.get<std::string>();
            }
            return names.empty() ? std::string() : "--clips " + names;
        }
        return std::string();
    }

    std::string issueLine(const VatIssue& issue){
        std::string line = issue.kind + ": " + issue.message;
        if (!issue.fix.empty()) line += " (fix: " + issue.fix + ")";
        std::string flags;
        for (const VatFix& fix : issue.fixes){
            const std::string flag = fixFlag(fix);
            if (!flag.empty()) flags += (flags.empty() ? "" : " | ") + flag;
        }
        if (!flags.empty()) line += " [try: " + flags + "]";
        return line;
    }

    struct Run{
        const Options* options = nullptr;
        std::string prefix;                 // "bake-vat: "
        std::ostream* err = nullptr;
        Json result = Json::object();
        Json errors = Json::array();
        std::string text;                   // the text summary (stdout)

        bool json() const{ return options->format == "json"; }

        // an error: in the JSON, or on stderr; returns `code`
        int fail(int code, const std::string& kind, const std::string& message, const std::string& fix){
            errors.push_back({{"kind", kind}, {"message", message}, {"fix", fix}});
            if (!json()) *err << prefix << kind << ": " << message << (fix.empty() ? std::string() : " (fix: " + fix + ")") << "\n";
            return code;
        }
        int failIssues(int code, const std::vector<VatIssue>& issues){
            for (const VatIssue& issue : issues){
                if (!issue.error) continue;
                Json item = issueJson(issue);
                item.erase("error");
                errors.push_back(item);
                if (!json()) *err << prefix << issueLine(issue) << "\n";
            }
            return code;
        }
        void progress(const std::string& line){
            if (!options->quiet && !json()) *err << prefix << line << "\n";
        }
    };

    // "Bones mode (every vertex follows one bone): 120 vertices, 2 bones, texture 2048x1 rgba32f,
    // 0.01 MB, error max 0.00 mm (mean 0.00), 4 fetches per vertex"
    std::string reportLine(const VatReport& report){
        std::string text = std::string(report.mode == VatMode::Bones ? "Bones" : "Vertices") + " mode";
        if (!report.modeReason.empty()) text += " (" + report.modeReason + ")";
        text += ": " + std::to_string(report.vertices) + " vertices";
        if (report.mode == VatMode::Bones) text += ", " + std::to_string(report.bones) + (report.bones == 1 ? " bone" : " bones");
        text += ", texture " + std::to_string(report.width) + "x" + std::to_string(report.height) + " " + textureFormatName(report.precision) +
                ", " + fixed(static_cast<double>(report.bytes) / (1024.0 * 1024.0), 2) + " MB, error max " + fixed(report.maxErrorMm, 2) +
                " mm (mean " + fixed(report.meanErrorMm, 2) + "), " + std::to_string(report.fetchesPerVertex) + " fetches per vertex";
        return text;
    }

    std::string clipsText(const std::vector<VatClip>& clips){
        std::string text;
        for (size_t i = 0; i < clips.size(); i++){
            text += (i ? ", " : "") + clips[i].name + " (" + std::to_string(clips[i].frames) + " frames, " + fixed(clips[i].fps, 2) + " fps)";
        }
        return text.empty() ? std::string("no clips") : text;
    }

    // the summary of a finished bake or import (`title`: "baked assets/models/hero.gltf")
    void summarize(Run& run, const std::string& title, const std::vector<VatClip>& clips, const VatReport& report){
        run.text += title + ": " + std::to_string(clips.size()) + " clip" + (clips.size() == 1 ? "" : "s") + ": " + clipsText(clips) + "\n";
        run.text += "  " + reportLine(report) + "\n";
        for (const VatIssue& warning : report.warnings) run.text += "  warning: " + issueLine(warning) + "\n";
    }

    void plannedText(Run& run, const std::string& vat, bool rewrite){
        run.result["output"] = {{"vat", vat}, {"rewrite", rewrite}};
        run.text += "  dry run, nothing written: would write " + vat + " (+ " + vat + ".glb" +
                    (rewrite ? ", rewriting the existing file: GUID and hand edits kept)" : ")") + "\n";
    }

    void writtenText(Run& run, const VatWriteResult& out){
        run.result["output"] = {{"vat", out.vat}, {"rewrite", out.rewritten}};
        run.result["written"] = {{"vat", out.vat}, {"mesh", out.mesh}, {"files", out.files}, {"guid", out.guid}, {"rewritten", out.rewritten}};
        std::string files;
        for (size_t i = 0; i < out.files.size(); i++) files += (i ? ", " : "") + out.files[i];
        run.text += "  wrote " + files + (out.rewritten ? " (rewrote the existing file: GUID and hand edits kept)" : "") + ", guid " + out.guid + "\n";
        run.text += "  use it: a GameObject with Model " + out.mesh + " and a Vertex Animator whose animation is " + out.vat + "\n";
    }

    // ---------------------------------------------------------------- paths and scenes

    bool fileThere(const fs::path& path){
        std::error_code ec;
        return fs::exists(path, ec);
    }

    bool inside(const fs::path& root, const fs::path& path){
        const fs::path rel = path.lexically_normal().lexically_relative(root.lexically_normal());
        return !rel.empty() && *rel.begin() != ".." && !rel.is_absolute();
    }

    // A path argument as an absolute path: absolute as given; relative to the current directory
    // when it is there (an output: when that lands inside the project), else to the project. A
    // "model#Clip" keeps its suffix.
    std::string resolveArg(const fs::path& root, const std::string& given, bool output){
        const fs::path path = FileUtils::pathFromUtf8(given);
        if (path.is_absolute()) return FileUtils::pathToUtf8(path.lexically_normal());
        const fs::path fromCwd = CommandLine::absoluteArgPath(path);
        const fs::path fromProject = (root / path).lexically_normal();
        if (fileThere(fromCwd)) return FileUtils::pathToUtf8(fromCwd);
        if (fileThere(fromProject)) return FileUtils::pathToUtf8(fromProject);
        const size_t hash = given.rfind('#');
        if (!output && hash != std::string::npos && hash > 0){
            return resolveArg(root, given.substr(0, hash), false) + given.substr(hash);
        }
        if (output && inside(root, fromCwd)) return FileUtils::pathToUtf8(fromCwd);
        return FileUtils::pathToUtf8(fromProject);
    }

    std::string sceneList(const Project& project){
        std::string names;
        for (const SceneProject& sp : project.getScenes()){
            if (sp.filepath.empty()) continue;
            names += (names.empty() ? "" : ", ") + sp.name + " (" + FileUtils::pathToGenericUtf8(sp.filepath) + ")";
        }
        return names.empty() ? std::string("none") : names;
    }

    fs::path sceneFile(Project& project, const SceneProject& sp){
        return (sp.filepath.is_absolute() ? sp.filepath : project.getProjectPath() / sp.filepath).lexically_normal();
    }

    // A scene of the project by name, file name or path (absolute, relative to the current
    // directory, else to the project), loaded with its models
    SceneProject* loadSceneArg(Project& project, const std::string& ref){
        const fs::path asPath = FileUtils::pathFromUtf8(resolveArg(project.getProjectPath(), ref, false));
        SceneProject* found = nullptr;
        for (SceneProject& sp : project.getScenes()){
            if (sp.filepath.empty()) continue;
            if (sp.name == ref || FileUtils::pathToUtf8(sp.filepath.filename()) == ref ||
                CommandLine::absoluteArgPath(sceneFile(project, sp)) == CommandLine::absoluteArgPath(asPath)){
                found = &sp;
                break;
            }
        }
        if (!found) return nullptr;
        const uint32_t id = found->id;
        if (!found->scene && !project.loadScene(found->filepath, false, false, true)) return nullptr;
        // loading turns asynchronous loading back on: the scene's models load now
        ::jasperengine::Engine::setAsyncLoading(false);
        SceneProject* sp = project.getScene(id);
        if (!sp || !sp->scene) return nullptr;
        if (std::shared_ptr<MeshSystem> meshSystem = sp->scene->getSystem<MeshSystem>()) meshSystem->load();
        return sp;
    }

    // ---------------------------------------------------------------- the command

    std::string sceneRelative(Project& project, const SceneProject& sp){
        return FileUtils::pathToGenericUtf8(project.normalizeToProjectRelative(sceneFile(project, sp)));
    }

    // a GameObject of `sp` by path (else a unique name); 0 with the error printed
    Entity gameObjectArg(Run& run, SceneProject& sp, const std::string& ref, const char* flag, std::string& path){
        const std::vector<Entity> found = EntityPath::resolve(sp, ref);
        if (found.empty()){
            run.fail(2, "invalid_source", std::string(flag) + ": no GameObject '" + ref + "' in scene " + sp.name,
                     "give its hierarchy path in the scene (e.g. Crowd/Hero)");
            return NULL_ENTITY;
        }
        if (found.size() > 1){
            EntityPath::Cache cache;
            std::string list;
            for (size_t i = 0; i < found.size(); i++) list += (i ? ", " : "") + EntityPath::of(sp, found[i], &cache);
            run.fail(2, "invalid_source", std::string(flag) + ": '" + ref + "' names " + std::to_string(found.size()) + " GameObjects in scene " +
                     sp.name + ": " + list, "give the path");
            return NULL_ENTITY;
        }
        path = EntityPath::of(sp, found.front());
        return found.front();
    }

    int bakeWithSession(Project& project, Run& run, VatBakeSession& session, const std::string& title){
        const Options& o = *run.options;
        const std::vector<VatIssue> issues = session.check();
        if (hasErrors(issues)){
            Json clips = Json::array();
            for (const VatSourceClip& clip : session.sourceClips()) clips.push_back(clip.name);
            if (!clips.empty()) run.result["source_clips"] = clips;
            return run.failIssues(1, issues);
        }
        // --clip-wrap / --clip-interpolate of a clip the source doesn't have
        std::string available;
        for (const VatSourceClip& clip : session.sourceClips()) available += (available.empty() ? "" : ", ") + clip.name;
        for (const VatClipOverride& override : session.spec().clipOverrides){
            bool known = false;
            for (const VatSourceClip& clip : session.sourceClips()) known = known || clip.name == override.name;
            if (!known){
                return run.fail(1, "invalid_settings", "the source has no clip '" + override.name + "' (--clip-wrap / --clip-interpolate)",
                                "choose from: " + available);
            }
        }
        const VatEstimate estimate = session.estimate();
        run.progress("baking " + std::to_string(estimate.clips.size()) + " clip(s), about " + std::to_string(estimate.width) + "x" +
                     std::to_string(estimate.height) + " texels");
        if (!session.run()){
            if (session.state() == VatBakeState::Cancelled) return run.fail(1, "cancelled", "the bake was cancelled", "bake again");
            if (!session.failureIssues().empty()) return run.failIssues(1, session.failureIssues());
            return run.fail(1, "bake_failed", session.error().empty() ? std::string("the bake failed") : session.error(), "check the source");
        }
        const VatBakeResult& result = *session.result();
        run.result["report"] = reportJson(result.report);
        summarize(run, title, result.data.clips, result.report);
        if (o.dryRun){
            std::string planned, error;
            bool rewrite = false;
            if (!session.plannedOutput(planned, rewrite, error)) return run.fail(1, "output_exists", error, "pass --overwrite, or choose another --output");
            plannedText(run, planned, rewrite);
            return 0;
        }
        VatWriteResult out;
        std::string error;
        if (!session.write(out, error)){
            const bool taken = error.find("exists") != std::string::npos;
            return run.fail(1, taken ? "output_exists" : "write_failed", "nothing was written: " + error,
                            taken ? "pass --overwrite, or choose another --output" : "check that the output folder is writable");
        }
        writtenText(run, out);
        return 0;
    }

    int importAndWrite(Project& project, Run& run, VatImportSpec spec, const VatImportDetection& detection,
                       const VatImportPlan& options, const std::string& title){
        const Options& o = *run.options;
        std::vector<VatIssue> detected;
        for (const VatIssue& issue : detection.issues){
            if (issue.error) detected.push_back(issue);
        }
        if (!detected.empty()) return run.failIssues(1, detected);
        run.progress("importing " + std::string(importKindName(detection.kind)) + " " + detection.summary);
        VatImportResult result = importVat(spec);
        if (!result.ok()){
            std::vector<VatIssue> errors = result.errors;
            for (VatIssue& issue : errors) issue.error = true;
            return run.failIssues(1, errors);
        }
        run.result["report"] = Json::parse(vatImportReportJson(result.report), nullptr, false);
        summarize(run, title, result.data.clips, result.report);
        if (o.dryRun){
            std::string planned, error;
            bool rewrite = false;
            if (!planVatImport(&project, result, detection, options, planned, rewrite, error)){
                return run.fail(1, error.find("exists") != std::string::npos ? "output_exists" : "invalid_output", error,
                                "pass --overwrite, or choose another --output");
            }
            plannedText(run, planned, rewrite);
            return 0;
        }
        VatWriteResult out;
        std::string error;
        if (!writeImportedVat(&project, result, detection, options, out, error)){
            const bool taken = error.find("exists") != std::string::npos;
            return run.fail(1, taken ? "output_exists" : "write_failed", "nothing was written: " + error,
                            taken ? "pass --overwrite, or choose another --output" : "check that the output folder is writable");
        }
        writtenText(run, out);
        return 0;
    }

    int rebake(Project& project, Run& run){
        const Options& o = *run.options;
        const std::string given = resolveArg(project.getProjectPath(), o.rebake, false);
        std::string relative, error;
        if (!projectRelativeFile(&project, given, true, relative, error) || !Util::isVertexAnimationFile(relative)){
            return run.fail(2, "usage", "--rebake: " + (error.empty() ? "'" + o.rebake + "' is not a .vat file" : error),
                            "give a .vat of the project (e.g. assets/vat/hero.vat)");
        }
        VatData data;
        if (!readVatAsset(&project, relative, data, &error)){
            return run.fail(1, "vat-invalid", relative + " can't be read: " + error, "bake it again from its source with --output " + relative + " --overwrite");
        }
        run.result["source"] = {{"rebake", relative}, {"kind", data.recipe.sources.empty() ? std::string() : data.recipe.sources[0].kind}};
        // the scenes its GameObject sources live in
        for (const ::jasperengine::vat::VatRecipeSource& source : data.recipe.sources){
            if (source.scene.empty()) continue;
            if (!loadSceneArg(project, source.scene)){
                return run.fail(1, "invalid_source", "the recipe's scene '" + source.scene + "' is not in the project or doesn't load",
                                "restore the scene, or bake again from another source");
            }
        }
        if (isImportRecipe(data.recipe)){
            // what the MCP tool's rebake does (and the baker window's Re-import): the recipe imported again, written
            // over the file with its GUID and hand edits
            VatImportSpec spec;
            if (!importSpecFromRecipe(data.recipe, FileUtils::pathToUtf8(project.getProjectPath()), spec, error)){
                return run.fail(1, "invalid_source", error, "bake it again from its source");
            }
            VatImportPlan options;
            options.output = relative;
            options.overwrite = true;
            return importAndWrite(project, run, spec, detectVatImport(spec.path), options, "re-imported " + relative);
        }
        VatBakeSession session(&project);
        if (!session.loadRebake(relative, error)) return run.fail(1, "invalid_source", "can't rebake " + relative + ": " + error, "bake it again from its source");
        return bakeWithSession(project, run, session, "rebaked " + relative);
    }

    int bake(Project& project, Run& run){
        const Options& o = *run.options;
        const fs::path root = project.getProjectPath();

        // the scene of a GameObject source or target
        SceneProject* sp = nullptr;
        if (!o.scene.empty()){
            sp = loadSceneArg(project, o.scene);
            if (!sp){
                return run.fail(2, "usage", "--scene: no scene '" + o.scene + "' in the project, or it doesn't load (scenes: " + sceneList(project) + ")",
                                "give a scene of the project by name, file name or path");
            }
        }

        VatBakeSpec spec;
        VatSourceRoute route;
        std::string onFile;
        if (sp && o.on.empty()){
            // a GameObject: its Animation Player, Animations, Pose Animator
            if (o.sources.size() != 1){
                return run.fail(2, "usage", "with --scene (and no --on) --source is one GameObject path", "bake one GameObject at a time");
            }
            std::string path;
            if (gameObjectArg(run, *sp, o.sources[0], "--source", path) == NULL_ENTITY) return 2;
            spec.source.kind = VatSourceKind::Entity;
            spec.source.scene = sceneRelative(project, *sp);
            spec.source.path = path;
        }else{
            std::vector<std::string> files;
            for (const std::string& source : o.sources) files.push_back(resolveArg(root, source, false));
            if (!o.on.empty() && !sp) onFile = resolveArg(root, o.on, false);
            VatIssue issue;
            if (!routeVatSource(&project, files, onFile, true, route, issue)){
                return run.fail(2, issue.kind.empty() ? std::string("invalid_source") : issue.kind, issue.message, issue.fix);
            }
            if (!route.import){
                spec.source = route.source;
                spec.clips = route.clips;
                if (sp){
                    std::string path;
                    if (gameObjectArg(run, *sp, o.on, "--on", path) == NULL_ENTITY) return 2;
                    spec.source.scene = sceneRelative(project, *sp);
                    spec.source.path = path;
                }else if (route.needsTarget){
                    return run.fail(2, "usage", std::string("--on is required: the model, prefab or GameObject (with --scene) the ") +
                                                (route.source.kind == VatSourceKind::Pose ? ".poseanim" : ".anim") + " plays on",
                                    "add --on assets/models/hero.gltf");
                }
            }
        }

        // what each kind of source takes
        if (route.import){
            for (const char* flag : {"--clips", "--sockets", "--on", "--scene", "--clip-wrap", "--clip-interpolate"}){
                if (o.given.count(flag)){
                    return run.fail(2, "usage", std::string(flag) + " is for baked sources; an import (" + importKindName(route.detection.kind) +
                                                ") takes the export's clips as they are", std::string("leave ") + flag + " out");
                }
            }
        }else{
            for (const char* flag : {"--axis", "--upsample", "--axis-scale", "--clip-name", "--colors"}){
                if (o.given.count(flag)){
                    return run.fail(2, "usage", std::string(flag) + " is an importer option (Houdini VAT 3 / OpenVAT exports); this source is baked",
                                    std::string("leave ") + flag + " out");
                }
            }
        }

        // the output: a .vat inside the assets folder
        std::string output;
        if (!o.output.empty()){
            const std::string absolute = resolveArg(root, o.output, true);
            std::string error;
            if (!projectRelativeFile(&project, absolute, false, output, error) || !Util::isVertexAnimationFile(output) ||
                !project.isInsideAssetsPath(root / FileUtils::pathFromUtf8(output))){
                return run.fail(2, "usage", "--output: " + (error.empty() ? "'" + o.output + "' is not a .vat path inside the assets folder" : error),
                                "give a .vat path inside the assets folder (e.g. assets/vat/hero.vat; the mesh goes to hero.vat.glb)");
            }
        }

        if (route.import){
            run.result["source"] = {{"kind", importKindName(route.detection.kind)}, {"path", FileUtils::pathToGenericUtf8(
                project.normalizeToProjectRelative(FileUtils::pathFromUtf8(route.importPath)))}};
            if (!output.empty() && !o.overwrite){
                const fs::path absolute = root / FileUtils::pathFromUtf8(output);
                if (fileThere(absolute) || fileThere(FileUtils::pathFromUtf8(meshPathFor(FileUtils::pathToUtf8(absolute))))){
                    return run.fail(1, "output_exists", "'" + output + "' exists", "pass --overwrite, or choose another --output (or leave it out: a free name next to the export)");
                }
            }
            VatImportSpec importSpec;
            importSpec.path = route.importPath;
            importSpec.projectRoot = FileUtils::pathToUtf8(root);
            importSpec.fps = o.fps;
            importSpec.axis = o.axis;
            importSpec.upsample = o.upsample;
            importSpec.mode = modeChoiceName(o.mode);
            importSpec.precision = precisionChoiceName(o.precision);
            importSpec.preset = presetName(o.preset);
            importSpec.normals = o.normals;
            importSpec.toleranceMm = o.toleranceMm;
            importSpec.axisScale = o.axisScale;
            importSpec.clipName = o.clipName;
            importSpec.colors = o.colors;
            VatImportPlan options;
            options.output = output;
            options.overwrite = o.overwrite;
            return importAndWrite(project, run, importSpec, route.detection, options,
                                  "imported " + std::string(importKindName(route.detection.kind)) + " " + run.result["source"]["path"].get<std::string>());
        }

        for (const std::string& clip : o.clips){
            if (std::find(spec.clips.begin(), spec.clips.end(), clip) == spec.clips.end()) spec.clips.push_back(clip);
        }
        spec.mode = o.mode;
        if (o.fps > 0.0f) spec.fps = o.fps;
        spec.normals = o.normals;
        spec.precision = o.precision;
        spec.preset = o.preset;
        spec.sockets = o.sockets;
        spec.toleranceMm = o.toleranceMm;
        spec.clipOverrides = o.clipOverrides;
        spec.output = output;
        spec.overwrite = o.overwrite;
        spec.place = VatPlace::None;
        run.result["source"] = {{"kind", sourceKindName(spec.source.kind)}, {"path", spec.source.path}};
        if (!spec.source.scene.empty()) run.result["source"]["scene"] = spec.source.scene;
        if (!spec.source.anims.empty()) run.result["source"]["anims"] = spec.source.anims;
        VatBakeSession session(&project);
        session.setSpec(spec);
        std::string what = spec.source.path;
        if (!spec.source.anims.empty()){
            what.clear();
            for (const std::string& anim : spec.source.anims) what += (what.empty() ? "" : ", ") + anim;
            what += " on " + spec.source.path;
        }
        if (!spec.source.scene.empty()) what += " (" + spec.source.scene + ")";
        return bakeWithSession(project, run, session, "baked " + what);
    }

    int runInProject(int argc, char** argv, Run& run){
        const Options& o = *run.options;
        EditorHostOverrideGuard hostOverride(nullptr);
        Out::setConsoleWindow(nullptr);
        AppSettings::initialize();

        Project project;
        auto platform = std::make_unique<Platform>(&project);
        ::jasperengine::Engine::systemInit(argc, argv, platform.get());
        EngineRuntimeGuard engineRuntime;
        ::jasperengine::Engine::pauseGameEvents(true);
        ::jasperengine::Engine::setAsyncLoading(false);

        // the asset database's identities stay in memory (S04: no metas or in-file GUIDs
        // written for the project's other files): a usage error or a dry run writes nothing,
        // a bake writes its .vat and .vat.glb only (the editor gives the mesh its meta)
        project.setReadOnlyAssets(true);
        if (!project.loadProject(o.project, false)){
            return run.fail(2, "usage", "the project could not be loaded: " + FileUtils::pathToUtf8(o.project), "give a project directory or its project.yaml");
        }
        // loading the project turns asynchronous loading back on: models load now
        ::jasperengine::Engine::setAsyncLoading(false);
        return o.rebake.empty() ? bake(project, run) : rebake(project, run);
    }

}

void printBakeVatUsage(const std::string& commandName, std::ostream& out){
    out << "Usage:\n"
        << "  " << commandName << " bake-vat --project <path> --source <path> [--source <path>...] [options]\n"
        << "  " << commandName << " bake-vat --project <path> --rebake <file.vat> [--dry-run] [--format text|json]\n\n"
        << "Bakes a Vertex Animation (a .vat and its .vat.glb mesh) without a window: the animations of a model, of\n"
        << ".anim clips played on a model, prefab or GameObject, of a pose animation or of a GameObject (its Animation\n"
        << "Player, Animations, Pose Animator), or imports a Houdini VAT 3 / OpenVAT export. The same bake and report as\n"
        << "the bake_vertex_animation MCP tool, files only (no GameObject is placed).\n\n"
        << "Options:\n"
        << "  -p, --project <path>      Project directory or project.yaml.\n"
        << "  -s, --source <path>       What to bake: a model (.gltf/.glb/.fbx; model#Clip for one clip), .anim files (repeat\n"
        << "                            --source; with --on), a .poseanim (--on: its model, default its preview model), a\n"
        << "                            GameObject path with --scene, or a Houdini VAT 3 / OpenVAT export (its folder, mesh,\n"
        << "                            .mat or -remap_info.json; may be outside the project).\n"
        << "      --on <path>           What .anim / .poseanim sources play on: a model or prefab file, or a GameObject path\n"
        << "                            with --scene.\n"
        << "      --scene <scene>       The scene of a GameObject --source or --on: name, file name or path.\n"
        << "      --clips <A,B>         The clips to bake, in this order (default: all).\n"
        << "      --mode <mode>         auto (default: Bones when skin and rigid parts explain every vertex), vertices or bones.\n"
        << "      --fps <n>             Samples per second, over 0 and at most 240 (default 30). For an import: the export's\n"
        << "                            frame rate when its metadata has none (or to override it).\n"
        << "      --normals <on|off>    Vertices mode: bake the tangent frame (default on).\n"
        << "      --precision <p>       auto (default: half, float when half is over the tolerance), half or float.\n"
        << "      --preset <preset>     mobile_web (default: 2048-wide payload, at most 2048 rows: WebGL2/GLES3) or desktop\n"
        << "                            (4096 wide, 8192 rows).\n"
        << "      --sockets <a,b>       Node paths below the source's root to bake as sockets.\n"
        << "      --clip-wrap <NAME=WRAP>  A clip's wrap in the .vat: once, loop, ping_pong or clamp_forever (default: the\n"
        << "                            source's; a loop is sampled for its seam). Repeat for several clips.\n"
        << "      --clip-interpolate <NAME=on|off>  off: the clip plays its nearest lower sample (the fix of a\n"
        << "                            large_interpolation_error the sample rate can't cure). Repeat for several clips.\n"
        << "      --tolerance <mm>      The error budget (default 1 mm; scaled up to 0.05% of the bounds' diagonal): half\n"
        << "                            precision is kept within it, larger interpolation errors are warned about.\n"
        << "  -o, --output <file.vat>   Where to write (inside the assets folder; the mesh goes to <file>.vat.glb). An\n"
        << "                            existing file is refused without --overwrite. Default: <source dir>/<source name>.vat:\n"
        << "                            the bake of the same source there is rewritten (GUID and hand edits kept), another\n"
        << "                            file there makes it take a free name (\"hero 2.vat\").\n"
        << "      --overwrite           Replace an existing --output (its GUID is kept).\n"
        << "      --rebake <file.vat>   Bake a .vat again from its recipe (an import's: import it again) and write over it,\n"
        << "                            keeping its GUID, clip names, events, speeds and wraps. Takes no other settings.\n"
        << "      --axis <axis>         Import: the Houdini export's target engine when its metadata doesn't say (unity,\n"
        << "                            unreal, houdini, zup).\n"
        << "      --axis-scale <n>      Import: engine units per metre of the export's target (default 1; Unreal 100).\n"
        << "      --upsample <n>        Import: Houdini rigid pieces' frame upsampling (0 = automatic, 1..1024).\n"
        << "      --clip-name <name>    Import: the clip's name (a Houdini export has one clip; default the asset name).\n"
        << "      --colors <on|off>     Import: keep the export's vertex colours (default on).\n"
        << "      --dry-run             Bake (or import) and print the report and where it would write; write nothing.\n"
        << "      --format <format>     text (default): a summary, warnings and their fixes; json: one object on stdout\n"
        << "                            {ok, dry_run, source, report, output: {vat, rewrite}, written: {vat, mesh, files, guid,\n"
        << "                            rewritten}, errors: [{kind, message, fix}], source_clips (a clip the source lacks)}.\n"
        << "  -q, --quiet               No engine log or progress on stderr.\n"
        << "  -h, --help                Show this help.\n\n"
        << "Paths are absolute, relative to the current directory, or relative to the project. The files are encoded\n"
        << "first and written all or nothing. Errors print \"bake-vat: <kind>: <message> (fix: <fix>)\" on stderr (in the\n"
        << "JSON with --format json).\n"
        << "Exit code: 0 done (warnings allowed), 1 the bake, import or write failed (e.g. too_large, no_animation,\n"
        << "output_exists), 2 usage error (bad option or value, missing --project or --source, a source that is not a bake\n"
        << "source, an unknown scene or GameObject, a project that doesn't load).\n";
}

int runBakeVat(int argc, char** argv, const std::string& commandName, std::ostream& out, std::ostream& err){
    Options o;
    Run run;
    run.options = &o;
    run.prefix = "bake-vat: ";
    run.err = &err;
    // the format first, so a usage error is reported in it
    for (int i = 1; i + 1 < argc; i++){
        if (std::strcmp(argv[i], "--format") == 0 && std::strcmp(argv[i + 1], "json") == 0) o.format = "json";
    }
    auto finish = [&](int code){
        if (o.format == "json"){
            Json result = {{"ok", code == 0}, {"dry_run", o.dryRun}};
            for (auto it = run.result.begin(); it != run.result.end(); ++it) result[it.key()] = it.value();
            if (!run.errors.empty()) result["errors"] = run.errors;
            out << result.dump(1, ' ', false, Json::error_handler_t::replace) << "\n";
        }else{
            out << run.text;
        }
        out.flush();
        return code;
    };
    std::string error;
    const std::string format = o.format;
    if (!parseArgs(argc, argv, o, error)){
        o.format = format;
        run.fail(2, "usage", error, "run " + commandName + " bake-vat --help for the options");
        return finish(2);
    }
    if (o.help){
        printBakeVatUsage(commandName, out);
        return 0;
    }
    std::error_code ec;
    if (!fs::is_regular_file(o.project / "project.yaml", ec)){
        run.fail(2, "usage", "no project.yaml in " + FileUtils::pathToUtf8(o.project), "give a project directory or its project.yaml");
        return finish(2);
    }
    int status = 0;
    {
        out.flush();
        StdoutToStderr redirect(o.quiet);
        status = runInProject(argc, argv, run);
    }
    return finish(status);
}

// ---------------------------------------------------------------- check --vat

namespace{

    // "12:5: message" -> 12, 5, "message"
    void splitLineColumn(const std::string& error, int& line, int& col, std::string& message){
        line = col = 0;
        message = error;
        int l = 0, c = 0, used = 0;
        if (std::sscanf(error.c_str(), "%d:%d: %n", &l, &c, &used) == 2 && used > 0){
            line = l;
            col = c;
            message = error.substr(static_cast<size_t>(used));
        }
    }

    bool readText(const fs::path& path, std::string& text){
        return ProjectChecks::readFile(path, text);
    }

}

VatCheck checkVatAssets(Project* project, const std::vector<fs::path>& files){
    VatCheck check;
    if (!project) return check;
    const fs::path root = project->getProjectPath();
    const std::vector<fs::path> assets = files.empty() ? ProjectChecks::findProjectFiles(root, {::jasperengine::vat::VAT_EXTENSION}) : files;
    for (const fs::path& file : assets){
        std::string text;
        const std::string rel = ProjectChecks::projectRelative(root, file);
        if (!readText(file, text)) continue;
        check.assets++;
        auto add = [&](const std::string& severity, const std::string& kind, const std::string& message, const std::string& fix, int line = 0,
                       int col = 0){
            check.findings.push_back({"vat", rel, line, col, severity, message, kind, fix});
        };
        const ::jasperengine::vat::VatParseResult parsed = ::jasperengine::vat::parseVat(text);
        if (!parsed.ok()){
            int line = 0, col = 0;
            std::string message;
            splitLineColumn(parsed.error, line, col, message);
            add("error", "vat-invalid", message, "bake it again from its source (bake-vat --source ... --output " + rel + " --overwrite), or restore the file",
                line, col);
            continue;
        }
        for (const ClipWarning& warning : parsed.warnings){
            add("warning", "vat-format", (warning.path.empty() ? "" : warning.path + ": ") + warning.message, "fix the value (it was repaired on load)");
        }
        // the staleness fixes name the Inspector and the MCP tool (where the window and agents read
        // them); on the command line the rebake is bake-vat's
        const std::string cliRebake = "rebake it: bake-vat --rebake " + rel + " (or Inspector > Rebake)";
        for (const VatIssue& issue : checkVatStaleness(project, rel)){
            add(issue.error ? "error" : "warning", issue.kind, issue.message, issue.fix.rfind("Rebake (", 0) == 0 ? cliRebake : issue.fix);
        }
        // the device limit of the recipe's preset (spec 2.4): a payload texture over it doesn't
        // upload on the devices the preset is for (the mesh draws at rest)
        const VatData& data = *parsed.data;
        VatPreset preset = VatPreset::MobileWeb;
        if (!presetFromName(data.recipe.preset, preset)) preset = VatPreset::MobileWeb;
        const VatPresetLimits limits = presetLimits(preset);
        const int side = std::max(data.layout.width, data.layout.height);
        if (side > limits.maxHeight){
            add("error", "vat-device-limit",
                "the payload texture is " + std::to_string(data.layout.width) + "x" + std::to_string(data.layout.height) + " texels, over the " +
                    presetName(preset) + " preset's " + std::to_string(limits.maxHeight) + " (mobile_web/WebGL2 devices guarantee 2048x2048, desktop " +
                    "8192): it doesn't upload on those devices and the mesh draws at rest",
                preset == VatPreset::MobileWeb
                    ? "rebake with a lower --fps, fewer --clips, --normals off or --mode bones (bake-vat --source ... --output " + rel +
                          " --overwrite), or the desktop preset if the game doesn't ship on the web or mobile"
                    : "rebake with a lower --fps, fewer --clips, --normals off or --mode bones (bake-vat --source ... --output " + rel + " --overwrite)");
        }
    }
    return check;
}

std::vector<fs::path> vatAssetsForFiles(const fs::path& projectRoot, const std::vector<fs::path>& files){
    std::vector<fs::path> out;
    std::set<std::string> seen;
    auto add = [&](const fs::path& vat){
        if (seen.insert(FileUtils::pathToGenericUtf8(vat.lexically_normal())).second) out.push_back(vat.lexically_normal());
    };
    std::set<std::string> sources;     // project-relative, other files
    for (const fs::path& file : files){
        const std::string name = FileUtils::pathToUtf8(file.filename());
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        if (Util::isVertexAnimationFile(name)){
            add(file);
        }else if (lower.size() > 8 && lower.compare(lower.size() - 8, 8, ".vat.glb") == 0){
            const fs::path vat = file.parent_path() / FileUtils::pathFromUtf8(name.substr(0, name.size() - 4));
            if (fileThere(vat)) add(vat);
        }else{
            sources.insert(ProjectChecks::projectRelative(projectRoot, file));
        }
    }
    if (sources.empty()) return out;
    // the project's bakes whose recipe read one of the other files
    for (const fs::path& vat : ProjectChecks::findProjectFiles(projectRoot, {::jasperengine::vat::VAT_EXTENSION})){
        std::string text;
        if (!readText(vat, text)) continue;
        const ::jasperengine::vat::VatParseResult parsed = ::jasperengine::vat::parseVat(text);
        if (!parsed.ok()) continue;
        bool uses = false;
        for (const ::jasperengine::vat::VatRecipeSource& source : parsed.data->recipe.sources){
            for (const auto& hash : source.hashes) uses = uses || sources.count(hash.first) > 0;
        }
        if (uses) add(vat);
    }
    return out;
}

}
