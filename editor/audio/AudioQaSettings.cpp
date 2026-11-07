// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AudioQaSettings.h"

using namespace jasperengine::editor;

const std::vector<AudioQaField>& jasperengine::editor::audioQaFields() {
    using S = AudioQaSettings;
    static const std::vector<AudioQaField> fields = {
        {"audio.qa.head_silence_max_ms", "headSilenceMaxMs", "Head Silence Max (ms)", "The longest a started voice may stay silent.", &S::headSilenceMaxMs, nullptr, 0, 500},
        {"audio.qa.sync_tolerance_ms", "syncToleranceMs", "Sync Tolerance (ms)", "How far a sound may be from the moment that asks for it.", &S::syncToleranceMs, nullptr, 0, 500},
        {"audio.qa.audibility_margin_db", "audibilityMarginDb", "Audibility Margin (dB)", "How far a voice must stand over the other groups in its main band.", &S::audibilityMarginDb, nullptr, 0, 24},
        {"audio.qa.voice_budget_warn", "voiceBudgetWarn", "Voice Budget Warning", "The share of Max Voices that warns.", &S::voiceBudgetWarn, nullptr, 0.1, 1},
        {"audio.qa.important_priority", "importantPriority", "Important Priority", "A voice of this priority or better must never be stolen.", nullptr, &S::importantPriority, 0, 256},
        {"audio.qa.voice_over_music_db", "voiceOverMusicDb", "Voice Over Music (dB)", "How far a side-chain group must stand over the group it ducks.", &S::voiceOverMusicDb, nullptr, 0, 40},
        {"audio.qa.max_silence_seconds", "maxSilenceSeconds", "Max Silence (s)", "The longest gap once music has started.", &S::maxSilenceSeconds, nullptr, 0.1, 60},
        {"audio.qa.loudness_min_seconds", "loudnessMinSeconds", "Loudness Min Seconds", "Less gated audio than this has no loudness verdict.", &S::loudnessMinSeconds, nullptr, 1, 600},
        {"audio.qa.limiter_warn_db", "limiterWarnDb", "Limiter Warning (dB)", "Gain reduction of the Master limiter that says the mix is too hot.", &S::limiterWarnDb, nullptr, 0, 24},
        {"audio.qa.fix_rounds", "fixRounds", "Fix Rounds", "Consecutive failures of a check after which an agent stops and reports.", nullptr, &S::fixRounds, 1, 10},
        {"audio.qa.repeat_min_plays", "repeatMinPlays", "Repeat Min Plays", "Plays of one clip in a row that warn.", nullptr, &S::repeatMinPlays, 2, 32},
        {"audio.qa.repeat_window_s", "repeatWindowS", "Repeat Window (s)", "Those plays count within this time.", &S::repeatWindowS, nullptr, 0.5, 60},
        {"audio.qa.regression_timing_ms", "regressionTimingMs", "Regression Timing (ms)", "How far a start may move against a baseline.", &S::regressionTimingMs, nullptr, 0, 100},
        {"audio.qa.regression_band_db", "regressionBandDb", "Regression Band (dB)", "How far a group's band may move against a baseline.", &S::regressionBandDb, nullptr, 0, 12},
    };
    return fields;
}

bool AudioQaSettings::operator==(const AudioQaSettings& other) const {
    if (musicGroup != other.musicGroup) return false;
    for (const AudioQaField& field : audioQaFields()) {
        if (field.real && this->*field.real != other.*field.real) return false;
        if (field.whole && this->*field.whole != other.*field.whole) return false;
    }
    return true;
}
