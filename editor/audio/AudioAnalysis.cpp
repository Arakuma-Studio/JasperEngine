// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AudioAnalysis.h"

#include "AudioImportSettings.h"
#include "AudioProvenance.h"
#include "SoundEssentials.h"
#include "SoundLibrary.h"
#include "command/type/SetAssetMetaCmd.h"
#include "SoundTypes.h"
#include "util/SHA1.h"

#include "audio/AudioFileCheck.h"
#include "io/AtomicFile.h"

#include "soloud.h"
#include "soloud_fft.h"
#include "soloud_wav.h"
// soloud.h defines MAX_CHANNELS as a macro
#undef MAX_CHANNELS

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;
using Json = AudioAnalysis::Json;

namespace {

const double PI = 3.14159265358979323846;
const double NaN = std::numeric_limits<double>::quiet_NaN();
// 10 log10(2): AES17, a full-scale sine reads 0 dBFS
const double AES17_OFFSET = 3.0103;
const double SILENCE_DB = AudioMeter::SILENCE_DB;

// spectral frames quieter than this are left out of the spectral features
const double SPECTRAL_FLOOR_DBFS = -70.0;
// onsets: a flux peak over +/-2 frames above the median of +/-8 frames plus a tenth
// of the largest flux, in a frame with more energy than the one before; refined to the first
// sample at 10% of the largest one within one frame length either side; onsets closer than 50 ms
// keep the first
const int ONSET_PEAK_FRAMES = 2;
const int ONSET_MEDIAN_FRAMES = 8;
const double ONSET_MAX_SHARE = 0.1;
const double ONSET_REFINE_SHARE = 0.1;
const double ONSET_MIN_GAP_SECONDS = 0.050;
// the attack envelope: a 1 ms RMS of the per-sample channel maximum
const double ENVELOPE_SECONDS = 0.001;
// the loop seam: 10 ms either side of the wrap; first differences floored at 2^-15
const double SEAM_SECONDS = 0.010;
const double SEAM_REFERENCE_FLOOR = 1.0 / 32768.0;
// pitch: the loudest 100 ms, lags for 50-2000 Hz; a later peak wins only when the first one
// reaching this share of the best is not there
const double PITCH_SECONDS = 0.100;
const double PITCH_MIN_HZ = 50.0;
const double PITCH_MAX_HZ = 2000.0;
const double PITCH_FIRST_PEAK_SHARE = 0.9;
const double ROLLOFF_SHARE = 0.85;

double dbOfAmplitude(double amplitude) {
    return amplitude > 0.0 ? std::max(SILENCE_DB, 20.0 * std::log10(amplitude)) : SILENCE_DB;
}

double rmsDb(double meanSquare) {
    return dbOfAmplitude(std::sqrt(std::max(0.0, meanSquare)));
}

uint16_t le16(const std::string& b, size_t at) {
    return (uint16_t)((unsigned char)b[at] | ((unsigned char)b[at + 1] << 8));
}

uint32_t le32(const std::string& b, size_t at) {
    return (uint32_t)(unsigned char)b[at] | ((uint32_t)(unsigned char)b[at + 1] << 8) |
           ((uint32_t)(unsigned char)b[at + 2] << 16) | ((uint32_t)(unsigned char)b[at + 3] << 24);
}

// the periodic Hann window, its sum is FFT_SIZE / 2
const std::vector<float>& hannWindow() {
    static const std::vector<float> window = []() {
        std::vector<float> w(AudioAnalysis::FFT_SIZE);
        for (int i = 0; i < AudioAnalysis::FFT_SIZE; i++) {
            w[i] = (float)(0.5 - 0.5 * std::cos(2.0 * PI * i / AudioAnalysis::FFT_SIZE));
        }
        return w;
    }();
    return window;
}

std::string formatText(const char* format, double value) {
    char text[64];
    std::snprintf(text, sizeof(text), format, value);
    return text;
}

double number(const Json& value, double fallback = NaN) {
    return value.is_number() ? value.get<double>() : fallback;
}

double field(const Json& item, const char* group, const char* key, double fallback = NaN) {
    if (!item.contains(group) || !item[group].is_object()) return fallback;
    const Json& g = item[group];
    return g.contains(key) ? number(g[key], fallback) : fallback;
}

// the median of values (a copy, partially sorted)
double median(std::vector<double> values) {
    if (values.empty()) return 0.0;
    const size_t mid = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + (long)mid, values.end());
    double m = values[mid];
    if (values.size() % 2 == 0) {
        m = (m + *std::max_element(values.begin(), values.begin() + (long)mid)) / 2.0;
    }
    return m;
}

// straight into the string at its size (a stream buffer and its copy would hold a long file twice)
bool readFileBytes(const fs::path& file, std::string& bytes, std::string& error) {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) {
        error = "can't open the file";
        return false;
    }
    const std::streamoff size = in.tellg();
    if (size < 0) {
        error = "can't read the file";
        return false;
    }
    bytes.assign((size_t)size, '\0');
    in.seekg(0);
    if (size > 0 && !in.read(&bytes[0], size)) {
        error = "can't read the file";
        return false;
    }
    return true;
}

// SHA-1 by path, size and modification time, so unchanged files are hashed once per session
struct HashMemo {
    uint64_t size = 0;
    fs::file_time_type mtime;
    std::string sha1;
};
std::mutex hashMutex;
std::map<std::string, HashMemo> hashes;

bool knownHash(const fs::path& file, uint64_t size, fs::file_time_type mtime, std::string& sha1) {
    std::lock_guard<std::mutex> lock(hashMutex);
    auto it = hashes.find(file.string());
    if (it == hashes.end() || it->second.size != size || it->second.mtime != mtime) return false;
    sha1 = it->second.sha1;
    return true;
}

void rememberHash(const fs::path& file, uint64_t size, fs::file_time_type mtime, const std::string& sha1) {
    std::lock_guard<std::mutex> lock(hashMutex);
    hashes[file.string()] = HashMemo{size, mtime, sha1};
}

// the largest |sample| of frame i over the channels
float channelMaximum(const AudioAnalysis::Signal& signal, size_t i) {
    float a = 0.0f;
    for (unsigned int c = 0; c < signal.channels; c++) a = std::max(a, std::fabs(signal.channel(c)[i]));
    return a;
}

// A centred RMS envelope frame by frame: the RMS of source.square(k) over [i - half, i + half]
// (clipped to the signal), by a running sum that is summed afresh every ENVELOPE_RESUM frames
// so its rounding cannot drift over a long signal
template <typename Source>
class RunningRms {
public:
    RunningRms(Source source, long frames, long half): source(source), half(half), n(frames) {}

    double next() {
        const long lo = std::max(0L, i - half);
        const long hi = std::min(n - 1, i + half);
        if (i % ENVELOPE_RESUM == 0) {
            sum = 0.0;
            for (long k = lo; k <= hi; k++) sum += square(k);
        } else {
            for (long k = windowHi + 1; k <= hi; k++) sum += square(k);
            for (long k = windowLo; k < lo; k++) sum -= square(k);
        }
        windowLo = lo;
        windowHi = hi;
        i++;
        return std::sqrt(std::max(0.0, sum / (double)(hi - lo + 1)));
    }

private:
    static constexpr long ENVELOPE_RESUM = 4096;
    const Source source;
    const long half;
    const long n;
    long i = 0;
    long windowLo = 0;
    long windowHi = -1;
    double sum = 0.0;

    double square(long k) const { return source.square(k); }
};

// the squares the envelopes are made of: a signal's channel maximum, or one channel (the mean)
struct ChannelMaximumSquares {
    const AudioAnalysis::Signal& signal;
    double square(long k) const {
        const double a = channelMaximum(signal, (size_t)k);
        return a * a;
    }
};

struct SampleSquares {
    const float* samples;
    double square(long k) const { return (double)samples[k] * samples[k]; }
};

// The attack envelope: the RMS of the channel maximum
class Envelope: public RunningRms<ChannelMaximumSquares> {
public:
    Envelope(const AudioAnalysis::Signal& signal, long half): RunningRms({signal}, (long)signal.frames, half) {}
};

void stripFullDetail(Json& item) {
    item.erase("onsets_ms");
    item.erase("momentary");
    item.erase("short_term");
}

} // namespace

// ======================================================================== signal

std::vector<float> AudioAnalysis::Signal::mono() const {
    std::vector<float> out(frames, 0.0f);
    if (channels == 0) return out;
    for (unsigned int c = 0; c < channels; c++) {
        const float* x = channel(c);
        for (size_t i = 0; i < frames; i++) out[i] += x[i];
    }
    const float scale = 1.0f / (float)channels;
    for (float& v : out) v *= scale;
    return out;
}

Json AudioAnalysis::rounded(double value, double step) {
    if (!std::isfinite(value)) return nullptr;
    double r;
    if (step < 1.0) {
        const double inverse = std::round(1.0 / step);
        r = std::round(value * inverse) / inverse;
    } else {
        r = std::round(value / step) * step;
    }
    return r + 0.0;  // no -0
}

// ======================================================================== probe and decode

void AudioAnalysis::bandPass(std::vector<float>& buffer, size_t size, double rate, double low, double high) {
    SoLoud::FFT::fft(buffer.data(), (unsigned int)(2 * size));
    for (size_t k = 0; k < size; k++) {
        const double hz = (double)(k <= size / 2 ? k : size - k) * rate / (double)size;
        if (hz < low || hz >= high) buffer[2 * k] = buffer[2 * k + 1] = 0.0f;
    }
    SoLoud::FFT::ifft(buffer.data(), (unsigned int)(2 * size));
}

bool AudioAnalysis::probe(const std::string& bytes, Format& format, std::string& error) {
    format = Format();
    const size_t n = bytes.size();
    // RF64: the 64-bit WAV, read as a RIFF one once AudioFileCheck::clampWavSizes has rewritten it
    if (n >= 12 && (bytes.compare(0, 4, "RIFF") == 0 || bytes.compare(0, 4, "RF64") == 0) && bytes.compare(8, 4, "WAVE") == 0) {
        format.codec = "wav";
        size_t pos = 12;
        while (pos + 8 <= n) {
            const uint32_t size = le32(bytes, pos + 4);
            if (bytes.compare(pos, 4, "fmt ") == 0) {
                if (size < 16 || pos + 8 + 16 > n) break;
                uint16_t tag = le16(bytes, pos + 8);
                const uint16_t bits = le16(bytes, pos + 8 + 14);
                if (tag == 0xFFFE && size >= 26 && pos + 8 + 26 <= n) tag = le16(bytes, pos + 8 + 24);
                if (tag == 1) format.encoding = "pcm" + std::to_string(bits);
                else if (tag == 3) format.encoding = "float" + std::to_string(bits);
                else if (tag == 2) format.encoding = "adpcm";
                else if (tag == 0x11) format.encoding = "ima_adpcm";
                else if (tag == 6) format.encoding = "alaw";
                else if (tag == 7) format.encoding = "mulaw";
                else format.encoding = "format_" + std::to_string(tag);
                return true;
            }
            pos += 8 + (size_t)size + (size & 1);
        }
        error = "WAV file without a fmt chunk";
        return false;
    }
    if (n >= 4 && bytes.compare(0, 4, "OggS") == 0) {
        // the only Ogg codec SoLoud decodes: AudioFileCheck::check, before the decode, refuses any
        // other and names it ("this Ogg file holds Opus, not Vorbis: ..."), as for every loader
        format.codec = "ogg";
        format.encoding = "vorbis";
        return true;
    }
    if (n >= 4 && bytes.compare(0, 4, "fLaC") == 0) {
        format.codec = "flac";
        format.encoding = "flac";
        return true;
    }
    if ((n >= 3 && bytes.compare(0, 3, "ID3") == 0) ||
        (n >= 2 && (unsigned char)bytes[0] == 0xFF && ((unsigned char)bytes[1] & 0xE0) == 0xE0)) {
        format.codec = "mp3";
        format.encoding = "mp3";
        return true;
    }
    error = "not a WAV, OGG, MP3 or FLAC file";
    return false;
}

namespace {

// NaN and infinite samples to 0 (they would poison every filter and sum); the count
uint64_t zeroNonFinite(float* samples, size_t count) {
    uint64_t found = 0;
    for (size_t i = 0; i < count; i++) {
        if (!std::isfinite(samples[i])) {
            samples[i] = 0.0f;
            found++;
        }
    }
    return found;
}

std::string decodedLimitError(uint64_t frames, unsigned int channels, unsigned int rate) {
    const double minutes = rate > 0 ? (double)frames / rate / 60.0 : 0.0;
    char text[256];
    std::snprintf(text, sizeof(text),
                  "decodes to %.1f min of audio (%u channel(s) at %u Hz): the analysis takes at most %.0f min of 48 kHz "
                  "stereo (%llu samples); trim or split it (a Streaming clip plays any length)",
                  minutes, channels, rate, AudioAnalysis::MAX_DECODED_MINUTES, (unsigned long long)AudioAnalysis::MAX_DECODED_SAMPLES);
    return text;
}

// decode and decodeReleasing: `owned`, when given, is the caller's `bytes`, freed once decoded
bool decodeBytes(const std::string& bytes, std::string* owned, AudioAnalysis::Signal& signal, std::string& error) {
    using Signal = AudioAnalysis::Signal;
    signal = Signal();
    AudioAnalysis::Format format;
    if (!AudioAnalysis::probe(bytes, format, error)) return false;
    if (bytes.size() > AudioAnalysis::MAX_FILE_BYTES) {
        error = "larger than " + std::to_string(AudioAnalysis::MAX_FILE_BYTES / (1024 * 1024)) + " MB";
        return false;
    }
    // a WAV's sizes as the bytes it holds (a pipe's 0xFFFFFFFF would decode as hours): in place
    // in the caller's bytes when they are ours to change, else in a copy, made only when needed
    std::string clamped;
    const std::string* input = &bytes;
    if (owned) {
        AudioFileCheck::clampWavSizes(reinterpret_cast<unsigned char*>(&(*owned)[0]), owned->size());
    } else if (AudioFileCheck::wavSizesNeedClamping(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size())) {
        clamped = bytes;
        AudioFileCheck::clampWavSizes(reinterpret_cast<unsigned char*>(&clamped[0]), clamped.size());
        input = &clamped;
    }
    const unsigned char* data = reinterpret_cast<const unsigned char*>(input->data());
    const unsigned int size = (unsigned int)input->size();
    // what SoLoud can decode safely, and the analysis's own limits, from the headers before decoding
    AudioFileFormat checked;
    if (!AudioFileCheck::check(data, input->size(), AudioDecode::Whole, checked, error, AudioAnalysis::MAX_DECODED_SAMPLES)) {
        if (checked.tooLong) error = decodedLimitError(checked.frames, checked.channels, checked.sampleRate);
        return false;
    }
    if (checked.sampleRate < AudioMeter::SAMPLE_RATE_MIN || checked.sampleRate > AudioMeter::SAMPLE_RATE_MAX) {
        error = "sample rate " + std::to_string(checked.sampleRate) + " Hz is outside 8000-192000 Hz";
        return false;
    }
    SoLoud::Wav wav;
    const SoLoud::result result = wav.loadMem(data, size, false, false);
    if (result != SoLoud::SO_NO_ERROR || !wav.mData || wav.mSampleCount == 0) {
        error = "can't decode this " + format.codec + " file";
        return false;
    }
    // the check read the same headers the decoder did: rate, channels and length are as checked
    const unsigned int rate = (unsigned int)std::lround(wav.mBaseSamplerate);
    // SoLoud holds its own copy now
    if (owned) std::string().swap(*owned);
    std::string().swap(clamped);
    signal.channels = wav.mChannels;
    signal.sampleRate = rate;
    signal.frames = wav.mSampleCount;
    signal.samples.assign(wav.mData, wav.mData + (size_t)wav.mSampleCount * wav.mChannels);
    signal.nonFinite = zeroNonFinite(signal.samples.data(), signal.samples.size());
    return true;
}

} // namespace

bool AudioAnalysis::decode(const std::string& bytes, Signal& signal, std::string& error) {
    return decodeBytes(bytes, nullptr, signal, error);
}

bool AudioAnalysis::decodeReleasing(std::string& bytes, Signal& signal, std::string& error) {
    return decodeBytes(bytes, &bytes, signal, error);
}

// ======================================================================== spectra

size_t AudioAnalysis::spectralFrames(size_t samples) {
    if (samples == 0) return 0;
    if (samples <= (size_t)FFT_SIZE) return 1;
    return 1 + (samples - FFT_SIZE + FFT_HOP - 1) / FFT_HOP;
}

void AudioAnalysis::magnitudes(const float* mono, size_t samples, size_t index, float* out) {
    // SoLoud::FFT::fft takes FFT_SIZE complex values interleaved (re, im); bin k is at 2k, 2k+1
    std::vector<float> buffer(2 * FFT_SIZE, 0.0f);
    const std::vector<float>& window = hannWindow();
    const size_t start = index * FFT_HOP;
    for (int j = 0; j < FFT_SIZE; j++) {
        const size_t at = start + (size_t)j;
        buffer[2 * j] = at < samples ? mono[at] * window[j] : 0.0f;
    }
    SoLoud::FFT::fft(buffer.data(), 2 * FFT_SIZE);
    for (int k = 0; k <= FFT_SIZE / 2; k++) {
        out[k] = std::hypot(buffer[2 * k], buffer[2 * k + 1]);
    }
}

// ======================================================================== measure

namespace {

// The choice of pitch from the lag sums of a window of `length` samples
// (sums[lag] = sum of x[i] x[i + lag], for lags minLag - 1 .. maxLag + 1) and the prefix sums of its
// squares: normalised autocorrelation, the first peak reaching PITCH_FIRST_PEAK_SHARE of the best,
// refined by a parabola. False when no lag peaks.
bool pitchOfSums(const std::vector<double>& sums, const std::vector<double>& prefix, size_t length, long minLag, long maxLag,
                 double rate, double& hz, double& confidence) {
    std::vector<double> corr((size_t)maxLag + 2, 0.0);
    for (long lag = minLag - 1; lag <= maxLag + 1; lag++) {
        if (lag < 1 || lag >= (long)length) continue;
        const size_t count = length - (size_t)lag;
        const double e0 = prefix[count];
        const double e1 = prefix[length] - prefix[(size_t)lag];
        corr[(size_t)lag] = (e0 > 0.0 && e1 > 0.0) ? sums[(size_t)lag] / std::sqrt(e0 * e1) : 0.0;
    }
    std::vector<long> peaks;
    double best = 0.0;
    for (long lag = minLag; lag <= maxLag; lag++) {
        const double v = corr[(size_t)lag];
        if (v > 0.0 && v > corr[(size_t)lag - 1] && v >= corr[(size_t)lag + 1]) {
            peaks.push_back(lag);
            best = std::max(best, v);
        }
    }
    long chosen = -1;
    for (long lag : peaks) {
        if (corr[(size_t)lag] >= PITCH_FIRST_PEAK_SHARE * best) {
            chosen = lag;
            break;
        }
    }
    if (chosen <= 0) return false;
    const double a = corr[(size_t)chosen - 1], b = corr[(size_t)chosen], c = corr[(size_t)chosen + 1];
    const double denominator = a - 2.0 * b + c;
    const double shift = denominator != 0.0 ? std::clamp(0.5 * (a - c) / denominator, -0.5, 0.5) : 0.0;
    hz = rate / ((double)chosen + shift);
    confidence = std::clamp(b, 0.0, 1.0);
    return true;
}

// The lags of the pitch for a window of `length` samples (50-2000 Hz); false when the
// window is too short for them
bool pitchLags(size_t length, double rate, long& minLag, long& maxLag) {
    minLag = std::max(1L, (long)std::floor(rate / PITCH_MAX_HZ));
    maxLag = std::min((long)std::ceil(rate / PITCH_MIN_HZ), (long)length / 2);
    return maxLag > minLag + 1;
}

std::vector<double> squarePrefix(const float* x, size_t length) {
    std::vector<double> prefix(length + 1, 0.0);
    for (size_t i = 0; i < length; i++) prefix[i + 1] = prefix[i] + (double)x[i] * x[i];
    return prefix;
}

// the pitch of a window, its lag sums added directly. False when no lag peaks
// (a silent window, or one too short for the lags).
bool pitchOfWindow(const float* x, size_t length, double rate, double& hz, double& confidence) {
    hz = 0.0;
    confidence = 0.0;
    long minLag = 0, maxLag = 0;
    if (!pitchLags(length, rate, minLag, maxLag)) return false;
    const std::vector<double> prefix = squarePrefix(x, length);
    std::vector<double> sums((size_t)maxLag + 2, 0.0);
    for (long lag = minLag - 1; lag <= maxLag + 1; lag++) {
        if (lag < 1 || lag >= (long)length) continue;
        const size_t count = length - (size_t)lag;
        double sum = 0.0;
        for (size_t i = 0; i < count; i++) sum += (double)x[i] * x[i + (size_t)lag];
        sums[(size_t)lag] = sum;
    }
    return pitchOfSums(sums, prefix, length, minLag, maxLag, rate, hz, confidence);
}

// The same pitch with the lag sums from one FFT autocorrelation (the window zero-padded to twice
// a power of two, so no lag wraps): the same correlations to float rounding, in O(n log n) instead
// of O(n x lags). For the aliasing frames, which judge hundreds of windows.
bool pitchOfWindowFft(const float* x, size_t length, double rate, double& hz, double& confidence) {
    hz = 0.0;
    confidence = 0.0;
    long minLag = 0, maxLag = 0;
    if (!pitchLags(length, rate, minLag, maxLag)) return false;
    size_t size = 1;
    while (size < 2 * length) size *= 2;
    // SoLoud::FFT takes complex values interleaved (re, im)
    std::vector<float> buffer(2 * size, 0.0f);
    for (size_t i = 0; i < length; i++) buffer[2 * i] = x[i];
    SoLoud::FFT::fft(buffer.data(), (unsigned int)(2 * size));
    for (size_t k = 0; k < size; k++) {
        const float re = buffer[2 * k], im = buffer[2 * k + 1];
        buffer[2 * k] = re * re + im * im;
        buffer[2 * k + 1] = 0.0f;
    }
    SoLoud::FFT::ifft(buffer.data(), (unsigned int)(2 * size));
    std::vector<double> sums((size_t)maxLag + 2, 0.0);
    for (long lag = std::max(1L, minLag - 1); lag <= maxLag + 1 && lag < (long)length; lag++) sums[(size_t)lag] = buffer[2 * (size_t)lag];
    return pitchOfSums(sums, squarePrefix(x, length), length, minLag, maxLag, rate, hz, confidence);
}

// The pitch of the PITCH_SECONDS of the mean centred on the attack envelope's maximum (clamped to
// the signal)
bool pitchOfLoudest(const float* mono, size_t n, double rate, size_t envelopeAt, double& hz, double& confidence) {
    const size_t length = std::min(n, (size_t)std::lround(PITCH_SECONDS * rate));
    const long start = std::clamp((long)envelopeAt - (long)length / 2, 0L, (long)(n - length));
    return pitchOfWindow(mono + start, length, rate, hz, confidence);
}

} // namespace


Json AudioAnalysis::loopSeam(const Signal& signal, size_t start, size_t end) {
    const double rate = (double)signal.sampleRate;
    const size_t n = end > start ? end - start : 0;
    double seamJump = 0.0;
    double stepDb = SILENCE_DB;
    double levelDb = 0.0;
    if (n >= 2) {
        const size_t w = (size_t)std::lround(SEAM_SECONDS * rate);
        const bool whole = n < 2 * w;
        for (unsigned int c = 0; c < signal.channels; c++) {
            const float* x = signal.channel(c) + start;
            const double jump = std::fabs((double)x[0] - x[n - 1]);
            seamJump = std::max(seamJump, jump);
            double square = 0.0;
            size_t count = 0;
            auto addSteps = [&](size_t from, size_t to) {  // differences x[i+1]-x[i], i in [from, to - 1)
                for (size_t i = from; i + 1 < to; i++) {
                    const double d = (double)x[i + 1] - x[i];
                    square += d * d;
                    count++;
                }
            };
            if (whole) {
                addSteps(0, n);
            } else {
                addSteps(n - w, n);
                addSteps(0, w);
            }
            const double reference = std::max(SEAM_REFERENCE_FLOOR, count > 0 ? std::sqrt(square / (double)count) : 0.0);
            if (jump > 0.0) stepDb = std::max(stepDb, 20.0 * std::log10(jump / reference));
        }
        const size_t edge = std::max<size_t>(1, std::min(n, w));
        double head = 0.0, tail = 0.0;
        for (unsigned int c = 0; c < signal.channels; c++) {
            const float* x = signal.channel(c) + start;
            for (size_t i = 0; i < edge; i++) {
                head += (double)x[i] * x[i];
                tail += (double)x[n - edge + i] * x[n - edge + i];
            }
        }
        const double samples = (double)edge * signal.channels;
        levelDb = rmsDb(tail / samples) - rmsDb(head / samples);
    }
    return {
        {"seam_jump", rounded(seamJump, 0.0001)},
        {"seam_step_db", rounded(std::max(SILENCE_DB, stepDb), 0.01)},
        {"seam_level_db", rounded(levelDb, 0.01)}
    };
}

Json AudioAnalysis::measure(const Signal& signal, AudioMeterMode mode) {
    return measure(signal, mode, nullptr);
}

Json AudioAnalysis::measure(const Signal& signal, AudioMeterMode mode, std::vector<double>* spectrum) {
    const size_t n = signal.frames;
    const unsigned int channels = signal.channels;
    const double rate = (double)signal.sampleRate;
    const double duration = signal.seconds();
    // -- AudioMeter: loudness, peaks, levels, bands
    AudioMeterOptions options;
    options.mode = mode;
    options.bands = true;
    AudioMeter meter(signal.sampleRate, channels, options);
    std::vector<const float*> planes;
    for (unsigned int c = 0; c < channels; c++) planes.push_back(signal.channel(c));
    meter.processPlanar(planes.data(), n);
    meter.finish();
    const LoudnessResult r = meter.result();

    Json item;
    item["non_finite"] = signal.nonFinite + r.nonFinite;
    item["loudness"] = {
        {"integrated_lufs", rounded(r.integrated.value_or(NaN), 0.01)},
        {"short_term_max", rounded(r.shortTermMax.value_or(NaN), 0.01)},
        {"momentary_max", rounded(r.momentaryMax.value_or(NaN), 0.01)},
        {"lra", rounded(r.lra.value_or(NaN), 0.01)}
    };
    item["peak"] = {
        {"sample_dbfs", rounded(r.samplePeakDb, 0.01)},
        {"true_peak_dbtp", rounded(r.truePeakDb, 0.01)},
        {"clipped", r.clipped}
    };
    item["rms_dbfs"] = rounded(r.rmsDb, 0.01);
    item["dc_offset"] = rounded(r.dcOffset, 0.0001);

    Json bandsDb = Json::object();
    Json bandShare = Json::object();
    double bandTotal = 0.0;
    for (int b = 0; b < AudioMeter::BANDS; b++) bandTotal += r.bandsMeanSquare[b];
    for (int b = 0; b < AudioMeter::BANDS; b++) {
        bandsDb[AudioMeter::bandLabel(b)] = rounded(r.bandsDb[b], 0.01);
        bandShare[AudioMeter::bandLabel(b)] = rounded(bandTotal > 0.0 ? r.bandsMeanSquare[b] / bandTotal : 0.0, 0.001);
    }
    item["bands_db"] = bandsDb;
    item["band_share"] = bandShare;
    item["sub_share"] = rounded(r.subShare, 0.001);

    Json momentary = Json::array();
    for (float v : meter.momentaryCurve()) momentary.push_back(rounded(v, 0.01));
    Json shortTerm = Json::array();
    for (float v : meter.shortTermCurve()) shortTerm.push_back(rounded(v, 0.01));
    item["momentary"] = momentary;
    item["short_term"] = shortTerm;
    // -- spectral features and flux on the mono downmix
    const std::vector<float> mono = signal.mono();
    const size_t frames = spectralFrames(n);
    const int bins = FFT_SIZE / 2;
    const double binHz = rate / FFT_SIZE;
    std::vector<float> magnitude(bins + 1), previous(bins + 1, 0.0f);
    std::vector<double> flux(frames, 0.0), framePower(frames, 0.0);
    std::vector<double> binEnergy(bins + 1, 0.0);
    double centroidSum = 0.0, flatnessSum = 0.0, weightSum = 0.0;
    if (spectrum) spectrum->assign(bins + 1, 0.0);
    for (size_t f = 0; f < frames; f++) {
        magnitudes(mono.data(), n, f, magnitude.data());
        if (spectrum) {
            for (int k = 0; k <= bins; k++) (*spectrum)[k] += (double)(magnitude[k] * magnitude[k]);
        }
        double positive = 0.0, power = 0.0;
        for (int k = 0; k <= bins; k++) {
            positive += std::max(0.0f, magnitude[k] - previous[k]);
            power += (double)magnitude[k] * magnitude[k];
        }
        flux[f] = positive;
        framePower[f] = power;
        previous = magnitude;

        const size_t start = f * FFT_HOP;
        const size_t end = std::min(n, start + FFT_SIZE);
        double square = 0.0;
        for (size_t i = start; i < end; i++) square += (double)mono[i] * mono[i];
        const double frameRms = end > start ? std::sqrt(square / (double)(end - start)) : 0.0;
        if (frameRms <= 0.0 || 20.0 * std::log10(frameRms) + AES17_OFFSET < SPECTRAL_FLOOR_DBFS) continue;

        double energy = 0.0, magSum = 0.0, weighted = 0.0, logSum = 0.0;
        for (int k = 1; k <= bins; k++) {
            const double m = magnitude[k];
            const double p = m * m;
            energy += p;
            magSum += m;
            weighted += k * binHz * m;
            logSum += std::log(p + 1e-30);
            binEnergy[k] += p;
        }
        if (energy <= 0.0 || magSum <= 0.0) continue;
        const double arithmetic = energy / bins + 1e-30;
        const double geometric = std::exp(logSum / bins);
        centroidSum += energy * (weighted / magSum);
        flatnessSum += energy * std::min(1.0, geometric / arithmetic);
        weightSum += energy;
    }
    if (weightSum > 0.0) {
        item["centroid_hz"] = rounded(centroidSum / weightSum, 1.0);
        item["flatness"] = rounded(flatnessSum / weightSum, 0.001);
        double total = 0.0;
        for (int k = 1; k <= bins; k++) total += binEnergy[k];
        double cumulative = 0.0;
        int rolloff = bins;
        for (int k = 1; k <= bins; k++) {
            cumulative += binEnergy[k];
            if (cumulative >= ROLLOFF_SHARE * total) {
                rolloff = k;
                break;
            }
        }
        item["rolloff_hz"] = rounded(rolloff * binHz, 1.0);
    } else {
        item["centroid_hz"] = nullptr;
        item["flatness"] = nullptr;
        item["rolloff_hz"] = nullptr;
    }
    // -- onsets: flux peaks, refined in the time domain
    std::vector<double> onsets;
    const double maxFlux = frames > 0 ? *std::max_element(flux.begin(), flux.end()) : 0.0;
    if (maxFlux > 0.0) {
        for (size_t f = 0; f < frames; f++) {
            // an onset adds energy: a sound that stops or falls (the zero-padded last frame of a
            // tone, a step down) spreads its spectrum and raises the flux without one
            if (flux[f] <= 0.0 || framePower[f] <= (f > 0 ? framePower[f - 1] : 0.0)) continue;
            bool peak = true;
            for (int d = -ONSET_PEAK_FRAMES; d <= ONSET_PEAK_FRAMES && peak; d++) {
                const long j = (long)f + d;
                if (d != 0 && j >= 0 && j < (long)frames && flux[(size_t)j] > flux[f]) peak = false;
            }
            if (!peak) continue;
            const size_t lo = f >= (size_t)ONSET_MEDIAN_FRAMES ? f - ONSET_MEDIAN_FRAMES : 0;
            const size_t hi = std::min(frames - 1, f + ONSET_MEDIAN_FRAMES);
            std::vector<double> around(flux.begin() + (long)lo, flux.begin() + (long)hi + 1);
            if (flux[f] <= median(around) + ONSET_MAX_SHARE * maxFlux) continue;

            const long frameStart = (long)(f * FFT_HOP);
            const size_t from = (size_t)std::max(0L, frameStart - FFT_SIZE);
            const size_t to = std::min(n, (size_t)(frameStart + FFT_SIZE));
            double largest = 0.0;
            for (size_t i = from; i < to; i++) largest = std::max(largest, (double)std::fabs(mono[i]));
            size_t at = std::min((size_t)frameStart, n > 0 ? n - 1 : 0);
            if (largest > 0.0) {
                for (size_t i = from; i < to; i++) {
                    if (std::fabs(mono[i]) >= ONSET_REFINE_SHARE * largest) {
                        at = i;
                        break;
                    }
                }
            }
            onsets.push_back((double)at / rate);
        }
        std::sort(onsets.begin(), onsets.end());
        std::vector<double> kept;
        for (double t : onsets) {
            if (kept.empty() || t - kept.back() >= ONSET_MIN_GAP_SECONDS) kept.push_back(t);
        }
        onsets = kept;
    }
    // -- per sample: peak position, envelope (no per-sample arrays: a long signal costs only its
    // own samples and the mono downmix)
    size_t peakAt = 0;
    double peakValue = 0.0;
    for (size_t i = 0; i < n; i++) {
        const double a = channelMaximum(signal, i);
        if (a > peakValue) {
            peakValue = a;
            peakAt = i;
        }
    }

    // 1 ms RMS, centred: the maximum, then the 10% and 90% crossings before it
    const long half = std::max(0L, (long)std::lround(ENVELOPE_SECONDS * rate) / 2);
    size_t envelopeAt = 0;
    double envelopeMax = 0.0;
    {
        Envelope envelope(signal, half);
        for (size_t i = 0; i < n; i++) {
            const double e = envelope.next();
            if (e > envelopeMax) {
                envelopeMax = e;
                envelopeAt = i;
            }
        }
    }
    double attackMs = NaN;
    if (envelopeMax > 0.0) {
        long low = -1, high = -1;
        Envelope envelope(signal, half);
        for (size_t i = 0; i < n; i++) {
            const double e = envelope.next();
            if (low < 0 && e >= 0.1 * envelopeMax) low = (long)i;
            if (e >= 0.9 * envelopeMax) {
                high = (long)i;
                break;
            }
        }
        if (low >= 0 && high >= low) attackMs = (double)(high - low) / rate * 1000.0;
    }

    // the peak of the 20 ms centred RMS envelope of the mean
    Json envelopePeakMs = nullptr;
    {
        const long wide = std::max(0L, (long)std::lround(SoundTypes::ENVELOPE_PEAK_MS / 1000.0 * rate) / 2);
        RunningRms<SampleSquares> envelope({mono.data()}, (long)n, wide);
        size_t at = 0;
        double largest = 0.0;
        for (size_t i = 0; i < n; i++) {
            const double e = envelope.next();
            if (e > largest) {
                largest = e;
                at = i;
            }
        }
        if (largest > 0.0) envelopePeakMs = rounded((double)at / rate * 1000.0, 0.1);
    }

    Json onsetsMs = Json::array();
    for (double t : onsets) onsetsMs.push_back(rounded(t * 1000.0, 0.1));
    item["onsets_ms"] = onsetsMs;
    item["onset"] = {
        {"count", onsets.size()},
        {"first_ms", onsets.empty() ? Json(nullptr) : rounded(onsets.front() * 1000.0, 0.1)},
        {"peak_ms", peakValue > 0.0 ? rounded((double)peakAt / rate * 1000.0, 0.1) : Json(nullptr)},
        {"attack_ms", rounded(attackMs, 0.1)},
        {"envelope_peak_ms", envelopePeakMs}
    };
    item["silence"] = silence(signal);
    // -- stereo: Pearson correlation of channels 0 and 1
    Json correlation = nullptr;
    if (channels >= 2 && n > 1) {
        const float* a = signal.channel(0);
        const float* b = signal.channel(1);
        double ma = 0.0, mb = 0.0;
        for (size_t i = 0; i < n; i++) {
            ma += a[i];
            mb += b[i];
        }
        ma /= (double)n;
        mb /= (double)n;
        double sab = 0.0, saa = 0.0, sbb = 0.0;
        for (size_t i = 0; i < n; i++) {
            const double da = a[i] - ma, db = b[i] - mb;
            sab += da * db;
            saa += da * da;
            sbb += db * db;
        }
        if (saa > 0.0 && sbb > 0.0) correlation = rounded(sab / std::sqrt(saa * sbb), 0.001);
    }
    item["stereo"] = {{"correlation", correlation}};
    // -- loop seam: the wrap against the signal's own steps next to it
    item["loop"] = loopSeam(signal, 0, n);
    // -- pitch: normalised autocorrelation of the loudest 100 ms
    {
        Json hz = nullptr;
        double frequency = 0.0, confidence = 0.0;
        if (envelopeMax > 0.0 && pitchOfLoudest(mono.data(), n, rate, envelopeAt, frequency, confidence)) {
            hz = rounded(frequency, 1.0);
        }
        item["pitch"] = {{"hz", hz}, {"confidence", rounded(confidence, 0.001)}};
    }

    item["measured_kind"] = measuredKind(item, duration);
    return item;
}

std::string AudioAnalysis::measuredKind(const Json& m, double duration) {
    const double integrated = field(m, "loudness", "integrated_lufs");
    const double lra = field(m, "loudness", "lra");
    if (duration >= SoundTypes::LOOP_MIN_SECONDS && std::isfinite(integrated) && std::isfinite(lra) &&
        lra <= SoundTypes::LOOP_RANGE_MAX_LU) {
        return SoundTypes::LOOP;
    }
    const double peakMs = field(m, "onset", "peak_ms");
    if (duration >= SoundTypes::RISER_MIN_SECONDS && std::isfinite(peakMs) &&
        peakMs >= SoundTypes::RISER_PEAK_SHARE * duration * 1000.0) {
        return SoundTypes::RISER;
    }
    const double onsets = field(m, "onset", "count", 0.0);
    const double flatness = m.contains("flatness") ? number(m["flatness"]) : NaN;
    if (onsets >= SoundTypes::GLITCH_MIN_ONSETS && duration <= SoundTypes::GLITCH_MAX_SECONDS &&
        std::isfinite(flatness) && flatness >= SoundTypes::GLITCH_MIN_FLATNESS) {
        return SoundTypes::GLITCH;
    }
    const double attack = field(m, "onset", "attack_ms");
    if (std::isfinite(attack) && attack > SoundTypes::SWELL_ATTACK_MS) {
        return SoundTypes::SWELL;
    }
    if (field(m, "pitch", "confidence", 0.0) >= SoundTypes::TONAL_MIN_CONFIDENCE) {
        return SoundTypes::TONAL;
    }
    return SoundTypes::TRANSIENT;
}

// ======================================================================== defects

// Idea from earshot (MIT), reimplemented: measuring a clip's edges, clicks inside, cut tails,
// digital-silence gaps, narrow resonances and aliasing. Every threshold and window is SoundTypes.h's `-- defects` block; the numbers written here
// are output steps and unit conversions.

namespace {

const double MS_PER_SECOND = 1000.0;
const double DB_STEP = 0.1;
const double TIME_STEP_SECONDS = 0.001;
const double SHARE_STEP = 0.001;
const double HZ_STEP = 1.0;
const double GAIN_STEP = 0.01;
const double PERCENT = 100.0;
// a fade a note proposes, in ms: what the import settings resolve to the same frames
const double FADE_MS_STEP = 0.001;
// the 4-term Blackman-Harris window, sum of a_t cos(2 pi t j / N) (sidelobes under -92 dB)
const int BLACKMAN_HARRIS_TERMS = 4;
const double BLACKMAN_HARRIS[BLACKMAN_HARRIS_TERMS] = {0.35875, -0.48829, 0.14128, -0.01168};

// round(ms x rate / 1000) frames
long framesOfMs(double ms, double rate) {
    return std::lround(ms * rate / MS_PER_SECOND);
}

// 10 log10 of a power, 20 log10 of an amplitude, floored at SILENCE_DB
double powerDb(double power) {
    return rmsDb(power);
}

double amplitudeDb(double amplitude) {
    return dbOfAmplitude(amplitude);
}

double amplitudeOfDb(double db) {
    return std::pow(10.0, db / 20.0);
}

// a clip's gain as AudioClipSource computes it (in float)
float clipGain(float gainDb) {
    return std::pow(10.0f, gainDb / 20.0f);
}

// the linear-interpolation percentile (numpy's default) of values, q in [0, 1]
double percentile(std::vector<double> values, double q) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const double position = q * (double)(values.size() - 1);
    const size_t below = (size_t)std::floor(position);
    const size_t above = std::min(values.size() - 1, below + 1);
    return values[below] + (values[above] - values[below]) * (position - (double)below);
}

int signOf(double v) {
    return (v > 0.0) - (v < 0.0);
}

// "3,020" (Hz), "48,000" (frames)
std::string grouped(double hz) {
    const std::string digits = std::to_string(std::lround(hz));
    std::string out;
    for (size_t i = 0; i < digits.size(); i++) {
        if (i > 0 && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return out;
}

// "2" or "0.5"
std::string msText(double ms) {
    return formatText("%g", ms);
}

// The played signal of a region: the file's frames in the region, times the gain when
// it is not 0 dB, times the fade weights, sample by sample as AudioClipSourceInstance::getAudio makes
// them (the same float gain and the weight tables of clipFadeWeight, multiplied in the same order),
// so the analysis measures exactly what plays. Nothing is copied but the tables.
class Played {
public:
    Played(const AudioAnalysis::Signal& file, const AudioAnalysis::Region& region): file(file) {
        start = std::min<size_t>(region.startFrame, file.frames);
        frames = std::max(start, std::min<size_t>(region.end(file.frames), file.frames)) - start;
        channels = file.channels;
        rate = (double)file.sampleRate;
        gained = region.gainDb != 0.0f;
        gain = gained ? clipGain(region.gainDb) : 1.0f;
        const uint32_t nIn = region.fadeIn(file.frames), nOut = region.fadeOut(file.frames);
        for (uint32_t k = 0; k < nIn; k++) fadeIn.push_back(clipFadeWeight(k, nIn, true));
        for (uint32_t j = 0; j < nOut; j++) fadeOut.push_back(clipFadeWeight(j, nOut, false));
    }

    size_t frames = 0;
    unsigned int channels = 0;
    double rate = 0.0;

    float sample(unsigned int c, size_t i) const {
        float v = file.channel(c)[start + i];
        if (gained) v *= gain;
        if (i < fadeIn.size()) v *= fadeIn[i];
        if (i + fadeOut.size() >= frames) v *= fadeOut[i + fadeOut.size() - frames];
        return v;
    }

    float channelMaximum(size_t i) const {
        float a = 0.0f;
        for (unsigned int c = 0; c < channels; c++) a = std::max(a, std::fabs(sample(c, i)));
        return a;
    }

    // frame i's energy: the sum over the channels of its squares
    double energy(size_t i) const {
        double sum = 0.0;
        for (unsigned int c = 0; c < channels; c++) sum += (double)sample(c, i) * (double)sample(c, i);
        return sum;
    }

    // the channel mean, as Signal::mono adds and scales it
    std::vector<float> mono() const {
        std::vector<float> out(frames, 0.0f);
        if (channels == 0) return out;
        for (unsigned int c = 0; c < channels; c++) {
            for (size_t i = 0; i < frames; i++) out[i] += sample(c, i);
        }
        const float scale = 1.0f / (float)channels;
        for (float& v : out) v *= scale;
        return out;
    }

private:
    const AudioAnalysis::Signal& file;
    size_t start = 0;
    bool gained = false;
    float gain = 1.0f;
    std::vector<float> fadeIn;
    std::vector<float> fadeOut;
};

struct PlayedMaximumSquares {
    const Played& played;
    double square(long k) const {
        const double a = played.channelMaximum((size_t)k);
        return a * a;
    }
};

// 2.2.1: an edge sample v (d the outward step) clicks: loud against the sound, audible on its own
// and not a zero crossing (moving toward 0 fast enough to reach it within two steps)
bool edgeClicks(double v, double d, double peak) {
    const bool crossing = signOf(d) != signOf(v) && std::fabs(v) <= SoundTypes::EDGE_ZERO_CROSSING_STEPS * std::fabs(d);
    return v != 0.0 && amplitudeDb(std::fabs(v) / peak) > SoundTypes::EDGE_MAX_DB &&
           amplitudeDb(std::fabs(v)) >= SoundTypes::SILENCE_THRESHOLD_DBFS && !crossing;
}

// 2.2.1: the first and the last sample against silence; and the longest click-safe fade-in its
// attack allows, for the notes' fix
Json edgesOf(const Played& x, double peak) {
    Json out = Json::object();
    const size_t n = x.frames;
    for (int side = 0; side < 2; side++) {
        const size_t edge = side == 0 ? 0 : n - 1;
        const size_t inner = side == 0 ? 1 : n - 2;
        bool clicks = false;
        double largest = 0.0;
        for (unsigned int c = 0; c < x.channels; c++) {
            const double v = x.sample(c, edge);
            largest = std::max(largest, std::fabs(v));
            if (edgeClicks(v, v - (double)x.sample(c, inner), peak)) clicks = true;
        }
        const std::string name = side == 0 ? "start" : "end";
        out[name + "_db"] = largest > 0.0 ? AudioAnalysis::rounded(amplitudeDb(largest / peak), DB_STEP) : Json(nullptr);
        out[name + "_click"] = clicks;
    }
    std::vector<double> squares(std::min(n, (size_t)framesOfMs(SoundTypes::ATTACK_WINDOW_SECONDS * MS_PER_SECOND, x.rate)));
    for (size_t i = 0; i < squares.size(); i++) squares[i] = x.energy(i);
    // the longest of the kinds' click-safe fade-ins; a note takes the kind's own at most
    const size_t longest = (size_t)std::max(0L, framesOfMs(SoundTypes::CLICK_SAFE_FADE_IN_MS, x.rate));
    out["safe_fade_in_frames"] = AudioAnalysis::safeFadeInFrames(squares, x.rate, longest);
    return out;
}

// 2.2.4: one side of a run of digital silence against the sound's own level there. v:
// the sample next to the run (the largest |x| over the channels); level: v re the far window's
// largest |x|, in dB; envelope: the near window's RMS re the far window's, in dB; largest: the far
// window's largest |x|. Not valid when the far window holds under 2 samples or only zeros
struct GapSide {
    bool valid = false;
    double v = 0.0;
    double level = 0.0;
    double envelope = 0.0;
    double largest = 0.0;
};

// the channel mean as a one-channel signal, for the clicks' dropouts
struct MonoView {
    const std::vector<float>& m;
    unsigned int channels = 1;
    size_t frames = 0;
    float sample(unsigned int, size_t i) const { return m[i]; }
};

// X: channels, frames and sample(c, i), as Played. b: the sample next to the run; before: the run
// follows b (the windows lie before it), else it precedes b
template <class X>
GapSide gapSideOf(const X& x, size_t b, bool before, double rate) {
    const long n = (long)x.frames;
    const long nearFrames = framesOfMs(SoundTypes::GAP_NEAR_MS, rate);
    const long farFrames = framesOfMs(SoundTypes::GAP_FAR_MS, rate);
    const long at = (long)b;
    const long lo = before ? std::max(0L, at - farFrames) : at + nearFrames + 1;
    const long hi = before ? at - nearFrames : std::min(n, at + farFrames + 1);
    GapSide out;
    if (hi - lo < 2) return out;
    double largest = 0.0, rms = 0.0, energy = 0.0, steps = 0.0;
    for (unsigned int c = 0; c < x.channels; c++) {
        double sum = 0.0;
        for (long i = lo; i < hi; i++) {
            const double v = x.sample(c, (size_t)i);
            largest = std::max(largest, std::fabs(v));
            sum += v * v;
            if (i > lo) {
                const double d = v - (double)x.sample(c, (size_t)i - 1);
                energy += v * v;
                steps += d * d;
            }
        }
        rms = std::max(rms, std::sqrt(sum / (double)(hi - lo)));
    }
    if (largest <= 0.0) return out;
    // the near window: a quarter of the local period, from the angular frequency the far window's
    // differences give (sqrt(sum d^2 / sum x^2) radians a sample)
    const double w = energy > 0.0 ? std::sqrt(steps / energy) : 0.0;
    const long quarter = w > 0.0 ? std::max(nearFrames, std::min(farFrames, (long)std::ceil(PI / 2.0 / w))) : nearFrames;
    const long nearLo = before ? std::max(0L, at - quarter + 1) : at;
    const long nearHi = before ? at + 1 : std::min(n, at + quarter);
    double nearRms = 0.0, v = 0.0;
    for (unsigned int c = 0; c < x.channels; c++) {
        double sum = 0.0;
        for (long i = nearLo; i < nearHi; i++) sum += (double)x.sample(c, (size_t)i) * (double)x.sample(c, (size_t)i);
        nearRms = std::max(nearRms, std::sqrt(sum / (double)(nearHi - nearLo)));
        v = std::max(v, (double)std::fabs(x.sample(c, b)));
    }
    out.valid = true;
    out.v = v;
    out.level = v > 0.0 ? 20.0 * std::log10(v / largest) : -std::numeric_limits<double>::infinity();
    out.envelope = rms > 0.0 && nearRms > 0.0 ? 20.0 * std::log10(nearRms / rms) : -std::numeric_limits<double>::infinity();
    out.largest = largest;
    return out;
}

// 2.2.4: the run [start, end) of digital silence is a dropout: cut on both sides (at a
// zero crossing too), or on one while the sound goes on at its level on both
template <class X>
bool isDropout(const X& x, size_t start, size_t end, double peak, double rate) {
    if (start == 0 || end >= x.frames) return false;
    const GapSide sides[2] = {gapSideOf(x, start - 1, true, rate), gapSideOf(x, end, false, rate)};
    auto cut = [](const GapSide& side, bool zeroCrossing) {
        if (!side.valid || side.v <= 0.0 || amplitudeDb(side.v) < SoundTypes::GAP_FLOOR_DBFS) return false;
        return side.level >= SoundTypes::GAP_CUT_DB ||
               (zeroCrossing && side.level >= SoundTypes::GAP_ZERO_CROSSING_DB && side.envelope >= SoundTypes::GAP_ZERO_CROSSING_ENV_DB);
    };
    if (cut(sides[0], true) && cut(sides[1], true)) return true;
    if ((cut(sides[0], false) || cut(sides[1], false)) && sides[0].valid && sides[1].valid) {
        const double before = amplitudeDb(sides[0].largest / peak), after = amplitudeDb(sides[1].largest / peak);
        return std::min(before, after) >= SoundTypes::GAP_AUDIBLE_DB && std::fabs(before - after) <= SoundTypes::GAP_CONTINUITY_DB;
    }
    return false;
}

// the samples next to each dropout of the channel mean (runs of CLICK_DROPOUT_MIN_SAMPLES or more),
// ascending
std::vector<size_t> dropoutEdgesOf(const std::vector<float>& m, double rate) {
    std::vector<size_t> edges;
    double peak = 0.0;
    for (float v : m) peak = std::max(peak, (double)std::fabs(v));
    if (peak <= 0.0) return edges;
    const MonoView x{m, 1, m.size()};
    const double zeroLevel = peak * amplitudeOfDb(SoundTypes::GAP_ZERO_DB);
    size_t run = 0;
    for (size_t i = 0; i <= m.size(); i++) {
        if (i < m.size() && std::fabs(m[i]) <= zeroLevel) {
            run++;
            continue;
        }
        if (run >= SoundTypes::CLICK_DROPOUT_MIN_SAMPLES && isDropout(x, i - run, i, peak, rate)) {
            edges.push_back(i - run - 1);
            edges.push_back(i);
        }
        run = 0;
    }
    return edges;
}

// the sound around the impulse at p repeats at the lag of the one at q (the impulses' own
// samples left out): the correlation over +/- max(lag / 2, CLICK_REPEAT_MIN_HALF_MS) peaks at
// CLICK_REPEAT_MIN or more, strictly inside lag +/- CLICK_REPEAT_SEARCH
bool repeatsAt(const std::vector<float>& m, size_t p, size_t q, double rate) {
    const long lag = std::labs((long)q - (long)p);
    const long sign = q > p ? 1 : -1;
    const long half = std::max(lag / 2, framesOfMs(SoundTypes::CLICK_REPEAT_MIN_HALF_MS, rate));
    const long n = (long)m.size();
    double best = -2.0;
    int bestAt = 0;
    for (int d = -SoundTypes::CLICK_REPEAT_SEARCH; d <= SoundTypes::CLICK_REPEAT_SEARCH; d++) {
        const long a0 = (long)p - half, b0 = (long)p - half + sign * (lag + d);
        if (std::min(a0, b0) < 0 || std::max(a0, b0) + 2 * half >= n) return false;
        double ab = 0.0, aa = 0.0, bb = 0.0;
        for (long i = 0; i <= 2 * half; i++) {
            if (std::labs(a0 + i - (long)p) <= SoundTypes::CLICK_REPEAT_GUARD || std::labs(b0 + i - (long)q) <= SoundTypes::CLICK_REPEAT_GUARD) continue;
            const double a = m[(size_t)(a0 + i)], b = m[(size_t)(b0 + i)];
            ab += a * b;
            aa += a * a;
            bb += b * b;
        }
        const double den = std::sqrt(aa * bb);
        const double r = den > 0.0 ? ab / den : 0.0;
        if (r > best) {
            best = r;
            bestAt = d;
        }
    }
    return best >= SoundTypes::CLICK_REPEAT_MIN && std::abs(bestAt) < SoundTypes::CLICK_REPEAT_SEARCH;
}

// clicks that make an irregular field through the region are its texture
bool crackleOf(const std::vector<double>& times, double seconds) {
    const size_t n = times.size();
    if (n < (size_t)SoundTypes::CRACKLE_MIN_CLICKS || (double)n < SoundTypes::CRACKLE_MIN_PER_SECOND * seconds ||
        times.back() - times.front() < SoundTypes::CRACKLE_MIN_SPREAD * seconds) {
        return false;
    }
    double mean = 0.0;
    for (size_t i = 1; i < n; i++) mean += times[i] - times[i - 1];
    mean /= (double)(n - 1);
    double variance = 0.0;
    for (size_t i = 1; i < n; i++) variance += (times[i] - times[i - 1] - mean) * (times[i] - times[i - 1] - mean);
    variance /= (double)(n - 1);
    return mean > 0.0 && std::sqrt(variance) / mean >= SoundTypes::CRACKLE_MIN_CV;
}

// 2.2.2: isolated discontinuities, by the second difference of the mean on 1 ms frames (spec 5.6:
// a candidate in an impulse texture, or recurring where the sound itself repeats, is the waveform;
// one at a dropout's edge is a click whatever surrounds it; crackle is flagged)
Json clicksOf(const std::vector<float>& m, double rate) {
    const size_t n = m.size();
    const size_t f = (size_t)std::max(1L, framesOfMs(SoundTypes::CLICK_FRAME_MS, rate));
    const size_t count = n / f;
    std::vector<double> times;
    if (count > 0) {
        std::vector<double> e(count), l(count);
        std::vector<char> impulsive(count);
        // each frame's impulse: its largest |second difference| (the first, on a tie)
        std::vector<size_t> impulseAt(count);
        for (size_t k = 0; k < count; k++) {
            double d2Sum = 0.0, mSum = 0.0, largest = -1.0;
            double top[SoundTypes::CLICK_IMPULSE_SAMPLES] = {};
            for (size_t i = k * f; i < (k + 1) * f; i++) {
                const double d2 = (i >= 1 && i + 1 < n) ? (double)m[i + 1] - 2.0 * m[i] + m[i - 1] : 0.0;
                const double square = d2 * d2;
                d2Sum += square;
                mSum += (double)m[i] * m[i];
                if (std::fabs(d2) > largest) {
                    largest = std::fabs(d2);
                    impulseAt[k] = i;
                }
                // the largest squares, descending
                for (int t = 0; t < SoundTypes::CLICK_IMPULSE_SAMPLES; t++) {
                    if (square > top[t]) {
                        for (int u = SoundTypes::CLICK_IMPULSE_SAMPLES - 1; u > t; u--) top[u] = top[u - 1];
                        top[t] = square;
                        break;
                    }
                }
            }
            double topSum = 0.0;
            for (double v : top) topSum += v;
            e[k] = powerDb(d2Sum / (double)f);
            l[k] = powerDb(mSum / (double)f);
            impulsive[k] = d2Sum > 0.0 && topSum / d2Sum >= SoundTypes::CLICK_IMPULSE_SHARE;
        }
        std::vector<size_t> impulsiveBefore(count + 1, 0);
        for (size_t k = 0; k < count; k++) impulsiveBefore[k + 1] = impulsiveBefore[k] + (impulsive[k] ? 1 : 0);
        const size_t loudest = (size_t)(std::max_element(l.begin(), l.end()) - l.begin());
        const double lMax = l[loudest];
        const long guard = std::lround(SoundTypes::CLICK_GUARD_MS / SoundTypes::CLICK_FRAME_MS);
        const long recur = std::lround(SoundTypes::CLICK_RECUR_MS / SoundTypes::CLICK_FRAME_MS);
        const long texture = std::lround(SoundTypes::CLICK_TEXTURE_MS / SoundTypes::CLICK_FRAME_MS);
        bool edgesFound = false;
        std::vector<size_t> edges;
        // the waveform: an impulse texture, or the impulse recurring where the sound repeats
        auto waveform = [&](long k) {
            const size_t p = impulseAt[(size_t)k];
            if (!edgesFound) {
                edges = dropoutEdgesOf(m, rate);
                edgesFound = true;
            }
            // edges ascend: the first at or after p - guard
            const size_t guardFrom = p >= (size_t)SoundTypes::CLICK_DROPOUT_GUARD ? p - (size_t)SoundTypes::CLICK_DROPOUT_GUARD : 0;
            const auto edge = std::lower_bound(edges.begin(), edges.end(), guardFrom);
            if (edge != edges.end() && *edge <= p + (size_t)SoundTypes::CLICK_DROPOUT_GUARD) return false;
            const long lo = std::max(0L, k - texture), hi = std::min((long)count, k + texture + 1);
            if ((double)(impulsiveBefore[(size_t)hi] - impulsiveBefore[(size_t)lo]) / (double)(hi - lo) >= SoundTypes::CLICK_TEXTURE_SHARE) {
                return true;
            }
            for (long j = std::max(0L, k - recur); j <= std::min((long)count - 1, k + recur); j++) {
                if (std::labs(j - k) > SoundTypes::CLICK_RECUR_SAME_FRAMES && impulsive[(size_t)j] &&
                    e[(size_t)j] >= e[(size_t)k] - SoundTypes::CLICK_RISE_DB && repeatsAt(m, p, impulseAt[(size_t)j], rate)) {
                    return true;
                }
            }
            return false;
        };
        long groupFirst = -1, groupLast = -1;
        auto flush = [&]() {
            if (groupFirst >= 0) times.push_back((double)(groupFirst + groupLast + 1) / 2.0 * (double)f / rate);
            groupFirst = groupLast = -1;
        };
        for (long k = 0; k < (long)count; k++) {
            const long preFirst = std::max(0L, k - SoundTypes::CLICK_CONTEXT_FAR_FRAMES);
            const long preLast = k - SoundTypes::CLICK_CONTEXT_NEAR_FRAMES;
            const long postFirst = k + SoundTypes::CLICK_CONTEXT_NEAR_FRAMES;
            const long postLast = std::min((long)count - 1, k + SoundTypes::CLICK_CONTEXT_FAR_FRAMES);
            bool hit = false;
            if (preLast - preFirst + 1 >= SoundTypes::CLICK_CONTEXT_MIN_FRAMES && postLast - postFirst + 1 >= SoundTypes::CLICK_CONTEXT_MIN_FRAMES) {
                const double pre = median(std::vector<double>(e.begin() + preFirst, e.begin() + preLast + 1));
                const double post = median(std::vector<double>(e.begin() + postFirst, e.begin() + postLast + 1));
                hit = e[(size_t)k] - std::max(pre, post) > SoundTypes::CLICK_RISE_DB &&
                      std::fabs(pre - post) < SoundTypes::CLICK_STEADY_DB &&
                      impulsive[(size_t)k] &&
                      l[(size_t)k] > lMax - SoundTypes::CLICK_AUDIBLE_DB &&
                      k > guard && (long)count - 1 - k > guard &&
                      std::labs(k - (long)loudest) > SoundTypes::CLICK_ATTACK_GUARD_FRAMES &&
                      !waveform(k);
            }
            if (hit && groupFirst >= 0 && k == groupLast + 1) {
                groupLast = k;
            } else if (hit) {
                flush();
                groupFirst = groupLast = k;
            } else {
                flush();
            }
        }
        flush();
    }
    Json listed = Json::array();
    for (size_t i = 0; i < times.size() && i < (size_t)SoundTypes::DEFECT_TIMES_LISTED; i++) {
        listed.push_back(AudioAnalysis::rounded(times[i], TIME_STEP_SECONDS));
    }
    return {{"count", times.size()}, {"times_s", listed}, {"crackle", crackleOf(times, (double)n / rate)}};
}

// 2.2.3: a tail that ends, by a stop or a fade-out under TAIL_RELEASE_MIN_MS, while it
// still decays; 5 ms frames aligned to the end. fadeOutFrames: the region's own fade-out, which
// counts as a release of its length
Json tailOf(const std::vector<float>& m, double rate, uint32_t fadeOutFrames) {
    const size_t n = m.size();
    const size_t f = (size_t)std::max(1L, framesOfMs(SoundTypes::TAIL_FRAME_MS, rate));
    const long count = (long)(n / f);
    if (count < SoundTypes::TAIL_MIN_FRAMES) return nullptr;
    const size_t offset = n - (size_t)count * f;
    auto levelOf = [&](size_t first, size_t length) {
        double sum = 0.0;
        for (size_t i = first; i < first + length; i++) sum += (double)m[i] * m[i];
        return powerDb(sum / (double)length);
    };
    std::vector<double> e((size_t)count);
    for (long k = 0; k < count; k++) e[(size_t)k] = levelOf(offset + (size_t)k * f, f);
    const long loudest = (long)(std::max_element(e.begin(), e.end()) - e.begin());
    if (count - 1 - loudest < SoundTypes::TAIL_MIN_FRAMES) return nullptr;
    const double reference = e[(size_t)loudest];
    for (double& v : e) v -= reference;
    double end = 0.0;
    for (long k = count - SoundTypes::TAIL_END_FRAMES; k < count; k++) end += e[(size_t)k];
    end /= (double)SoundTypes::TAIL_END_FRAMES;
    // the decay line, least squares over its frames' times
    const long first = std::max(loudest, count - SoundTypes::TAIL_FIT_FIRST_FRAMES);
    const long last = count - SoundTypes::TAIL_FIT_LAST_FRAMES;
    const double frameSeconds = (double)f / rate;
    double slope = 0.0, intercept = 0.0;
    if (last - first + 1 >= SoundTypes::TAIL_FIT_MIN_FRAMES) {
        double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
        const double points = (double)(last - first + 1);
        for (long k = first; k <= last; k++) {
            const double x = (double)k * frameSeconds;
            sx += x;
            sy += e[(size_t)k];
            sxx += x * x;
            sxy += x * e[(size_t)k];
        }
        const double denominator = points * sxx - sx * sx;
        slope = denominator != 0.0 ? (points * sxy - sx * sy) / denominator : 0.0;
        intercept = (sy - slope * sx) / points;
    }
    // the release: the first window back from the end within TAIL_RELEASE_DROP_DB of the line
    const size_t step = (size_t)std::max(1L, framesOfMs(SoundTypes::TAIL_RELEASE_STEP_MS, rate));
    const size_t earliest = offset + (size_t)last * f;
    long departure = -1;
    size_t j = 0;
    for (; n >= j * step + f && n - j * step - f >= earliest; j++) {
        const size_t start = n - j * step - f;
        const double line = intercept + slope * (double)(start - offset) / rate;
        if (levelOf(start, f) - reference >= line - SoundTypes::TAIL_RELEASE_DROP_DB) {
            departure = (long)j;
            break;
        }
    }
    const double departureMs = (double)(departure >= 0 ? (size_t)departure : j) * (double)step / rate * MS_PER_SECOND;
    const double release = std::max((departureMs + SoundTypes::TAIL_FRAME_MS / 2.0) / SoundTypes::TAIL_RELEASE_SHARE,
                                    (double)fadeOutFrames * MS_PER_SECOND / rate);
    // each condition only gets less true as a fade-out gets longer (SoundTypes.h)
    Json cut = nullptr, how = nullptr;
    if (slope < SoundTypes::TAIL_DECAY_DB_PER_S && end >= SoundTypes::TAIL_CUT_MIN_DB && release < SoundTypes::TAIL_RELEASE_MIN_MS) {
        cut = AudioAnalysis::rounded(end, DB_STEP);
        how = departure == 0 ? "stop" : "fade";
    }
    return {{"cut_db", cut}, {"how", how}, {"slope_db_per_s", AudioAnalysis::rounded(slope, DB_STEP)},
            {"end_db", AudioAnalysis::rounded(end, DB_STEP)}, {"release_ms", AudioAnalysis::rounded(release, DB_STEP)}};
}

// 2.2.4: runs of digital silence between the first and the last audible sample that
// are dropouts (isDropout)
Json gapsOf(const Played& x, double peak) {
    const size_t n = x.frames;
    const double audible = peak * amplitudeOfDb(SoundTypes::GAP_AUDIBLE_DB);
    const double zeroLevel = peak * amplitudeOfDb(SoundTypes::GAP_ZERO_DB);
    long first = -1, last = -1;
    for (size_t i = 0; i < n; i++) {
        if (x.channelMaximum(i) > audible) {
            if (first < 0) first = (long)i;
            last = (long)i;
        }
    }
    std::vector<std::pair<size_t, size_t>> found;  // start, length
    if (first >= 0) {
        size_t run = 0;
        for (size_t i = (size_t)first; i <= (size_t)last + 1; i++) {
            const bool zero = i <= (size_t)last && x.channelMaximum(i) <= zeroLevel;
            if (zero) {
                run++;
                continue;
            }
            if (run > 0 && (double)run * MS_PER_SECOND / x.rate >= SoundTypes::GAP_MIN_MS) {
                const size_t start = i - run;  // >= first + 1: the run lies between audible samples
                if (isDropout(x, start, i, peak, x.rate)) found.push_back({start, run});
            }
            run = 0;
        }
    }
    size_t longest = 0;
    Json listed = Json::array();
    for (size_t i = 0; i < found.size(); i++) {
        longest = std::max(longest, found[i].second);
        if (i < (size_t)SoundTypes::DEFECT_TIMES_LISTED) listed.push_back(AudioAnalysis::rounded((double)found[i].first / x.rate, TIME_STEP_SECONDS));
    }
    return {{"count", found.size()}, {"longest_ms", AudioAnalysis::rounded((double)longest * MS_PER_SECOND / x.rate, DB_STEP)},
            {"times_s", listed}};
}

// The power spectra |X_k|^2 of the mean's spectral frames, summed over the frames (FFT_SIZE / 2 + 1
// values; measure() sums the same while it transforms the whole file, AudioAnalysis.h)
std::vector<double> summedPower(const std::vector<float>& m) {
    const size_t bins = AudioAnalysis::FFT_SIZE / 2 + 1;
    const size_t frames = AudioAnalysis::spectralFrames(m.size());
    std::vector<double> power(bins, 0.0);
    std::vector<float> magnitude(bins);
    for (size_t f = 0; f < frames; f++) {
        AudioAnalysis::magnitudes(m.data(), m.size(), f, magnitude.data());
        for (size_t k = 0; k < bins; k++) power[k] += (double)(magnitude[k] * magnitude[k]);
    }
    return power;
}

double resonanceWeight(double hz) {
    for (const SoundTypes::ResonanceWeight& band : SoundTypes::RESONANCE_WEIGHTS) {
        if (hz >= band.fromHz && hz <= band.toHz) return band.weight;
    }
    return SoundTypes::RESONANCE_OTHER_WEIGHT;
}

// 2.2.5: the most prominent narrow peak of the summed spectrum, weighted toward 2-6 kHz
Json resonanceOf(const std::vector<double>& power, double rate) {
    const size_t bins = AudioAnalysis::FFT_SIZE / 2 + 1;
    const double binHz = rate / AudioAnalysis::FFT_SIZE;
    std::vector<double> level(bins, 0.0);
    for (size_t k = 0; k < bins; k++) level[k] = powerDb(power[k]);
    const double top = std::min(SoundTypes::RESONANCE_MAX_HZ, SoundTypes::RESONANCE_MAX_RATE_SHARE * rate);
    std::vector<double> grid, points;
    for (int i = 0;; i++) {
        const double g = SoundTypes::RESONANCE_MIN_HZ * std::pow(2.0, i / SoundTypes::RESONANCE_POINTS_PER_OCTAVE);
        if (g > top) break;
        const double half = std::pow(2.0, 1.0 / (2.0 * SoundTypes::RESONANCE_POINTS_PER_OCTAVE));
        const long k0 = (long)std::ceil(g / half / binHz);
        const long k1 = std::min((long)bins - 1, (long)std::floor(g * half / binHz));
        double largest;
        if (k1 >= k0) {
            largest = level[(size_t)k0];
            for (long k = k0 + 1; k <= k1; k++) largest = std::max(largest, level[(size_t)k]);
        } else {
            largest = level[(size_t)std::min((long)bins - 1, std::lround(g / binHz))];
        }
        grid.push_back(g);
        points.push_back(largest);
    }
    const long count = (long)points.size();
    if (count == 0) return {{"score_db", nullptr}, {"hz", nullptr}, {"prominence_db", nullptr}};
    std::vector<double> prominence((size_t)count);
    for (long i = 0; i < count; i++) {
        const long lo = std::max(0L, i - SoundTypes::RESONANCE_NEIGHBOUR_POINTS);
        const long hi = std::min(count - 1, i + SoundTypes::RESONANCE_NEIGHBOUR_POINTS);
        prominence[(size_t)i] = points[(size_t)i] - median(std::vector<double>(points.begin() + lo, points.begin() + hi + 1));
    }
    const double largest = *std::max_element(points.begin(), points.end());
    long best = -1;
    double bestScore = 0.0;
    for (long i = 0; i < count; i++) {
        const double p = prominence[(size_t)i];
        const bool left = i == 0 || p > prominence[(size_t)i - 1];
        const bool right = i == count - 1 || p >= prominence[(size_t)i + 1];
        if (!left || !right || p <= 0.0 || points[(size_t)i] <= largest - SoundTypes::RESONANCE_RANGE_DB) continue;
        const double score = p * resonanceWeight(grid[(size_t)i]);
        if (best < 0 || score > bestScore) {
            best = i;
            bestScore = score;
        }
    }
    if (best < 0) return {{"score_db", nullptr}, {"hz", nullptr}, {"prominence_db", nullptr}};
    return {{"score_db", AudioAnalysis::rounded(bestScore, DB_STEP)}, {"hz", AudioAnalysis::rounded(grid[(size_t)best], HZ_STEP)},
            {"prominence_db", AudioAnalysis::rounded(prominence[(size_t)best], DB_STEP)}};
}

// The aliasing frames: the power spectra |X_k|^2 of ALIASING_FFT_SIZE points of the mean
// every ALIASING_HOP, 4-term Blackman-Harris windowed, the last one zero-padded, frame after frame
// (ALIASING_FFT_SIZE / 2 + 1 each)
std::vector<float> aliasingPower(const std::vector<float>& m, size_t& frames) {
    const size_t size = (size_t)SoundTypes::ALIASING_FFT_SIZE, hop = (size_t)SoundTypes::ALIASING_HOP, bins = size / 2 + 1;
    static const std::vector<float> window = []() {
        std::vector<float> w(SoundTypes::ALIASING_FFT_SIZE);
        for (int j = 0; j < SoundTypes::ALIASING_FFT_SIZE; j++) {
            double v = 0.0;
            for (int t = 0; t < BLACKMAN_HARRIS_TERMS; t++) v += BLACKMAN_HARRIS[t] * std::cos(2.0 * PI * t * j / SoundTypes::ALIASING_FFT_SIZE);
            w[j] = (float)v;
        }
        return w;
    }();
    const size_t n = m.size();
    frames = n == 0 ? 0 : n <= size ? 1 : 1 + (n - size + hop - 1) / hop;
    std::vector<float> power(frames * bins);
    std::vector<float> buffer(2 * size);
    for (size_t f = 0; f < frames; f++) {
        std::fill(buffer.begin(), buffer.end(), 0.0f);
        for (size_t j = 0; j < size && f * hop + j < n; j++) buffer[2 * j] = m[f * hop + j] * window[j];
        SoLoud::FFT::fft(buffer.data(), (unsigned int)(2 * size));
        for (size_t k = 0; k < bins; k++) power[f * bins + k] = buffer[2 * k] * buffer[2 * k] + buffer[2 * k + 1] * buffer[2 * k + 1];
    }
    return power;
}

// The largest bin within +/- ALIASING_PEAK_BINS of b when it is a local maximum ALIASING_PEAK_DB
// over the median of +/- ALIASING_MEDIAN_BINS: its position refined by a parabola through the dB
// of it and its neighbours; NaN when there is no such peak
double harmonicPeak(const std::vector<double>& levels, long b) {
    const long count = (long)levels.size();
    const long lo = std::max(1L, b - SoundTypes::ALIASING_PEAK_BINS), hi = std::min(count - 2, b + SoundTypes::ALIASING_PEAK_BINS);
    if (hi < lo) return NaN;
    long k = lo;
    for (long q = lo + 1; q <= hi; q++) {
        if (levels[(size_t)q] > levels[(size_t)k]) k = q;
    }
    const long aroundLo = std::max(0L, k - SoundTypes::ALIASING_MEDIAN_BINS), aroundHi = std::min(count - 1, k + SoundTypes::ALIASING_MEDIAN_BINS);
    const double around = median(std::vector<double>(levels.begin() + aroundLo, levels.begin() + aroundHi + 1));
    const double a = levels[(size_t)k - 1], c = levels[(size_t)k], d = levels[(size_t)k + 1];
    if (c < around + SoundTypes::ALIASING_PEAK_DB || !(c > a && c >= d)) return NaN;
    const double denominator = a - 2.0 * c + d;
    return (double)k + (denominator != 0.0 ? 0.5 * (a - d) / denominator : 0.0);
}

// f0 from the frame's own harmonics: least squares through the origin of their peaks,
// first from the lowest ALIASING_REFINE_FIRST, then from every one under Nyquist; f0 as it was
// when fewer than 2 are found
double refinedF0(const std::vector<double>& levels, double f0, double rate) {
    const double binHz = rate / SoundTypes::ALIASING_FFT_SIZE;
    const double nyquist = rate / 2.0;
    for (int pass = 0; pass < 2; pass++) {
        double num = 0.0, den = 0.0;
        int found = 0;
        for (long j = 1; (double)j * f0 < nyquist && (pass == 1 || j <= SoundTypes::ALIASING_REFINE_FIRST); j++) {
            const double at = harmonicPeak(levels, std::lround((double)j * f0 / binHz));
            if (std::isnan(at)) continue;
            num += (double)j * at * binHz;
            den += (double)(j * j);
            found++;
        }
        if (found >= 2) f0 = num / den;
    }
    return f0;
}

// 2.2.6: in harmonic frames, the share of the 4-20 kHz energy at the positions where partials
// above Nyquist fold back (on the aliasing frames)
Json aliasingOf(const std::vector<float>& m, double rate) {
    size_t frames = 0;
    const std::vector<float> power = aliasingPower(m, frames);
    const size_t size = (size_t)SoundTypes::ALIASING_FFT_SIZE, bins = size / 2 + 1;
    const double binHz = rate / (double)size;
    const double nyquist = rate / 2.0;
    const double high = std::min(SoundTypes::ALIASING_MAX_HZ, nyquist);
    std::vector<double> framePower(frames, 0.0);
    for (size_t f = 0; f < frames; f++) {
        for (size_t k = 0; k < bins; k++) framePower[f] += power[f * bins + k];
    }
    const double loudest = frames > 0 ? *std::max_element(framePower.begin(), framePower.end()) : 0.0;
    std::vector<double> folds;
    std::vector<double> levels(bins);
    // the folds counted for the family of every partial (0) and of the odd ones (1)
    std::vector<char> counted[2] = {std::vector<char>(bins), std::vector<char>(bins)};
    for (size_t f = 0; f < frames; f++) {
        if (framePower[f] <= 0.0 || powerDb(framePower[f] / loudest) < -SoundTypes::ALIASING_RANGE_DB) continue;
        // the pitch of the frame's middle FFT_SIZE samples, then refined from its harmonics
        const size_t pitchLength = std::min((size_t)AudioAnalysis::FFT_SIZE, m.size());
        const size_t start = std::min(f * (size_t)SoundTypes::ALIASING_HOP + (size - (size_t)AudioAnalysis::FFT_SIZE) / 2, m.size() - pitchLength);
        double f0 = 0.0, confidence = 0.0;
        if (!pitchOfWindowFft(m.data() + start, pitchLength, rate, f0, confidence) || confidence < SoundTypes::TONAL_MIN_CONFIDENCE) continue;
        const float* p = power.data() + f * bins;
        for (size_t k = 0; k < bins; k++) levels[k] = powerDb(p[k]);
        f0 = refinedF0(levels, f0, rate);
        if (!(f0 > 0.0)) continue;
        std::fill(counted[0].begin(), counted[0].end(), 0);
        std::fill(counted[1].begin(), counted[1].end(), 0);
        size_t judged[2] = {0, 0}, hits[2] = {0, 0};
        for (long k = 1; (double)k * f0 <= SoundTypes::ALIASING_MAX_RATE_MULTIPLE * rate; k++) {
            const double partial = (double)k * f0;
            if (partial <= nyquist) continue;
            const double position = std::fabs(partial - rate * std::round(partial / rate));
            if (position < SoundTypes::ALIASING_MIN_HZ || position > high) continue;
            // more than the guard from every harmonic under Nyquist (the nearest two)
            bool offGrid = true;
            const long below = (long)std::floor(position / f0);
            for (long j = std::max(1L, below); j <= below + 1; j++) {
                if ((double)j * f0 < nyquist && std::fabs(position - (double)j * f0) <= SoundTypes::ALIASING_HARMONIC_GUARD_BINS * binHz) offGrid = false;
            }
            if (!offGrid) continue;
            const long b = std::lround(position / binHz);
            const long peakLo = std::max(0L, b - SoundTypes::ALIASING_PEAK_BINS);
            const long peakHi = std::min((long)bins - 1, b + SoundTypes::ALIASING_PEAK_BINS);
            const long aroundLo = std::max(0L, b - SoundTypes::ALIASING_MEDIAN_BINS);
            const long aroundHi = std::min((long)bins - 1, b + SoundTypes::ALIASING_MEDIAN_BINS);
            const double peak = *std::max_element(levels.begin() + peakLo, levels.begin() + peakHi + 1);
            const double floorDb = percentile(std::vector<double>(levels.begin() + aroundLo, levels.begin() + aroundHi + 1),
                                              SoundTypes::ALIASING_FLOOR_PERCENTILE);
            const bool shows = peak >= floorDb + SoundTypes::ALIASING_PEAK_DB;
            for (int family = 0; family < (k % 2 == 1 ? 2 : 1); family++) {
                judged[family]++;
                if (!shows) continue;
                hits[family]++;
                for (long q = peakLo; q <= peakHi; q++) counted[family][(size_t)q] = 1;
            }
        }
        // a family of folds, not a partial that happens to sit at one position: every
        // partial's, else the odd partials' (a square's)
        int family = -1;
        for (int candidate = 0; candidate < 2 && family < 0; candidate++) {
            if (judged[candidate] > 0 && (double)hits[candidate] >= SoundTypes::ALIASING_FAMILY_SHARE * (double)judged[candidate]) family = candidate;
        }
        double band = 0.0, folded = 0.0;
        for (size_t k = 0; k < bins; k++) {
            const double hz = (double)k * binHz;
            if (hz < SoundTypes::ALIASING_MIN_HZ || hz > high) continue;
            band += p[k];
            if (family >= 0 && counted[family][k]) folded += p[k];
        }
        if (band <= 0.0) continue;
        folds.push_back(powerDb(band / framePower[f]) < SoundTypes::ALIASING_BAND_MIN_DB ? 0.0 : folded / band);
    }
    if (folds.empty()) return {{"fold", nullptr}, {"frames", 0}};
    return {{"fold", AudioAnalysis::rounded(median(folds), SHARE_STEP)}, {"frames", folds.size()}};
}

} // namespace

AudioAnalysis::Signal AudioAnalysis::played(const Signal& file, const Region& region) {
    const Played played(file, region);
    Signal out;
    out.channels = file.channels;
    out.sampleRate = file.sampleRate;
    out.frames = played.frames;
    out.samples.resize(out.frames * out.channels);
    for (unsigned int c = 0; c < out.channels; c++) {
        float* to = out.samples.data() + (size_t)c * out.frames;
        for (size_t i = 0; i < out.frames; i++) to[i] = played.sample(c, i);
    }
    return out;
}

AudioAnalysis::Region AudioAnalysis::Region::of(const ClipImportSettings& settings, uint32_t fileFrames) {
    Region region;
    region.startFrame = std::min(settings.trimHeadFrames, fileFrames);
    // as AudioClipSource::applyRegion: trims that leave nothing leave an empty region at the trimmed start
    region.endFrame = settings.trimTailFrames > 0 ? std::max(region.startFrame, fileFrames - std::min(settings.trimTailFrames, fileFrames)) : FILE_END;
    region.gainDb = settings.gainDb;
    const uint32_t length = region.end(fileFrames) - region.startFrame;
    region.fadeInFrames = std::min(settings.fadeInFrames, length);
    region.fadeOutFrames = std::min(settings.fadeOutFrames, length);
    return region;
}

bool AudioAnalysis::Region::isWholeFile() const {
    return startFrame == 0 && endFrame == FILE_END && fadeInFrames == 0 && fadeOutFrames == 0 && gainDb == 0.0f;
}

uint32_t AudioAnalysis::Region::end(size_t fileFrames) const {
    const uint32_t frames = (uint32_t)std::min<size_t>(fileFrames, UINT32_MAX);
    return endFrame == FILE_END ? frames : std::min(endFrame, frames);
}

uint32_t AudioAnalysis::Region::fadeIn(size_t fileFrames) const {
    const uint32_t stop = end(fileFrames);
    return std::min(fadeInFrames, stop > startFrame ? stop - startFrame : 0u);
}

uint32_t AudioAnalysis::Region::fadeOut(size_t fileFrames) const {
    const uint32_t stop = end(fileFrames);
    return std::min(fadeOutFrames, stop > startFrame ? stop - startFrame : 0u);
}

Json AudioAnalysis::Region::toJson(size_t fileFrames) const {
    return {{"start_frame", startFrame}, {"end_frame", end(fileFrames)}, {"fade_in_frames", fadeIn(fileFrames)},
            {"fade_out_frames", fadeOut(fileFrames)}, {"gain_db", rounded(gainDb, GAIN_STEP)}};
}

bool AudioAnalysis::Region::sameAs(const Region& other, size_t fileFrames) const {
    return startFrame == other.startFrame && end(fileFrames) == other.end(fileFrames) && fadeIn(fileFrames) == other.fadeIn(fileFrames) &&
           fadeOut(fileFrames) == other.fadeOut(fileFrames) && gainDb == other.gainDb;
}

bool AudioAnalysis::edgeSampleClicks(double v, double d, double peak) {
    return edgeClicks(v, d, peak);
}

double AudioAnalysis::cutLevelDb(const Signal& file, uint32_t frame) {
    if (file.frames == 0) return SILENCE_DB;
    float peak = 0.0f;
    for (size_t i = 0; i < file.frames; i++) peak = std::max(peak, channelMaximum(file, i));
    if (peak <= 0.0f) return SILENCE_DB;
    const long half = framesOfMs(SoundTypes::CUT_WINDOW_MS, (double)file.sampleRate);
    const size_t lo = (size_t)std::max(0L, (long)frame - half);
    const size_t hi = std::min(file.frames - 1, (size_t)frame + (size_t)half);
    float level = 0.0f;
    for (size_t i = lo; i <= hi; i++) level = std::max(level, channelMaximum(file, i));
    return amplitudeDb((double)level / peak);
}

namespace {

// trims that leave under 2 frames play nothing; said, not measured
bool trimmedAway(const AudioAnalysis::Region& region, size_t fileFrames) {
    const uint32_t end = region.end(fileFrames);
    const uint32_t start = std::min<uint32_t>(region.startFrame, end);
    return end - start < 2 && (start > 0 || end < fileFrames);
}

Json trimmedAwayDefects(const AudioAnalysis::Region& region, size_t fileFrames) {
    const uint32_t end = region.end(fileFrames);
    return {{"region", region.toJson(fileFrames)}, {"empty", true}, {"frames", end - std::min<uint32_t>(region.startFrame, end)}};
}

} // namespace

size_t AudioAnalysis::safeFadeInFrames(const std::vector<double>& squares, double rate, size_t longest) {
    longest = std::min(longest, squares.size());
    if (longest < SoundTypes::FADE_IN_MIN_FRAMES) return longest;
    const size_t window = std::min(squares.size(), (size_t)std::llround(SoundTypes::ATTACK_WINDOW_SECONDS * rate));
    double energy = 0.0;
    for (size_t k = 0; k < window; k++) energy += squares[k];
    const double allowed = energy * (1.0 - std::pow(10.0, -SoundTypes::FADE_IN_MAX_LOSS_DB / 10.0));
    for (size_t fade = longest; fade >= SoundTypes::FADE_IN_MIN_FRAMES; fade--) {
        double loss = 0.0;
        for (size_t k = 0; k < fade; k++) {
            const double w = clipFadeWeight((uint32_t)k, (uint32_t)fade, true);
            loss += squares[k] * (1.0 - w * w);
        }
        if (loss <= allowed) return fade;
    }
    return 0;
}

Json AudioAnalysis::defects(const Signal& signal, const Region& region) {
    return defects(signal, region, nullptr);
}

Json AudioAnalysis::defects(const Signal& signal, const Region& region, const std::vector<double>* spectrum) {
    if (signal.channels == 0 || signal.sampleRate == 0) return nullptr;
    if (trimmedAway(region, signal.frames)) return trimmedAwayDefects(region, signal.frames);
    const Played x(signal, region);
    const size_t n = x.frames;
    if (n < 2) return nullptr;
    double peak = 0.0;
    for (size_t i = 0; i < n; i++) peak = std::max(peak, (double)x.channelMaximum(i));
    if (peak <= 0.0 || amplitudeDb(peak) < SoundTypes::SILENT_PEAK_DBFS) return nullptr;
    const std::vector<float> m = x.mono();

    Json out;
    out["region"] = region.toJson(signal.frames);
    out["edges"] = edgesOf(x, peak);
    out["clicks"] = clicksOf(m, x.rate);
    out["tail"] = tailOf(m, x.rate, region.fadeOut(signal.frames));
    out["gaps"] = gapsOf(x, peak);
    out["resonance"] = nullptr;
    out["aliasing"] = nullptr;
    if ((double)n / x.rate <= SoundTypes::SPECTRAL_DEFECTS_MAX_SECONDS) {
        out["resonance"] = resonanceOf(spectrum && region.isWholeFile() ? *spectrum : summedPower(m), x.rate);
        // the region's own pitch (Phase 1's, of its loudest 100 ms) gates the aliasing measurement
        size_t envelopeAt = 0;
        double envelopeMax = 0.0;
        RunningRms<PlayedMaximumSquares> envelope({x}, (long)n, std::max(0L, framesOfMs(ENVELOPE_SECONDS * MS_PER_SECOND, x.rate) / 2));
        for (size_t i = 0; i < n; i++) {
            const double e = envelope.next();
            if (e > envelopeMax) {
                envelopeMax = e;
                envelopeAt = i;
            }
        }
        double hz = 0.0, confidence = 0.0;
        if (envelopeMax > 0.0 && pitchOfLoudest(m.data(), n, x.rate, envelopeAt, hz, confidence) &&
            confidence >= SoundTypes::TONAL_MIN_CONFIDENCE) {
            out["aliasing"] = aliasingOf(m, x.rate);
        }
    }
    const uint32_t end = region.end(signal.frames);
    out["cuts"] = {
        {"head_db", region.startFrame > 0 ? rounded(cutLevelDb(signal, region.startFrame), DB_STEP) : Json(nullptr)},
        {"tail_db", end < signal.frames ? rounded(cutLevelDb(signal, end), DB_STEP) : Json(nullptr)}
    };
    return out;
}

// ======================================================================== files and the cache

namespace {

// The cached item of a decoded signal
Json cachedItem(const AudioAnalysis::Signal& signal, const AudioAnalysis::Format& format, const std::string& sha1, uint64_t bytes) {
    // one transform of the file for both: the defects' resonance sums measure()'s spectra
    std::vector<double> spectrum;
    Json item = AudioAnalysis::measure(signal, AudioMeterMode::File, &spectrum);
    // the whole file as it is: what plays with default import settings (others: analyzePlayed)
    item["defects"] = AudioAnalysis::defects(signal, AudioAnalysis::Region(), &spectrum);
    item["analyzer_version"] = AudioAnalysis::VERSION;
    item["sha1"] = sha1;
    item["format"] = {
        {"codec", format.codec},
        {"encoding", format.encoding},
        {"channels", signal.channels},
        {"sample_rate", signal.sampleRate},
        {"frames", signal.frames},
        {"duration_s", AudioAnalysis::rounded(signal.seconds(), 0.001)},
        {"bytes", bytes}
    };
    return item;
}

} // namespace

bool AudioAnalysis::analyzeBytes(const std::string& bytes, Json& cached, std::string& error, Signal* decoded) {
    Format format;
    if (!probe(bytes, format, error)) return false;
    Signal signal;
    if (!decode(bytes, signal, error)) return false;
    cached = cachedItem(signal, format, SHA1::hash(bytes), bytes.size());
    if (decoded) *decoded = std::move(signal);
    return true;
}

Json AudioAnalysis::silence(const Signal& signal) {
    const size_t n = signal.frames;
    const double rate = signal.sampleRate > 0 ? (double)signal.sampleRate : 1.0;
    const double silenceLevel = std::pow(10.0, SoundTypes::SILENCE_THRESHOLD_DBFS / 20.0);
    long first = -1, last = -1;
    for (size_t i = 0; i < n; i++) {
        if (channelMaximum(signal, i) >= silenceLevel) {
            if (first < 0) first = (long)i;
            last = (long)i;
        }
    }
    const double durationMs = signal.seconds() * 1000.0;
    return {
        {"head_ms", rounded(first < 0 ? durationMs : (double)first / rate * 1000.0, 0.1)},
        {"tail_ms", rounded(last < 0 ? durationMs : (double)((long)n - 1 - last) / rate * 1000.0, 0.1)}
    };
}

bool AudioAnalysis::silenceOfFile(const fs::path& file, const std::string& bytes, const fs::path& cacheDirectory,
                                  Json& silenceOut, std::string& error) {
    std::error_code ec;
    const uint64_t size = fs::file_size(file, ec);
    const fs::file_time_type mtime = fs::last_write_time(file, ec);
    std::string sha1;
    if (ec || size != bytes.size() || !knownHash(file, size, mtime, sha1)) {
        sha1 = SHA1::hash(bytes);
        if (!ec && size == bytes.size()) rememberHash(file, size, mtime, sha1);
    }
    Json cached;
    if (readCache(cacheDirectory, sha1, cached) && cached.contains("silence")) {
        silenceOut = cached["silence"];
        return true;
    }
    if (bytes.size() > MAX_FILE_BYTES) {
        error = "larger than " + std::to_string(MAX_FILE_BYTES / (1024 * 1024)) + " MB";
        return false;
    }
    Signal signal;
    if (!decode(bytes, signal, error)) return false;
    silenceOut = silence(signal);
    return true;
}

bool AudioAnalysis::analyzeFile(const fs::path& file, const fs::path& cacheDirectory, Json& cached, std::string& error,
                                Signal* decoded) {
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
        error = "not a file";
        return false;
    }
    const uint64_t size = fs::file_size(file, ec);
    if (ec) {
        error = "can't read the file size";
        return false;
    }
    if (size == 0) {
        error = "the file is empty";
        return false;
    }
    if (size > MAX_FILE_BYTES) {
        error = "larger than " + std::to_string(MAX_FILE_BYTES / (1024 * 1024)) + " MB";
        return false;
    }
    const fs::file_time_type mtime = fs::last_write_time(file, ec);

    std::string sha1;
    if (!decoded && knownHash(file, size, mtime, sha1) && readCache(cacheDirectory, sha1, cached)) {
        return true;
    }

    std::string bytes;
    if (!readFileBytes(file, bytes, error)) return false;
    sha1 = SHA1::hash(bytes);
    rememberHash(file, size, mtime, sha1);

    if (readCache(cacheDirectory, sha1, cached)) {
        if (decoded && !decodeReleasing(bytes, *decoded, error)) return false;
        return true;
    }
    Format format;
    if (!probe(bytes, format, error)) return false;
    const uint64_t length = bytes.size();
    // the bytes go as soon as SoLoud has decoded them: a long file is never held three times
    Signal signal;
    if (!decodeReleasing(bytes, signal, error)) return false;
    cached = cachedItem(signal, format, sha1, length);
    if (decoded) *decoded = std::move(signal);
    writeCache(cacheDirectory, cached);
    return true;
}

bool AudioAnalysis::analyzePlayed(const fs::path& file, const fs::path& cacheDirectory, const Json& cached, const ClipImportSettings& settings,
                                  Json& out, std::string& error, const Signal* decoded) {
    if (settings.playsAsFile()) {
        out = cached;
        return true;
    }
    const std::string sha1 = cached.value("sha1", std::string());
    const uint64_t hash = settings.hash();
    if (readCache(cacheDirectory, sha1, out, hash)) return true;

    Signal own;
    const Signal* signal = decoded;
    if (!signal) {
        std::string bytes;
        if (file.empty()) {
            error = "no file and no decoded signal";
            return false;
        }
        if (!readFileBytes(file, bytes, error) || !decodeReleasing(bytes, own, error)) return false;
        signal = &own;
    }
    const Region region = Region::of(settings, (uint32_t)std::min<size_t>(signal->frames, UINT32_MAX));
    const size_t start = region.startFrame;
    const size_t end = region.end(signal->frames);
    Json source = Json::object();
    for (const char* key : {"loudness", "peak", "rms_dbfs", "dc_offset", "silence", "onset", "measured_kind"}) {
        if (cached.contains(key)) source[key] = cached[key];
    }
    if (end <= start + 1) {
        // the trims leave nothing to measure: the file's numbers stand, and the note says so
        out = cached;
        out["defects"] = defects(*signal, region);
    } else {
        const Signal playing = played(*signal, region);
        out = measure(playing, AudioMeterMode::File);
        out["defects"] = defects(*signal, region);
        // a looping voice wraps at the loop region, not at the trimmed ends
        const int64_t loopStart = settings.loopStartFrame >= 0 ? settings.loopStartFrame : (int64_t)start;
        const int64_t loopEnd = settings.loopEndFrame >= 0 ? settings.loopEndFrame : (int64_t)end;
        if (loopStart >= (int64_t)start && loopEnd <= (int64_t)end && loopEnd > loopStart) {
            // fades never shape a wrap: the voice crosses it at the gain alone
            Region unfaded = region;
            unfaded.fadeInFrames = 0;
            unfaded.fadeOutFrames = 0;
            out["loop"] = loopSeam(played(*signal, unfaded), (size_t)(loopStart - (int64_t)start), (size_t)(loopEnd - (int64_t)start));
        }
    }
    out["analyzer_version"] = VERSION;
    out["sha1"] = sha1;
    out["format"] = cached["format"];
    out["source"] = source;
    out["import_hash"] = hashText(hash);
    out["played"] = region.toJson(signal->frames);
    out["played"]["frames"] = end > start ? end - start : 0;
    out["played"]["duration_s"] = rounded(signal->sampleRate > 0 ? (double)(end > start ? end - start : 0) / signal->sampleRate : 0.0, 0.001);
    writeCache(cacheDirectory, out);
    return true;
}

bool AudioAnalysis::analyzeClip(const fs::path& file, const fs::path& cacheDirectory, Json& out, std::string& error, Signal* decoded) {
    Signal raw;
    Json cached;
    if (!analyzeFile(file, cacheDirectory, cached, error, decoded ? &raw : nullptr)) return false;
    AudioImportSettings settings;
    ClipImportSettings clip;
    std::string problem;
    Json resolved;
    const double headMs = field(cached, "silence", "head_ms", 0.0);
    const double tailMs = field(cached, "silence", "tail_ms", 0.0);
    const Json& format = cached["format"];
    if (!AudioImportSettings::load(file, settings, problem) ||
        !settings.resolve(format.value("frames", (uint64_t)0), format.value("sample_rate", 0u), headMs, tailMs, clip, problem, &resolved)) {
        // the file's numbers, and a note that says why they are not the clip's
        out = cached;
        out["import_error"] = problem;
        if (decoded) *decoded = std::move(raw);
        return true;
    }
    if (!analyzePlayed(file, cacheDirectory, cached, clip, out, error, decoded ? &raw : nullptr)) return false;
    if (!settings.isDefault()) out["import"] = std::move(resolved);
    if (decoded) {
        *decoded = clip.playsAsFile() ? std::move(raw) : played(raw, Region::of(clip, (uint32_t)std::min<size_t>(raw.frames, UINT32_MAX)));
    }
    return true;
}

fs::path AudioAnalysis::cacheDirectory(const fs::path& projectDirectory) {
    return projectDirectory / ".jasperengine" / "cache" / "audio";
}

std::string AudioAnalysis::hashText(uint64_t hash) {
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", (unsigned long long)hash);
    return text;
}

fs::path AudioAnalysis::cacheFile(const fs::path& cacheDirectory, const std::string& sha1, uint64_t importHash) {
    const std::string name = importHash != 0 ? sha1 + "-i" + hashText(importHash) : sha1;
    return cacheDirectory / (name + "-v" + std::to_string(VERSION) + ".json");
}

bool AudioAnalysis::readCache(const fs::path& cacheDirectory, const std::string& sha1, Json& cached, uint64_t importHash) {
    if (cacheDirectory.empty() || sha1.size() != 40) return false;
    std::ifstream in(cacheFile(cacheDirectory, sha1, importHash), std::ios::binary);
    if (!in) return false;
    Json value = Json::parse(in, nullptr, false);
    if (value.is_discarded() || !value.is_object() || value.value("analyzer_version", 0) != VERSION ||
        value.value("sha1", std::string()) != sha1 || !value.contains("format")) {
        return false;
    }
    cached = std::move(value);
    return true;
}

bool AudioAnalysis::writeCache(const fs::path& cacheDirectory, const Json& cached) {
    if (cacheDirectory.empty() || !cached.contains("sha1")) return false;
    uint64_t importHash = 0;
    if (cached.contains("import_hash")) importHash = std::strtoull(cached["import_hash"].get<std::string>().c_str(), nullptr, 16);
    // regenerable, so a crash may lose it: no flush to the platters on every analysed file
    io::AtomicWriteOptions options;
    options.durability = io::Durability::Cache;
    return io::writeFileAtomic(cacheFile(cacheDirectory, cached["sha1"].get<std::string>(), importHash), cached.dump(1) + "\n", options).ok();
}

AudioAnalysis::Labels AudioAnalysis::labelsOf(const fs::path& file) {
    // the importer's type, the S25 labels and the provenance's licence
    Labels labels;
    AudioImportSettings settings;
    std::string error;
    if (AudioImportSettings::load(file, settings, error)) labels.type = settings.type;
    AudioProvenance provenance;
    if (AudioProvenance::load(file, provenance, error) && !provenance.isEmpty()) {
        labels.licence = provenance.licenceJson();
        // an imported built-in clip: the kind of its manifest entry, whether or not the library is installed
        const auto builtin = provenance.values.find("builtin");
        if (builtin != provenance.values.end() && builtin->is_object()) {
            const std::string id = builtin->value("id", std::string());
            const bool library = builtin->value("set", std::string()) == "library";
            const SoundManifestClip* clip = library ? SoundLibrary::get().manifest().find(id) : SoundEssentials::find(id);
            if (clip) {
                labels.manifestKind = clip->kind;
                labels.library = library;
            }
        }
    }
    const YAML::Node assetLabels = SetAssetMetaCmd::read(file, "labels");
    if (assetLabels.IsSequence() && assetLabels.size() > 0) {
        labels.tags = Json::array();
        for (const YAML::Node& label : assetLabels) labels.tags.push_back(label.as<std::string>(""));
    }
    return labels;
}

void AudioAnalysis::resolveKind(const std::string& measuredKind, const Labels& labels, std::string& kind, std::string& kindSource) {
    if (!labels.manifestKind.empty()) {
        kind = labels.manifestKind;
        kindSource = "type";
        return;
    }
    if (const SoundTypes::SoundType* type = SoundTypes::find(labels.type)) {
        kind = type->kind;
        kindSource = "type";
        return;
    }
    kind = measuredKind;
    kindSource = "measured";
}

Json AudioAnalysis::fileItem(const Json& cached, int n, const std::string& path, const Labels& labels, bool full,
                             double headSilenceMaxMs) {
    Json item = cached;
    item.erase("analyzer_version");
    item.erase("import_hash");
    if (!full) stripFullDetail(item);
    item["n"] = n;
    item["path"] = path;
    std::string kind, kindSource;
    resolveKind(item.value("measured_kind", std::string(SoundTypes::TRANSIENT)), labels, kind, kindSource);
    item["kind"] = kind;
    item["kind_source"] = kindSource;
    if (!labels.type.empty()) item["type"] = labels.type;
    if (labels.tags.is_array()) item["labels"] = labels.tags;
    if (labels.licence.is_object()) item["licence"] = labels.licence;
    item["notes"] = notes(item, kind, labels.type, headSilenceMaxMs);
    if (labels.library) curatedNotes(item);
    return item;
}

void AudioAnalysis::curatedNotes(Json& item) {
    // A starter library clip's level is the curation's: its gain
    // stops at the true-peak ceiling, so a one-shot with a high crest factor ships up to 6 LU under its
    // type's target. The note says so, as information on the clip as the library ships it, not a level
    // to fix.
    if (!item.contains("notes") || !item["notes"].is_array()) return;
    for (Json& note : item["notes"]) {
        if (note.value("id", std::string()) != "loudness_target") continue;
        const std::string text = note.value("text", std::string());
        const size_t colon = text.find(": ");
        if (text.find(" under the ") == std::string::npos || colon == std::string::npos) continue;
        note["text"] = text.substr(0, colon) + " (information): the library levels a clip to its type's target as far as its " +
                       formatText("%.0f", SoundTypes::HEADROOM_DBTP) + " dBTP true peak allows; " + text.substr(colon + 2) +
                       " (the source's volume)";
    }
}

namespace {

// The defect notes, gated by the kind and the type: a
// glitch clicks and stutters by design; a loop's end is its seam; a designed end is a cut on
// purpose; a narrow resonance is the sound of a tonal sound and the ring of an impact. `fix` names
// the fades that fix it: a side with a fade never clicks (its edge sample is 0); the
// fade-in is the longest click-safe one the attack allows; when the tail is cut, the fade-out that
// fixes the tail fixes the end's click too, so every fix of a note clears it and adds none.
// rate and fileFrames: the file's (format.sample_rate, format.frames).
void defectNotes(const Json& defects, const std::string& kind, const std::string& type, double rate, double fileFrames, Json& out) {
    using namespace SoundTypes;
    const bool loop = kind == LOOP;
    const bool glitch = kind == GLITCH;
    const bool oneShot = kind == TRANSIENT || kind == TONAL || kind == SWELL || kind == RISER || kind == SPEECH;
    const bool designedEnd = isDesignedEnd(type);
    const Json empty = Json::object();
    auto group = [&](const char* name) -> const Json& {
        return defects.contains(name) && defects[name].is_object() ? defects[name] : empty;
    };
    auto add = [&](const char* id, const std::string& text, const Json& fix) {
        Json note = {{"id", id}, {"text", text}};
        if (fix.is_object() && !fix.empty()) note["fix"] = fix;
        out.push_back(note);
    };

    // trims that leave nothing to play: that alone, and nothing a fade could fix
    if (defects.value("empty", false)) {
        const double left = number(defects.value("frames", Json(0)), 0.0);
        add("empty_region", "its trims leave " + formatText("%.0f", left) + " of its " + grouped(fileFrames) +
                                " frames: it plays nothing; trim less", nullptr);
        return;
    }

    const Json& region = group("region");
    const bool fadedIn = number(region.value("fade_in_frames", Json(0)), 0.0) > 0.0;
    const bool fadedOut = number(region.value("fade_out_frames", Json(0)), 0.0) > 0.0;

    // the fade-in: the kind's click-safe length, shorter when the attack is in the first frames
    const Json& edges = group("edges");
    const double kindInMs = clickSafeFadeInMs(kind);
    double fadeInMs = kindInMs;
    if (rate > 0.0 && edges.contains("safe_fade_in_frames") && edges["safe_fade_in_frames"].is_number()) {
        const double kindFrames = (double)std::llround(kindInMs * rate / MS_PER_SECOND);
        const double safe = edges["safe_fade_in_frames"].get<double>();
        const double frames = safe > 0.0 ? std::min(safe, kindFrames) : (double)FADE_IN_FLOOR_FRAMES;
        if (frames < kindFrames) fadeInMs = number(AudioAnalysis::rounded(frames * MS_PER_SECOND / rate, FADE_MS_STEP), kindInMs);
    }

    const Json& tail = group("tail");
    const bool tailCut = oneShot && !designedEnd && tail.contains("cut_db") && tail["cut_db"].is_number();
    const double fadeOutMs = tailCut ? std::max(clickSafeFadeOutMs(kind), TAIL_RELEASE_FIX_MS) : clickSafeFadeOutMs(kind);

    const bool startClick = (oneShot || loop) && edges.value("start_click", false);
    const bool endClick = oneShot && edges.value("end_click", false);
    if (startClick || endClick) {
        std::string text;
        Json fix = Json::object();
        if (startClick) {
            text = "starts at " + formatText("%.1f", number(edges.value("start_db", Json(0.0)), 0.0)) + " dB re its peak: clicks " +
                   (loop ? "when it starts" : "every time it plays") + "; fade_in_ms " + msText(fadeInMs) + " in its import settings fixes it";
            fix["fade_in_ms"] = fadeInMs;
        }
        if (endClick) {
            if (!text.empty()) text += "; ";
            text += "ends at " + formatText("%.1f", number(edges.value("end_db", Json(0.0)), 0.0)) + " dB re its peak: clicks when it stops; fade_out_ms " +
                    msText(fadeOutMs) + (startClick ? " fixes it" : " in its import settings fixes it");
            fix["fade_out_ms"] = fadeOutMs;
        }
        add("edge_click", text, fix);
    }

    // crackle is the sound's texture: information, not a defect
    const Json& clicks = group("clicks");
    const double clickCount = number(clicks.value("count", Json(0)), 0.0);
    const bool crackle = clicks.value("crackle", false);
    if (!glitch && clickCount > 0 && crackle) {
        add("crackle", formatText("%.0f", clickCount) +
                           " clicks spread irregularly through the sound (information): crackle, the texture of fire, rain or vinyl; "
                           "in a sound that should be clean, damage",
            nullptr);
    }
    if (!glitch && clickCount > 0 && !crackle) {
        std::string times;
        const Json listed = clicks.value("times_s", Json::array());
        for (const Json& t : listed) times += (times.empty() ? "" : ", ") + formatText("%.3f", number(t, 0.0));
        if (clickCount > (double)listed.size()) times += ", ...";
        add("clicks", formatText("%.0f", clickCount) + (clickCount == 1 ? " click" : " clicks") + " inside the sound at " + times +
                          " s: a discontinuity (an edit or a dropout)", nullptr);
    }

    if (tailCut) {
        const std::string level = formatText("%.1f", tail["cut_db"].get<double>());
        const std::string slope = formatText("%.0f", number(tail.value("slope_db_per_s", Json(0.0)), 0.0));
        const std::string fixText = "; fade_out_ms " + msText(TAIL_RELEASE_FIX_MS) + " in its import settings fixes it";
        const Json fix = {{"fade_out_ms", TAIL_RELEASE_FIX_MS}};
        if (tail.value("how", std::string()) == "fade") {
            add("truncated_tail", "ends in a " + formatText("%.0f", number(tail.value("release_ms", Json(0.0)), 0.0)) + " ms fade at " + level +
                                      " dB re its peak while still decaying (" + slope + " dB/s): it sounds cut" + fixText, fix);
        } else {
            add("truncated_tail", "tail cut at " + level + " dB re its peak while still decaying (" + slope + " dB/s): it ends abruptly" + fixText,
                fix);
        }
    }

    const Json& gaps = group("gaps");
    const double gapCount = number(gaps.value("count", Json(0)), 0.0);
    if (!glitch && gapCount > 0) {
        const Json listed = gaps.value("times_s", Json::array());
        const std::string longest = formatText("%.1f", number(gaps.value("longest_ms", Json(0.0)), 0.0));
        std::string text;
        if (gapCount == 1) {
            text = "1 gap of digital silence inside the sound (" + longest + " ms at " +
                   formatText("%.3f", listed.empty() ? 0.0 : number(listed[0], 0.0)) + " s): it drops out";
        } else {
            std::string times;
            for (const Json& t : listed) times += (times.empty() ? "" : ", ") + formatText("%.3f", number(t, 0.0));
            if (gapCount > (double)listed.size()) times += ", ...";
            text = formatText("%.0f", gapCount) + " gaps of digital silence inside the sound (the longest " + longest + " ms; at " + times +
                   " s): it drops out";
        }
        add("silence_gap", text, nullptr);
    }

    const Json& resonance = group("resonance");
    if ((kind == SWELL || kind == RISER || kind == SPEECH) && resonance.contains("score_db") && resonance["score_db"].is_number() &&
        resonance["score_db"].get<double>() > RESONANCE_MAX_DB) {
        add("harsh_resonance", "narrow peak " + formatText("%+.1f", number(resonance.value("prominence_db", Json(0.0)), 0.0)) + " dB at " +
                                   grouped(number(resonance.value("hz", Json(0.0)), 0.0)) +
                                   " Hz over its third-octave neighbours: whistles or sounds harsh; dip it with an EQ", nullptr);
    }

    const Json& aliasing = group("aliasing");
    if ((kind == TRANSIENT || kind == TONAL || kind == SWELL || kind == RISER || kind == GLITCH) && aliasing.contains("fold") &&
        aliasing["fold"].is_number() && aliasing["fold"].get<double>() > ALIASING_INFO_SHARE) {
        add("aliasing", formatText("%.0f", aliasing["fold"].get<double>() * PERCENT) +
                            "% of the 4-20 kHz energy sits where partials fold back (information): naive synthesis; fine for a retro sound, else "
                            "render it band-limited",
            nullptr);
    }

    const Json& cuts = group("cuts");
    const bool headCut = cuts.contains("head_db") && cuts["head_db"].is_number() && cuts["head_db"].get<double>() > TRIM_CUT_MAX_DB;
    const bool tailTrimCut = cuts.contains("tail_db") && cuts["tail_db"].is_number() && cuts["tail_db"].get<double>() > TRIM_CUT_MAX_DB;
    if (headCut || tailTrimCut) {
        std::string text;
        Json fix = Json::object();
        if (headCut) {
            text = "trim_head_ms cuts into the sound at " + formatText("%.1f", cuts["head_db"].get<double>()) + " dB re its peak: it starts mid-sound";
            if (fadedIn) {
                text += "; the fade hides the click";
            } else {
                text += "; add fade_in_ms " + msText(fadeInMs);
                fix["fade_in_ms"] = fadeInMs;
            }
        }
        if (tailTrimCut) {
            if (!text.empty()) text += "; ";
            text += "trim_tail_ms cuts into the sound at " + formatText("%.1f", cuts["tail_db"].get<double>()) + " dB re its peak: it ends mid-sound";
            if (fadedOut && !tailCut) {
                text += "; the fade hides the click";
            } else if (fadeOutMs > 0.0) {
                text += "; add fade_out_ms " + msText(fadeOutMs);
                fix["fade_out_ms"] = fadeOutMs;
            }
        }
        add("trim_cut", text, fix);
    }
}

} // namespace

Json AudioAnalysis::notes(const Json& item, const std::string& kind, const std::string& type, double headSilenceMaxMs, bool capture) {
    Json out = Json::array();
    auto add = [&](const char* id, const std::string& text) { out.push_back({{"id", id}, {"text", text}}); };
    const Json& lengthOf = item.contains("played") ? item["played"] : item.contains("format") ? item["format"] : item;
    const double duration = lengthOf.is_object() ? number(lengthOf.value("duration_s", Json(0.0)), 0.0) : 0.0;
    const double samplePeak = field(item, "peak", "sample_dbfs", SILENCE_DB);
    const double truePeak = field(item, "peak", "true_peak_dbtp", SILENCE_DB);
    const double clipped = field(item, "peak", "clipped", 0.0);
    const double nonFinite = item.contains("non_finite") ? number(item["non_finite"], 0.0) : 0.0;

    if (item.contains("import_error")) {
        add("import_settings", "its import settings are not applied: " + item["import_error"].get<std::string>());
    }
    if (item.contains("played")) {
        add("as_it_plays", "measured as the engine plays it, with its import settings; the raw file's numbers are under `source`");
    }
    if (nonFinite > 0) {
        add("non_finite", formatText("%.0f", nonFinite) + (nonFinite == 1 ? " sample is" : " samples are") +
                              " NaN or infinite: the audio is corrupt; every measurement counts them as 0 (silence)");
    }
    if (samplePeak < SoundTypes::SILENT_PEAK_DBFS) {
        add("silent", "silent: peak below " + formatText("%.0f", SoundTypes::SILENT_PEAK_DBFS) + " dBFS");
    }
    if (clipped > 0) {
        add("clipping", formatText("%.0f", clipped) + " samples at full scale: clipped");
    }
    if (truePeak > SoundTypes::TRUE_PEAK_MAX_DBTP) {
        add("true_peak", "true peak " + formatText("%+.2f", truePeak) + " dBTP: will clip after resampling or encoding");
    }
    const double dc = item.contains("dc_offset") ? number(item["dc_offset"], 0.0) : 0.0;
    if (dc > SoundTypes::DC_OFFSET_MAX) {
        add("dc_offset", "DC offset " + formatText("%.3f", dc) + ": wastes headroom, clicks on start and stop");
    }
    const double head = field(item, "silence", "head_ms", 0.0);
    if (!capture && head > headSilenceMaxMs && kind != SoundTypes::LOOP) {
        add("head_silence", "head silence " + formatText("%.1f", head) + " ms: will sound late on triggers");
    }
    const double lowShare = field(item, "band_share", "31.5", 0.0) + field(item, "band_share", "63", 0.0);
    if (lowShare >= SoundTypes::LOW_END_SHARE_MAX && !SoundTypes::isLowEndType(type)) {
        add("low_end", formatText("%.0f", lowShare * 100.0) + "% of energy below 90 Hz: boomy, lost on phone speakers");
    }
    const double attack = field(item, "onset", "attack_ms");
    if (!capture && kind == SoundTypes::TRANSIENT && std::isfinite(attack) && attack > SoundTypes::SOFT_ATTACK_MS) {
        add("soft_attack", "attack " + formatText("%.1f", attack) + " ms: soft for a " + (type.empty() ? std::string("transient") : type));
    }
    const double peakMs = field(item, "onset", "peak_ms");
    if (!capture && kind == SoundTypes::RISER && std::isfinite(peakMs) && duration > 0.0 &&
        peakMs < SoundTypes::RISER_PEAK_MIN_SHARE * duration * 1000.0) {
        add("riser_early", "riser peaks at " + formatText("%.0f", peakMs / (duration * 10.0)) + "%: should peak at the end");
    }
    if (!capture && kind == SoundTypes::LOOP) {
        const double step = field(item, "loop", "seam_step_db", SILENCE_DB);
        const double level = field(item, "loop", "seam_level_db", 0.0);
        if (step > SoundTypes::LOOP_SEAM_STEP_MAX_DB) {
            add("loop_seam", "loop seam step " + formatText("%+.1f", step) + " dB over its neighbouring samples: will click");
        } else if (std::fabs(level) > SoundTypes::LOOP_SEAM_LEVEL_MAX_DB) {
            add("loop_seam", "loop seam level jump " + formatText("%.1f", level) + " dB: will bump");
        }
    }
    if (!capture) {
        if (const SoundTypes::SoundType* t = SoundTypes::find(type)) {
            const double measured = t->integratedTarget ? field(item, "loudness", "integrated_lufs")
                                                        : field(item, "loudness", "momentary_max");
            const std::string unit = t->integratedTarget ? "LUFS" : "LUFS-M";
            if (std::isfinite(measured)) {
                const double difference = measured - t->targetLufs;
                const std::string target = std::string(t->name) + " target (" + formatText("%.0f", t->targetLufs) + " " + unit + ")";
                if (difference < -SoundTypes::LOUDNESS_TARGET_TOLERANCE_LU) {
                    const double under = -difference;
                    const double fits = std::min(under, SoundTypes::HEADROOM_DBTP - truePeak);
                    add("loudness_target", formatText("%.1f", under) + " LU under the " + target + ": " +
                                           formatText("%+.1f", fits) + " dB fits before " + formatText("%.0f", SoundTypes::HEADROOM_DBTP) + " dBTP");
                } else if (difference > SoundTypes::LOUDNESS_TARGET_TOLERANCE_LU) {
                    add("loudness_target", formatText("%.1f", difference) + " LU over the " + target + ": apply " +
                                           formatText("%.1f", -difference) + " dB");
                }
            }
        }
    }
    const double correlation = field(item, "stereo", "correlation");
    if (std::isfinite(correlation) && correlation < SoundTypes::PHASE_CORRELATION_MIN) {
        add("phase", "stereo correlation " + formatText("%.2f", correlation) + ": cancels in mono");
    }
    if (!capture && item.contains("defects") && item["defects"].is_object()) {
        const Json format = item.value("format", Json::object());
        defectNotes(item["defects"], kind, type, number(format.value("sample_rate", Json(0)), 0.0), number(format.value("frames", Json(0)), 0.0), out);
    }
    return out;
}

std::string AudioAnalysis::notesSummary(const Json& notes) {
    // each note's headlines: of the clauses between "; ", those with a colon, each up to it (a
    // note with two halves, as edge_click's start and end, keeps both, joined by " and "); the
    // first clause when none has one
    std::string out;
    if (!notes.is_array()) return out;
    for (const Json& note : notes) {
        const std::string text = note.value("text", std::string());
        std::vector<std::string> clauses;
        for (size_t at = 0;;) {
            const size_t next = text.find("; ", at);
            clauses.push_back(text.substr(at, next == std::string::npos ? std::string::npos : next - at));
            if (next == std::string::npos) break;
            at = next + 2;
        }
        std::string headline;
        for (const std::string& clause : clauses) {
            const size_t colon = clause.find(':');
            if (colon == std::string::npos) continue;
            headline += (headline.empty() ? "" : " and ") + clause.substr(0, colon);
        }
        if (headline.empty()) headline = clauses.front();
        if (!out.empty()) out += "; ";
        out += headline;
    }
    return out;
}

// ======================================================================== captures

fs::path AudioAnalysis::captureRoot(const fs::path& projectDirectory) {
    return projectDirectory / ".jasperengine" / "audio" / "captures";
}

bool AudioAnalysis::isSessionId(const std::string& name) {
    // YYYYMMDD-HHMMSS-NN
    if (name.size() != 18 || name[8] != '-' || name[15] != '-') return false;
    for (size_t i = 0; i < name.size(); i++) {
        if (i == 8 || i == 15) continue;
        if (name[i] < '0' || name[i] > '9') return false;
    }
    return true;
}

std::vector<AudioAnalysis::Capture> AudioAnalysis::listCaptures(const fs::path& root, const std::string& liveSession) {
    std::vector<Capture> out;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return out;
    for (const fs::directory_entry& entry : fs::directory_iterator(root, ec)) {
        const std::string name = entry.path().filename().string();
        if (!isSessionId(name) || !entry.is_directory(ec)) continue;
        std::ifstream in(entry.path() / "session.json", std::ios::binary);
        if (!in) continue;
        Json session = Json::parse(in, nullptr, false);
        if (session.is_discarded() || !session.is_object()) continue;
        Capture capture;
        capture.id = name;
        capture.directory = entry.path();
        capture.state = session.value("state", std::string("aborted"));
        capture.session = std::move(session);
        out.push_back(std::move(capture));
    }
    std::sort(out.begin(), out.end(), [](const Capture& a, const Capture& b) { return a.id < b.id; });
    for (Capture& capture : out) {
        if (capture.state != "capturing") continue;
        if (!liveSession.empty() && capture.id == liveSession) {
            capture.live = true;
        } else {
            capture.state = "aborted";
        }
    }
    return out;
}

bool AudioAnalysis::findCapture(const fs::path& root, const std::string& reference, const std::string& liveSession, Capture& capture,
                                std::string& error) {
    // a directory: a player's --audio-capture session, anywhere
    if (reference.find_first_of("/\\") != std::string::npos) {
        std::ifstream in(fs::path(reference) / "session.json", std::ios::binary);
        Json session = in ? Json::parse(in, nullptr, false) : Json();
        if (!session.is_object()) {
            error = reference + " is not a capture session: it has no session.json.";
            return false;
        }
        capture.id = fs::path(reference).lexically_normal().filename().string();
        capture.directory = reference;
        capture.state = session.value("state", std::string("aborted"));
        capture.live = !liveSession.empty() && capture.id == liveSession && capture.state == "capturing";
        if (capture.state == "capturing" && !capture.live) capture.state = "aborted";
        capture.session = std::move(session);
        return true;
    }
    const std::vector<Capture> sessions = listCaptures(root, liveSession);
    if (sessions.empty()) {
        error = "No capture yet: a Play captures without an audio device, or after audio_session {action: \"arm\"}.";
        return false;
    }
    if (reference == "current") {
        for (const Capture& session : sessions) {
            if (session.live) {
                capture = session;
                return true;
            }
        }
        error = "No capture is running: use \"last\" or a session id (" + sessions.back().id + " is the newest).";
        return false;
    }
    if (reference == "last") {
        for (auto it = sessions.rbegin(); it != sessions.rend(); ++it) {
            if (!it->live) {
                capture = *it;
                return true;
            }
        }
        error = "The only capture is still running: use capture \"current\".";
        return false;
    }
    for (const Capture& session : sessions) {
        if (session.id == reference) {
            capture = session;
            return true;
        }
    }
    std::string ids;
    for (const Capture& session : sessions) ids += (ids.empty() ? "" : ", ") + session.id;
    error = "No capture session \"" + reference + "\"; sessions: " + ids + " (or \"last\", \"current\").";
    return false;
}

bool AudioAnalysis::readCaptureAudio(const std::string& bytes, Signal& signal, std::string& error) {
    signal = Signal();
    const size_t n = bytes.size();
    if (n < 12 || bytes.compare(0, 4, "RIFF") != 0 || bytes.compare(8, 4, "WAVE") != 0) {
        error = "master.wav is not a RIFF/WAVE file";
        return false;
    }
    unsigned int channels = 0, rate = 0, bits = 0, tag = 0;
    size_t pos = 12;
    while (pos + 8 <= n) {
        const uint32_t size = le32(bytes, pos + 4);
        if (bytes.compare(pos, 4, "fmt ") == 0 && size >= 16 && pos + 8 + 16 <= n) {
            tag = le16(bytes, pos + 8);
            channels = le16(bytes, pos + 10);
            rate = le32(bytes, pos + 12);
            bits = le16(bytes, pos + 22);
            if (tag == 0xFFFE && size >= 26 && pos + 8 + 26 <= n) tag = le16(bytes, pos + 8 + 24);
        } else if (bytes.compare(pos, 4, "data") == 0) {
            if (tag != 3 || bits != 32) {
                error = "master.wav is not float32";
                return false;
            }
            if (channels < AudioMeter::CHANNELS_MIN || channels > AudioMeter::CHANNELS_MAX ||
                rate < AudioMeter::SAMPLE_RATE_MIN || rate > AudioMeter::SAMPLE_RATE_MAX) {
                error = "master.wav has an unsupported layout";
                return false;
            }
            // what the header says is flushed, as far as the file holds it
            const size_t available = std::min<size_t>(size, n - (pos + 8));
            const size_t frames = available / (4 * (size_t)channels);
            signal.channels = channels;
            signal.sampleRate = rate;
            signal.frames = frames;
            signal.samples.resize(frames * channels);
            const char* data = bytes.data() + pos + 8;
            for (size_t i = 0; i < frames; i++) {
                for (unsigned int c = 0; c < channels; c++) {
                    float v;
                    std::memcpy(&v, data + (i * channels + c) * 4, 4);
                    if (!std::isfinite(v)) signal.nonFinite++;
                    signal.samples[(size_t)c * frames + i] = v;
                }
            }
            return true;
        }
        pos += 8 + (size_t)size + (size & 1);
    }
    error = "master.wav has no data chunk";
    return false;
}

uint64_t AudioAnalysis::captureSamples(const Capture& capture, const Signal& audio) {
    const Json& info = capture.session;
    uint64_t samples = audio.frames;
    if (info.contains("samples") && info["samples"].is_number_unsigned()) samples = std::max<uint64_t>(samples, info["samples"].get<uint64_t>());
    return samples;
}

bool AudioAnalysis::captureItem(const Capture& capture, Signal audio, uint64_t bytes, double start, double end,
                                bool full, Json& item, Signal& range, std::string& error) {
    const double rate = (double)audio.sampleRate;
    const size_t frames = audio.frames;
    const double total = audio.seconds();
    const bool truncated = capture.session.value("truncated", false);
    const size_t from = (size_t)std::max<long long>(0, std::llround(start * rate));
    const size_t asked = end < 0.0 ? frames : (size_t)std::max<long long>(0, std::llround(end * rate));
    const size_t to = std::min(asked, frames);
    if (from >= to) {
        error = "the range [" + formatText("%.3f", start) + ", " + (end < 0.0 ? std::string("end") : formatText("%.3f", end)) +
                "] s holds no audio: capture " + capture.id + " has " + formatText("%.3f", total) + " s";
        if (truncated) error += " of audio (truncated at its max_seconds; audio_session report lists the events after it)";
        return false;
    }
    if (from == 0 && to == frames) {
        range = std::move(audio);
    } else {
        range = Signal();
        range.channels = audio.channels;
        range.sampleRate = audio.sampleRate;
        range.frames = to - from;
        range.samples.resize(range.frames * range.channels);
        for (unsigned int c = 0; c < audio.channels; c++) {
            std::copy(audio.channel(c) + from, audio.channel(c) + to, range.samples.begin() + (long)((size_t)c * range.frames));
        }
        audio = Signal();
    }
    // readCaptureAudio leaves them as read: the count is the range's own
    range.nonFinite = zeroNonFinite(range.samples.data(), range.samples.size());

    item = measure(range, AudioMeterMode::Stream);
    if (!full) stripFullDetail(item);
    item["path"] = "capture:" + capture.id;
    item["range"] = {{"start", rounded((double)from / rate, 0.000001)}, {"end", rounded((double)to / rate, 0.000001)}};
    item["format"] = {
        {"codec", "wav"},
        {"encoding", "float32"},
        {"channels", range.channels},
        {"sample_rate", range.sampleRate},
        {"frames", range.frames},
        {"duration_s", rounded(range.seconds(), 0.001)},
        {"bytes", bytes}
    };
    const std::string kind = item.value("measured_kind", std::string(SoundTypes::TRANSIENT));
    item["kind"] = kind;
    item["kind_source"] = "measured";
    item["notes"] = notes(item, kind, "", 0.0, true);
    // a range that reaches a truncated capture's cap: the session goes on without audio
    if (truncated && to == frames) {
        item["notes"].push_back({{"id", "truncated"},
                                 {"text", "truncated at " + formatText("%.3f", total) +
                                              " s: the capture's audio stops there (its max_seconds); audio_session report lists the events after it"}});
    }
    return true;
}
