// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef AUDIOBASELINE_H
#define AUDIOBASELINE_H

#include "audio/AudioSessionReader.h"

#include <filesystem>
#include <string>

namespace jasperengine::editor {

    // What audio_session check's regression compares (docs/specs/S35-agent-audio.md R25):
    // baselines/audio/<name>.json, jasperengine.audio-baseline v1: the starts, their onsets and the
    // group bands per marker. A project file: one undo step, under version control.
    class AudioBaseline {
    public:
        using Json = nlohmann::json;

        static std::filesystem::path file(const std::filesystem::path& projectDirectory, const std::string& name);
        // A name without path separators
        static bool isName(const std::string& name);
        static Json build(const AudioSessionData& data, double marginDb);
        // false with why when the file is not a baseline
        static bool read(const std::filesystem::path& file, Json& out, std::string& why);
    };

}

#endif //AUDIOBASELINE_H
