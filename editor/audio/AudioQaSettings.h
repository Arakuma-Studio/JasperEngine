// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef AUDIOQASETTINGS_H
#define AUDIOQASETTINGS_H

#include <string>
#include <vector>

namespace jasperengine::editor {

    // Project Settings > Audio > Capture and QA: the thresholds of audio_session check
    // (docs/specs/S35-agent-audio.md 6.3). Changing one does not fix a sound; a check reports it.
    struct AudioQaSettings {
        float headSilenceMaxMs = 15.0f;
        float syncToleranceMs = 20.0f;
        float audibilityMarginDb = 3.0f;
        float voiceBudgetWarn = 0.8f;
        int importantPriority = 55;
        float voiceOverMusicDb = 10.0f;
        float maxSilenceSeconds = 2.0f;
        std::string musicGroup = "Music";
        float loudnessMinSeconds = 10.0f;
        float limiterWarnDb = 6.0f;
        int fixRounds = 2;
        int repeatMinPlays = 4;
        float repeatWindowS = 4.0f;
        float regressionTimingMs = 5.0f;
        float regressionBandDb = 1.0f;

        bool operator==(const AudioQaSettings& other) const;
    };

    // One numeric threshold: every place that lists them (the registry, project.yaml, the window, a
    // check's audit) walks this table
    struct AudioQaField {
        const char* key;           // audio.qa.<name>, the MCP key
        const char* yaml;          // its key under project.yaml's audio.qa
        const char* label;
        const char* help;
        float AudioQaSettings::*real;     // one of these two
        int AudioQaSettings::*whole;
        double minimum;
        double maximum;
    };

    const std::vector<AudioQaField>& audioQaFields();

    // S38's targets a check compares with: its settings when they exist, else these defaults
    struct AudioLoudnessTargets {
        double targetLufs = -18.0;
        double toleranceLu = 2.0;
        double ceilingDbtp = -1.0;
    };

}

#endif //AUDIOQASETTINGS_H
