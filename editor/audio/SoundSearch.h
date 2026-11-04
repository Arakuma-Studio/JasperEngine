// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "audio/AudioMeter.h"
#include "audio/SoundEvent.h"

#include "json.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::editor {

    class Project;

    // Finding sounds: an index of the project's audio files under its assets directory, the 40
    // essentials and the starter library's clips, with the features of each from the analysis cache
    // (AudioAnalysis, analysed when missing; a library clip's from the library's analysis.json when
    // it is current), and the search over it that search_sounds runs: words, a type, character
    // filters and "similar to". It never uses the network: its sources are files on disk, the
    // essentials compiled into the editor and the library installed next to it.
    //
    // Build the index in two steps: sources() on the main thread (the project's paths), then
    // index() on any thread (the directory walk, the metas, the cache and the analyses). search()
    // is a plain function of an index.
    class SoundSearch {
    public:
        using Json = nlohmann::json;

        static constexpr int DEFAULT_LIMIT = 20;
        static constexpr int MAX_LIMIT = 200;
        static constexpr int EVERY_MATCH = 0;

        // the character rules
        static constexpr double DARK_CENTROID_HZ = 800.0;
        static constexpr double BRIGHT_CENTROID_HZ = 3000.0;
        static constexpr double SHARP_ATTACK_MS = 10.0;
        static constexpr double SOFT_ATTACK_MS = 50.0;
        static constexpr double TONAL_CONFIDENCE = 0.6;

        enum class Scope {
            Project,
            Essentials,
            Library,
            All
        };

        // One sound of the index: a project clip, a project Sound Event, an essential or a
        // starter library clip
        struct Entry {
            // "project", "essentials" or "library"; "clip" or "event"
            std::string source;
            std::string kind;
            // what search_sounds names it by: the project-relative path, "essentials:<id>" or
            // "library:<id>"
            std::string path;
            // the assets-relative path (a project clip's SoundPool id, an event's path), or the
            // built-in clip's id ("footstep/concrete_1")
            std::string id;
            // a project entry's file, absolute
            std::filesystem::path file;
            // an event's GUID
            std::string guid;
            // the file stem ("step_1"), an event's too
            std::string name;
            // the importer's (or the event's, or the manifest's) type and labels; a built-in clip's
            // description; the licence ({id, source, ...}, null when none is known)
            std::string type;
            std::vector<std::string> tags;
            std::string description;
            Json licence = nullptr;
            // a library clip's heard tags (the manifest's semantic.heard: an
            // offline model's guess, matched by query words after its own tags)
            std::vector<std::string> heardTags;
            // an event's clips, assets-relative, in list order
            std::vector<std::string> clips;

            // -- features, from the analysis cache (an event's are its first clip's; its duration
            // the longest clip's). analysed false (with error) when the file could not be read or
            // analysed: such an entry matches words but no character filter and no similar_to.
            bool analysed = false;
            std::string error;
            // the SHA-1 of the bytes (the analysis cache's and AudioWaveform's key; an event's
            // first clip's)
            std::string sha1;
            // kind resolved per call from the cache's measured_kind and the labels (P1-10)
            std::string soundKind;
            double durationMs = 0.0;
            // momentary max, LUFS-M (NaN when the file has none: silence); its true peak
            double lufs = NAN;
            double truePeakDbtp = NAN;
            // NaN for a silent file (the analysis has no spectrum to measure)
            double centroidHz = NAN;
            double flatness = NAN;
            double attackMs = 0.0;
            double pitchConfidence = 0.0;
            std::array<double, AudioMeter::BANDS> bandShare = {};
            // the importer has a loop region (loop_start_ms or loop_end_ms)
            bool loopRegion = false;
            // the clip's importer gain_db (as Play
            // loads it): its level as it plays is lufs + gainDb (0 for an essential and an event)
            double gainDb = 0.0;
            // a built-in clip whose bytes a project file has (SHA-256 against the manifest's);
            // project entries are in the project
            bool inProject = false;
            // a result of similar_to: its distance to the reference
            double distance = NAN;
        };

        // The character filters. An unset filter is a negative length, an empty
        // text or -1.
        struct Filters {
            double minMs = -1.0;
            double maxMs = -1.0;
            // "dark", "neutral", "bright"; "sharp", "soft"; "clip", "event"
            std::string brightness;
            std::string attack;
            std::string kind;
            // -1 unset, 0 false, 1 true
            int tonal = -1;
            int loop = -1;

            bool any() const { return minMs >= 0.0 || maxMs >= 0.0 || !brightness.empty() || !attack.empty() || tonal >= 0 || loop >= 0; }
        };

        struct Query {
            // words, every one matched case-insensitively in the stem, the path, the type, a tag,
            // an event's name, a built-in clip's description or, as whole words, a library clip's
            // heard tags
            std::string text;
            // a SoundTypes.h type, exactly
            std::string type;
            Scope scope = Scope::All;
            Filters filters;
            // an Entry::path of the index; results ordered by their distance to it
            std::string similarTo;
            // 1..MAX_LIMIT (search_sounds' cap), or EVERY_MATCH: the Sound Browser shows a whole
            // source, its grid drawing only the visible cards
            int limit = DEFAULT_LIMIT;
        };

        // What index() reads, gathered by sources() on the main thread
        struct Sources {
            Scope scope = Scope::All;
            std::filesystem::path projectDirectory;
            std::filesystem::path assetsDirectory;
            std::filesystem::path cacheDirectory;
            // indexed whatever the scope, for similar_to: a project file (absolute), an
            // essential's id or a library clip's id
            std::filesystem::path referenceFile;
            std::string referenceEssential;
            std::string referenceLibrary;
            // the project's Sound Events: file (absolute) and GUID
            std::vector<std::pair<std::filesystem::path, std::string>> events;
        };

        static bool scopeFromName(const std::string& name, Scope& scope);
        static const char* scopeName(Scope scope);

        // Main thread
        static Sources sources(const Project* project, Scope scope);
        // The entries of the sources (project clips and events by path, then essentials by id,
        // then the available library clips by id), any thread. analysed counts the files analysed
        // now (a cache miss; a library clip with a current analysis.json item is not one). progress, when given,
        // is called with (done, total) as the files are read; cancel stops between files.
        static std::vector<Entry> index(const Sources& sources, int& analysed, const std::atomic<bool>* cancel = nullptr,
                                        const std::function<void(size_t, size_t)>& progress = {});

        // The results of a query over an index, cut to its limit; total gets how many matched.
        // False with error when similar_to names no entry of the index, or one with no features.
        // A word matches a field that contains it, or its singular (folded() of the word) when the
        // field's words are folded too: "footsteps" finds footstep, "body" finds a tag "bodies".
        static bool search(const std::vector<Entry>& index, const Query& query, std::vector<Entry>& results, size_t& total,
                           std::string& error);

        // A word's singular by the plural rules English sound names use: "-ies" -> "-y",
        // "-sses"/"-shes"/"-ches"/"-xes"/"-zzes" lose "es", else a final "s" goes (not "-ss", "-us",
        // "-is"); a word of 3 letters or fewer stays. Lower case in, lower case out.
        static std::string folded(const std::string& word);

        // Why a query found nothing and what to try (search_sounds and the Browser). text
        // is one or two sentences; queries are whole queries to run instead (the words that match
        // no sound replaced by the nearest terms of the index: names, tags, types, folders and
        // descriptions, by edit distance), at most MAX_SUGGESTIONS. Empty for a query with results.
        static constexpr size_t MAX_SUGGESTIONS = 3;
        struct Hint {
            std::string text;
            std::vector<std::string> queries;
        };
        static Hint emptyHint(const std::vector<Entry>& index, const Query& query);

        // What the clips' cached analyses say of a Sound Event (`clipFiles`: absolute, in its order; an
        // empty path: not on disk): for random, shuffle and sequence, a loudness spread over
        // EVENT_SPREAD_LU across the clips as they play, and near-duplicates (equal bytes, or a distance
        // under SoundTypes::NEAR_DUPLICATE_DISTANCE at levels within NEAR_DUPLICATE_LEVEL_LU); for layer,
        // each layer that adds nothing measurable. analysed counts the files analysed now. Any thread.
        static constexpr double EVENT_SPREAD_LU = 3.0;
        static constexpr double NEAR_DUPLICATE_LEVEL_LU = 1.0;
        static std::vector<std::string> eventProblems(const SoundEventDef& event, const std::vector<std::filesystem::path>& clipFiles,
                                                      const std::filesystem::path& cacheDirectory, int& analysed);

        // the character of an entry ("" when it has no features)
        static std::string brightnessOf(const Entry& entry);
        static std::string attackOf(const Entry& entry);
        static bool isTonal(const Entry& entry);
        static bool isLoop(const Entry& entry);
        // An entry with the features of a cached analysis, for distance() between files that are in no index
        static Entry entryOfAnalysis(const Json& cached);

        // similar_to's distance between two entries' features: with
        // d1 = log2(max(duration_s, 0.01)) / 4, d2 = log2(max(attack_ms, 0.5)) / 4,
        // d3 = log2(max(centroid_hz, 1) / 1000) / 2, d4 = flatness, d5 = pitch confidence,
        // sqrt(sum (a_i - b_i)^2) + 0.5 x sum |band_share_a - band_share_b| over the ten bands.
        // NaN when either has no features (not analysed, or silent).
        static double distance(const Entry& a, const Entry& b);

        // search_sounds' item: {n, path (project) or id (essentials:<id>, library:<id>), source,
        // kind, guid (events), type, sound_kind, tags, heard_tags (library clips that have them),
        // duration_ms, lufs, centroid_hz, licence (a library clip's {id, author, source}),
        // in_project, distance (similar_to), clips (events: project-relative paths), error (not
        // analysed)}
        static Json toJson(const Entry& entry, int n, const Project* project = nullptr);
        // one line for the reply's text
        static std::string line(const Entry& entry, int n);
    };

}
