// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef AUDIOCAPTURE_H
#define AUDIOCAPTURE_H

#include "Export.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine {

    class Scene;

    struct AudioCaptureOptions {
        // the session is written to <root>/<id>; the oldest over `keep` go first
        std::filesystem::path root;
        std::string project;
        std::string scene;
        // the name a voice's or a marker's scene is logged with; empty: SceneManager's
        std::function<std::string(const Scene*)> sceneName;
        int maxSeconds = 120;
        int keep = 3;
        bool stems = false;
        uint32_t seed = 1;
    };

    // Writes what the deterministic driver mixes and what started it to a session directory, on one
    // sample clock (docs/specs/S35-agent-audio.md R10-R17): master.wav, events.jsonl, levels.jsonl,
    // session.json and optional stems. Main thread. Off in release players
    // (JASPERENGINE_AUDIO_CAPTURE): the stubs below keep a script behaving the same everywhere.
    class JASPERENGINE_API AudioCapture {
    public:
#if JASPERENGINE_AUDIO_CAPTURE
        // Opens the driver when it is not open. False, with the reason, when nothing can be written:
        // the game goes on uncaptured.
        static bool begin(const AudioCaptureOptions& options, std::string* error);
        static void end();

        static bool isActive() {
            return active.load(std::memory_order_relaxed);
        }

        // the session being written or the last one ("" before the first)
        static std::filesystem::path lastSession();
        static std::string sessionId();
        // the capture's clock: frames since sample 0
        static uint64_t cursor();

        // Audio.mark
        static void mark(std::string_view label, bool fromLua);

        // AudioSystem::pumpDeterministic, after its chunks: voices that ended, lines to disk
        static void frameMixed();

        // True for a name begin() gives a session (YYYYMMDD-HHMMSS-NN)
        static bool isSessionId(const std::string& name);

        // A line of events.jsonl with its common fields; the rest is added field by field and it
        // is written when it goes out of scope. Only while isActive().
        class JASPERENGINE_API Line {
        public:
            explicit Line(const char* kind);
            ~Line();
            Line(const Line&) = delete;
            Line& operator=(const Line&) = delete;

            Line& text(const char* key, std::string_view value);
            // rounded to `decimals`, no trailing zeros, null when not finite
            Line& number(const char* key, double value, int decimals);
            Line& integer(const char* key, int64_t value);
            Line& flag(const char* key, bool value);
            Line& null(const char* key);
            Line& texts(const char* key, const std::vector<std::string>& values);
            Line& numbers(const char* key, const std::vector<float>& values, int decimals);
            // already JSON
            Line& raw(const char* key, std::string_view json);
            // the entity's fields: its hierarchy path and id; the scene's name
            Line& entity(const Scene* scene, uint32_t entityId, const char* pathKey = "entity", const char* idKey = "entity_id");
            Line& scene(const Scene* scene);

        private:
            std::string out;
        };

        // the hierarchy path every MCP tool uses (editor/util/EntityPath), "" for no entity
        static std::string entityPath(const Scene* scene, uint32_t entity);

    private:
        static std::atomic<bool> active;
#else
        static bool begin(const AudioCaptureOptions&, std::string* error) {
            if (error) *error = "this build has no audio capture";
            return false;
        }
        static void end() {}
        static bool isActive() { return false; }
        static std::filesystem::path lastSession() { return {}; }
        static std::string sessionId() { return {}; }
        static uint64_t cursor() { return 0; }
        static void mark(std::string_view, bool) {}
#endif
    };

}

#endif //AUDIOCAPTURE_H
