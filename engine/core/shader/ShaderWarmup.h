// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SHADERWARMUP_H
#define SHADERWARMUP_H

#include "Export.h"
#include "ShaderBuildTypes.h"

#include <cstddef>
#include <string>
#include <vector>

namespace jasperengine {

    // The shader warm-up list (spec S32 R10-R12): the shader variants and pipelines a game uses,
    // recorded while it plays (the editor's Play with Record During Play on, a development build's
    // --record-shader-warmup <file>), shipped with the export (shaders/warmup.json, or embedded with
    // the shader headers) and replayed when the game starts, so what it draws is created before it
    // is needed. Replay requests the variants a few a frame and, once a variant is ready, its
    // pipelines; the pipelines are held for the session, so an object that needs one later gets it
    // ready (pipeline interning).
    //
    // Format (version 1): {"version":1, "family":"gl"|"modern", "shaders":[{"key","custom"}],
    // "pipelines":[{"shader":<index>, "desc":{...}}]}. A key is the storage key's text (a fork's
    // session id stripped), "custom" the fork's base path; "desc" is SokolPipelineRecord's JSON.
    // Main thread, except the recorder, which only queues.
    class JASPERENGINE_API ShaderWarmup {
    public:
        // ---- the list
        // false (with *error) for text that is not a version 1 list; malformed entries are skipped
        // and counted (getSkippedCount), an unknown version is ignored with a warning
        static bool loadFromString(const std::string& text, std::string* error = nullptr);
        static bool load(const std::string& path, std::string* error = nullptr);
        static std::string saveToString();
        // atomic (S03); false with *error
        static bool save(const std::string& path, std::string* error = nullptr);
        // adds the entries of another list's text that are not in this one
        static bool mergeFromString(const std::string& text, std::string* error = nullptr);
        static void clear();
        static size_t getShaderCount();
        static size_t getPipelineCount();
        static size_t getSkippedCount();
        // the variants' keys (forks registered in this session); the export ships them
        static std::vector<ShaderKey> getShaderKeys();
        // the variants' keys of a list's text, leaving the list loaded now as it is (any thread: a
        // build's shader list reads the file during Play's replay); false with *error as loadFromString
        static bool readShaderKeys(const std::string& text, std::vector<ShaderKey>& keys, std::string* error = nullptr);
        // the list without the variants (and their pipelines) not in `keep` (the export's selection)
        static std::string filteredString(const std::vector<ShaderKey>& keep);

        // ---- recording
        static void startRecording();
        static void stopRecording();
        static bool isRecording();

        // ---- replay
        // the build's list (embedded "warmup", else shader://warmup.json), replayed from now; false
        // when the build has none
        static bool beginShipped();
        // replays the list loaded now
        static void begin();
        static bool isActive();
        // every variant ready or failed and every pipeline created or failed (true when not active)
        static bool isDone();
        static float getProgress();
        static size_t getReadyCount();
        static size_t getEntryCount();
        // drops the pipelines the replay holds and stops it
        static void end();

        // once a frame (Engine, the editor's loop): resolves what was recorded and replays a step
        static void update();
    };

}

#endif // SHADERWARMUP_H
