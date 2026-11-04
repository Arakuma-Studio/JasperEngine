// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SynthesisCli.h"
#include "util/FileUtils.h"

#include "SynthesisRecipe.h"
#include "TextureSynthesis.h"

#include "cli/CommandLine.h"

#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <ostream>
#include <set>
#include <type_traits>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;

namespace jasperengine::editor::texsynth {

    namespace {

        struct CompanionArg {
            fs::path path;
            fs::path out;   // explicit <image>=<out>, else empty
            std::string name;
        };

        struct ExampleArg {
            fs::path path;
            fs::path sampleMask;
            fs::path guide;
            std::vector<CompanionArg> companions;
        };

        struct Options {
            fs::path project;
            Mode mode = Mode::Extend;
            std::vector<ExampleArg> examples;
            fs::path targetGuide;
            fs::path mask;
            MaskChannel maskChannel = MaskChannel::Luma;
            bool invertMask = false;
            fs::path out;
            fs::path uncertaintyOut;
            int width = 0;
            int height = 0;
            Quality quality = Quality::Balanced;
            ParamOverrides overrides;
            uint64_t seed = 0;
            int threads = 0;
            bool overwrite = false;
            std::string format = "text";
            bool quiet = false;
            bool help = false;
        };

        bool parseInt(const std::string& text, int& value) {
            if (text.empty()) return false;
            char* end = nullptr;
            const long v = std::strtol(text.c_str(), &end, 10);
            if (*end != 0 || v < -1000000000L || v > 1000000000L) return false;
            value = static_cast<int>(v);
            return true;
        }

        bool parseFloat(const std::string& text, float& value) {
            if (text.empty()) return false;
            char* end = nullptr;
            const double v = std::strtod(text.c_str(), &end);
            if (*end != 0 || !std::isfinite(v)) return false;
            value = static_cast<float>(v);
            return true;
        }

        bool parseSeed(const std::string& text, uint64_t& value) {
            if (text.empty() || !std::all_of(text.begin(), text.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) return false;
            errno = 0;
            char* end = nullptr;
            const unsigned long long v = std::strtoull(text.c_str(), &end, 10);
            if (errno == ERANGE || *end != 0) return false;
            value = v;
            return true;
        }

        bool parseSize(const std::string& text, int& width, int& height) {
            const size_t x = text.find_first_of("xX");
            if (x == std::string::npos) {
                if (!parseInt(text, width)) return false;
                height = width;
            } else if (!parseInt(text.substr(0, x), width) || !parseInt(text.substr(x + 1), height)) {
                return false;
            }
            return width > 0 && height > 0;
        }

        bool isPng(const fs::path& path) {
            std::string ext = path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return ext == ".png";
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
                auto lastExample = [&]() -> ExampleArg* {
                    if (o.examples.empty()) {
                        error = arg + " belongs to an example: give --example first";
                        return nullptr;
                    }
                    return &o.examples.back();
                };
                // an override: std::optional<int|float>
                auto number = [&](auto& target, auto parse, const char* what) {
                    if (!need()) return false;
                    typename std::decay_t<decltype(target)>::value_type v{};
                    if (!parse(value, v)) {
                        error = "invalid " + arg + " value '" + value + "' (" + what + ")";
                        return false;
                    }
                    target = v;
                    return true;
                };
                if (arg == "-h" || arg == "--help") {
                    o.help = true;
                } else if (arg == "-p" || arg == "--project") {
                    if (!need()) return false;
                    o.project = CommandLine::projectArgPath(editor::FileUtils::pathFromUtf8(value));
                } else if (arg == "--mode") {
                    if (!need()) return false;
                    if (!parseMode(value, o.mode)) {
                        error = "unknown mode '" + value + "' (extend, tileable, make_tileable, inpaint, guided)";
                        return false;
                    }
                } else if (arg == "-e" || arg == "--example") {
                    if (!need()) return false;
                    ExampleArg ex;
                    ex.path = CommandLine::absoluteArgPath(editor::FileUtils::pathFromUtf8(value));
                    o.examples.push_back(ex);
                } else if (arg == "--companion") {
                    if (!need()) return false;
                    ExampleArg* ex = lastExample();
                    if (!ex) return false;
                    CompanionArg companion;
                    std::error_code ec;
                    const size_t eq = value.rfind('=');
                    if (eq != std::string::npos && eq > 0 && eq + 1 < value.size() &&
                        !fs::exists(CommandLine::absoluteArgPath(editor::FileUtils::pathFromUtf8(value)), ec)) {
                        companion.path = CommandLine::absoluteArgPath(editor::FileUtils::pathFromUtf8(value.substr(0, eq)));
                        companion.out = CommandLine::absoluteArgPath(editor::FileUtils::pathFromUtf8(value.substr(eq + 1)));
                    } else {
                        companion.path = CommandLine::absoluteArgPath(editor::FileUtils::pathFromUtf8(value));
                    }
                    ex->companions.push_back(companion);
                } else if (arg == "--sample-mask") {
                    if (!need()) return false;
                    ExampleArg* ex = lastExample();
                    if (!ex) return false;
                    ex->sampleMask = CommandLine::absoluteArgPath(editor::FileUtils::pathFromUtf8(value));
                } else if (arg == "--guide") {
                    if (!need()) return false;
                    ExampleArg* ex = lastExample();
                    if (!ex) return false;
                    ex->guide = CommandLine::absoluteArgPath(editor::FileUtils::pathFromUtf8(value));
                } else if (arg == "--target-guide") {
                    if (!need()) return false;
                    o.targetGuide = CommandLine::absoluteArgPath(editor::FileUtils::pathFromUtf8(value));
                } else if (arg == "--mask") {
                    if (!need()) return false;
                    o.mask = CommandLine::absoluteArgPath(editor::FileUtils::pathFromUtf8(value));
                } else if (arg == "--mask-channel") {
                    if (!need()) return false;
                    if (!parseMaskChannel(value, o.maskChannel)) {
                        error = "unknown mask channel '" + value + "' (luma, r, g, b, a)";
                        return false;
                    }
                } else if (arg == "--invert-mask") {
                    o.invertMask = true;
                } else if (arg == "-o" || arg == "--out") {
                    if (!need()) return false;
                    o.out = CommandLine::absoluteArgPath(editor::FileUtils::pathFromUtf8(value));
                } else if (arg == "--uncertainty-out") {
                    if (!need()) return false;
                    o.uncertaintyOut = CommandLine::absoluteArgPath(editor::FileUtils::pathFromUtf8(value));
                } else if (arg == "--size") {
                    if (!need()) return false;
                    if (!parseSize(value, o.width, o.height)) {
                        error = "invalid --size '" + value + "' (WxH or N, e.g. 1024x512 or 1024)";
                        return false;
                    }
                } else if (arg == "--quality") {
                    if (!need()) return false;
                    if (!parseQuality(value, o.quality)) {
                        error = "unknown quality '" + value + "' (draft, balanced, high)";
                        return false;
                    }
                } else if (arg == "--seed") {
                    if (!need()) return false;
                    if (!parseSeed(value, o.seed)) {
                        error = "invalid --seed '" + value + "' (an integer 0..18446744073709551615)";
                        return false;
                    }
                } else if (arg == "--threads") {
                    if (!need()) return false;
                    if (!parseInt(value, o.threads) || o.threads < 0) {
                        error = "invalid --threads '" + value + "' (0 = all cores, or a count)";
                        return false;
                    }
                } else if (arg == "--k-neighbors") {
                    if (!number(o.overrides.nearestNeighbors, parseInt, "an integer")) return false;
                } else if (arg == "--m-rand") {
                    if (!number(o.overrides.randomSampleLocations, parseInt, "an integer")) return false;
                } else if (arg == "--cauchy") {
                    if (!number(o.overrides.cauchyDispersion, parseFloat, "a number")) return false;
                } else if (arg == "--backtrack-pct") {
                    if (!number(o.overrides.backtrackPercent, parseFloat, "a number")) return false;
                } else if (arg == "--backtrack-stages") {
                    if (!number(o.overrides.backtrackStages, parseInt, "an integer")) return false;
                } else if (arg == "--guide-alpha") {
                    if (!number(o.overrides.guideAlpha, parseFloat, "a number")) return false;
                } else if (arg == "--no-guide-decay") {
                    o.overrides.guideDecay = false;
                } else if (arg == "--border") {
                    if (!number(o.overrides.borderFraction, parseFloat, "a number")) return false;
                } else if (arg == "--overwrite") {
                    o.overwrite = true;
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
            if (o.examples.empty()) {
                error = "--example is required";
                return false;
            }
            if (o.out.empty()) {
                error = "--out is required";
                return false;
            }
            if (!isPng(o.out)) {
                error = "--out must be a .png file: " + FileUtils::pathToUtf8(o.out);
                return false;
            }
            if (!o.uncertaintyOut.empty() && !isPng(o.uncertaintyOut)) {
                error = "--uncertainty-out must be a .png file: " + FileUtils::pathToUtf8(o.uncertaintyOut);
                return false;
            }
            const bool needsSize = o.mode == Mode::Extend || o.mode == Mode::Tileable;
            if (needsSize && o.width == 0) {
                error = std::string("--size is required in ") + toString(o.mode) + " mode";
                return false;
            }
            if ((o.mode == Mode::MakeTileable || o.mode == Mode::Inpaint) && o.width != 0) {
                error = std::string(toString(o.mode)) + " keeps the image's size: leave --size out";
                return false;
            }
            if (o.mode == Mode::Guided && o.targetGuide.empty()) {
                error = "guided mode needs --target-guide";
                return false;
            }
            if (o.mode != Mode::Guided && !o.targetGuide.empty()) {
                error = "--target-guide is only used in guided mode";
                return false;
            }
            if (o.mode != Mode::Inpaint && (!o.mask.empty() || o.invertMask)) {
                error = "--mask and --invert-mask are only used in inpaint mode";
                return false;
            }
            for (const ExampleArg& ex : o.examples) {
                if (!ex.guide.empty() && o.mode != Mode::Guided) {
                    error = "--guide is only used in guided mode";
                    return false;
                }
            }
            const Params params = resolveParams(o.quality, o.overrides);
            const std::vector<Problem> problems = validateParams(params);
            if (!problems.empty()) {
                error = problems.front().message;
                return false;
            }
            return true;
        }

        // Every example gives the same companions (matched by name); name them by their suffix
        bool nameCompanions(Options& o, std::string& error) {
            for (ExampleArg& ex : o.examples) {
                std::set<std::string> seen;
                for (size_t i = 0; i < ex.companions.size(); i++) {
                    CompanionArg& companion = ex.companions[i];
                    char separator = '_';
                    companion.name = companionSuffix(FileUtils::pathToUtf8(ex.path.stem()), FileUtils::pathToUtf8(companion.path.stem()), separator);
                    if (companion.name.empty()) companion.name = "companion" + std::to_string(i + 1);
                    if (companion.name == "primary" || companion.name == "uncertainty") companion.name += "_map";
                    if (!seen.insert(companion.name).second) {
                        error = "two companions of " + FileUtils::pathToUtf8(ex.path.filename()) + " are named '" + companion.name + "' (rename one file)";
                        return false;
                    }
                }
            }
            const ExampleArg& first = o.examples.front();
            for (size_t e = 1; e < o.examples.size(); e++) {
                const ExampleArg& ex = o.examples[e];
                std::set<std::string> a, b;
                for (const CompanionArg& c : first.companions) a.insert(c.name);
                for (const CompanionArg& c : ex.companions) b.insert(c.name);
                if (a != b) {
                    std::string names;
                    for (const std::string& n : a) names += (names.empty() ? "" : ", ") + n;
                    error = "every example needs the same companions (by name: " + (names.empty() ? std::string("none") : names) + "); " +
                            FileUtils::pathToUtf8(ex.path.filename()) + " differs";
                    return false;
                }
            }
            return true;
        }

        std::string shown(const fs::path& path, const fs::path& project) {
            if (!project.empty()) {
                const fs::path rel = path.lexically_relative(project);
                if (!rel.empty() && *rel.begin() != ".." && !rel.is_absolute()) return FileUtils::pathToGenericUtf8(rel);
            }
            return FileUtils::pathToUtf8(path);
        }

        std::string fixed(double v, int digits) {
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%.*f", digits, v);
            return buffer;
        }

        double rounded(double v, int digits) {
            const double scale = std::pow(10.0, digits);
            return std::round(v * scale) / scale;
        }

    }

    void printSynthesizeTextureUsage(const std::string& commandName, std::ostream& out) {
        out << "Usage:\n"
            << "  " << commandName << " synthesize-texture --example <image> --out <png> [options]\n\n"
            << "Synthesizes a texture from example images (a port of Embark Studios' texture-synthesis): a new\n"
            << "texture of any size, a seamlessly tiling one, an existing image made seamless, an inpainted image,\n"
            << "or one that follows a guide. Companion maps (normal, roughness, height, ...) get the same arrangement,\n"
            << "texel for texel, in their own bit depth and channels. Every PNG written embeds its recipe (mode,\n"
            << "parameters, seed, sources and their SHA-1s) as an iTXt chunk named jasper.synthesis.\n\n"
            << "Options:\n"
            << "  -p, --project <path>        Project directory or project.yaml: the recipe's paths are relative to it.\n"
            << "      --mode <mode>           extend (default): a new texture of --size; tileable: a new seamlessly tiling\n"
            << "                              texture of --size; make_tileable: the first example made seamless (a band\n"
            << "                              along its edges re-synthesized); inpaint: the first example's --mask region\n"
            << "                              regenerated; guided: the examples arranged after --target-guide.\n"
            << "  -e, --example <image>       An example (the primary map, e.g. albedo). Can repeat.\n"
            << "      --companion <image>[=<png>]\n"
            << "                              A companion map of the preceding --example (same size). Can repeat; every\n"
            << "                              example gives the same companions. Written next to --out as <out stem> +\n"
            << "                              what its name adds to its example's (out rock_tile.png, example\n"
            << "                              rock_albedo.png, companion rock_normal.png -> rock_tile_normal.png), or to <png>.\n"
            << "      --sample-mask <image>   Of the preceding --example: white pixels may be copied, black never.\n"
            << "      --guide <image>         Of the preceding --example, guided mode: its guide map.\n"
            << "      --target-guide <image>  Guided mode: the layout to follow (without --guide: a style transfer).\n"
            << "      --mask <image>          Inpaint: white is regenerated, black kept (the first example's size).\n"
            << "                              Without it, the first example's transparent pixels are regenerated.\n"
            << "      --mask-channel <c>      luma (default), r, g, b or a: what of --mask is read.\n"
            << "      --invert-mask           Regenerate the other side of the mask.\n"
            << "  -o, --out <png>             The primary output.\n"
            << "      --uncertainty-out <png> Also write the uncertainty map (8-bit grey, brighter = less certain).\n"
            << "      --size <WxH|N>          Output size: required for extend and tileable; guided: the target guide's\n"
            << "                              size by default. make_tileable and inpaint keep the image's size.\n"
            << "      --quality <q>           draft, balanced (default) or high.\n"
            << "      --seed <n>              Seed (default 0). Same inputs, options and seed: the same bytes.\n"
            << "      --threads <n>           Threads, 0 = all cores (default). Never changes the output.\n"
            << "      --overwrite             Replace existing output files (refused otherwise).\n"
            << "      --format <format>       text (default) or json (one object on stdout).\n"
            << "  -q, --quiet                 No progress and summary on stderr.\n"
            << "  -h, --help                  Show this help.\n\n"
            << "Parameter overrides (on top of --quality):\n"
            << "      --k-neighbors <n>       Neighbours compared per pixel, 1..256 (balanced 50).\n"
            << "      --m-rand <n>            Random candidates per pixel, 1..1024 (balanced 50).\n"
            << "      --cauchy <x>            Cauchy dispersion, (0, 1]: low = hard patch seams, 1 = smooth (1).\n"
            << "      --backtrack-pct <x>     Share of pixels each backtracking stage redoes, [0, 0.95] (0.5).\n"
            << "      --backtrack-stages <n>  Backtracking stages, 0..10 (balanced 5).\n"
            << "      --guide-alpha <x>       Guided: guide weight, [0, 1] (0.8).\n"
            << "      --no-guide-decay        Guided: keep the guide weight in every stage.\n"
            << "      --border <x>            make_tileable: band width per edge as a share of the size, (0, 0.5] (0.125).\n\n"
            << "Paths are absolute or relative to the current directory.\n"
            << "Problems go to stdout as <file>: error|warning: message; the summary to stderr.\n"
            << "Exit code: 0 written, 1 synthesis or write failed (then no file is written or replaced), 2 usage\n"
            << "error (bad option, existing output without --overwrite).\n";
    }

    int runSynthesizeTexture(int argc, char** argv, const std::string& commandName, std::ostream& out, std::ostream& err) {
        const std::string prefix = commandName + " synthesize-texture: ";
        Options o;
        std::string error;
        if (!parseArgs(argc, argv, o, error)) {
            err << prefix << error << "\n\n";
            printSynthesizeTextureUsage(commandName, err);
            return 2;
        }
        if (o.help) {
            printSynthesizeTextureUsage(commandName, out);
            return 0;
        }
        std::error_code ec;
        if (!o.project.empty() && !fs::is_regular_file(o.project / "project.yaml", ec)) {
            err << prefix << "no project.yaml in " << FileUtils::pathToUtf8(o.project) << "\n";
            return 2;
        }
        if (!nameCompanions(o, error)) {
            err << prefix << error << "\n";
            return 2;
        }

        // the outputs, every one checked before anything is synthesized
        std::map<std::string, fs::path> outputs;   // role -> path
        std::vector<std::string> roles = {"primary"};
        outputs["primary"] = o.out;
        for (const CompanionArg& companion : o.examples.front().companions) {
            fs::path path = companion.out;
            if (path.empty()) {
                // an explicit <image>=<png> of another example names it too
                for (const ExampleArg& ex : o.examples) {
                    for (const CompanionArg& c : ex.companions) {
                        if (c.name == companion.name && !c.out.empty() && path.empty()) path = c.out;
                    }
                }
            }
            if (path.empty()) {
                char separator = '_';
                companionSuffix(FileUtils::pathToUtf8(o.examples.front().path.stem()), FileUtils::pathToUtf8(companion.path.stem()), separator);
                path = o.out.parent_path() / editor::FileUtils::pathFromUtf8(FileUtils::pathToUtf8(o.out.stem()) + separator + companion.name + ".png");
            }
            if (!isPng(path)) {
                err << prefix << "companion output must be a .png file: " << FileUtils::pathToUtf8(path) << "\n";
                return 2;
            }
            outputs[companion.name] = path;
            roles.push_back(companion.name);
        }
        if (!o.uncertaintyOut.empty()) {
            outputs["uncertainty"] = o.uncertaintyOut;
            roles.push_back("uncertainty");
        }
        std::set<fs::path> inputs;
        for (const ExampleArg& ex : o.examples) {
            inputs.insert(ex.path);
            if (!ex.sampleMask.empty()) inputs.insert(ex.sampleMask);
            if (!ex.guide.empty()) inputs.insert(ex.guide);
            for (const CompanionArg& c : ex.companions) inputs.insert(c.path);
        }
        if (!o.mask.empty()) inputs.insert(o.mask);
        if (!o.targetGuide.empty()) inputs.insert(o.targetGuide);
        std::set<fs::path> written;
        std::vector<std::string> existing;
        for (const std::string& role : roles) {
            const fs::path& path = outputs[role];
            if (!written.insert(path).second) {
                err << prefix << "two outputs would be written to " << FileUtils::pathToUtf8(path) << "\n";
                return 2;
            }
            if (inputs.count(path)) {
                err << prefix << "the " << role << " output would replace an input: " << FileUtils::pathToUtf8(path) << " (write it elsewhere)\n";
                return 2;
            }
            if (fs::exists(path, ec)) existing.push_back(FileUtils::pathToUtf8(path));
        }
        if (!existing.empty() && !o.overwrite) {
            for (const std::string& path : existing) err << prefix << path << " exists (pass --overwrite to replace it)\n";
            return 2;
        }
        // every path the recipes record must be UTF-8 text (JSON): refused before the synthesis
        // runs, not found out when the first recipe is written
        {
            RecipePaths recorded;
            recorded.projectDir = o.project;
            for (const std::string& role : roles) {
                recorded.outputDir = outputs[role].parent_path();
                std::vector<fs::path> named(inputs.begin(), inputs.end());
                for (const std::string& other : roles) named.push_back(outputs[other]);
                for (const fs::path& path : named) {
                    if (isValidUtf8(recipePath(path, recorded))) continue;
                    err << prefix << printableUtf8(FileUtils::pathToUtf8(path)) << ": the file name is not valid UTF-8, so the recipe (JSON text) could not "
                        << "record it; rename it\n";
                    return 2;
                }
            }
        }

        // the request
        Request request;
        request.mode = o.mode;
        request.width = o.width;
        request.height = o.height;
        request.quality = o.quality;
        request.overrides = o.overrides;
        request.seed = o.seed;
        request.threads = o.threads;
        std::map<std::string, std::string> fieldPaths;   // request field -> the file, for the messages
        for (size_t e = 0; e < o.examples.size(); e++) {
            const ExampleArg& arg = o.examples[e];
            Example ex;
            ex.image = ImageInput::fromPath(arg.path);
            const std::string field = "examples[" + std::to_string(e) + "]";
            fieldPaths[field + ".image"] = FileUtils::pathToUtf8(arg.path);
            if (!arg.sampleMask.empty()) {
                ex.sampleMask = ImageInput::fromPath(arg.sampleMask);
                fieldPaths[field + ".sampleMask"] = FileUtils::pathToUtf8(arg.sampleMask);
            }
            if (!arg.guide.empty()) {
                ex.guide = ImageInput::fromPath(arg.guide);
                fieldPaths[field + ".guide"] = FileUtils::pathToUtf8(arg.guide);
            }
            request.examples.push_back(ex);
        }
        for (size_t c = 0; c < o.examples.front().companions.size(); c++) {
            Companion companion;
            companion.name = o.examples.front().companions[c].name;
            for (size_t e = 0; e < o.examples.size(); e++) {
                for (const CompanionArg& arg : o.examples[e].companions) {
                    if (arg.name != companion.name) continue;
                    fieldPaths["companions[" + std::to_string(c) + "].images[" + std::to_string(companion.images.size()) + "]"] = FileUtils::pathToUtf8(arg.path);
                    companion.images.push_back(ImageInput::fromPath(arg.path));
                }
            }
            request.companions.push_back(companion);
        }
        if (!o.mask.empty()) {
            request.inpaintMask.image = ImageInput::fromPath(o.mask);
            fieldPaths["inpaintMask"] = FileUtils::pathToUtf8(o.mask);
        }
        request.inpaintMask.channel = o.maskChannel;
        request.inpaintMask.invert = o.invertMask;
        if (!o.targetGuide.empty()) {
            request.targetGuide = ImageInput::fromPath(o.targetGuide);
            fieldPaths["targetGuide"] = FileUtils::pathToUtf8(o.targetGuide);
        }

        Control control;
        int lastStage = -1;
        if (!o.quiet) {
            control.progressIntervalSeconds = 1e9;   // only the stage starts and ends (forced reports)
            control.onProgress = [&](const Progress& p) {
                if (p.stageFraction < 1.0 || p.stage == lastStage) return;
                lastStage = p.stage;
                err << "synthesize-texture: stage " << (p.stage + 1) << "/" << p.stageCount << " done, " << fixed(p.fraction * 100.0, 0) << "% in "
                    << fixed(p.elapsedSeconds, 2) << "s" << (p.etaSeconds >= 0 ? ", about " + fixed(p.etaSeconds, 1) + "s left" : std::string())
                    << "\n";
                err.flush();
            };
        }
        Result result = synthesize(request, control);

        // ---- write (the request now holds the loaded inputs and their hashes)
        // every file encoded first, then written all or nothing: a companion that can't be
        // written (a path component that is a file, a full disk) leaves the primary as it was
        std::vector<Problem> writeErrors;
        if (result.status == Status::Ok) {
            RecipePaths paths;
            paths.projectDir = o.project;
            paths.outputs = outputs;
            std::vector<StagedFile> files;
            for (const std::string& role : roles) {
                const fs::path& path = outputs[role];
                const Image* image = &result.primary;
                if (role == "uncertainty") image = &result.uncertaintyMap;
                for (const CompanionOutput& c : result.companions) {
                    if (c.name == role) image = &c.image;
                }
                paths.outputDir = path.parent_path();
                StagedFile file;
                file.path = path;
                std::string encodeError;
                if (!encodePngWithRecipe(*image, makeRecipe(request, result, role, paths), file.bytes, encodeError)) {
                    writeErrors.push_back({FileUtils::pathToUtf8(path), encodeError + "; nothing was written"});
                    break;
                }
                files.push_back(std::move(file));
            }
            std::string failedPath, writeError;
            if (writeErrors.empty() && !writeFilesAllOrNothing(files, failedPath, writeError)) writeErrors.push_back({failedPath, writeError});
        }

        // ---- report
        auto location = [&](const std::string& field) {
            auto it = fieldPaths.find(field);
            return it != fieldPaths.end() ? it->second : std::string("synthesize-texture");
        };
        std::vector<Problem> errors = result.errors;
        const bool ok = result.status == Status::Ok && writeErrors.empty();
        if (o.format == "json") {
            json j = json::object();
            j["ok"] = ok;
            j["status"] = writeErrors.empty() ? toString(result.status) : "write_failed";
            j["mode"] = toString(o.mode);
            j["quality"] = toString(o.quality);
            j["seed"] = o.seed;
            j["threads"] = result.threads;
            j["width"] = result.width;
            j["height"] = result.height;
            j["seconds"] = rounded(result.seconds, 3);
            j["params"] = paramsToJson(result.params);
            json outs = json::object();
            if (ok) {
                for (const std::string& role : roles) outs[role] = shown(outputs[role], o.project);
            }
            j["outputs"] = outs;
            if (result.status == Status::Ok) {
                const Metrics& m = result.metrics;
                j["metrics"] = {{"meanUncertainty", rounded(m.meanUncertainty, 5)},
                                {"highUncertaintyFraction", rounded(m.highUncertaintyFraction, 5)},
                                {"highUncertaintyThreshold", m.highUncertaintyThreshold},
                                {"seamScore", rounded(m.seamScore, 4)},
                                {"seamScoreX", rounded(m.seamScoreX, 4)},
                                {"seamScoreY", rounded(m.seamScoreY, 4)},
                                {"synthesizedPixels", m.synthesizedPixels},
                                {"lockedPixels", m.lockedPixels}};
            }
            json errs = json::array();
            for (const Problem& problem : errors) {
                errs.push_back({{"field", problem.field}, {"path", location(problem.field)}, {"message", problem.message}});
            }
            for (const Problem& problem : writeErrors) errs.push_back({{"field", "output"}, {"path", problem.field}, {"message", problem.message}});
            j["errors"] = errs;
            j["warnings"] = result.warnings;
            // lossy only where a message quotes a path that is not UTF-8 (never throws)
            out << jsonTextLossy(j, 1) << std::endl;
        } else {
            for (const Problem& problem : errors) out << location(problem.field) << ": error: " << problem.message << "\n";
            for (const Problem& problem : writeErrors) out << problem.field << ": error: " << problem.message << "\n";
            for (const std::string& warning : result.warnings) out << "synthesize-texture: warning: " << warning << "\n";
            out.flush();
        }
        if (!o.quiet) {
            if (ok) {
                const Metrics& m = result.metrics;
                err << "synthesize-texture: wrote " << roles.size() << (roles.size() == 1 ? " file" : " files") << " (" << result.width << "x"
                    << result.height << ", " << toString(o.mode) << ", " << toString(o.quality) << ", seed " << o.seed << ") in "
                    << fixed(result.seconds, 2) << "s: mean uncertainty " << fixed(m.meanUncertainty, 4) << ", "
                    << fixed(m.highUncertaintyFraction * 100.0, 1) << "% high, seam score " << fixed(m.seamScore, 2) << "\n";
            } else if (result.status == Status::Cancelled) {
                err << "synthesize-texture: cancelled\n";
            } else {
                err << "synthesize-texture: failed (" << (errors.size() + writeErrors.size()) << (errors.size() + writeErrors.size() == 1 ? " error" : " errors")
                    << "), nothing written\n";
            }
        }
        return ok ? 0 : 1;
    }

}
