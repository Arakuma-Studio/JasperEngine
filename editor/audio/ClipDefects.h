// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "json.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace jasperengine::editor {

    class Project;

    // The notes of a project clip as it plays, for the editor's windows: the Inspector's clip view
    // shows them beside the measurements table from one result. The notes are AudioAnalysis's of the
    // clip with its import settings, through the project's analysis cache.
    //
    // Never decodes on the calling thread: a miss queues the clip for one worker thread, which
    // runs one job at a time, the newest request first, and wakes the editor loop when a result
    // is in. Results are kept by the clip's id and the stamps (size and modification time) of the
    // file and its .meta, so a repeat costs nothing and any change measures again. Everything is
    // dropped when the project closes (clear) and, as a second guard, when another project asks.
    class ClipDefects {
    public:
        using Json = nlohmann::json;

        struct Result {
            // queued or being analysed: draw nothing yet
            bool pending = true;
            // the played region's notes ({id, text[, fix]}, AudioAnalysis::notes)
            Json notes = Json::array();
            // the item the notes come from (AudioAnalysis::fileItem), with onsets_ms kept for the
            // waveform's onset ticks; null while pending or on an error
            Json item = nullptr;
            // why there are none (the file can't be read or decoded)
            std::string error;
        };

        // The clip `id` (its path from the assets directory, as Audio Sources name it) of project.
        // Main thread.
        static Result get(const Project* project, const std::string& id);

        // drops every result and the queue (the project closes; also done when another project
        // asks); a job running then is dropped when it ends
        static void clear();

        // A note's `fix` (fade_in_ms / fade_out_ms) written into the clip's import settings as one
        // project history step named "Add Click-Safe Fades". A section that does not read gets an
        // alert and nothing is written. True when it wrote. Main thread.
        static bool addClickSafeFades(Project* project, const std::filesystem::path& asset, const Json& fix);

    private:
        struct Stamp {
            bool known = false;
            uintmax_t size = 0;
            int64_t modified = 0;

            bool operator==(const Stamp& other) const {
                return known == other.known && size == other.size && modified == other.modified;
            }
        };

        struct Key {
            std::string id;
            Stamp file;
            Stamp meta;

            bool operator==(const Key& other) const {
                return id == other.id && file == other.file && meta == other.meta;
            }
        };

        struct Job {
            Key key;
            std::filesystem::path file;
            std::filesystem::path cacheDirectory;
        };

        // the results, the queue and the worker thread (ClipDefects.cpp)
        struct State;
        static State& state();

        static Stamp stampOf(const std::filesystem::path& path);
        // the cached result of the job's key, else the job queued first (main thread)
        static Result request(const Project* project, const Job& job);
        static Result run(const Job& job);
        static void work();
    };

}
