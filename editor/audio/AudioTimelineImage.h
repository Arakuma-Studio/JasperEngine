// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef AUDIOTIMELINEIMAGE_H
#define AUDIOTIMELINEIMAGE_H

#include "audio/AudioImages.h"
#include "audio/AudioSessionReader.h"

#include <string>
#include <vector>

namespace jasperengine::editor {

    // A session's picture (docs/specs/S35-agent-audio.md R22): title, master waveform, spectrogram,
    // loudness, one lane per group with a level above -60 dB, markers, events, a check's failures and
    // its listen moments, time. Also what Window > Audio > Captures shows.
    class AudioTimelineImage {
    public:
        struct Tick {
            double seconds = 0.0;
            std::string text;
        };
        struct Options {
            std::vector<AudioImages::Label> markers;
            std::vector<AudioImages::Label> events;
            std::vector<Tick> failures;
            std::vector<Tick> warnings;
            std::vector<Tick> listen;
        };
        struct Result {
            AudioImages::Image image;
            // lanes top to bottom: {name, y, height}
            nlohmann::json lanes;
        };

        static constexpr int GROUP_LANE_HEIGHT = 32;
        static constexpr int CHECK_LANE_HEIGHT = 24;

        static Result render(const AudioSessionData& data, const Options& options, int width);
    };

}

#endif //AUDIOTIMELINEIMAGE_H
