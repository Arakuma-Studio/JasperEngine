// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AudioCompare.h"

#include "SoundTypes.h"
#include "audio/AudioMeter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

using Json = AudioCompare::Json;

namespace {

std::string format(const char* pattern, double value) {
    char text[64];
    std::snprintf(text, sizeof(text), pattern, value);
    return text;
}

bool number(const Json& item, const char* group, const char* key, double& out) {
    const Json& g = group ? item.value(group, Json(nullptr)) : item;
    if (!g.is_object() || !g.contains(key) || !g[key].is_number()) return false;
    out = g[key].get<double>();
    return std::isfinite(out);
}

std::string kindOf(const Json& item) {
    if (item.contains("kind") && item["kind"].is_string()) return item["kind"].get<std::string>();
    return item.value("measured_kind", std::string());
}

// "2.1 -> 1.6 kHz", "850 -> 600 Hz", "850 Hz -> 1.2 kHz"
std::string hertz(double a, double b) {
    auto one = [](double v) { return v >= 1000.0 ? format("%.1f", v / 1000.0) : format("%.0f", v); };
    const bool ka = a >= 1000.0, kb = b >= 1000.0;
    if (ka == kb) return one(a) + " -> " + one(b) + (ka ? " kHz" : " Hz");
    return one(a) + (ka ? " kHz" : " Hz") + " -> " + one(b) + (kb ? " kHz" : " Hz");
}

std::string millis(double a, double b) {
    if (a < 1000.0 && b < 1000.0) return format("%.0f", a) + " -> " + format("%.0f ms", b);
    return format("%.2f", a / 1000.0) + " -> " + format("%.2f s", b / 1000.0);
}

// how far apart two positive values are, as the larger over the smaller (infinite when one is 0)
double ratioOf(double a, double b) {
    const double lo = std::min(a, b), hi = std::max(a, b);
    if (hi <= 0.0) return 1.0;
    return lo > 0.0 ? hi / lo : INFINITY;
}

// the envelope peak to the last sample at or above the silence level, ms
bool tailOf(const Json& item, double& out) {
    double duration = 0.0, tail = 0.0, peak = 0.0;
    if (!number(item, "format", "duration_s", duration) || !number(item, "silence", "tail_ms", tail) ||
        !number(item, "onset", "envelope_peak_ms", peak)) {
        return false;
    }
    out = std::max(0.0, duration * 1000.0 - tail - peak);
    return true;
}

// the first to the last sample at or above the silence level, ms
bool lengthOf(const Json& item, double& out) {
    double duration = 0.0, head = 0.0, tail = 0.0;
    if (!number(item, "format", "duration_s", duration) || !number(item, "silence", "head_ms", head) ||
        !number(item, "silence", "tail_ms", tail)) {
        return false;
    }
    out = std::max(0.0, duration * 1000.0 - head - tail);
    return true;
}

// the low octave bands' share of the energy of all ten, dB (false without bands): the sound's balance,
// so a copy at another gain has the same low end
bool lowEndOf(const Json& item, double& out) {
    const Json& bands = item.value("bands_db", Json(nullptr));
    if (!bands.is_object()) return false;
    double low = 0.0, all = 0.0;
    for (int b = 0; b < AudioMeter::BANDS; b++) {
        const char* label = AudioMeter::bandLabel(b);
        if (!bands.contains(label) || !bands[label].is_number()) return false;
        const double energy = std::pow(10.0, bands[label].get<double>() / 10.0);
        all += energy;
        if (b < SoundTypes::DIFFERENCE_LOW_END_BANDS) low += energy;
    }
    if (!(all > 0.0)) return false;
    out = low > 0.0 ? 10.0 * std::log10(low / all) : AudioMeter::SILENCE_DB;
    return true;
}

struct Phrase {
    // "is" or "has"
    const char* verb;
    std::string words;
};

} // namespace

Json AudioCompare::differences(const Json& a, const Json& b, const std::string& name) {
    std::vector<std::string> keys;
    std::vector<Phrase> phrases;
    auto add = [&](const char* key, const char* verb, const std::string& words) {
        if ((int)keys.size() >= SoundTypes::DIFFERENCES_LISTED) return;
        keys.push_back(key);
        phrases.push_back({verb, words});
    };

    // loudness: momentary max, integrated for the kinds measured over their whole length
    const std::string kind = kindOf(a);
    const char* loudnessKey = kind == SoundTypes::LOOP || kind == SoundTypes::SPEECH ? "integrated_lufs" : "momentary_max";
    double la = 0.0, lb = 0.0;
    if (number(a, "loudness", loudnessKey, la) && number(b, "loudness", loudnessKey, lb) &&
        std::fabs(lb - la) >= SoundTypes::DIFFERENCE_LOUDNESS_LU) {
        add("loudness", "is", format("%.1f dB ", std::fabs(lb - la)) + (lb > la ? "louder" : "quieter"));
    }
    double ca = 0.0, cb = 0.0;
    if (number(a, nullptr, "centroid_hz", ca) && number(b, nullptr, "centroid_hz", cb) && ca > 0.0 && cb > 0.0 &&
        ratioOf(ca, cb) >= SoundTypes::DIFFERENCE_BRIGHTNESS_RATIO) {
        add("brightness", "is", std::string(cb > ca ? "brighter" : "darker") + " (" + hertz(ca, cb) + ")");
    }
    double aa = 0.0, ab = 0.0;
    if (number(a, "onset", "attack_ms", aa) && number(b, "onset", "attack_ms", ab) &&
        ratioOf(aa, ab) >= SoundTypes::DIFFERENCE_ATTACK_RATIO && std::fabs(ab - aa) >= SoundTypes::DIFFERENCE_ATTACK_MIN_MS) {
        add("attack", "has", std::string(ab < aa ? "a sharper" : "a softer") + " attack (" + millis(aa, ab) + ")");
    }
    double ta = 0.0, tb = 0.0;
    if (tailOf(a, ta) && tailOf(b, tb) && ratioOf(ta, tb) >= SoundTypes::DIFFERENCE_TAIL_RATIO) {
        add("tail", "has", std::string(tb > ta ? "a longer" : "a shorter") + " tail (" + millis(ta, tb) + ")");
    }
    double ea = 0.0, eb = 0.0;
    if (lowEndOf(a, ea) && lowEndOf(b, eb) && std::fabs(eb - ea) >= SoundTypes::DIFFERENCE_LOW_END_DB) {
        add("low_end", "has", format("%.1f dB ", std::fabs(eb - ea)) + (eb > ea ? "more" : "less") + " low end");
    }
    double na = 0.0, nb = 0.0;
    if (lengthOf(a, na) && lengthOf(b, nb) && ratioOf(na, nb) >= SoundTypes::DIFFERENCE_LENGTH_RATIO) {
        add("length", "is", std::string(nb > na ? "longer" : "shorter") + " (" + millis(na, nb) + ")");
    }
    double ra = 0.0, rb = 0.0;
    if (number(a, nullptr, "rolloff_hz", ra) && number(b, nullptr, "rolloff_hz", rb) && ra > 0.0 && rb > 0.0 &&
        ratioOf(ra, rb) >= SoundTypes::DIFFERENCE_BANDWIDTH_RATIO) {
        add("bandwidth", "is", std::string(rb > ra ? "wider" : "narrower") + " (" + hertz(ra, rb) + ")");
    }

    if (phrases.empty()) return {{"keys", Json::array()}, {"text", NONE}};
    // "2 is 3.0 dB quieter, darker (...) and has a shorter tail (...)": a verb where it changes
    std::string text = name;
    for (size_t i = 0; i < phrases.size(); i++) {
        if (i > 0) text += i + 1 == phrases.size() ? " and" : ",";
        const bool verb = i == 0 || std::string(phrases[i].verb) != phrases[i - 1].verb;
        text += std::string(" ") + (verb ? std::string(phrases[i].verb) + " " : std::string()) + phrases[i].words;
    }
    return {{"keys", keys}, {"text", text}};
}
