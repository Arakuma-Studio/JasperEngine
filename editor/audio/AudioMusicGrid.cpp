// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AudioMusicGrid.h"

#include "SoundTypes.h"
#include "util/SHA1.h"

#include "io/AtomicFile.h"
#include "jobs/JobSystem.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

using Json = AudioMusicGrid::Json;

// samples between spectral frames
constexpr double HOP = (double)AudioAnalysis::FFT_HOP;
// frames of the onset envelope each job computes (each also transforms the frame before its first)
constexpr size_t ONSET_CHUNK_FRAMES = 128;

// to a decimal step (0.1, 0.01) as the nearest double to the decimal, so 7.93 reads 7.93
double roundedTo(double value, double step) {
    const double scale = std::round(1.0 / step);
    return std::round(value * scale) / scale;
}

std::string gridText(const char* pattern, double value) {
    char text[64];
    std::snprintf(text, sizeof(text), pattern, value);
    return text;
}

// The bins [first, last] of a band in an FFT_SIZE transform at `rate`
void bandBins(double rate, double low, double high, int& first, int& last) {
    const int n = AudioAnalysis::FFT_SIZE;
    first = std::max(1, (int)std::ceil(low * n / rate));
    last = std::min(n / 2, (int)std::floor(high * n / rate));
}

// The onset strength of the full band and of the low band, a value per spectral frame: the summed
// rise of the log-compressed magnitudes from the frame before (P6 §2.4.8 step 1, before its
// normalisation). Frames are independent but for their predecessor, so chunks run in parallel
// and give the same numbers as one pass.
void onsetEnvelopes(const std::vector<float>& mono, double rate, std::vector<double>& full, std::vector<double>& low) {
    const size_t frames = AudioAnalysis::spectralFrames(mono.size());
    full.assign(frames, 0.0);
    low.assign(frames, 0.0);
    int first = 0, last = 0, lowLast = 0, unused = 0;
    bandBins(rate, SoundTypes::MUSIC_GRID_LOW_HZ, SoundTypes::MUSIC_GRID_HIGH_HZ, first, last);
    bandBins(rate, SoundTypes::MUSIC_GRID_LOW_HZ, SoundTypes::MUSIC_GRID_DOWNBEAT_HIGH_HZ, unused, lowLast);
    if (last < first) return;
    // |X| / (FFT_SIZE / 2): a full-scale sine's bin reads about 0.5
    const double scale = 2.0 / AudioAnalysis::FFT_SIZE;
    const size_t bins = (size_t)(last - first + 1);
    jobs::parallelForChunked(0, frames, ONSET_CHUNK_FRAMES, [&](size_t begin, size_t end) {
        std::vector<float> magnitude(AudioAnalysis::FFT_SIZE / 2 + 1);
        std::vector<double> previous(bins), current(bins);
        auto logSpectrum = [&](size_t frame, std::vector<double>& out) {
            AudioAnalysis::magnitudes(mono.data(), mono.size(), frame, magnitude.data());
            for (size_t k = 0; k < bins; k++) {
                out[k] = std::log1p(SoundTypes::MUSIC_GRID_COMPRESSION * scale * magnitude[(size_t)first + k]);
            }
        };
        if (begin > 0) logSpectrum(begin - 1, previous);
        for (size_t t = begin; t < end; t++) {
            logSpectrum(t, current);
            if (t > 0) {
                double sum = 0.0, lowSum = 0.0;
                for (size_t k = 0; k < bins; k++) {
                    const double rise = std::max(0.0, current[k] - previous[k]);
                    sum += rise;
                    if ((int)k + first <= lowLast) lowSum += rise;
                }
                full[t] = sum;
                low[t] = lowSum;
            }
            std::swap(previous, current);
        }
    });
}

// Minus the moving mean over +- MUSIC_GRID_MEAN_SECONDS, negative values to 0, over the
// standard deviation (a flat envelope stays 0)
void normalise(std::vector<double>& o, double frameRate) {
    const size_t n = o.size();
    if (n == 0) return;
    const long half = std::max(1L, std::lround(SoundTypes::MUSIC_GRID_MEAN_SECONDS * frameRate));
    std::vector<double> prefix(n + 1, 0.0);
    for (size_t t = 0; t < n; t++) prefix[t + 1] = prefix[t] + o[t];
    std::vector<double> out(n);
    for (size_t t = 0; t < n; t++) {
        const size_t a = (size_t)std::max(0L, (long)t - half);
        const size_t b = std::min(n - 1, t + (size_t)half);
        out[t] = std::max(0.0, o[t] - (prefix[b + 1] - prefix[a]) / (double)(b - a + 1));
    }
    double mean = 0.0;
    for (double v : out) mean += v;
    mean /= (double)n;
    double variance = 0.0;
    for (double v : out) variance += (v - mean) * (v - mean);
    const double deviation = std::sqrt(variance / (double)n);
    if (deviation > 0.0) {
        for (double& v : out) v /= deviation;
    }
    o = std::move(out);
}

// The tempo's autocorrelation A[lag] for lags 0..maxLag, of the envelope less its mean: the
// clamped envelope's mean alone would read 0.32 on white noise
std::vector<double> autocorrelation(const std::vector<double>& o, size_t maxLag) {
    std::vector<double> a(maxLag + 1, 0.0);
    const size_t n = o.size();
    double mean = 0.0;
    for (double v : o) mean += v;
    mean /= (double)std::max<size_t>(1, n);
    std::vector<double> centred(n);
    for (size_t t = 0; t < n; t++) centred[t] = o[t] - mean;
    double energy = 0.0;
    for (double v : centred) energy += v * v;
    if (energy <= 0.0) return a;
    for (size_t lag = 0; lag <= maxLag && lag < n; lag++) {
        double sum = 0.0;
        for (size_t t = 0; t + lag < n; t++) sum += centred[t] * centred[t + lag];
        a[lag] = sum / energy;
    }
    return a;
}

double interpolated(const std::vector<double>& a, double lag) {
    if (lag < 0.0 || a.empty()) return 0.0;
    const size_t i = (size_t)std::floor(lag);
    if (i + 1 >= a.size()) return i < a.size() ? a[i] : 0.0;
    const double f = lag - (double)i;
    return a[i] * (1.0 - f) + a[i + 1] * f;
}

double priorWeight(double bpm) {
    const double octaves = std::log2(bpm / SoundTypes::MUSIC_GRID_PRIOR_BPM);
    return std::exp(-0.5 * octaves * octaves);
}

// Ellis 2007: C[t] = o[t] + max over u in [t - 2P, t - P/2] of C[u] - tightness x ln((t - u) / P)^2,
// backtracked from the best frame of the last period
std::vector<size_t> trackBeats(const std::vector<double>& o, double period) {
    const size_t n = o.size();
    std::vector<size_t> beats;
    if (n == 0 || period <= 0.0) return beats;
    const long nearest = (long)std::ceil(period / 2.0);
    const long farthest = (long)std::floor(2.0 * period);
    std::vector<double> penalty((size_t)farthest + 1, 0.0);
    for (long d = std::max(1L, nearest); d <= farthest; d++) {
        const double l = std::log((double)d / period);
        penalty[(size_t)d] = SoundTypes::MUSIC_GRID_TIGHTNESS * l * l;
    }
    std::vector<double> score(n);
    std::vector<long> back(n, -1);
    for (size_t t = 0; t < n; t++) {
        double best = 0.0;
        long from = -1;
        for (long d = std::max(1L, nearest); d <= farthest && d <= (long)t; d++) {
            const double v = score[t - (size_t)d] - penalty[(size_t)d];
            if (from < 0 || v > best) {
                best = v;
                from = (long)t - d;
            }
        }
        score[t] = o[t] + (from >= 0 ? best : 0.0);
        back[t] = from;
    }
    const size_t tail = std::min(n, (size_t)std::ceil(period));
    size_t at = n - tail;
    for (size_t t = n - tail; t < n; t++) {
        if (score[t] > score[at]) at = t;
    }
    for (long t = (long)at; t >= 0; t = back[(size_t)t]) beats.push_back((size_t)t);
    std::reverse(beats.begin(), beats.end());
    return beats;
}

// The sample of the largest rise of the 1 ms attack envelope (the RMS of the channel maximum) within
// [low, high]: the 1 ms after a sample against the 1 ms before it, the file zero-padded
size_t largestRise(const AudioAnalysis::Signal& signal, size_t low, size_t high, long half) {
    const long n = (long)signal.frames;
    const long from = std::max(0L, (long)low - 2 * half);
    const long to = std::min(n - 1, (long)high + 2 * half);
    std::vector<double> prefix((size_t)(to - from + 2), 0.0);
    for (long i = from; i <= to; i++) {
        float a = 0.0f;
        for (unsigned int c = 0; c < signal.channels; c++) a = std::max(a, std::fabs(signal.channel(c)[i]));
        prefix[(size_t)(i - from + 1)] = prefix[(size_t)(i - from)] + (double)a * a;
    }
    const double window = (double)(2 * half + 1);
    auto rms = [&](long a, long b) {
        a = std::clamp(a, from, to + 1);
        b = std::clamp(b + 1, from, to + 1);
        return std::sqrt(std::max(0.0, prefix[(size_t)(b - from)] - prefix[(size_t)(a - from)]) / window);
    };
    size_t best = low;
    double bestRise = -1.0;
    for (long i = (long)low; i <= (long)high; i++) {
        const double rise = rms(i, i + 2 * half) - rms(i - 2 * half, i);
        if (rise > bestRise) {
            bestRise = rise;
            best = (size_t)i;
        }
    }
    return best;
}

// The least-squares line s = t0 + k T through (k, s)
bool fitLine(const std::vector<double>& k, const std::vector<double>& s, const std::vector<bool>& use, double& t0, double& period) {
    double n = 0.0, sk = 0.0, ss = 0.0, skk = 0.0, sks = 0.0;
    for (size_t i = 0; i < k.size(); i++) {
        if (!use[i]) continue;
        n += 1.0;
        sk += k[i];
        ss += s[i];
        skk += k[i] * k[i];
        sks += k[i] * s[i];
    }
    const double d = n * skk - sk * sk;
    if (n < 2.0 || d <= 0.0) return false;
    period = (n * sks - sk * ss) / d;
    t0 = (ss - period * sk) / n;
    return period > 0.0;
}

void addNote(AudioMusicGrid::Grid& grid, const char* id, const std::string& text) {
    grid.notes.push_back({{"id", id}, {"text", text}});
}

bool readGridFile(const fs::path& file, std::string& bytes, std::string& error) {
    std::error_code ec;
    const uint64_t size = fs::file_size(file, ec);
    if (ec || !fs::is_regular_file(file, ec)) {
        error = "not a file";
        return false;
    }
    if (size > AudioAnalysis::MAX_FILE_BYTES) {
        error = "larger than " + std::to_string(AudioAnalysis::MAX_FILE_BYTES / (1024 * 1024)) + " MB";
        return false;
    }
    std::ifstream in(file, std::ios::binary);
    bytes.assign((size_t)size, '\0');
    if (!in || (size > 0 && !in.read(&bytes[0], (std::streamsize)size))) {
        error = "can't read the file";
        return false;
    }
    return true;
}

bool readCached(const fs::path& cacheDirectory, const std::string& sha1, AudioMusicGrid::Grid& out) {
    if (cacheDirectory.empty() || sha1.size() != 40) return false;
    std::ifstream in(AudioMusicGrid::cacheFile(cacheDirectory, sha1), std::ios::binary);
    if (!in) return false;
    const Json value = Json::parse(in, nullptr, false);
    AudioMusicGrid::Grid grid;
    if (value.is_discarded() || !AudioMusicGrid::Grid::fromJson(value, grid) || grid.sha1 != sha1) return false;
    out = std::move(grid);
    return true;
}

void writeCached(const fs::path& cacheDirectory, const AudioMusicGrid::Grid& grid) {
    if (cacheDirectory.empty()) return;
    // regenerable, as the analysis cache: no flush to the platters for each file
    io::AtomicWriteOptions options;
    options.durability = io::Durability::Cache;
    io::writeFileAtomic(AudioMusicGrid::cacheFile(cacheDirectory, grid.sha1), grid.toJson().dump(1) + "\n", options);
}

bool gridFromBytes(const std::string& bytes, std::string* owned, const fs::path& cacheDirectory, AudioMusicGrid::Grid& out, std::string& error) {
    const std::string sha1 = SHA1::hash(bytes);
    if (readCached(cacheDirectory, sha1, out)) return true;
    AudioAnalysis::Signal signal;
    if (!(owned ? AudioAnalysis::decodeReleasing(*owned, signal, error) : AudioAnalysis::decode(bytes, signal, error))) return false;
    out = AudioMusicGrid::detect(signal);
    out.sha1 = sha1;
    writeCached(cacheDirectory, out);
    return true;
}

} // namespace

// ======================================================================== detection

AudioMusicGrid::Grid AudioMusicGrid::detect(const AudioAnalysis::Signal& signal) {
    using namespace SoundTypes;
    Grid grid;
    grid.sampleRate = signal.sampleRate;
    grid.frames = signal.frames;
    const double seconds = signal.seconds();
    if (seconds < MUSIC_GRID_MIN_SECONDS || signal.sampleRate == 0) {
        addNote(grid, "too_short", gridText("too short for a beat grid (%.1f s; needs ", seconds) + gridText("%.0f)", MUSIC_GRID_MIN_SECONDS));
        return grid;
    }
    const double rate = signal.sampleRate;
    const double frameRate = rate / HOP;
    const std::vector<float> mono = signal.mono();

    // 1. onset strength
    std::vector<double> onset, lowOnset;
    onsetEnvelopes(mono, rate, onset, lowOnset);
    normalise(onset, frameRate);
    normalise(lowOnset, frameRate);

    // 2. tempo: the autocorrelation under the prior; its value at the tempo is the pulse clarity
    const double shortest = 60.0 * frameRate / MUSIC_GRID_MAX_BPM;
    const double longest = 60.0 * frameRate / MUSIC_GRID_MIN_BPM;
    const std::vector<double> a = autocorrelation(onset, (size_t)std::ceil(longest) + 2);
    const size_t firstLag = (size_t)std::ceil(shortest);
    const size_t lastLag = std::min((size_t)std::floor(longest), a.size() - 2);
    size_t bestLag = firstLag;
    auto weighted = [&](size_t lag) { return a[lag] * priorWeight(60.0 * frameRate / (double)lag); };
    for (size_t lag = firstLag; lag <= lastLag; lag++) {
        if (weighted(lag) > weighted(bestLag)) bestLag = lag;
    }
    double lag = (double)bestLag;
    if (bestLag > firstLag && bestLag < lastLag) {
        const double before = weighted(bestLag - 1), at = weighted(bestLag), after = weighted(bestLag + 1);
        const double curve = before - 2.0 * at + after;
        if (curve < 0.0) lag += std::clamp(0.5 * (before - after) / curve, -0.5, 0.5);
    }
    const double clarity = interpolated(a, lag);
    grid.pulseClarity = roundedTo(std::max(0.0, clarity), 0.01);
    if (clarity < GRID_NO_PULSE) {
        addNote(grid, "no_pulse", gridText("no steady pulse (pulse clarity %.2f)", std::max(0.0, clarity)));
        return grid;
    }

    // 3. beats
    const std::vector<size_t> tracked = trackBeats(onset, lag);

    // 4. onto the attacks, then the line through them
    const double beatSamples = lag * HOP;
    const long half = std::max(1L, std::lround(0.0005 * rate));
    const size_t reach = (size_t)(beatSamples * MUSIC_GRID_SNAP_BEATS);
    std::vector<double> moved;
    for (size_t frame : tracked) {
        const size_t centre = frame * (size_t)AudioAnalysis::FFT_HOP + (size_t)(AudioAnalysis::FFT_SIZE / 2);
        if (centre >= signal.frames + reach) continue;
        const size_t low = centre > reach ? centre - reach : 0;
        const size_t high = std::min(signal.frames - 1, centre + reach);
        if (high < low) continue;
        moved.push_back((double)largestRise(signal, low, high, half));
    }
    std::vector<double> index;
    for (size_t i = 0; i < moved.size(); i++) {
        // a beat the tracker skipped keeps the count
        index.push_back(i == 0 ? 0.0 : index.back() + std::max(1.0, std::round((moved[i] - moved[i - 1]) / beatSamples)));
    }
    std::vector<bool> use(moved.size(), true);
    double t0 = 0.0, period = 0.0;
    if (!fitLine(index, moved, use, t0, period)) {
        addNote(grid, "no_pulse", "no steady pulse (too few beats)");
        return grid;
    }
    for (int pass = 0; pass < 2; pass++) {
        std::vector<bool> near(moved.size());
        size_t kept = 0;
        for (size_t i = 0; i < moved.size(); i++) {
            near[i] = std::fabs(moved[i] - (t0 + index[i] * period)) <= period * MUSIC_GRID_OUTLIER_BEATS;
            kept += near[i] ? 1 : 0;
        }
        double nextT0 = t0, nextPeriod = period;
        if (kept < 2 || !fitLine(index, moved, near, nextT0, nextPeriod)) break;
        t0 = nextT0;
        period = nextPeriod;
    }
    const double bpm = roundedTo(60.0 * rate / period, 0.01);

    // 5. downbeat: the low band's accent at each phase of 3 and 4 beats, on the line's beats
    const long firstBeat = (long)std::ceil((-period * MUSIC_GRID_OUTLIER_BEATS - t0) / period);
    const long lastBeat = (long)std::floor(((double)signal.frames - 1.0 - t0) / period);
    std::vector<long> beatIndex;
    std::vector<double> accent;
    const long accentReach = std::max(1L, std::lround(beatSamples * MUSIC_GRID_SNAP_BEATS / HOP));
    for (long k = firstBeat; k <= lastBeat; k++) {
        const double sample = t0 + (double)k * period;
        const long frame = std::lround((sample - AudioAnalysis::FFT_SIZE / 2) / HOP);
        double strongest = 0.0;
        for (long f = std::max(0L, frame - accentReach); f <= std::min((long)lowOnset.size() - 1, frame + accentReach); f++) {
            strongest = std::max(strongest, lowOnset[(size_t)f]);
        }
        beatIndex.push_back(k);
        accent.push_back(strongest);
    }
    struct Meter {
        int beats = 4;
        int phase = 0;
        double best = 0.0;
        double second = 0.0;
    };
    auto meterOf = [&](int beats) {
        Meter meter;
        meter.beats = beats;
        std::vector<double> scores;
        for (int phase = 0; phase < beats; phase++) {
            double in = 0.0, out = 0.0;
            int inCount = 0, outCount = 0;
            for (size_t i = 0; i < beatIndex.size(); i++) {
                if ((int)(((beatIndex[i] % beats) + beats) % beats) == phase) {
                    in += accent[i];
                    inCount++;
                } else {
                    out += accent[i];
                    outCount++;
                }
            }
            scores.push_back(inCount > 0 && outCount > 0 ? in / inCount - out / outCount : 0.0);
        }
        meter.phase = (int)(std::max_element(scores.begin(), scores.end()) - scores.begin());
        meter.best = scores[(size_t)meter.phase];
        scores.erase(scores.begin() + meter.phase);
        meter.second = *std::max_element(scores.begin(), scores.end());
        return meter;
    };
    const Meter four = meterOf(4);
    const Meter three = meterOf(3);
    const Meter& meter = three.best > 0.0 && three.best > MUSIC_GRID_THREE_MARGIN * std::max(0.0, four.best) ? three : four;
    const double downbeat = meter.best > 0.0 ? std::clamp((meter.best - meter.second) / meter.best, 0.0, 1.0) : 0.0;
    long bar = firstBeat;
    while ((((bar - meter.phase) % meter.beats) + meter.beats) % meter.beats != 0) bar++;
    const double firstBeatMs = roundedTo(std::max(0.0, (t0 + (double)bar * period) / rate * 1000.0), 0.1);

    grid.bpm = bpm;
    grid.firstBeatMs = firstBeatMs;
    grid.beatsPerBarGuess = meter.beats;
    grid.downbeatConfidence = roundedTo(downbeat, 0.01);
    grid.confident = clarity >= GRID_LOW_CONFIDENCE && downbeat >= GRID_LOW_CONFIDENCE;
    grid.candidates.push_back({bpm, firstBeatMs, meter.beats, grid.pulseClarity});

    // the alternative: of 2x, 1/2x, 3/2x and 2/3x the tempo inside the range, the best correlated
    const double tempo = 60.0 * frameRate / lag;
    double altRatio = 0.0, altScore = 0.0;
    for (double ratio : {2.0, 0.5, 1.5, 2.0 / 3.0}) {
        const double alt = tempo * ratio;
        if (alt < MUSIC_GRID_MIN_BPM || alt > MUSIC_GRID_MAX_BPM) continue;
        const double score = interpolated(a, 60.0 * frameRate / alt);
        if (altRatio == 0.0 || score > altScore) {
            altRatio = ratio;
            altScore = score;
        }
    }
    if (altRatio > 0.0) {
        grid.bpmAlt = roundedTo(bpm * altRatio, 0.01);
        grid.candidates.push_back({grid.bpmAlt, firstBeatMs, meter.beats, roundedTo(std::max(0.0, altScore), 0.01)});
    }
    return grid;
}

// ======================================================================== cache

fs::path AudioMusicGrid::cacheFile(const fs::path& cacheDirectory, const std::string& sha1) {
    return cacheDirectory / (sha1 + "-grid-v" + std::to_string(GRID_VERSION) + ".json");
}

bool AudioMusicGrid::get(const fs::path& file, const fs::path& cacheDirectory, Grid& out, std::string& error) {
    std::string bytes;
    if (!readGridFile(file, bytes, error)) return false;
    if (bytes.empty()) {
        error = "the file is empty";
        return false;
    }
    return gridFromBytes(bytes, &bytes, cacheDirectory, out, error);
}

bool AudioMusicGrid::getBytes(const std::string& bytes, const fs::path& cacheDirectory, Grid& out, std::string& error) {
    return gridFromBytes(bytes, nullptr, cacheDirectory, out, error);
}

Json AudioMusicGrid::Grid::toJson() const {
    Json candidateList = Json::array();
    for (const Candidate& c : candidates) {
        candidateList.push_back({{"bpm", c.bpm}, {"first_beat_ms", c.firstBeatMs}, {"beats_per_bar", c.beatsPerBar}, {"score", c.score}});
    }
    const bool tempo = hasTempo();
    return {{"grid_version", GRID_VERSION},
            {"sha1", sha1},
            {"sample_rate", sampleRate},
            {"frames", frames},
            {"bpm", tempo ? Json(bpm) : Json(nullptr)},
            {"bpm_alt", bpmAlt > 0.0 ? Json(bpmAlt) : Json(nullptr)},
            {"first_beat_ms", tempo ? Json(firstBeatMs) : Json(nullptr)},
            {"beats_per_bar_guess", tempo ? Json(beatsPerBarGuess) : Json(nullptr)},
            {"downbeat_confidence", downbeatConfidence},
            {"pulse_clarity", pulseClarity},
            {"confident", confident},
            {"candidates", candidateList},
            {"notes", notes}};
}

bool AudioMusicGrid::Grid::fromJson(const Json& value, Grid& out) {
    if (!value.is_object() || value.value("grid_version", 0) != GRID_VERSION) return false;
    auto number = [&value](const char* key) {
        const auto it = value.find(key);
        return it != value.end() && it->is_number() ? it->get<double>() : 0.0;
    };
    Grid grid;
    grid.sha1 = value.value("sha1", std::string());
    grid.sampleRate = value.value("sample_rate", 0u);
    grid.frames = value.value("frames", (uint64_t)0);
    grid.bpm = number("bpm");
    grid.bpmAlt = number("bpm_alt");
    grid.firstBeatMs = number("first_beat_ms");
    grid.beatsPerBarGuess = value.value("beats_per_bar_guess", Json(4)).is_number_integer() ? value["beats_per_bar_guess"].get<int>() : 4;
    grid.downbeatConfidence = number("downbeat_confidence");
    grid.pulseClarity = number("pulse_clarity");
    grid.confident = value.value("confident", false);
    const Json candidates = value.value("candidates", Json::array());
    for (const Json& c : candidates.is_array() ? candidates : Json::array()) {
        if (!c.is_object()) return false;
        grid.candidates.push_back({c.value("bpm", 0.0), c.value("first_beat_ms", 0.0), c.value("beats_per_bar", 4), c.value("score", 0.0)});
    }
    grid.notes = value.value("notes", Json::array());
    if (!grid.notes.is_array()) return false;
    out = std::move(grid);
    return true;
}

// ======================================================================== the analyze_audio item

AudioImportSettings::Music AudioMusicGrid::musicOf(const Candidate& candidate) {
    AudioImportSettings::Music music;
    music.bpm = candidate.bpm;
    music.beatsPerBar = candidate.beatsPerBar;
    music.firstBeatMs = candidate.firstBeatMs;
    return music;
}

std::string AudioMusicGrid::line(const Grid& grid) {
    if (!grid.hasTempo()) {
        return grid.notes.is_array() && !grid.notes.empty() ? grid.notes[0].value("text", std::string()) : std::string("no tempo");
    }
    std::string text = gridText("tempo %.2f bpm", grid.bpm);
    if (grid.bpmAlt > 0.0) text += gridText(" (alt %.2f)", grid.bpmAlt);
    text += ", " + std::to_string(grid.beatsPerBarGuess) + "/4 guess, first beat " + gridText("%.1f ms", grid.firstBeatMs) +
            gridText(", pulse %.2f", grid.pulseClarity) + gridText(", downbeat %.2f", grid.downbeatConfidence);
    if (!grid.confident) text += ", not confident: check the candidates";
    return text;
}

Json AudioMusicGrid::item(const Grid& grid, const AudioImportSettings* current, double headMs, double tailMs) {
    using namespace SoundTypes;
    Json notes = grid.notes;
    Json candidates = Json::array();
    for (const Candidate& c : grid.candidates) {
        candidates.push_back({{"bpm", c.bpm}, {"first_beat_ms", c.firstBeatMs}, {"beats_per_bar", c.beatsPerBar}, {"score", c.score}});
    }
    const bool tempo = grid.hasTempo();
    Json loopBeats = nullptr;
    if (tempo && grid.sampleRate > 0) {
        // the region a looping voice wraps over: the loop region, else the trimmed clip, else the file
        int64_t from = 0, to = (int64_t)grid.frames;
        ClipImportSettings clip;
        std::string ignored;
        if (current && current->resolve(grid.frames, grid.sampleRate, headMs, tailMs, clip, ignored)) {
            from = clip.loopStartFrame >= 0 ? clip.loopStartFrame : (int64_t)clip.trimHeadFrames;
            to = clip.loopEndFrame >= 0 ? clip.loopEndFrame : (int64_t)grid.frames - (int64_t)clip.trimTailFrames;
        }
        const double beats = (double)std::max<int64_t>(0, to - from) / ((double)grid.sampleRate * 60.0 / grid.bpm);
        loopBeats = roundedTo(beats, 0.01);
        if (std::fabs(beats - std::round(beats)) > MUSIC_GRID_DRIFT_BEATS) {
            notes.push_back({{"id", "beat_drift"},
                             {"text", "loop region is " + gridText("%.2f", beats) + " beats at " + gridText("%.2f", grid.bpm) +
                                          " bpm: stingers will drift; make the loop a whole number of beats"}});
        }
    }
    Json currentJson = nullptr;
    Json agrees = nullptr;
    if (current) {
        const AudioImportSettings::Music& music = current->music;
        if (!music.isDefault()) currentJson = music.toJson();
        bool same = false;
        if (tempo && !music.isDefault()) {
            const double barMs = 60000.0 / grid.bpm * grid.beatsPerBarGuess;
            const double offset = std::fmod(std::fabs(music.firstBeatMs - grid.firstBeatMs), barMs);
            same = std::fabs(music.bpm - grid.bpm) <= MUSIC_GRID_AGREE_BPM + 1e-9 && music.beatsPerBar == grid.beatsPerBarGuess &&
                   std::min(offset, barMs - offset) <= MUSIC_GRID_AGREE_MS + 1e-9;
        }
        agrees = same;
    }
    Json fix = nullptr;
    if (tempo && !grid.candidates.empty()) fix = {{"music", musicOf(grid.candidates[0]).toJson()}};
    return {{"bpm", tempo ? Json(grid.bpm) : Json(nullptr)},
            {"bpm_alt", grid.bpmAlt > 0.0 ? Json(grid.bpmAlt) : Json(nullptr)},
            {"first_beat_ms", tempo ? Json(grid.firstBeatMs) : Json(nullptr)},
            {"beats_per_bar_guess", tempo ? Json(grid.beatsPerBarGuess) : Json(nullptr)},
            {"downbeat_confidence", grid.downbeatConfidence},
            {"pulse_clarity", grid.pulseClarity},
            {"confident", grid.confident},
            {"loop_beats", loopBeats},
            {"candidates", candidates},
            {"current", currentJson},
            {"agrees", agrees},
            {"fix", fix},
            {"notes", notes}};
}
