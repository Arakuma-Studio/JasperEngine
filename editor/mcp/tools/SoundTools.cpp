// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Sound files (spec S36): analyze_audio, numbers and pictures of sound files, built-in clips and
// Play captures, the agent's way to hear; generate_sound, placeholder sounds; search_sounds, finding
// sounds; set_audio_import_settings, how a clip plays and where it came from, in its .meta
// (docs/mcp/guide/sound.md).
#include "McpTools.h"
#include "SoundShared.h"
#include "W4Shared.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpJobs.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"

#include "Project.h"
#include "util/AssetDatabase.h"
#include "audio/AudioAnalysis.h"
#include "audio/AudioCapture.h"
#include "audio/AudioCompare.h"
#include "audio/AudioSessionReader.h"
#include "audio/AudioImages.h"
#include "audio/AudioImportSettings.h"
#include "audio/AudioProvenance.h"
#include "audio/BuiltinSoundImport.h"
#include "audio/SoundEssentials.h"
#include "audio/SoundLibrary.h"
#include "audio/SoundSearch.h"
#include "audio/SoundDesign.h"
#include "audio/SoundFit.h"
#include "audio/SoundSynth.h"
#include "audio/SoundTypes.h"
#include "command/CommandHistory.h"
#include "command/type/SetAssetMetaCmd.h"
#include "jobs/JobSystem.h"
#include "organisation/AssetLabels.h"
#include "organisation/OrganisationCommands.h"
#include "window/ProjectWindow.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace jasperengine::editor::mcp {

namespace fs = std::filesystem;

namespace {

using namespace w4;

// paths given, and items after expanding references
const size_t MAX_PATHS = 24;
const size_t MAX_ITEMS = 48;
const int DEFAULT_WIDTH = 768;
// the call waits this long before it hands an agent the job to poll
const double WAIT_SECONDS = 60.0;

const std::vector<std::string> IMAGE_MODES = {"none", "waveform", "spectrogram", "both", "sheet"};

std::string formatted(const char* pattern, double first, double second) {
    char text[64];
    std::snprintf(text, sizeof(text), pattern, first, second);
    return text;
}

std::string formatted(const char* pattern, double value) {
    char text[64];
    std::snprintf(text, sizeof(text), pattern, value);
    return text;
}

double numberOr(const Json& value, double fallback) {
    return value.is_number() ? value.get<double>() : fallback;
}

std::vector<double> curveOf(const Json& values) {
    std::vector<double> out;
    if (!values.is_array()) return out;
    for (const Json& v : values) out.push_back(v.is_number() ? v.get<double>() : std::nan(""));
    return out;
}

// "1. sounds/jump.wav: 0.18 s, -18.2 LUFS-M, TP -3.1 dBTP, transient (jump); head silence 22.0 ms"
std::string itemLine(const Json& item) {
    const Json& loudness = item["loudness"];
    const Json& peak = item["peak"];
    std::string line = std::to_string(item.value("n", 0)) + ". " + item.value("path", std::string()) + ": " +
                       formatted("%.2f s", numberOr(item["format"]["duration_s"], 0.0)) + ", " +
                       (loudness["momentary_max"].is_number() ? formatted("%.1f LUFS-M", loudness["momentary_max"].get<double>()) : std::string("no loudness")) +
                       ", TP " + formatted("%.1f dBTP", numberOr(peak["true_peak_dbtp"], AudioMeter::SILENCE_DB)) + ", " + item.value("kind", std::string());
    if (item.contains("type")) line += " (" + item["type"].get<std::string>() + ")";
    const std::string notes = AudioAnalysis::notesSummary(item["notes"]);
    if (!notes.empty()) line += "; " + notes;
    return line;
}

bool readWhole(const fs::path& file, std::string& bytes) {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    const std::streamoff size = in ? (std::streamoff)in.tellg() : -1;
    if (size < 0) return false;
    bytes.assign((size_t)size, '\0');
    in.seekg(0);
    return size == 0 || (bool)in.read(&bytes[0], size);
}

// One analyze_audio call: everything the worker reads, and what it leaves
struct AnalyzeWork {
    struct Input {
        std::string path;
        fs::path file;
        // a built-in clip (essentials:<id>, library:<id>): no file, its bytes
        std::optional<BuiltinSoundImport::Clip> builtin;
        std::string error;
    };

    // files
    std::vector<Input> inputs;
    fs::path cacheDirectory;
    // or a capture (master.wav or the stem of `group`)
    bool capture = false;
    AudioAnalysis::Capture session;
    std::string group;
    std::string masterBytes;
    uint64_t masterSize = 0;
    double start = 0.0;
    double end = -1.0;

    bool full = false;
    std::string image;
    int width = DEFAULT_WIDTH;
    // a page of the paths' items (offset given): its first item and the items of all the pages
    bool paged = false;
    size_t offset = 0;
    size_t total = 0;

    Json items = Json::array();
    Json errors = Json::array();
    Json imageInfo = nullptr;
    std::string png;

    void addError(const std::string& path, const std::string& error) { errors.push_back({{"path", path}, {"error", error}}); }
};

// The image of numbered items (full detail: the curves) and their signals: a sheet, or the lanes
// of one file or capture. png and info stay empty when the mode is none or encoding fails.
void renderImage(AnalyzeWork& work, const std::vector<AudioAnalysis::Signal>& signals) {
    if (work.image == "none" || work.items.empty()) return;
    AudioImages::Image image;
    if (work.image == "sheet") {
        std::vector<AudioImages::SheetItem> tiles;
        for (size_t i = 0; i < work.items.size(); i++) {
            const std::string path = work.items[i]["path"].get<std::string>();
            const std::string stem = work.capture ? path : fs::path(path).stem().string();
            tiles.push_back({&signals[i], std::to_string(work.items[i]["n"].get<int>()) + " " + stem});
        }
        image = AudioImages::sheet(tiles, work.width);
    } else {
        const Json& item = work.items[0];
        AudioImages::Input input;
        input.signal = &signals[0];
        input.title = std::to_string(item["n"].get<int>()) + ". " + item["path"].get<std::string>() + "  " + formatted("%.3f s", signals[0].seconds());
        if (work.capture) {
            input.startSeconds = numberOr(item["range"]["start"], 0.0);
            input.title += formatted(" from %.3f s", input.startSeconds);
        }
        input.momentary = curveOf(item.value("momentary", Json()));
        input.shortTerm = curveOf(item.value("short_term", Json()));
        input.truePeakDbtp = numberOr(item["peak"]["true_peak_dbtp"], AudioMeter::SILENCE_DB);
        const std::vector<AudioImages::Lane> lanes = work.capture ? AudioImages::captureLanes(work.image) : AudioImages::fileLanes(work.image);
        image = AudioImages::render(input, lanes, work.width);
    }
    if (AudioImages::encodePng(image, work.png)) {
        work.imageInfo = {{"mode", work.image}, {"width", image.width}, {"height", image.height}};
    }
}

void runFiles(AnalyzeWork& work, const JobPtr& job) {
    const bool wantImage = work.image != "none";
    std::vector<AudioAnalysis::Signal> signals;
    for (size_t i = 0; i < work.inputs.size(); i++) {
        if (job->cancelRequested()) return;
        const AnalyzeWork::Input& input = work.inputs[i];
        job->setProgress((double)i / (double)work.inputs.size(), input.path);
        if (!input.error.empty()) {
            work.addError(input.path, input.error);
            continue;
        }
        Json cached;
        AudioAnalysis::Signal signal;
        std::string error;
        const bool analysed = input.builtin ? BuiltinSoundImport::analyze(*input.builtin, work.cacheDirectory, cached, error, wantImage ? &signal : nullptr)
                                            : AudioAnalysis::analyzeClip(input.file, work.cacheDirectory, cached, error, wantImage ? &signal : nullptr);
        if (!analysed) {
            work.addError(input.path, error);
            continue;
        }
        const int n = (int)work.items.size() + 1;
        // an image is drawn from the curves: full detail until it has been, then trimmed below
        work.items.push_back(AudioAnalysis::fileItem(cached, n, input.path, input.builtin ? input.builtin->labels() : AudioAnalysis::labelsOf(input.file),
                                                     work.full || wantImage, SoundTypes::HEAD_SILENCE_MAX_MS));
        if (wantImage) signals.push_back(std::move(signal));
    }
    if (job->cancelRequested()) return;
    // every item after the first against it, in words
    for (size_t i = 1; i < work.items.size(); i++) {
        std::string name = work.items[i].value("path", std::string());
        name = name.substr(name.find_last_of("/:") + 1);
        name = fs::path(name).stem().string();
        work.items[i]["differences_vs_1"] = AudioCompare::differences(work.items[0], work.items[i], name);
    }
    renderImage(work, signals);
}

void runCapture(AnalyzeWork& work, const JobPtr& job) {
    const std::string path = "capture:" + work.session.id + (work.group.empty() ? std::string() : ":" + work.group);
    AudioAnalysis::Signal audio;
    std::string error;
    if (!AudioAnalysis::readCaptureAudio(work.masterBytes, audio, error)) {
        work.addError(path, error);
        return;
    }
    std::string().swap(work.masterBytes);
    if (job->cancelRequested()) return;
    Json item;
    std::vector<AudioAnalysis::Signal> signals(1);
    if (!AudioAnalysis::captureItem(work.session, std::move(audio), work.masterSize, work.start, work.end, true, item, signals[0], error)) {
        work.addError(path, error);
        return;
    }
    item["n"] = 1;
    if (!work.group.empty()) {
        item["path"] = path;
        item["group"] = work.group;
    }
    work.items.push_back(std::move(item));
    if (job->cancelRequested()) return;
    renderImage(work, signals);
}

ToolResult analyzeResult(const AnalyzeWork& work, const McpJob& job) {
    if (job.status() == JobStatus::Cancelled) return ToolResult::error("The analysis was cancelled.");
    Json items = work.items;
    if (!work.full) {
        for (Json& item : items) {
            item.erase("onsets_ms");
            item.erase("momentary");
            item.erase("short_term");
        }
    }
    Json data = {{"items", items}, {"errors", work.errors}, {"image", work.imageInfo}};
    std::string text;
    if (work.paged) {
        const size_t shown = work.inputs.size();
        const size_t next = work.offset + shown;
        data["page"] = {{"offset", work.offset}, {"count", shown}, {"total", work.total}, {"next_offset", next < work.total ? Json(next) : Json(nullptr)}};
        text = "Items " + std::to_string(work.offset + 1) + "-" + std::to_string(next) + " of " + std::to_string(work.total) +
               (next < work.total ? " (the next page: offset " + std::to_string(next) + ")" : std::string(" (the last page)")) + "; n counts within this page:";
    }
    for (const Json& item : work.items) {
        text += (text.empty() ? "" : "\n") + itemLine(item);
        if (item.contains("differences_vs_1")) text += "\n   vs 1: " + item["differences_vs_1"]["text"].get<std::string>();
    }
    for (const Json& error : work.errors) {
        text += (text.empty() ? "" : "\n") + std::string("error: ") + error["path"].get<std::string>() + ": " + error["error"].get<std::string>();
    }
    if (work.items.empty()) return ToolResult::error(text.empty() ? std::string("Nothing to analyse.") : text, data);
    if (work.image != "none" && work.imageInfo.is_null()) text += "\n(the image could not be encoded)";
    ToolResult result = ToolResult::success(text, data);
    if (!work.png.empty()) result.images.push_back({work.png, "image/png"});
    return result;
}

// the session's files: master.wav, or the stem of a group
bool readCaptureFile(const CallPtr& call, AnalyzeWork& work, const std::string& group) {
    std::string master = "master.wav";
    const Json files = work.session.session.value("files", Json::object());
    if (files.is_object() && files.contains("master") && files["master"].is_string()) master = files["master"].get<std::string>();
    if (master.empty() || master.find_first_of("/\\") != std::string::npos || master == "." || master == "..") {
        call->fail("Session " + work.session.id + " names an invalid master file.");
        return false;
    }
    // (a live or crashed session is still under its temporary name)
    fs::path audioFile = AudioSessionReader::audioFile(work.session.directory, master);
    if (audioFile.empty()) audioFile = work.session.directory / master;
    if (group != "master") {
        const Json stems = files.is_object() ? files.value("stems", Json::array()) : Json::array();
        std::string stem;
        for (const Json& entry : stems.is_array() ? stems : Json::array()) {
            if (entry.is_string() && fs::path(entry.get<std::string>()).stem().string() == group) stem = entry.get<std::string>();
        }
        if (stem.empty()) {
            call->fail("Capture " + work.session.id + " has no stem of group \"" + group + "\": arm the capture with stems true, or use group master.");
            return false;
        }
        const fs::path relative = fs::path(stem).lexically_normal();
        if (relative.is_absolute() || relative.empty() || *relative.begin() == "..") {
            call->fail("Session " + work.session.id + " names an invalid stem file for " + group + ".");
            return false;
        }
        audioFile = AudioSessionReader::audioFile(work.session.directory, relative.generic_string());
        if (audioFile.empty()) audioFile = work.session.directory / relative;
        master = stem;
        work.group = group;
    }
    if (!readWhole(audioFile, work.masterBytes)) {
        call->fail("Can't read " + master + " of capture " + work.session.id + ".");
        return false;
    }
    work.masterSize = work.masterBytes.size();
    return true;
}

void analyzeAudioTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    const bool hasPaths = args.contains("paths") && !args["paths"].is_null();
    const bool hasCapture = args.contains("capture") && !args["capture"].is_null();
    if (hasPaths == hasCapture) {
        call->fail(hasPaths ? "Give paths or capture, not both." : "paths (audio files) or capture (\"last\", \"current\" or a session) is required.");
        return;
    }

    auto work = std::make_shared<AnalyzeWork>();
    const std::string detail = args.value("detail", std::string("summary"));
    if (detail != "summary" && detail != "full") {
        call->fail("detail is summary or full.");
        return;
    }
    work->full = detail == "full";
    work->width = args.value("max_width", DEFAULT_WIDTH);
    if (work->width < AudioImages::MIN_WIDTH || work->width > AudioImages::MAX_WIDTH) {
        call->fail("max_width is a whole number from " + std::to_string(AudioImages::MIN_WIDTH) + " to " + std::to_string(AudioImages::MAX_WIDTH) + ".");
        return;
    }
    if (hasPaths && (!args["paths"].is_array() || args["paths"].empty() || args["paths"].size() > MAX_PATHS)) {
        call->fail("paths is a list of 1 to " + std::to_string(MAX_PATHS) + " sound files or built-in references.");
        return;
    }
    if (!hasCapture) {
        for (const char* key : {"start", "end", "group"}) {
            if (args.contains(key)) {
                call->fail(std::string(key) + " applies to capture only.");
                return;
            }
        }
    }
    const bool hasOffset = args.contains("offset") && !args["offset"].is_null();
    if (hasOffset) {
        if (!hasPaths) {
            call->fail("offset applies to paths only.");
            return;
        }
        work->offset = (size_t)args["offset"].get<long long>();
    }

    size_t count = 0;
    if (hasPaths) {
        for (const Json& value : args["paths"]) {
            AnalyzeWork::Input input;
            if (!value.is_string() || value.get<std::string>().empty()) {
                input.path = value.dump();
                input.error = "not a path";
                work->inputs.push_back(std::move(input));
                continue;
            }
            input.path = value.get<std::string>();
            std::string error;
            if (BuiltinSoundImport::isReference(input.path)) {
                std::vector<const SoundManifestClip*> clips;
                bool library = SoundLibrary::isReference(input.path);
                if (library) clips = SoundLibrary::get().match(input.path, error);
                else SoundEssentials::match(input.path, clips, error);
                if (clips.empty()) {
                    input.error = error;
                    work->inputs.push_back(std::move(input));
                }
                for (const SoundManifestClip* entry : clips) {
                    AnalyzeWork::Input one;
                    BuiltinSoundImport::Clip clip;
                    clip.library = library;
                    clip.entry = entry;
                    one.path = clip.reference();
                    one.builtin = clip;
                    work->inputs.push_back(std::move(one));
                }
                continue;
            }
            fs::path file;
            std::error_code ec;
            if (!McpRefs::resolveProjectPath(project, input.path, file, error, true)) {
                input.error = error;
            } else if (fs::is_directory(file, ec)) {
                input.error = "a folder: list its sounds with list_assets type audio";
            } else {
                input.file = file.lexically_normal();
            }
            work->inputs.push_back(std::move(input));
        }
        count = work->inputs.size();
        if (hasOffset) {
            work->total = count;
            if (work->offset >= count) {
                call->fail("offset " + std::to_string(work->offset) + " is past these paths' " + std::to_string(count) + " items (offsets 0 to " +
                           std::to_string(count - 1) + ").");
                return;
            }
            const size_t end = std::min(count, work->offset + MAX_ITEMS);
            work->inputs = std::vector<AnalyzeWork::Input>(work->inputs.begin() + (std::ptrdiff_t)work->offset, work->inputs.begin() + (std::ptrdiff_t)end);
            work->paged = true;
            count = work->inputs.size();
        } else if (count > MAX_ITEMS) {
            std::string pages;
            for (size_t offset = 0; offset < count; offset += MAX_ITEMS) {
                pages += (offset == 0 ? "" : offset + MAX_ITEMS >= count ? " and " : ", ") + std::to_string(offset);
            }
            call->fail("These paths are " + std::to_string(count) + " items; at most " + std::to_string(MAX_ITEMS) +
                       " per call: call again with the same paths and offset " + pages + ", a page each, or name fewer.");
            return;
        }
        work->cacheDirectory = AudioAnalysis::cacheDirectory(project->getProjectPath());
    } else {
        std::string error;
        const fs::path root = AudioAnalysis::captureRoot(project->getProjectPath());
        if (!args["capture"].is_string() || !AudioAnalysis::findCapture(root, args["capture"].get<std::string>(), AudioCapture::sessionId(), work->session, error)) {
            call->fail(error.empty() ? std::string("capture is \"last\", \"current\", a session id or a session directory.") : error);
            return;
        }
        if (!readCaptureFile(call, *work, args.value("group", std::string("master")))) return;
        work->capture = true;
        work->start = args.value("start", 0.0);
        work->end = args.value("end", -1.0);
        if (work->end >= 0.0 && work->end <= work->start) {
            call->fail("end must be after start.");
            return;
        }
        count = 1;
    }

    work->image = args.value("image", std::string(count > 1 ? "sheet" : "both"));
    if (std::find(IMAGE_MODES.begin(), IMAGE_MODES.end(), work->image) == IMAGE_MODES.end()) {
        call->fail("image is none, waveform, spectrogram, both or sheet.");
        return;
    }
    if (count > 1 && work->image != "none" && work->image != "sheet") {
        call->fail("image " + work->image + " shows one sound; use sheet (the default for several paths) or one path.");
        return;
    }

    JobPtr job = call->ctx().jobs().create("analyze_audio", "Analyze " + std::to_string(count) + " sound(s)");
    jobs::submit([work, job]() {
        if (work->capture) runCapture(*work, job);
        else runFiles(*work, job);
        if (job->cancelRequested()) job->markCancelled();
        else job->succeed(std::to_string(work->items.size()) + " sound(s) analysed.", Json{{"items", work->items}, {"errors", work->errors}});
    }, {jobs::Lane::Background, jobs::GateClass::Default, "analyze-audio"});
    replyWhenJobDone(call, job, true, WAIT_SECONDS, [work](const McpJob& finished) { return analyzeResult(*work, finished); });
}

// ------------------------------------------------------------------ generate_sound

const int MAX_VARIATIONS = 8;
const long long MAX_SEED = 2147483647;
const double MIN_LOUDNESS = -40.0;
const double MAX_LOUDNESS = -6.0;

struct GenerateWork {
    struct File {
        fs::path wav;
        std::string rel;
        uint32_t seed = 0;
        SoundSynth::Output output;
        std::string metaText;
        Json cached;
        AudioAnalysis::Signal signal;
    };
    const SoundSynth::Preset* preset = nullptr;
    // a layered preset's, resolved to its default
    std::string family;
    Json params = Json::object();
    double targetLufs = 0.0;
    fs::path cacheDirectory;
    std::string image;
    std::vector<File> files;
    std::string error;
    std::string png;
    Json imageInfo = nullptr;
    // like: the reference
    std::string like;
    fs::path likeFile;
    std::optional<BuiltinSoundImport::Clip> likeBuiltin;
    Json reference = nullptr;
    SoundFit::Result fit;
    SoundFit::Measure referenceMeasure;
    Json referenceItem;
    std::string fitWords;
};

// the sections of a generated clip's .meta
YAML::Node generatedSections(const GenerateWork& work, const GenerateWork::File& file) {
    AudioImportSettings settings;
    std::string unread;
    // an overwrite keeps the other import settings
    if (!AudioImportSettings::load(file.wav, settings, unread)) settings = AudioImportSettings();
    settings.type = work.preset->type;
    AudioProvenance provenance;
    std::string ignored;
    Json generator = {{"preset", work.preset->name}, {"seed", file.seed}, {"params", work.params},
                      {"loudness_lufs", work.targetLufs}, {"version", SoundSynth::VERSION}};
    if (!work.family.empty()) generator["family"] = work.family;
    Json keys = {{"licence", AudioProvenance::LICENCE_GENERATED}, {"source", "generate_sound"}, {"generator", generator}};
    if (!work.reference.is_null()) keys["reference"] = work.reference;
    provenance.apply(keys, ignored);
    YAML::Node importSection(YAML::NodeType::Map);
    importSection["audioImporter"] = settings.toYaml();
    YAML::Node sections(YAML::NodeType::Map);
    sections["import"] = importSection;
    sections["provenance"] = provenance.toYaml();
    return sections;
}

const char* const FIT_IMAGE = "compare";

// The reference of a like call as it plays, measured, then the fit. False + work.error: nothing is written.
bool fitToReference(GenerateWork& work, uint32_t seed) {
    Json cached;
    AudioAnalysis::Signal played;
    std::string error;
    AudioAnalysis::Labels labels;
    if (work.likeBuiltin) {
        labels = work.likeBuiltin->labels();
        if (!BuiltinSoundImport::analyze(*work.likeBuiltin, work.cacheDirectory, cached, error, &played)) {
            work.error = "like: " + work.like + ": " + error;
            return false;
        }
    } else {
        labels = AudioAnalysis::labelsOf(work.likeFile);
        if (!AudioAnalysis::analyzeClip(work.likeFile, work.cacheDirectory, cached, error, &played)) {
            work.error = "like: " + work.like + ": " + error;
            return false;
        }
    }
    work.referenceItem = AudioAnalysis::fileItem(cached, 1, work.like, labels, false, SoundTypes::HEAD_SILENCE_MAX_MS);
    const std::string kind = work.referenceItem.value("kind", std::string());
    if (kind == SoundTypes::LOOP || kind == SoundTypes::SPEECH) {
        work.error = "like: " + work.like + " is a " + kind + ": a fit shapes a one-shot sound.";
        return false;
    }
    if (played.seconds() > SoundTypes::FIT_MAX_SECONDS) {
        work.error = "like: " + work.like + formatted(" plays %.1f s, longer than the ", played.seconds()) +
                     formatted("%.0f s a fit takes: trim it with set_audio_import_settings or use a shorter take.", SoundTypes::FIT_MAX_SECONDS);
        return false;
    }
    work.referenceMeasure = SoundFit::measure(played);
    if (work.referenceMeasure.silent) {
        work.error = "like: " + work.like + " is silent: nothing to fit to.";
        return false;
    }
    if (!SoundFit::fit(work.referenceMeasure, *work.preset, work.family, seed, work.fit, error)) {
        work.error = "like: " + error;
        return false;
    }
    work.params = work.fit.params;
    work.family = work.fit.family;
    // only numbers reach the result, whatever the reference's licence: the reference is recorded
    Json reference = {{"path", work.like}};
    const Json licence = labels.licence;
    if (licence.is_object()) {
        reference["licence"] = licence.value("id", std::string(AudioProvenance::LICENCE_UNKNOWN));
        for (const char* key : {"author", "url"}) {
            if (licence.contains(key) && licence[key].is_string()) reference[key] = licence[key];
        }
    } else {
        reference["licence"] = AudioProvenance::LICENCE_UNKNOWN;
    }
    work.reference = std::move(reference);
    return true;
}

// The variety of a call's variations: their distances, the nearest pair and the verdict
Json varietyOf(const GenerateWork& work) {
    const size_t n = work.files.size();
    std::vector<SoundSearch::Entry> entries;
    double quietest = INFINITY, loudest = -INFINITY;
    for (const GenerateWork::File& f : work.files) {
        entries.push_back(SoundSearch::entryOfAnalysis(f.cached));
        const double lufs = entries.back().lufs;
        if (std::isfinite(lufs)) {
            quietest = std::min(quietest, lufs);
            loudest = std::max(loudest, lufs);
        }
    }
    int pairs = 0;
    double smallest = INFINITY, largest = 0.0, sum = 0.0;
    size_t nearA = 0, nearB = 0;
    Json duplicates = Json::array();
    std::string duplicateWords;
    for (size_t a = 0; a < n; a++) {
        for (size_t b = a + 1; b < n; b++) {
            double d = SoundSearch::distance(entries[a], entries[b]);
            if (!std::isfinite(d)) d = 0.0;
            pairs++;
            sum += d;
            largest = std::max(largest, d);
            if (d < smallest) {
                smallest = d;
                nearA = a;
                nearB = b;
            }
            if (d < SoundTypes::VARIETY_NEAR_DUPLICATE_DISTANCE || work.files[a].output.wav == work.files[b].output.wav) {
                duplicates.push_back({a + 1, b + 1, AudioAnalysis::rounded(d, 0.001)});
                if (duplicateWords.empty()) {
                    duplicateWords = "variations " + std::to_string(a + 1) + " and " + std::to_string(b + 1) + " are near-duplicates" + formatted(" (%.3f)", d);
                }
            }
        }
    }
    const double spread = std::isfinite(quietest) ? loudest - quietest : 0.0;
    const std::string verdict = !duplicates.empty() ? "near_duplicates" : largest > SoundTypes::VARIETY_WIDE_DISTANCE ? "wide" : "one_palette";
    std::string name = work.preset->name;
    if (!work.family.empty()) name += "/" + work.family;
    std::string text = std::to_string(n) + " variations of " + name + ": ";
    if (verdict == "near_duplicates") text += duplicateWords + ": use other seeds or a different force";
    else if (verdict == "wide") text += "wide, they read as different sounds rather than takes of one" + formatted(" (distances %.3f-%.3f)", smallest, largest);
    else text += "one palette" + formatted(" (distances %.3f-%.3f, none under ", smallest, largest) + formatted("%.3f)", SoundTypes::VARIETY_NEAR_DUPLICATE_DISTANCE);
    text += formatted("; loudness spread %.1f LU.", spread);
    return {{"pairs", pairs},
            {"distance", {{"min", AudioAnalysis::rounded(smallest, 0.001)}, {"mean", AudioAnalysis::rounded(sum / std::max(pairs, 1), 0.001)}, {"max", AudioAnalysis::rounded(largest, 0.001)}}},
            {"nearest", {nearA + 1, nearB + 1, AudioAnalysis::rounded(smallest, 0.001)}},
            {"near_duplicates", duplicates},
            {"loudness_spread_lu", AudioAnalysis::rounded(spread, 0.1)},
            {"verdict", verdict},
            {"text", text}};
}

// The compare image of one fitted file: the reference against the file as written
void renderFitImage(GenerateWork& work, const AudioAnalysis::Signal& written) {
    const SoundFit::Measure result = SoundFit::measure(written);
    AudioImages::CompareInput input;
    input.title = "like " + work.like;
    const auto referenceBands = SoundFit::bandLevels(work.referenceMeasure, result.rate);
    const auto resultBands = SoundFit::bandLevels(result, work.referenceMeasure.rate);
    input.referenceBandsDb.assign(referenceBands.begin(), referenceBands.end());
    input.resultBandsDb.assign(resultBands.begin(), resultBands.end());
    for (int b = 0; b < SoundFit::BANDS; b++) input.bandHz.push_back(SoundFit::bandCentre(b));
    input.referenceEnvelopeDb = work.referenceMeasure.envelopeDb;
    input.resultEnvelopeDb = result.envelopeDb;
    const AudioImages::Image image = AudioImages::renderCompare(input, 768);
    if (AudioImages::encodePng(image, work.png)) {
        work.imageInfo = {{"mode", "compare"}, {"width", image.width}, {"height", image.height}};
    }
}

void runGenerate(GenerateWork& work) {
    if (!work.like.empty() && !fitToReference(work, work.files.front().seed)) return;
    const bool fitImage = work.image == FIT_IMAGE;
    const bool wantImage = work.image != "none";
    AnalyzeWork pictures;
    pictures.image = work.image;
    std::vector<AudioAnalysis::Signal> signals;
    for (size_t i = 0; i < work.files.size(); i++) {
        GenerateWork::File& file = work.files[i];
        std::string error;
        if (!SoundSynth::generate(*work.preset, file.seed, work.params, work.targetLufs, file.output, error, work.family)) {
            work.error = file.rel + ": " + error;
            return;
        }
        // measured on the file as written (PCM16)
        if (!AudioAnalysis::analyzeBytes(file.output.wav, file.cached, error, wantImage ? &file.signal : nullptr)) {
            work.error = file.rel + ": the generated file does not analyse: " + error;
            return;
        }
        AudioAnalysis::writeCache(work.cacheDirectory, file.cached);
        if (!clipMetaText(file.wav, generatedSections(work, file), file.metaText, error)) {
            work.error = file.rel + ": " + error;
            return;
        }
        if (i == 0 && !work.like.empty()) {
            const std::string differences = AudioCompare::differences(work.referenceItem, AudioAnalysis::fileItem(file.cached, 1, file.rel, AudioAnalysis::Labels(), false, SoundTypes::HEAD_SILENCE_MAX_MS), "the result").value("text", std::string());
            const std::string bands = SoundFit::bandWords(work.referenceMeasure, work.fit.measure);
            work.fitWords = bands + (bands.empty() || differences.empty() ? "" : "; ") + differences;
        }
        if (fitImage) {
            renderFitImage(work, file.signal);
        } else if (wantImage) {
            pictures.items.push_back(AudioAnalysis::fileItem(file.cached, (int)i + 1, file.rel, AudioAnalysis::Labels(), true, SoundTypes::HEAD_SILENCE_MAX_MS));
            signals.push_back(std::move(file.signal));
        }
    }
    if (fitImage) return;
    renderImage(pictures, signals);
    work.png = std::move(pictures.png);
    work.imageInfo = std::move(pictures.imageInfo);
}

void generateSoundTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    auto work = std::make_shared<GenerateWork>();
    const std::string given = args.value("path", std::string());
    fs::path file;
    std::string error;
    if (!McpRefs::resolveProjectWritePath(project, given, file, error, false)) {
        call->fail(error);
        return;
    }
    file = file.lexically_normal();
    const std::string name = file.filename().string();
    if (name.empty() || name == "." || name[0] == '.') {
        call->fail("path needs a file name (\"" + given + "\" names a folder or a hidden file), e.g. \"assets/sounds/jump.wav\".");
        return;
    }
    if (extensionOf(file) != ".wav") file += ".wav";
    if (!project->isInsideAssetsPath(file)) {
        call->fail("\"" + projectRelative(project, file) + "\" is outside the assets directory (\"" + projectRelative(project, project->getAssetsPath()) +
                   "\"), so no AudioSource could use it.");
        return;
    }
    const std::string presetName = args.value("preset", std::string());
    const int variations = args.value("variations", 1);
    if (variations < 1 || variations > MAX_VARIATIONS) {
        call->fail("variations is a whole number from 1 to " + std::to_string(MAX_VARIATIONS) + ".");
        return;
    }
    if (args.contains("seed") && (!args["seed"].is_number_integer() || args["seed"].get<long long>() < 0 || args["seed"].get<long long>() > MAX_SEED)) {
        call->fail("seed is a whole number from 0 to " + std::to_string(MAX_SEED) + ".");
        return;
    }
    // variation i has seed + i - 1: every one must be a seed the tool takes, to reproduce it alone
    uint32_t seed = 0;
    if (args.contains("seed")) {
        if (args["seed"].get<long long>() + variations - 1 > MAX_SEED) {
            call->fail("seed + variations - 1 is at most " + std::to_string(MAX_SEED) + ": use a seed up to " + std::to_string(MAX_SEED - variations + 1) + ".");
            return;
        }
        seed = (uint32_t)args["seed"].get<long long>();
    } else {
        std::random_device device;
        seed = (uint32_t)(device() % (uint32_t)(MAX_SEED - MAX_VARIATIONS + 2));
    }
    const std::string like = args.value("like", std::string());
    // a fit is reproducible by default
    if (!like.empty() && !args.contains("seed")) seed = 1;
    if (presetName == SoundSynth::RANDOM) {
        if (!like.empty()) {
            std::string refusal;
            SoundFit::canFit(nullptr, refusal);
            call->fail(refusal);
            return;
        }
        if (args.contains("params") && !args["params"].is_null() && !(args["params"].is_object() && args["params"].empty())) {
            call->fail("params need a named preset: random picks one by the seed (the reply names it).");
            return;
        }
        // one pick per call: every variation is the same preset
        work->preset = &SoundSynth::pick(seed);
    } else {
        work->preset = SoundSynth::find(presetName);
    }
    if (!work->preset) {
        call->fail("preset \"" + presetName + "\" is not one of the list.");
        return;
    }
    const std::vector<std::string> families = SoundDesign::families(work->preset->name);
    const std::string family = args.value("family", std::string());
    if (!family.empty()) {
        if (families.empty()) {
            call->fail("family applies to ui_click, swoosh, impact and pop.");
            return;
        }
        if (std::find(families.begin(), families.end(), family) == families.end()) {
            std::string list;
            for (const std::string& name : families) list += (list.empty() ? "" : ", ") + name;
            call->fail("unknown family '" + family + "' for " + work->preset->name + ": " + list + ".");
            return;
        }
    }
    // with like, an unset family is the fit's to choose
    work->family = family.empty() && !families.empty() && like.empty() ? families.front() : family;
    if (!like.empty()) {
        if (!SoundFit::canFit(work->preset, error)) {
            call->fail(error);
            return;
        }
        if (args.contains("params") && !args["params"].is_null() && !(args["params"].is_object() && args["params"].empty())) {
            call->fail("like searches the params: leave params out, then adjust the result with params and its seed.");
            return;
        }
        work->like = like;
        if (SoundEssentials::isReference(like) || SoundLibrary::isReference(like)) {
            std::vector<BuiltinSoundImport::Clip> clip;
            if (!BuiltinSoundImport::resolve({like}, clip, error) || clip.empty()) {
                call->fail("like: " + error);
                return;
            }
            work->likeBuiltin = clip[0];
        } else {
            if (!McpRefs::resolveProjectPath(project, like, work->likeFile, error)) {
                call->fail("like: " + error);
                return;
            }
        }
    }
    if (args.contains("params") && !args["params"].is_null()) {
        if (!SoundSynth::checkParams(*work->preset, args["params"], error)) {
            call->fail(error);
            return;
        }
        work->params = args["params"];
    }
    const SoundTypes::SoundType* type = SoundTypes::find(work->preset->type);
    work->targetLufs = args.value("loudness_lufs", type ? type->targetLufs : -18.0);
    if (work->targetLufs < MIN_LOUDNESS || work->targetLufs > MAX_LOUDNESS) {
        call->fail("loudness_lufs is a momentary max from -40 to -6 LUFS (default: the type's target).");
        return;
    }
    work->image = args.value("image", std::string(variations > 1 ? "sheet" : "both"));
    if (std::find(IMAGE_MODES.begin(), IMAGE_MODES.end(), work->image) == IMAGE_MODES.end()) {
        call->fail("image is none, waveform, spectrogram, both or sheet.");
        return;
    }
    if (variations > 1 && work->image != "none" && work->image != "sheet") {
        call->fail("image " + work->image + " shows one sound; use sheet (the default for variations) or one variation.");
        return;
    }
    // one fitted file, unless the call asks for another: the reference against it
    if (!like.empty() && variations == 1 && !args.contains("image")) work->image = FIT_IMAGE;
    const bool overwrite = args.value("overwrite", false);
    for (int i = 1; i <= variations; i++) {
        GenerateWork::File target;
        target.wav = variations == 1 ? file : file.parent_path() / (file.stem().string() + "_" + std::to_string(i) + ".wav");
        target.rel = projectRelative(project, target.wav);
        target.seed = seed + (uint32_t)(i - 1);
        std::error_code ec;
        if (fs::is_directory(target.wav, ec)) {
            call->fail(target.rel + " is a folder.");
            return;
        }
        if (fs::exists(target.wav, ec) && !overwrite) {
            call->fail(target.rel + " already exists; pass overwrite: true to replace it (its other import settings stay).");
            return;
        }
        work->files.push_back(std::move(target));
    }
    work->cacheDirectory = AudioAnalysis::cacheDirectory(project->getProjectPath());

    JobPtr job = call->ctx().jobs().create("generate_sound", "Generate " + std::to_string(variations) + " sound(s)");
    jobs::submit([work, job]() {
        runGenerate(*work);
        if (work->error.empty()) job->succeed("Generated.");
        else job->fail(work->error);
    }, {jobs::Lane::Background, jobs::GateClass::Default, "generate-sound"});
    call->waitUntil([job]() { return job->finished(); }, (int)(WAIT_SECONDS * 1000.0), [call, work](bool timedOut) {
        if (timedOut) {
            call->fail("Generating took longer than 60 s.");
            return;
        }
        if (!work->error.empty()) {
            call->fail(work->error);
            return;
        }
        // every file and its .meta are one undo step
        std::vector<WriteFilesCmd::Item> items;
        for (const GenerateWork::File& f : work->files) {
            items.push_back({f.wav, f.output.wav});
            items.push_back({SetAssetMetaCmd::metaPathOf(f.wav), f.metaText});
        }
        std::string writeError;
        if (!writeFilesUndoable(call->ctx(), std::move(items), writeError)) {
            call->fail(writeError);
            return;
        }
        const SoundSynth::Preset& preset = *work->preset;
        Json files = Json::array();
        std::string lines;
        for (size_t i = 0; i < work->files.size(); i++) {
            const GenerateWork::File& f = work->files[i];
            Json item = AudioAnalysis::fileItem(f.cached, (int)i + 1, f.rel, AudioAnalysis::labelsOf(f.wav), false, SoundTypes::HEAD_SILENCE_MAX_MS);
            Json entry = {{"path", f.rel}, {"preset", preset.name}, {"seed", f.seed},
                          {"gain_db", AudioAnalysis::rounded(f.output.gainDb, 0.01)},
                          {"limited_by", f.output.limitedByTruePeak ? Json("true_peak") : Json(nullptr)},
                          {"analysis", item}};
            if (!work->family.empty()) entry["family"] = work->family;
            files.push_back(std::move(entry));
            lines += "\n" + itemLine(item) + " [seed " + std::to_string(f.seed) + formatted(", gain %+.2f dB", f.output.gainDb) +
                     (f.output.limitedByTruePeak ? ", held at -1 dBTP: under the target" : "") + "]";
        }
        std::string text = "Generated " + std::to_string(work->files.size()) + " placeholder sound(s), preset " + preset.name + " (type " + preset.type +
                           formatted(", target %.1f LUFS-M", work->targetLufs) + "); licence generated, one undo step:" + lines;
        Json data = {{"files", files}, {"type", preset.type}, {"licence", AudioProvenance::LICENCE_GENERATED}, {"image", work->imageInfo}};
        if (work->files.size() > 1) {
            data["variety"] = varietyOf(*work);
            text += "\n" + data["variety"]["text"].get<std::string>();
        }
        if (!work->like.empty()) {
            const SoundFit::Quality& q = work->fit.quality;
            Json fit = {{"reference", work->reference}, {"preset", preset.name}, {"params", work->fit.params},
                        {"distance", AudioAnalysis::rounded(q.distance, 0.001)}, {"grade", q.grade},
                        {"band_db", AudioAnalysis::rounded(q.bandDb, 0.01)}, {"envelope_db", AudioAnalysis::rounded(q.envelopeDb, 0.01)},
                        {"onset_ms", AudioAnalysis::rounded(q.onsetMs, 0.1)}, {"length_ratio", AudioAnalysis::rounded(q.lengthRatio, 0.001)},
                        {"start_distance", AudioAnalysis::rounded(work->fit.startDistance, 0.001)}, {"renders", work->fit.renders},
                        {"words", work->fitWords}, {"method", "bounded search of the preset's params; no audio of the reference is copied"}};
            if (!work->family.empty()) fit["family"] = work->family;
            data["fit"] = fit;
            text += "\nfit to " + work->like + formatted(": distance %.2f (", q.distance) + q.grade + formatted("; defaults %.2f) after ", work->fit.startDistance) +
                    std::to_string(work->fit.renders) + " renders: " + formatted("band balance %.1f dB, ", q.bandDb) + formatted("envelope %.1f dB; ", q.envelopeDb) + work->fitWords +
                    ". The distance says how alike the measurements are, not that it sounds right: listen, or measure it in context.";
        }
        if (work->image != "none" && work->imageInfo.is_null()) text += "\n(the image could not be encoded)";
        ToolResult result = ToolResult::success(text, data);
        if (!work->png.empty()) result.images.push_back({work->png, "image/png"});
        call->reply(std::move(result));
    });
}

// ------------------------------------------------------------------ search_sounds

struct SearchWork {
    SoundSearch::Sources sources;
    SoundSearch::Query query;
    std::vector<SoundSearch::Entry> results;
    size_t total = 0;
    int analysed = 0;
    std::string error;
    SoundSearch::Hint hint;
    bool sheet = false;
    std::string png;
    Json imageInfo = nullptr;
    Json items = Json::array();
};

void runSearch(SearchWork& work, const JobPtr& job) {
    std::atomic<bool> cancel{false};
    const std::vector<SoundSearch::Entry> index = SoundSearch::index(work.sources, work.analysed, &cancel, [job](size_t done, size_t total) {
        if (total > 0) job->setProgress((double)done / (double)total, "Indexing sounds...");
    });
    if (!SoundSearch::search(index, work.query, work.results, work.total, work.error)) return;
    if (work.results.empty()) work.hint = SoundSearch::emptyHint(index, work.query);
    for (size_t i = 0; i < work.results.size(); i++) work.items.push_back(SoundSearch::toJson(work.results[i], (int)i + 1));
    if (!work.sheet || work.results.empty()) return;
    AnalyzeWork pictures;
    pictures.image = "sheet";
    std::vector<AudioAnalysis::Signal> signals;
    for (const SoundSearch::Entry& entry : work.results) {
        std::string bytes, error;
        std::vector<BuiltinSoundImport::Clip> clip;
        AudioAnalysis::Signal signal;
        bool read = false;
        if (entry.source == "project") read = readWhole(entry.file, bytes);
        else if (BuiltinSoundImport::resolve({entry.path}, clip, error) && !clip.empty()) read = clip[0].read(bytes, error);
        if (!read || !AudioAnalysis::decodeReleasing(bytes, signal, error)) continue;
        Json item = {{"n", (int)pictures.items.size() + 1}, {"path", entry.path}};
        pictures.items.push_back(item);
        signals.push_back(std::move(signal));
    }
    renderImage(pictures, signals);
    work.png = std::move(pictures.png);
    work.imageInfo = std::move(pictures.imageInfo);
}

void searchSoundsTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    SoundSearch::Scope scope = SoundSearch::Scope::All;
    if (!SoundSearch::scopeFromName(args.value("scope", std::string("all")), scope)) {
        call->fail("scope is project, essentials, library or all.");
        return;
    }
    auto work = std::make_shared<SearchWork>();
    work->sources = SoundSearch::sources(project, scope);
    SoundSearch::Query& query = work->query;
    query.scope = scope;
    query.text = args.value("query", std::string());
    query.type = args.value("type", std::string());
    if (!query.type.empty() && !SoundTypes::find(query.type)) {
        call->fail("type \"" + query.type + "\" is not a sound type (the list is in the tool's schema).");
        return;
    }
    query.limit = args.value("limit", SoundSearch::DEFAULT_LIMIT);
    if (query.limit < 1 || query.limit > SoundSearch::MAX_LIMIT) {
        call->fail("limit is a whole number from 1 to " + std::to_string(SoundSearch::MAX_LIMIT) + ".");
        return;
    }
    work->sheet = args.value("sheet", false);
    if (args.contains("filters") && args["filters"].is_object()) {
        const Json& f = args["filters"];
        for (auto it = f.begin(); it != f.end(); ++it) {
            static const std::vector<std::string> keys = {"min_ms", "max_ms", "brightness", "attack", "tonal", "loop", "kind"};
            if (std::find(keys.begin(), keys.end(), it.key()) == keys.end()) {
                call->fail("filters." + it.key() + " is not a filter: min_ms, max_ms, brightness, attack, tonal, loop, kind.");
                return;
            }
        }
        auto oneOf = [&](const char* key, std::initializer_list<const char*> values) {
            const std::string value = f.value(key, std::string());
            if (value.empty()) return true;
            std::string list;
            for (const char* v : values) {
                if (value == v) return true;
                list += (list.empty() ? "" : ", ") + std::string(v);
            }
            call->fail(std::string("filters.") + key + " is " + list + ".");
            return false;
        };
        if (!oneOf("brightness", {"dark", "neutral", "bright"}) || !oneOf("attack", {"sharp", "soft"}) || !oneOf("kind", {"clip", "event"})) return;
        if (f.contains("min_ms") && f.contains("max_ms") && f["max_ms"].get<double>() < f["min_ms"].get<double>()) {
            call->fail("filters.max_ms is under filters.min_ms: nothing can match.");
            return;
        }
        query.filters.minMs = f.value("min_ms", -1.0);
        query.filters.maxMs = f.value("max_ms", -1.0);
        query.filters.brightness = f.value("brightness", std::string());
        query.filters.attack = f.value("attack", std::string());
        query.filters.kind = f.value("kind", std::string());
        if (f.contains("tonal")) query.filters.tonal = f["tonal"].get<bool>() ? 1 : 0;
        if (f.contains("loop")) query.filters.loop = f["loop"].get<bool>() ? 1 : 0;
    }
    if (args.contains("similar_to") && args["similar_to"].is_string()) {
        const std::string reference = args["similar_to"].get<std::string>();
        std::string error;
        if (SoundEssentials::isReference(reference)) {
            work->sources.referenceEssential = reference.substr(std::string(SoundEssentials::PREFIX).size());
            if (!SoundEssentials::find(work->sources.referenceEssential)) {
                call->fail(reference + " is not an essential (search_sounds scope essentials lists them).");
                return;
            }
        } else if (SoundLibrary::isReference(reference)) {
            work->sources.referenceLibrary = reference.substr(std::string(SoundLibrary::PREFIX).size());
        } else {
            fs::path file;
            if (!McpRefs::resolveProjectPath(project, reference, file, error, true)) {
                call->fail(error);
                return;
            }
            work->sources.referenceFile = file;
        }
        query.similarTo = reference;
    }
    const std::string missingLibrary = (scope == SoundSearch::Scope::Library || scope == SoundSearch::Scope::All) && !SoundLibrary::get().status().available
                                           ? SoundLibrary::get().status().error : std::string();

    JobPtr job = call->ctx().jobs().create("search_sounds", "Search sounds");
    jobs::submit([work, job]() {
        runSearch(*work, job);
        if (work->error.empty()) job->succeed(std::to_string(work->total) + " sound(s) found.", Json{{"items", work->items}, {"total", work->total}});
        else job->fail(work->error);
    }, {jobs::Lane::Background, jobs::GateClass::Default, "search-sounds"});
    replyWhenJobDone(call, job, true, WAIT_SECONDS, [work, missingLibrary](const McpJob&) {
        if (!work->error.empty()) return ToolResult::error(work->error);
        Json data = {{"results", work->items}, {"total", work->total}, {"analysed", work->analysed}, {"image", work->imageInfo},
                     {"scope", SoundSearch::scopeName(work->query.scope)}};
        std::string text = std::to_string(work->total) + " sound(s)" + (work->analysed > 0 ? ", " + std::to_string(work->analysed) + " analysed now" : std::string()) + ":";
        for (size_t i = 0; i < work->results.size(); i++) text += "\n" + SoundSearch::line(work->results[i], (int)i + 1);
        if (work->results.empty() && !work->hint.text.empty()) {
            text += " " + work->hint.text;
            data["next"] = {{"text", work->hint.text}, {"queries", work->hint.queries}};
        }
        if (!missingLibrary.empty()) {
            data["library"] = {{"available", false}, {"why", missingLibrary}};
            text += "\nThe starter library is not installed: " + missingLibrary;
        }
        if (work->total > work->results.size()) text += "\nThe first " + std::to_string(work->results.size()) + "; a higher limit lists more.";
        ToolResult result = ToolResult::success(text, data);
        if (!work->png.empty()) result.images.push_back({work->png, "image/png"});
        return result;
    });
}

// ------------------------------------------------------------------ set_audio_import_settings

// A path of set_audio_import_settings: the file, the section values it ends with
struct ClipEdit {
    std::string rel;
    fs::path file;
    AudioImportSettings before;
    AudioImportSettings settings;
    AudioProvenance provenanceBefore;
    AudioProvenance provenance;
    Json resolved;
};

struct ImportWork {
    std::vector<ClipEdit> clips;
    fs::path cacheDirectory;
    std::string error;
    Json items = Json::array();
};

// the settings as they fit each file: analysis of the raw file, then resolve (a worker)
void checkFit(ImportWork& work) {
    for (ClipEdit& clip : work.clips) {
        Json cached;
        std::string error;
        if (!AudioAnalysis::analyzeFile(clip.file, work.cacheDirectory, cached, error)) {
            work.error = clip.rel + ": " + error;
            return;
        }
        const Json& format = cached["format"];
        ClipImportSettings resolved;
        const Json silence = cached.value("silence", Json::object());
        if (!clip.settings.resolve(format.value("frames", (uint64_t)0), format.value("sample_rate", 0u), numberOr(silence.value("head_ms", Json(0.0)), 0.0),
                                   numberOr(silence.value("tail_ms", Json(0.0)), 0.0), resolved, error, &clip.resolved)) {
            work.error = clip.rel + ": " + error;
            return;
        }
    }
}

void summarize(ImportWork& work) {
    int n = 0;
    for (const ClipEdit& clip : work.clips) {
        n++;
        Json cached;
        std::string error;
        if (!AudioAnalysis::analyzeClip(clip.file, work.cacheDirectory, cached, error)) {
            work.items.push_back({{"n", n}, {"path", clip.rel}, {"error", error}});
            continue;
        }
        Json analysis = AudioAnalysis::fileItem(cached, n, clip.rel, AudioAnalysis::labelsOf(clip.file), false, SoundTypes::HEAD_SILENCE_MAX_MS);
        Json item = {{"n", n}, {"path", clip.rel}, {"settings", clip.settings.toJson()}, {"resolved", clip.resolved},
                     {"analysis", {{"duration_s", analysis.contains("played") ? analysis["played"]["duration_s"] : analysis["format"]["duration_s"]},
                                   {"momentary_max", analysis["loudness"]["momentary_max"]},
                                   {"integrated_lufs", analysis["loudness"]["integrated_lufs"]},
                                   {"true_peak_dbtp", analysis["peak"]["true_peak_dbtp"]},
                                   {"kind", analysis["kind"]},
                                   {"loop", analysis["loop"]},
                                   {"notes", analysis["notes"]}}}};
        if (!clip.provenance.isEmpty()) item["provenance"] = clip.provenance.values;
        work.items.push_back(std::move(item));
    }
}

bool commitClipEdits(McpContext& ctx, const std::vector<ClipEdit>& clips, const std::vector<std::string>* labels, std::string& error) {
    Project* project = ctx.project();
    CommandHistory* history = project->getProjectCommandHistory();
    InteractiveEdit edit = history->beginInteractive("Set Audio Import Settings");
    SetAssetLabelsCmd::Changes before, after;
    for (const ClipEdit& clip : clips) {
        const fs::path relative = project->normalizeToProjectRelative(clip.file);
        auto add = [&](Command* command) {
            if (command->isNoOp()) {
                delete command;
                return true;
            }
            return history->addCommand(command);
        };
        if (!(clip.settings == clip.before) && !add(new SetAssetMetaCmd(project, relative, AudioImportSettings::metaKey, clip.settings.toYaml()))) {
            edit.cancel();
            error = "Couldn't write the import settings of " + clip.rel + ".";
            return false;
        }
        if (clip.provenance.values != clip.provenanceBefore.values && !add(new SetAssetMetaCmd(project, relative, AudioProvenance::metaKey, clip.provenance.toYaml()))) {
            edit.cancel();
            error = "Couldn't write the provenance of " + clip.rel + ".";
            return false;
        }
        if (labels) {
            AssetDatabase::AssetInfo identity;
            if (!project->getAssetDatabase().getInfo(relative, identity)) {
                edit.cancel();
                error = clip.rel + " has no asset identity yet: call sync_files first.";
                return false;
            }
            before.push_back({identity.guid, project->getAssetLabels().labelsOf(identity.guid)});
            after.push_back({identity.guid, *labels});
        }
    }
    if (labels && !history->addCommand(new SetAssetLabelsCmd(project, "Set Asset Labels", before, after))) {
        edit.cancel();
        error = "The labels could not be written (see the Console).";
        return false;
    }
    edit.commit();
    return true;
}

void setAudioImportSettingsTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    const bool hasSettings = args.contains("settings") && !args["settings"].is_null();
    const bool hasProvenance = args.contains("provenance") && !args["provenance"].is_null();
    const bool hasLabels = args.contains("labels") && !args["labels"].is_null();
    if (!hasSettings && !hasProvenance && !hasLabels) {
        call->fail("Give settings, provenance or labels to set.");
        return;
    }
    // a file written since the last look has no .meta yet, and the section needs its GUID beside it
    project->refreshAssetDatabase(true);
    auto labels = std::make_shared<std::vector<std::string>>();
    if (hasLabels) {
        for (const Json& label : args["labels"]) {
            std::string normalized, why;
            if (!label.is_string() || !AssetLabels::normalizeLabel(label.get<std::string>(), normalized, &why)) {
                call->fail(label.dump() + ": " + (why.empty() ? std::string("a label is a string") : why));
                return;
            }
            labels->push_back(normalized);
        }
    }
    auto work = std::make_shared<ImportWork>();
    work->cacheDirectory = AudioAnalysis::cacheDirectory(project->getProjectPath());
    for (const Json& value : args["paths"]) {
        ClipEdit clip;
        clip.rel = value.get<std::string>();
        std::string error;
        if (!McpRefs::resolveProjectWritePath(project, clip.rel, clip.file, error, true)) {
            call->fail(error);
            return;
        }
        clip.file = clip.file.lexically_normal();
        if (assetType(clip.file) != "audio") {
            call->fail(clip.rel + " is not an audio file.");
            return;
        }
        if (!project->isInsideAssetsPath(clip.file)) {
            call->fail(clip.rel + " is outside the assets folder: only an asset has a .meta.");
            return;
        }
        if (!AudioImportSettings::load(clip.file, clip.before, error) || !AudioProvenance::load(clip.file, clip.provenanceBefore, error)) {
            call->fail(clip.rel + ": its .meta does not read: " + error);
            return;
        }
        clip.settings = clip.before;
        clip.provenance = clip.provenanceBefore;
        if (hasSettings && !clip.settings.apply(args["settings"], error)) {
            call->fail(clip.rel + ": " + error);
            return;
        }
        if (hasProvenance && !clip.provenance.apply(args["provenance"], error)) {
            call->fail(clip.rel + ": " + error);
            return;
        }
        work->clips.push_back(std::move(clip));
    }

    JobPtr job = call->ctx().jobs().create("set_audio_import_settings", "Check the import settings of " + std::to_string(work->clips.size()) + " clip(s)");
    jobs::submit([work, job]() {
        checkFit(*work);
        if (work->error.empty()) job->succeed("Checked.");
        else job->fail(work->error);
    }, {jobs::Lane::Background, jobs::GateClass::Default, "audio-import-check"});
    call->waitUntil([job]() { return job->finished(); }, (int)(WAIT_SECONDS * 1000.0), [call, work, labels, hasLabels](bool timedOut) {
        if (timedOut) {
            call->fail("Measuring the clips took longer than 60 s: call again, the measurements are cached.");
            return;
        }
        if (!work->error.empty()) {
            call->fail(work->error);
            return;
        }
        std::string error;
        if (!commitClipEdits(call->ctx(), work->clips, hasLabels ? labels.get() : nullptr, error)) {
            call->fail(error);
            return;
        }
        if (ProjectWindow* window = call->ctx().projectWindow()) window->refreshCurrentDirectory();
        JobPtr summary = call->ctx().jobs().create("set_audio_import_settings", "Analyze " + std::to_string(work->clips.size()) + " clip(s)");
        jobs::submit([work, summary]() {
            summarize(*work);
            summary->succeed("Analysed.");
        }, {jobs::Lane::Background, jobs::GateClass::Default, "audio-import-summary"});
        replyWhenJobDone(call, summary, true, WAIT_SECONDS, [work](const McpJob&) {
            std::string text = "Set the import settings of " + std::to_string(work->items.size()) + " clip(s); undo with scope project.";
            for (const Json& item : work->items) {
                if (item.contains("error")) {
                    text += "\n" + std::to_string(item.value("n", 0)) + ". " + item.value("path", std::string()) + ": " + item.value("error", std::string());
                    continue;
                }
                const Json& analysis = item["analysis"];
                text += "\n" + std::to_string(item.value("n", 0)) + ". " + item.value("path", std::string()) + ": " +
                        (analysis["momentary_max"].is_number() ? formatted("%.1f LUFS-M", analysis["momentary_max"].get<double>()) : std::string("no loudness")) + ", " +
                        formatted("%.3f s", numberOr(analysis["duration_s"], 0.0));
                const std::string notes = AudioAnalysis::notesSummary(analysis["notes"]);
                if (!notes.empty()) text += "; " + notes;
            }
            return ToolResult::success(text, Json{{"items", work->items}});
        });
    });
}

} // namespace

bool importBuiltinSounds(const CallPtr& call, const std::vector<std::string>& references, const fs::path& directory, bool overwrite) {
    std::vector<BuiltinSoundImport::Clip> clips;
    std::string error;
    if (!BuiltinSoundImport::resolve(references, clips, error)) {
        call->fail(error);
        return false;
    }
    BuiltinSoundImport::Plan plan;
    if (!BuiltinSoundImport::plan(clips, directory, overwrite ? BuiltinSoundImport::Existing::Replace : BuiltinSoundImport::Existing::RefuseDifferent, plan, error)) {
        call->fail(error);
        return false;
    }
    std::vector<WriteFilesCmd::Item> items;
    for (BuiltinSoundImport::File& file : plan.writes) items.push_back({file.path, std::move(file.content)});
    if (!items.empty() && !writeFilesUndoable(call->ctx(), std::move(items), error)) {
        call->fail(error);
        return false;
    }
    Project* project = call->ctx().project();
    Json imported = Json::array(), skipped = Json::array();
    for (const fs::path& file : plan.imported) imported.push_back(projectRelative(project, file));
    for (const fs::path& file : plan.skipped) skipped.push_back(projectRelative(project, file));
    call->ok("Imported " + std::to_string(imported.size()) + " built-in sound(s)" +
             (skipped.empty() ? std::string() : ", " + std::to_string(skipped.size()) + " already there") + " into " + projectRelative(project, directory) +
             "; their licences are in provenance, undo with scope project.",
             Json{{"imported", imported}, {"skipped", skipped}});
    return true;
}

void assetInfoAudio(const CallPtr& call, const fs::path& file, const std::string& rel, std::function<void(const Json& audio, const std::string& error)> done) {
    struct Work {
        Json audio;
        std::string error;
    };
    auto work = std::make_shared<Work>();
    auto finished = std::make_shared<std::atomic<bool>>(false);
    const fs::path cacheDirectory = AudioAnalysis::cacheDirectory(call->ctx().project()->getProjectPath());
    jobs::submit([work, finished, file, rel, cacheDirectory]() {
        Json cached;
        if (AudioAnalysis::analyzeClip(file, cacheDirectory, cached, work->error)) {
            const Json item = AudioAnalysis::fileItem(cached, 1, rel, AudioAnalysis::labelsOf(file), false, SoundTypes::HEAD_SILENCE_MAX_MS);
            const Json& format = item["format"];
            Json notes = Json::array();
            for (const Json& note : item["notes"]) notes.push_back(note["text"]);
            work->audio = {
                {"codec", format["codec"]},
                {"encoding", format["encoding"]},
                {"channels", format["channels"]},
                {"sample_rate", format["sample_rate"]},
                {"frames", format["frames"]},
                {"duration_seconds", format["duration_s"]},
                {"bytes", format["bytes"]},
                {"analysis", {
                    {"duration_s", item.contains("played") ? item["played"]["duration_s"] : format["duration_s"]},
                    {"momentary_max", item["loudness"]["momentary_max"]},
                    {"integrated_lufs", item["loudness"]["integrated_lufs"]},
                    {"true_peak_dbtp", item["peak"]["true_peak_dbtp"]},
                    {"sample_peak_dbfs", item["peak"]["sample_dbfs"]},
                    {"peak_ms", item["onset"]["peak_ms"]},
                    {"head_ms", item["silence"]["head_ms"]},
                    {"kind", item["kind"]},
                    {"notes", notes}
                }}
            };
            AudioImportSettings settings;
            std::string ignored;
            if (AudioImportSettings::load(file, settings, ignored) && !settings.isDefault()) work->audio["import"] = settings.toJson();
            AudioProvenance provenance;
            if (AudioProvenance::load(file, provenance, ignored) && !provenance.isEmpty()) work->audio["provenance"] = provenance.values;
        }
        finished->store(true);
    }, {jobs::Lane::Background, jobs::GateClass::Default, "asset-info-audio"});
    call->waitUntil([finished]() { return finished->load(); }, (int)(WAIT_SECONDS * 1000.0), [work, done](bool timedOut) {
        done(timedOut ? Json() : work->audio, timedOut ? std::string("the analysis took longer than 60 s; analyze_audio reports the job") : work->error);
    });
}

void registerSoundTools(McpRegistry& registry) {
    using namespace schema;

    ToolDef tool;
    tool.name = "analyze_audio";
    tool.title = "Analyze Audio";
    tool.description =
        "You can't hear: measure. Loudness, peaks, bands, onsets, silence, loop seam, pitch, a kind and plain-language "
        "notes of sound files, built-in clips or a Play capture, with an image. A clip with import settings is measured "
        "as it plays. Later items say how they differ from the first. Fix a clip with set_audio_import_settings.";
    Json paths = array(assetPath(""), "Sound files, essentials:<id>, essentials:<group>/*, essentials:*, library:<id>, library:<type>/*.");
    paths["minItems"] = 1;
    paths["maxItems"] = (int)MAX_PATHS;
    Json start = number("Capture start, s.");
    start["minimum"] = 0;
    Json end = number("Capture end, s.");
    end["minimum"] = 0;
    Json offset = integer("Over 48 items: the page from this item.");
    offset["minimum"] = 0;
    tool.inputSchema = object({
        {"paths", paths},
        {"capture", string("Instead of paths: last, current, or a session id or directory.")},
        {"start", start},
        {"end", end},
        {"group", withDefault(string("Capture: master or a group with stems."), "master")},
        {"image", stringEnum("Default: both, sheet for several.", IMAGE_MODES)},
        {"detail", withDefault(stringEnum("full adds onsets and loudness curves.", {"summary", "full"}), "summary")},
        {"max_width", withDefault(integer("Image width, px.", AudioImages::MIN_WIDTH, AudioImages::MAX_WIDTH), DEFAULT_WIDTH)},
        {"offset", offset}
    });
    tool.annotations.readOnly = true;
    tool.annotations.idempotent = true;
    tool.activity = ToolActivity::Read;
    tool.allowedWhilePlaying = true;
    tool.handler = analyzeAudioTool;
    registry.addTool(std::move(tool));

    {
        ToolDef set;
        set.name = "set_audio_import_settings";
        set.title = "Set Audio Import Settings";
        set.description =
            "Sets how clips play and where they came from, in their .meta: gain, trims, click-safe fades, loop region, "
            "type, licence and labels (jasper://docs/sound lists the keys). A null key resets it. All paths or none, one "
            "undo step. Replies with the settings resolved to frames and a fresh analysis, as the clip now plays.";
        Json clips = array(assetPath(""), "Sound files inside the assets folder.");
        clips["minItems"] = 1;
        clips["maxItems"] = 64;
        set.inputSchema = object({
            {"paths", clips},
            {"settings", anyObject("Import settings keys: type, gain_db, trim_head_ms, trim_tail_ms, fade_in_ms, fade_out_ms, loop_start_ms, loop_end_ms.")},
            {"provenance", anyObject("Provenance keys: licence, licence_url, author, attribution, source, url.")},
            {"labels", array(Json{{"type", "string"}}, "S25 labels to set: replaces the clips' labels.")}
        }, {"paths"});
        set.annotations.idempotent = true;
        set.activity = ToolActivity::Edit;
        set.handler = setAudioImportSettingsTool;
        registry.addTool(std::move(set));
    }

    {
        ToolDef generate;
        generate.name = "generate_sound";
        generate.title = "Generate Sound";
        generate.description =
            "Makes a placeholder sound, no licence needed: a mono WAV from a preset at its type's loudness, with a .meta. The same "
            "request gives the same bytes; variations report their variety. like fits a recipe or layered preset to a clip. Builds "
            "warn about placeholders: replace them with search_sounds' clips. One undo step.";
        generate.inputSchema = object({
            {"path", assetPath("A .wav in the assets folder.")},
            {"preset", stringEnum("", SoundSynth::names())},
            {"seed", integer("Omitted: random, reported.", 0, MAX_SEED)},
            {"variations", withDefault(integer("", 1, MAX_VARIATIONS), 1)},
            {"family", string("impact: wood, metal, stone, soft, heavy. Others: jasper://docs/sound.")},
            {"like", string("Clip to fit: path, essentials: or library: id.")},
            {"params", anyObject("Overrides (jasper://docs/sound).")},
            {"loudness_lufs", number("Momentary max (default: the type's).", MIN_LOUDNESS, MAX_LOUDNESS)},
            {"image", stringEnum("Default: both; like: compare; variations: sheet.", IMAGE_MODES)},
            {"overwrite", withDefault(boolean(""), false)}
        }, {"path", "preset"});
        generate.annotations.idempotent = false;
        generate.activity = ToolActivity::Edit;
        generate.allowedWhilePlaying = true;
        generate.handler = generateSoundTool;
        registry.addTool(std::move(generate));
    }

    {
        ToolDef search;
        search.name = "search_sounds";
        search.title = "Search Sounds";
        search.description =
            "Finds sounds by words, type, character or similarity in the project and the built-in CC0 clips (40 essentials, 927 "
            "in the starter library), without a network. Bring a built-in clip into the project with import_files sources:[<id>]; "
            "heard_tags are a model's guess. Files not yet measured are analysed on the call.";
        std::vector<std::string> types;
        for (const SoundTypes::SoundType& type : SoundTypes::all()) types.push_back(type.name);
        Json limit = withDefault(integer("", 1, SoundSearch::MAX_LIMIT), SoundSearch::DEFAULT_LIMIT);
        search.inputSchema = object({
            {"query", string("Words, all must match name, path, type, labels or description.")},
            {"type", stringEnum("", types)},
            {"scope", withDefault(stringEnum("", {"project", "essentials", "library", "all"}), "all")},
            {"filters", object({
                {"min_ms", number("")},
                {"max_ms", number("")},
                {"brightness", stringEnum("", {"dark", "neutral", "bright"})},
                {"attack", stringEnum("", {"sharp", "soft"})},
                {"tonal", boolean("")},
                {"loop", boolean("")},
                {"kind", stringEnum("", {"clip", "event"})}
            })},
            {"similar_to", string("A project path or a built-in id: results by distance to it.")},
            {"limit", limit},
            {"sheet", withDefault(boolean("An image of the results, numbered as n."), false)}
        });
        search.annotations.readOnly = true;
        search.annotations.idempotent = true;
        search.activity = ToolActivity::Read;
        search.allowedWhilePlaying = true;
        search.handler = searchSoundsTool;
        registry.addTool(std::move(search));
    }
}

}
