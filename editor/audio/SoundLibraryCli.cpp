// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SoundLibraryCli.h"

#include "AudioAnalysis.h"
#include "AudioImages.h"
#include "AudioImportSettings.h"
#include "SoundEssentials.h"
#include "SoundLibrary.h"
#include "SoundManifest.h"
#include "SoundTypes.h"

#include "audio/AudioImport.h"
#include "io/AtomicFile.h"
#include "util/Base64.h"
#include "util/Util.h"

#include "json.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

using Json = nlohmann::json;

constexpr int EXIT_OK = 0;
constexpr int EXIT_FAILED = 1;
constexpr int EXIT_ERROR = 2;

// analyze_audio's image width (Phase 1 spec 2.8)
constexpr int DEFAULT_WIDTH = 768;
const std::vector<std::string> IMAGE_MODES = {"none", "waveform", "spectrogram", "both", "sheet"};

std::string commandName(const char* executableName) {
    const std::string name = executableName && *executableName ? fs::path(executableName).filename().string() : std::string();
    return name.empty() ? std::string("jasperengine-editor") : name;
}

std::string format(const char* pattern, double value) {
    char text[64];
    std::snprintf(text, sizeof(text), pattern, value);
    return text;
}

double numberOr(const Json& value, double fallback) {
    return value.is_number() ? value.get<double>() : fallback;
}

bool parseNumber(const std::string& text, double& out) {
    if (text.empty()) return false;
    char* end = nullptr;
    out = std::strtod(text.c_str(), &end);
    return end && *end == '\0' && std::isfinite(out);
}

bool parseInteger(const std::string& text, long& out) {
    if (text.empty()) return false;
    char* end = nullptr;
    out = std::strtol(text.c_str(), &end, 10);
    return end && *end == '\0';
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return text;
}

// JSON on stdout: invalid UTF-8 in a path is replaced, never thrown
void printJson(const Json& value) {
    std::cout << value.dump(-1, ' ', false, Json::error_handler_t::replace) << "\n";
    std::cout.flush();
}

// -- audio-analyze --------------------------------------------------------------------------------

void printAnalyzeUsage(std::ostream& out, const std::string& command) {
    out << "Usage:\n"
        << "  " << command << " audio-analyze [options] <file|essentials:id|library:id>...\n"
        << "  " << command << " audio-analyze --list-types [--json]\n\n"
        << "Measures audio files and built-in clips with the editor's analyzer: the numbers, kind and notes\n"
        << "analyze_audio gives, without a project (no analysis cache, and no .meta is read).\n"
        << "Inputs: audio files (.wav, .ogg, .mp3, .flac); essentials:<id>, essentials:<group>/* and essentials:*\n"
        << "(the 40 essentials); library:<id>, library:<type>/* and library:* (the starter library, at\n"
        << "$" << SoundLibrary::ENV_DIR << " or next to the editor). Built-in clips are analysed on the region\n"
        << "their import plays (the manifest's import fades), and take their type, kind, tags and licence from it.\n\n"
        << "Options:\n"
        << "      --json               Print {\"items\": [...], \"errors\": [...], \"image\": ...}: analyze_audio's\n"
        << "                           structured output, items numbered from 1.\n"
        << "      --detail <level>     summary (default) or full (adds onsets_ms, momentary and short_term).\n"
        << "      --type <type>        The sound type of file inputs (kind_source \"type\"; the loudness_target note).\n"
        << "      --kind <kind>        With --type: the clip's own kind when it differs from its type's.\n"
        << "      --fade-in-ms <ms>    File inputs: analyse the region a clip with this fade-in plays (0 to 1000).\n"
        << "      --fade-out-ms <ms>   File inputs: the same for a fade-out.\n"
        << "      --image <mode>       none (default), waveform, spectrogram, both (one input) or sheet.\n"
        << "      --png <file>         The PNG file --image writes.\n"
        << "      --max-width <px>     The image's width, " << AudioImages::MIN_WIDTH << " to " << AudioImages::MAX_WIDTH
        << " (default " << DEFAULT_WIDTH << ").\n"
        << "      --list-types         Print the 31 sound types with their kinds and loudness targets.\n"
        << "  -h, --help               Show this help.\n\n"
        << "Without --json, one line per item as analyze_audio's text. The same inputs print the same bytes.\n"
        << "Exit codes: 0 every input was analysed; 2 an input could not be (see errors) or a usage error.\n";
}

int analyzeUsageError(const std::string& command, const std::string& message) {
    std::cerr << command << " audio-analyze: " << message << "\n\n";
    printAnalyzeUsage(std::cerr, command);
    return EXIT_ERROR;
}

struct AnalyzeOptions {
    bool json = false;
    bool full = false;
    bool listTypes = false;
    std::string type;
    std::string kind;
    std::optional<double> fadeInMs;
    std::optional<double> fadeOutMs;
    std::string image = "none";
    fs::path png;
    int width = DEFAULT_WIDTH;
    std::vector<std::string> inputs;
};

struct Input {
    std::string path;                                   // as given; a built-in clip: essentials:<id>, library:<id>
    fs::path file;                                      // a file input
    const SoundManifestClip* essential = nullptr;
    const SoundManifestClip* libraryClip = nullptr;
    std::string error;                                  // set: the input failed before its analysis
};

struct Outcome {
    bool ok = false;
    Json item;
    std::string error;
    AudioAnalysis::Signal signal;
};

// "1. assets/sounds/jump.wav: 0.31 s, -18.0 LUFS-M, TP -3.3 dBTP, tonal (jump); head silence 22.0 ms",
// analyze_audio's line
std::string itemLine(const Json& item) {
    const Json& loudness = item["loudness"];
    const Json& peak = item["peak"];
    std::string line = std::to_string(item.value("n", 0)) + ". " + item.value("path", std::string()) + ": " +
                       format("%.2f s", numberOr(item["format"]["duration_s"], 0.0)) + ", ";
    line += loudness["momentary_max"].is_number() ? format("%.1f LUFS-M", loudness["momentary_max"].get<double>()) : std::string("no loudness");
    line += ", TP " + format("%.1f dBTP", numberOr(peak["true_peak_dbtp"], AudioMeter::SILENCE_DB));
    line += ", " + item.value("kind", std::string());
    if (item.contains("type")) line += " (" + item["type"].get<std::string>() + ")";
    const std::string notes = AudioAnalysis::notesSummary(item["notes"]);
    if (!notes.empty()) line += "; " + notes;
    return line;
}

// analyze_audio's image of numbered items and their signals: a sheet, or the lanes of one file
bool renderImage(const Json& items, const std::vector<const AudioAnalysis::Signal*>& signals, const std::string& mode, int width,
                 std::string& png, Json& info) {
    if (mode == "none" || items.empty()) return false;
    AudioImages::Image image;
    if (mode == "sheet") {
        std::vector<AudioImages::SheetItem> tiles;
        for (size_t i = 0; i < items.size(); i++) {
            const std::string stem = fs::path(items[i]["path"].get<std::string>()).stem().string();
            tiles.push_back({signals[i], std::to_string(items[i]["n"].get<int>()) + " " + stem});
        }
        image = AudioImages::sheet(tiles, width);
    } else {
        const Json& item = items[0];
        AudioImages::Input input;
        input.signal = signals[0];
        input.title = std::to_string(item["n"].get<int>()) + ". " + item["path"].get<std::string>() + "  " + format("%.3f s", signals[0]->seconds());
        auto curve = [](const Json& values) {
            std::vector<double> out;
            if (!values.is_array()) return out;
            for (const Json& v : values) out.push_back(v.is_number() ? v.get<double>() : std::nan(""));
            return out;
        };
        input.momentary = curve(item.value("momentary", Json()));
        input.shortTerm = curve(item.value("short_term", Json()));
        input.truePeakDbtp = numberOr(item["peak"]["true_peak_dbtp"], AudioMeter::SILENCE_DB);
        image = AudioImages::render(input, AudioImages::fileLanes(mode), width);
    }
    if (!AudioImages::encodePng(image, png)) return false;
    info = {{"mode", mode}, {"width", image.width}, {"height", image.height}};
    return true;
}

// One input to its item: as analyze_audio makes it, without a project (no cache, no .meta)
void analyzeInput(const Input& input, const AnalyzeOptions& options, bool keepSignal, Outcome& out) {
    if (!input.error.empty()) {
        out.error = input.error;
        return;
    }
    std::string bytes;
    AudioAnalysis::Labels labels;
    AudioImportSettings settings;
    std::string error;
    if (input.essential) {
        bytes = SoundEssentials::bytes(*input.essential);
        labels = SoundEssentials::labels(*input.essential);
    } else if (input.libraryClip) {
        if (!SoundLibrary::get().read(*input.libraryClip, bytes, error)) {
            out.error = error;
            return;
        }
        labels = SoundLibrary::labels(*input.libraryClip);
    } else {
        std::error_code ec;
        if (!fs::is_regular_file(input.file, ec)) {
            out.error = fs::exists(input.file, ec) ? "not a file" : "no such file";
            return;
        }
        if (!Util::isAudioFile(input.file.string())) {
            out.error = "not an audio file (.wav, .ogg, .mp3 or .flac)";
            return;
        }
        const uint64_t size = fs::file_size(input.file, ec);
        if (ec || size > AudioAnalysis::MAX_FILE_BYTES) {
            out.error = ec ? "can't read the file size" : "larger than " + std::to_string(AudioAnalysis::MAX_FILE_BYTES / (1024 * 1024)) + " MB";
            return;
        }
        if (size == 0) {
            out.error = "the file is empty";
            return;
        }
        std::ifstream in(input.file, std::ios::binary);
        bytes.assign((size_t)size, '\0');
        if (!in || !in.read(&bytes[0], (std::streamsize)size)) {
            out.error = "can't read the file";
            return;
        }
        labels.type = options.type;
        labels.manifestKind = options.kind;
        if (options.fadeInMs) settings.fadeInMs = *options.fadeInMs;
        if (options.fadeOutMs) settings.fadeOutMs = *options.fadeOutMs;
    }

    Json cached;
    AudioAnalysis::Signal signal;
    if (!AudioAnalysis::analyzeBytes(bytes, cached, error, &signal)) {
        out.error = error;
        return;
    }
    std::string().swap(bytes);
    const Json& fileFormat = cached["format"];
    const uint64_t frames = fileFormat.value("frames", (uint64_t)0);
    const unsigned int rate = fileFormat.value("sample_rate", 0u);
    ClipImportSettings clip;
    // what the pipeline's judge records of the region it had measured
    Json applied = nullptr;
    if (input.essential || input.libraryClip) {
        const AudioAnalysis::Region region = (input.essential ? *input.essential : *input.libraryClip).importRegion(rate);
        clip.fadeInFrames = region.fadeInFrames;
        clip.fadeOutFrames = region.fadeOutFrames;
        if (!clip.playsAsFile()) applied = {{"fade_in_frames", clip.fadeInFrames}, {"fade_out_frames", clip.fadeOutFrames}, {"from", "manifest"}};
    } else {
        Json resolved;
        if (!settings.resolve(frames, rate, 0.0, 0.0, clip, error, &resolved)) {
            out.error = error;
            return;
        }
        if (!clip.playsAsFile()) {
            applied = std::move(resolved);
            applied["from"] = "options";
        }
    }
    Json item = cached;
    if (!clip.playsAsFile() && !AudioAnalysis::analyzePlayed(fs::path(), fs::path(), cached, clip, item, error, &signal)) {
        out.error = error;
        return;
    }
    out.item = AudioAnalysis::fileItem(item, 0, input.path, labels, options.full, SoundTypes::HEAD_SILENCE_MAX_MS);
    if (!applied.is_null()) out.item["import"] = std::move(applied);
    if (keepSignal) out.signal = std::move(signal);
    out.ok = true;
}

int listTypes(const AnalyzeOptions& options) {
    Json types = Json::array();
    for (const SoundTypes::SoundType& type : SoundTypes::all()) {
        types.push_back({{"type", type.name}, {"kind", type.kind}, {"target_lufs", type.targetLufs},
                         {"measure", type.integratedTarget ? "integrated" : "momentary_max"}});
    }
    if (options.json) {
        printJson({{"analyzer_version", AudioAnalysis::VERSION}, {"types", types}});
        return EXIT_OK;
    }
    std::cout << "Sound types (analyzer version " << AudioAnalysis::VERSION << "): type, kind, loudness target\n";
    for (const SoundTypes::SoundType& type : SoundTypes::all()) {
        char line[128];
        std::snprintf(line, sizeof(line), "  %-11s %-10s %6.1f %s\n", type.name, type.kind, type.targetLufs,
                      type.integratedTarget ? "LUFS integrated" : "LUFS-M (momentary max)");
        std::cout << line;
    }
    return EXIT_OK;
}

// -- sound-library ----------------------------------------------------------------------------------

// the installed library's own count for the help ("Installed: 900 clips, version 3, at <root>."), or
// why there is none; nothing is hashed
std::string installedLibrary() {
    const SoundLibraryStatus status = SoundLibrary::get().status();
    if (!status.available) return "Installed: none. " + status.error + ".";
    return "Installed: " + std::to_string(status.clips) + " clips, version " + std::to_string(status.version) + ", at " +
           status.root.generic_string() + ".";
}

void printLibraryUsage(std::ostream& out, const std::string& command) {
    out << "Usage:\n"
        << "  " << command << " sound-library verify [--dir <dir>] [--json]\n"
        << "  " << command << " sound-library list [--dir <dir>] [--type <type>] [--query <words>] [--json]\n\n"
        << "The starter library: CC0 clips installed next to the editor (sound-library/ beside the\n"
        << "executable, or $" << SoundLibrary::ENV_DIR << "), read in place.\n"
        << installedLibrary() << "\n"
        << "  verify   Checks every clip's size and SHA-256 against manifest.json, that the folder holds\n"
        << "           nothing else, the licence files and the 36 MiB cap. Prints \"ok: ...\" or one line per\n"
        << "           issue; --json prints {ok, root, version, clips, bytes, licences, issues: [{path, problem}]}.\n"
        << "  list     Lists the installed clips: library:<id>, type/kind, duration, LUFS-M, source, author.\n"
        << "           --json prints {root, version, clips: [the manifest's clip objects], ...}.\n\n"
        << "Options:\n"
        << "      --dir <dir>          The library folder (default: the editor's, as above).\n"
        << "      --type <type>        list: clips of one of the 31 sound types.\n"
        << "      --query <words>      list: clips whose id, tags or description contain every word (any case).\n"
        << "      --json               Machine-readable output.\n"
        << "  -h, --help               Show this help.\n\n"
        << "Exit codes: verify 0 (no issues) or 1 (an issue, or no library); list 0, or 2 when the library is\n"
        << "not available; 2 for a usage error.\n";
}

int libraryUsageError(const std::string& command, const std::string& message) {
    std::cerr << command << " sound-library: " << message << "\n\n";
    printLibraryUsage(std::cerr, command);
    return EXIT_ERROR;
}

int verifyLibrary(bool json) {
    SoundLibrary& library = SoundLibrary::get();
    const std::vector<SoundLibrary::VerifyIssue> issues = library.verify();
    const SoundLibraryStatus status = library.status();
    const size_t licences = library.manifest().packs.size();
    if (json) {
        Json list = Json::array();
        for (const SoundLibrary::VerifyIssue& issue : issues) list.push_back({{"path", issue.path}, {"problem", issue.problem}});
        printJson({{"ok", issues.empty()}, {"root", status.root.generic_string()}, {"version", status.version}, {"clips", status.clips},
                   {"bytes", status.bytes}, {"licences", licences}, {"issues", list}});
    } else if (issues.empty()) {
        std::cout << "ok: " << status.clips << " clips, " << status.bytes << " bytes, version " << status.version << ", " << licences
                  << (licences == 1 ? " licence" : " licences") << "\n";
    } else {
        for (const SoundLibrary::VerifyIssue& issue : issues) std::cout << issue.path << ": " << issue.problem << "\n";
    }
    return issues.empty() ? EXIT_OK : EXIT_FAILED;
}

int listLibrary(bool json, const std::string& type, const std::string& query) {
    SoundLibrary& library = SoundLibrary::get();
    const SoundLibraryStatus status = library.status();
    if (!status.available) {
        if (json) printJson({{"available", false}, {"root", status.root.generic_string()}, {"error", status.error}});
        else std::cerr << status.error << "\n";
        return EXIT_ERROR;
    }
    std::vector<std::string> words;
    std::istringstream split(lower(query));
    for (std::string word; split >> word;) words.push_back(word);
    std::vector<const SoundManifestClip*> clips;
    for (const SoundManifestClip& clip : library.all()) {
        if (!type.empty() && clip.type != type) continue;
        std::string haystack = lower(clip.id) + "\n" + lower(clip.description);
        for (const std::string& tag : clip.tags) haystack += "\n" + lower(tag);
        if (std::all_of(words.begin(), words.end(), [&](const std::string& w) { return haystack.find(w) != std::string::npos; })) {
            clips.push_back(&clip);
        }
    }
    if (json) {
        Json list = Json::array();
        for (const SoundManifestClip* clip : clips) list.push_back(clip->raw);
        printJson({{"available", true}, {"root", status.root.generic_string()}, {"version", status.version}, {"bytes", status.bytes},
                   {"missing", status.missing}, {"analysis_current", status.analysisCurrent}, {"clips", list}});
        return EXIT_OK;
    }
    std::cout << "Starter library v" << status.version << " at " << status.root.generic_string() << ": " << status.clips << " clips, "
              << format("%.1f", (double)status.bytes / (1024.0 * 1024.0)) << " MiB\n";
    for (const SoundManifestClip* clip : clips) {
        const SoundManifestMeasured& m = clip->measured;
        std::cout << SoundLibrary::PREFIX << clip->id << "  " << clip->type << "/" << clip->kind << "  " << format("%.2f", m.durationS) << " s  "
                  << (m.momentaryMaxLufs ? format("%.1f", *m.momentaryMaxLufs) : std::string("-")) << " LUFS-M  " << clip->source << " "
                  << clip->author << "\n";
    }
    if (status.missing > 0) {
        std::cout << status.missing << (status.missing == 1 ? " clip of the manifest is" : " clips of the manifest are")
                  << " missing (sound-library verify names them)\n";
    }
    return EXIT_OK;
}

} // namespace

// ======================================================================== audio-analyze

int SoundLibraryCli::runAudioAnalyze(int argc, char** argv, const char* executableName) {
    const std::string command = commandName(executableName);
    // Out without a Console window prints to stdout, where the JSON goes: the reader's warnings
    // are the errors[] entries here
    SoundLibrary::get().setLogWarnings(false);

    AnalyzeOptions options;
    bool hasPng = false, hasWidth = false, optionsEnded = false;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (optionsEnded || arg.empty() || arg[0] != '-' || arg == "-") {
            if (arg == "help" && i == 1 && argc == 2) {
                printAnalyzeUsage(std::cout, command);
                return EXIT_OK;
            }
            options.inputs.push_back(arg);
            continue;
        }
        if (arg == "--") {
            optionsEnded = true;
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            printAnalyzeUsage(std::cout, command);
            return EXIT_OK;
        }
        if (arg == "--json") {
            options.json = true;
            continue;
        }
        if (arg == "--list-types") {
            options.listTypes = true;
            continue;
        }
        const bool takesValue = arg == "--detail" || arg == "--type" || arg == "--kind" || arg == "--fade-in-ms" || arg == "--fade-out-ms" ||
                                arg == "--image" || arg == "--png" || arg == "--max-width";
        if (!takesValue) return analyzeUsageError(command, "unknown option '" + arg + "'");
        if (i + 1 >= argc) return analyzeUsageError(command, arg + " needs a value");
        const std::string value = argv[++i];
        if (arg == "--detail") {
            if (value != "summary" && value != "full") return analyzeUsageError(command, "--detail is summary or full, not '" + value + "'");
            options.full = value == "full";
        } else if (arg == "--type") {
            if (!SoundTypes::find(value)) {
                std::string names;
                for (const SoundTypes::SoundType& type : SoundTypes::all()) names += (names.empty() ? "" : ", ") + std::string(type.name);
                return analyzeUsageError(command, "--type '" + value + "' is not a sound type (one of: " + names + ")");
            }
            options.type = value;
        } else if (arg == "--kind") {
            if (!SoundManifest::isKind(value)) {
                return analyzeUsageError(command, "--kind '" + value + "' is not a kind (transient, tonal, swell, riser, glitch, loop or speech)");
            }
            options.kind = value;
        } else if (arg == "--fade-in-ms" || arg == "--fade-out-ms") {
            double ms = 0.0;
            if (!parseNumber(value, ms) || ms < 0.0 || ms > AudioImportSettings::FADE_MAX_MS) {
                return analyzeUsageError(command, arg + " takes a number of milliseconds from 0 to 1000, not '" + value + "'");
            }
            (arg == "--fade-in-ms" ? options.fadeInMs : options.fadeOutMs) = ms;
        } else if (arg == "--image") {
            if (std::find(IMAGE_MODES.begin(), IMAGE_MODES.end(), value) == IMAGE_MODES.end()) {
                return analyzeUsageError(command, "--image is none, waveform, spectrogram, both or sheet, not '" + value + "'");
            }
            options.image = value;
        } else if (arg == "--png") {
            options.png = value;
            hasPng = true;
        } else if (arg == "--max-width") {
            long width = 0;
            if (!parseInteger(value, width) || width < AudioImages::MIN_WIDTH || width > AudioImages::MAX_WIDTH) {
                return analyzeUsageError(command, "--max-width is a whole number from " + std::to_string(AudioImages::MIN_WIDTH) + " to " +
                                                      std::to_string(AudioImages::MAX_WIDTH) + ", not '" + value + "'");
            }
            options.width = (int)width;
            hasWidth = true;
        }
    }

    if (options.listTypes) {
        if (!options.inputs.empty()) return analyzeUsageError(command, "--list-types takes no inputs");
        return listTypes(options);
    }
    if (options.inputs.empty()) return analyzeUsageError(command, "give at least one input (a file, essentials:<id> or library:<id>)");
    if (!options.kind.empty() && options.type.empty()) return analyzeUsageError(command, "--kind needs --type");
    const bool wantImage = options.image != "none";
    if (wantImage && !hasPng) return analyzeUsageError(command, "--image " + options.image + " needs --png <file>");
    if (!wantImage && (hasPng || hasWidth)) return analyzeUsageError(command, "--png and --max-width need --image");
    if (wantImage) {
        std::error_code ec;
        const fs::path folder = fs::absolute(options.png, ec).parent_path();
        if (!fs::is_directory(folder, ec)) return analyzeUsageError(command, "--png: no folder " + folder.string());
    }

    // the inputs, references expanded in order (a reference that matches nothing is an error entry)
    std::vector<Input> inputs;
    bool references = false;
    for (const std::string& text : options.inputs) {
        if (SoundEssentials::isReference(text)) {
            references = true;
            std::vector<const SoundManifestClip*> clips;
            std::string error;
            if (!SoundEssentials::match(text, clips, error)) {
                inputs.push_back({text, fs::path(), nullptr, nullptr, error});
                continue;
            }
            for (const SoundManifestClip* clip : clips) inputs.push_back({SoundEssentials::PREFIX + clip->id, fs::path(), clip, nullptr, std::string()});
        } else if (SoundLibrary::isReference(text)) {
            references = true;
            std::string error;
            const std::vector<const SoundManifestClip*> clips = SoundLibrary::get().match(text, error);
            if (clips.empty()) {
                inputs.push_back({text, fs::path(), nullptr, nullptr, error});
                continue;
            }
            for (const SoundManifestClip* clip : clips) inputs.push_back({SoundLibrary::PREFIX + clip->id, fs::path(), nullptr, clip, std::string()});
        } else {
            inputs.push_back({text, fs::path(text), nullptr, nullptr, std::string()});
        }
    }
    if (references && (options.fadeInMs || options.fadeOutMs)) {
        return analyzeUsageError(command, "--fade-in-ms and --fade-out-ms apply to file inputs only (a built-in clip plays its manifest's import fades)");
    }
    if (wantImage && inputs.size() > 1 && options.image != "sheet") {
        return analyzeUsageError(command, "--image " + options.image + " shows one sound; use sheet for several inputs");
    }

    // every input on its own: the workers fill their outcomes, the output keeps the input order
    std::vector<Outcome> outcomes(inputs.size());
        std::atomic<size_t> next{0};
    auto work = [&]() {
        for (size_t i = next++; i < inputs.size(); i = next++) analyzeInput(inputs[i], options, wantImage, outcomes[i]);
    };
    const size_t threads = std::min<size_t>(std::max(1u, std::thread::hardware_concurrency()), inputs.size());
    std::vector<std::thread> pool;
    for (size_t t = 1; t < threads; t++) pool.emplace_back(work);
    work();
    for (std::thread& thread : pool) thread.join();

    Json items = Json::array();
    Json errors = Json::array();
    std::vector<const AudioAnalysis::Signal*> signals;
    for (size_t i = 0; i < inputs.size(); i++) {
        Outcome& outcome = outcomes[i];
        if (!outcome.ok) {
            errors.push_back({{"path", inputs[i].path}, {"error", outcome.error}});
            continue;
        }
        outcome.item["n"] = (int)items.size() + 1;
        items.push_back(std::move(outcome.item));
        signals.push_back(&outcome.signal);
    }

    Json imageInfo = nullptr;
    std::string imageError;
    if (wantImage && !items.empty()) {
        std::string png;
        if (!renderImage(items, signals, options.image, options.width, png, imageInfo)) {
            imageError = "the image could not be encoded";
        } else {
            const std::vector<unsigned char> data = Base64::decode(png);
            if (!io::writeFileAtomic(options.png, std::string_view(reinterpret_cast<const char*>(data.data()), data.size())).ok()) {
                imageError = "can't write " + options.png.string();
                imageInfo = nullptr;
            } else {
                imageInfo["png"] = options.png.generic_string();
            }
        }
    }

    if (options.json) {
        printJson({{"items", items}, {"errors", errors}, {"image", imageInfo}});
    } else {
        for (const Json& item : items) std::cout << itemLine(item) << "\n";
        if (imageInfo.is_object()) {
            std::cout << "image: " << options.png.string() << " (" << imageInfo["width"].get<int>() << "x" << imageInfo["height"].get<int>() << ")\n";
        }
        for (const Json& error : errors) std::cerr << "error: " << error["path"].get<std::string>() << ": " << error["error"].get<std::string>() << "\n";
    }
    if (!imageError.empty()) std::cerr << command << " audio-analyze: " << imageError << "\n";
    return errors.empty() && imageError.empty() ? EXIT_OK : EXIT_ERROR;
}

// ======================================================================== sound-library

int SoundLibraryCli::runSoundLibrary(int argc, char** argv, const char* executableName) {
    const std::string command = commandName(executableName);
    SoundLibrary::get().setLogWarnings(false);
    if (argc < 2) return libraryUsageError(command, "give a command: verify or list");
    const std::string sub = argv[1];
    if (sub == "-h" || sub == "--help" || sub == "help") {
        printLibraryUsage(std::cout, command);
        return EXIT_OK;
    }
    if (sub != "verify" && sub != "list") return libraryUsageError(command, "unknown command '" + sub + "' (verify or list)");

    // --dir first, so that the help and a usage error describe that library (the closure's I-3);
    // each option's value is taken as the parse below takes it
    for (int i = 2; i + 1 < argc; i++) {
        const std::string arg = argv[i];
        if (arg != "--dir" && arg != "--type" && arg != "--query") continue;
        const std::string value = argv[++i];
        if (arg == "--dir" && !value.empty()) SoundLibrary::get().reopen(value);
    }
    bool json = false;
    std::string type, query;
    for (int i = 2; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            printLibraryUsage(std::cout, command);
            return EXIT_OK;
        }
        if (arg == "--json") {
            json = true;
            continue;
        }
        const bool listOnly = arg == "--type" || arg == "--query";
        if (arg != "--dir" && !listOnly) return libraryUsageError(command, "unknown option '" + arg + "' for " + sub);
        if (listOnly && sub != "list") return libraryUsageError(command, arg + " applies to list only");
        if (i + 1 >= argc) return libraryUsageError(command, arg + " needs a value");
        const std::string value = argv[++i];
        if (arg == "--dir") {
            if (value.empty()) return libraryUsageError(command, "--dir needs a folder");
        } else if (arg == "--type") {
            if (!SoundTypes::find(value)) return libraryUsageError(command, "--type '" + value + "' is not one of the 31 sound types");
            type = value;
        } else {
            query = value;
        }
    }
    return sub == "verify" ? verifyLibrary(json) : listLibrary(json, type, query);
}
