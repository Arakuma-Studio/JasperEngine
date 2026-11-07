// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef AUDIOSESSIONREADER_H
#define AUDIOSESSIONREADER_H

#include "audio/AudioAnalysis.h"
#include "audio/AudioImages.h"
#include "audio/AudioMeter.h"

#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {

    // A capture session read back (docs/specs/S35-agent-audio.md R19): its lines, and its audio over a
    // range, for audio_session report and check. Plain data, any thread.
    struct AudioSessionData {
        using Json = nlohmann::json;

        AudioAnalysis::Capture capture;
        std::vector<Json> events;    // events.jsonl without its header
        std::vector<Json> levels;    // levels.jsonl without its header
        unsigned int rate = 48000;

        // [startFrame, startFrame + audio.frames) of master.wav; the range asked for may go past it
        AudioAnalysis::Signal audio;
        uint64_t startFrame = 0;
        double startSeconds = 0.0;
        double endSeconds = 0.0;
        // what AudioMeter says of that audio, in Stream mode, and its curves
        LoudnessResult loudness;
        std::vector<double> momentary;
        std::vector<double> shortTerm;

        // the range holds this sample of the capture's clock
        bool inRange(const Json& line) const;
        double seconds(const Json& line) const;
    };

    class AudioSessionReader {
    public:
        using Json = nlohmann::json;

        // A kind of line that is a gameplay moment (M<n>), not a voice (E<n>)
        static bool isMarkerKind(const std::string& kind);
        static bool isVoiceKind(const std::string& kind);

        // A session's WAV by its name in session.json: the published file, else the temporary sibling
        // a live or crashed session is still writing. Empty when neither is there.
        static std::filesystem::path audioFile(const std::filesystem::path& directory, const std::string& relative);

        // The lines and the audio over [start, end) seconds (end < 0: to the end of the session)
        static bool load(const AudioAnalysis::Capture& capture, double start, double end, AudioSessionData& data, std::string& error);

        // status's entry of a session
        static Json statusEntry(const AudioAnalysis::Capture& capture);

        // report's data, with events_limit lines of each list; labels receives the image's
        static Json report(const AudioSessionData& data, int eventsLimit, std::vector<AudioImages::Label>& markerLabels,
                           std::vector<AudioImages::Label>& eventLabels);
        // the play stop summary: the session's loudness and counts
        static Json stopSummary(const AudioSessionData& data);
        // the voices playing at once through a group, at most, over the range: "" is every group
        static int voicesPeak(const AudioSessionData& data, const std::string& group);
    };

}

#endif //AUDIOSESSIONREADER_H
