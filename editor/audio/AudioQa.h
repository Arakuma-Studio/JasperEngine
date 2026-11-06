// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef AUDIOQA_H
#define AUDIOQA_H

#include "audio/AudioQaSettings.h"
#include "audio/AudioSessionReader.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace jasperengine::editor {

    // audio_session check (docs/specs/S35-agent-audio.md R21, R23): twelve checks of a capture, each
    // with the measured value, the threshold and where it came from, what limits it and how to fix
    // it. A check never changes a sound or a mix. Any thread.
    class AudioQa {
    public:
        using Json = nlohmann::json;

        struct DuckEffect {
            std::string group;
            std::string sidechain;
            double level = 0.5;   // the ducked group's linear gain while the side chain plays
        };

        struct Request {
            std::vector<std::string> checks;   // empty: all
            Json expect = Json::array();
            Json baseline;                     // null: none
            AudioQaSettings qa;
            AudioLoudnessTargets targets;
            int maxVoices = 256;
            std::vector<DuckEffect> ducks;
            std::filesystem::path projectDirectory;
            std::filesystem::path assetsDirectory;
            // consecutive FAILs before this run: check id > rounds
            std::map<std::string, int> rounds;
        };

        static const std::vector<std::string>& checkIds();

        // verdict, checks[], listen[], thresholds{values, changed}, guidance
        static Json run(const AudioSessionData& data, const Request& request);
        // the detection of a voice's onset: the first frame of the range from `from` where the master
        // rises marginDb over the loudest 1 ms of the 50 ms before it; -1 when none in `searchFrames`
        static int64_t onsetFrame(const AudioSessionData& data, int64_t from, int64_t searchFrames, double marginDb);
    };

}

#endif //AUDIOQA_H
