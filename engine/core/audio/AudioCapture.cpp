// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AudioCapture.h"

#if JASPERENGINE_AUDIO_CAPTURE

#include "Engine.h"
#include "Log.h"
#include "Scene.h"
#include "audio/AudioFilters.h"
#include "audio/AudioMarkers.h"
#include "audio/AudioMeter.h"
#include "audio/AudioMixer.h"
#include "audio/AudioTap.h"
#include "component/Transform.h"
#include "io/AtomicFile.h"
#include "manager/SceneManager.h"
#include "subsystem/AudioSystem.h"

#include "soloud.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <set>
#include <vector>

using namespace jasperengine;
namespace fs = std::filesystem;

std::atomic<bool> AudioCapture::active{false};

namespace {

#ifdef JASPERENGINE_VERSION_STRING
    constexpr const char* captureEngineVersion = JASPERENGINE_VERSION_STRING;
#else
    constexpr const char* captureEngineVersion = "";
#endif
    constexpr const char* FORMAT_CAPTURE = "jasperengine.audio-capture";
    constexpr const char* FORMAT_EVENTS = "jasperengine.audio-events";
    constexpr const char* FORMAT_LEVELS = "jasperengine.audio-levels";
    constexpr int FORMAT_VERSION = 1;
    constexpr uint64_t MAX_EVENT_LINES = 100000;
    constexpr uint32_t LEVEL_FRAMES = 4800;
    constexpr size_t MAX_LABEL_BYTES = 200;
    constexpr size_t STREAM_BUFFER_BYTES = 256 * 1024;

    // ---- JSON text: flat lines and one small file, in the same bytes on every run

    void jsonQuoted(std::string& out, std::string_view value) {
        out += '"';
        const size_t size = value.size();
        for (size_t i = 0; i < size;) {
            const unsigned char c = (unsigned char)value[i];
            if (c < 0x80) {
                if (c == '"' || c == '\\') {
                    out += '\\';
                    out += (char)c;
                } else if (c < 0x20) {
                    char escaped[8];
                    std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
                    out += escaped;
                } else {
                    out += (char)c;
                }
                i++;
                continue;
            }
            // UTF-8 kept; an invalid byte becomes U+FFFD, so a reader never fails on a game's label
            const size_t length = c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
            bool valid = length > 0 && i + length <= size;
            for (size_t k = 1; valid && k < length; k++) valid = (((unsigned char)value[i + k]) & 0xC0) == 0x80;
            if (valid) {
                out.append(value.substr(i, length));
                i += length;
            } else {
                out += "\xEF\xBF\xBD";
                i++;
            }
        }
        out += '"';
    }

    void jsonNumber(std::string& out, double value, int decimals) {
        if (!std::isfinite(value)) {
            out += "null";
            return;
        }
        char text[64];
        std::snprintf(text, sizeof(text), "%.*f", decimals, value);
        std::string digits = text;
        if (digits.find('.') != std::string::npos) {
            while (digits.back() == '0') digits.pop_back();
            if (digits.back() == '.') digits.pop_back();
        }
        out += digits == "-0" ? "0" : digits;
    }

    void jsonKey(std::string& out, const char* key) {
        if (out.back() != '{' && out.back() != '[') out += ',';
        out += '"';
        out += key;
        out += "\":";
    }

    std::string cutUtf8(std::string_view text, size_t bytes) {
        if (text.size() <= bytes) return std::string(text);
        size_t end = bytes;
        while (end > 0 && (((unsigned char)text[end]) & 0xC0) == 0x80) end--;
        return std::string(text.substr(0, end));
    }

    double levelDb(double linear) {
        return linear > 0.0 ? std::max(20.0 * std::log10(linear), AudioMeter::SILENCE_DB) : AudioMeter::SILENCE_DB;
    }

    // AES17 (a full-scale sine reads 0 dB), as AudioMeter
    double aes17Db(double meanSquare) {
        return meanSquare > 0.0 ? std::max(10.0 * std::log10(2.0 * meanSquare), AudioMeter::SILENCE_DB) : AudioMeter::SILENCE_DB;
    }

    std::string utcText(const char* format, std::time_t when) {
        std::tm parts{};
#if defined(_WIN32)
        gmtime_s(&parts, &when);
#else
        gmtime_r(&when, &parts);
#endif
        char text[40];
        std::strftime(text, sizeof(text), format, &parts);
        return text;
    }

    void wavU16(unsigned char* at, uint16_t value) {
        at[0] = (unsigned char)(value & 0xFF);
        at[1] = (unsigned char)(value >> 8);
    }

    void wavU32(unsigned char* at, uint32_t value) {
        for (int i = 0; i < 4; i++) at[i] = (unsigned char)((value >> (8 * i)) & 0xFF);
    }

    // ---- a float32 WAV written as it grows: a temporary sibling whose sizes are right after every
    // flush, so a reader (or a crashed captureSession) has a valid file up to there

    constexpr long WAV_HEADER_BYTES = 12 + 8 + 16 + 8 + 4 + 8;

    class StreamWav {
    public:
        bool open(const fs::path& path, unsigned int rate, unsigned int channelCount) {
            target = path;
            sampleRate = rate;
            channels = channelCount;
            temporary = io::makeTemporarySiblingPath(target);
            file.rdbuf()->pubsetbuf(buffer.data(), (std::streamsize)buffer.size());
            file.open(temporary, std::ios::binary | std::ios::trunc);
            return file.is_open() && patchHeader();
        }

        bool write(const float* interleaved, uint32_t count) {
            file.write(reinterpret_cast<const char*>(interleaved), (std::streamsize)((size_t)count * channels * sizeof(float)));
            if (!file) return false;
            frames += count;
            return true;
        }

        bool writeSilence(uint32_t count) {
            static const float zeros[1024] = {};
            while (count > 0) {
                const uint32_t now = std::min<uint32_t>(count, 1024 / channels);
                if (!write(zeros, now)) return false;
                count -= now;
            }
            return true;
        }

        // the sizes, then everything to the disk
        bool patchHeader() {
            const uint32_t blockAlign = channels * 4;
            const uint32_t dataBytes = (uint32_t)std::min<uint64_t>(frames * blockAlign, 0xFFFFFFFFull - WAV_HEADER_BYTES);
            unsigned char header[WAV_HEADER_BYTES];
            std::memcpy(header, "RIFF", 4);
            wavU32(header + 4, (uint32_t)(WAV_HEADER_BYTES - 8) + dataBytes);
            std::memcpy(header + 8, "WAVE", 4);
            std::memcpy(header + 12, "fmt ", 4);
            wavU32(header + 16, 16);
            wavU16(header + 20, 3);   // WAVE_FORMAT_IEEE_FLOAT
            wavU16(header + 22, (uint16_t)channels);
            wavU32(header + 24, sampleRate);
            wavU32(header + 28, sampleRate * blockAlign);
            wavU16(header + 32, (uint16_t)blockAlign);
            wavU16(header + 34, 32);
            std::memcpy(header + 36, "fact", 4);
            wavU32(header + 40, 4);
            wavU32(header + 44, (uint32_t)frames);
            std::memcpy(header + 48, "data", 4);
            wavU32(header + 52, dataBytes);
            file.seekp(0);
            file.write(reinterpret_cast<const char*>(header), sizeof(header));
            file.seekp((std::streamoff)(WAV_HEADER_BYTES + (int64_t)(frames * blockAlign)));
            file.flush();
            return (bool)file;
        }

        // the sizes written and the name published, without the last `trim` frames; on failure the
        // temporary stays gone
        bool close(std::string* why, uint32_t trim = 0) {
            frames -= std::min<uint64_t>(trim, frames);
            bool ok = patchHeader();
            file.close();
            ok = ok && !file.fail();
            if (ok && trim > 0) {
                std::error_code ec;
                fs::resize_file(temporary, (uintmax_t)WAV_HEADER_BYTES + frames * channels * 4, ec);
                ok = !ec;
            }
            if (!ok) {
                std::error_code ignored;
                fs::remove(temporary, ignored);
                if (why) *why = "could not finish " + target.filename().string();
                return false;
            }
            const io::WriteResult published = io::publishFile(temporary, target);
            if (!published.ok() && why) *why = published.message;
            return published.ok();
        }

        uint64_t getFrames() const { return frames; }

    private:
        std::ofstream file;
        std::vector<char> buffer = std::vector<char>(STREAM_BUFFER_BYTES);
        fs::path target;
        fs::path temporary;
        unsigned int sampleRate = 0;
        unsigned int channels = 0;
        uint64_t frames = 0;
    };

    // ---- 100 ms of a signal: peak, RMS and the octave bands. AudioOctaveBank on every signal of every
    // sample cost 6.7 ms a frame (32 voices, 4 groups): the bands come from a spectrum of each window's
    // last 85 ms, a thousandth of that work, on one complex FFT per signal (L + iR: both channels' energy)

    constexpr int SPECTRUM_SIZE = 4096;

    struct SpectrumTables {
        std::vector<float> hann;
        std::vector<float> cosines;
        std::vector<float> sines;
        std::vector<int> reversed;
        double windowEnergy = 0.0;

        SpectrumTables() {
            hann.resize(SPECTRUM_SIZE);
            cosines.resize(SPECTRUM_SIZE / 2);
            sines.resize(SPECTRUM_SIZE / 2);
            reversed.resize(SPECTRUM_SIZE);
            const double tau = 6.283185307179586476925;
            int bits = 0;
            while ((1 << bits) < SPECTRUM_SIZE) bits++;
            for (int i = 0; i < SPECTRUM_SIZE; i++) {
                hann[i] = (float)(0.5 - 0.5 * std::cos(tau * (i + 0.5) / SPECTRUM_SIZE));
                windowEnergy += (double)hann[i] * hann[i];
                int r = 0;
                for (int b = 0; b < bits; b++) r |= ((i >> b) & 1) << (bits - 1 - b);
                reversed[i] = r;
            }
            for (int i = 0; i < SPECTRUM_SIZE / 2; i++) {
                cosines[i] = (float)std::cos(tau * i / SPECTRUM_SIZE);
                sines[i] = (float)-std::sin(tau * i / SPECTRUM_SIZE);
            }
        }
    };

    const SpectrumTables& spectrumTables() {
        static const SpectrumTables tables;
        return tables;
    }

    // what the bands' energy is at: mean squares of the two channels
    void octaveMeanSquares(const std::vector<float>& ring, size_t next, unsigned int rate, double out[AudioOctaveBank::BANDS]) {
        const SpectrumTables& tables = spectrumTables();
        static std::vector<float> re(SPECTRUM_SIZE), im(SPECTRUM_SIZE);
        for (int i = 0; i < SPECTRUM_SIZE; i++) {
            const size_t at = (next + (size_t)i) % SPECTRUM_SIZE;
            const int to = tables.reversed[i];
            re[to] = ring[at * 2] * tables.hann[i];
            im[to] = ring[at * 2 + 1] * tables.hann[i];
        }
        for (int length = 2; length <= SPECTRUM_SIZE; length *= 2) {
            const int half = length / 2;
            const int step = SPECTRUM_SIZE / length;
            for (int start = 0; start < SPECTRUM_SIZE; start += length) {
                for (int k = 0; k < half; k++) {
                    const float wr = tables.cosines[k * step], wi = tables.sines[k * step];
                    const int a = start + k, b = a + half;
                    const float tr = re[b] * wr - im[b] * wi;
                    const float ti = re[b] * wi + im[b] * wr;
                    re[b] = re[a] - tr;
                    im[b] = im[a] - ti;
                    re[a] += tr;
                    im[a] += ti;
                }
            }
        }
        const double perBin = (double)rate / SPECTRUM_SIZE;
        const double scale = 1.0 / (2.0 * SPECTRUM_SIZE * tables.windowEnergy);
        for (int band = 0; band < AudioOctaveBank::BANDS; band++) {
            const double centre = AudioMeter::bandCentre(band);
            const int low = std::max(1, (int)std::ceil(centre / std::sqrt(2.0) / perBin));
            const int high = std::min(SPECTRUM_SIZE / 2 - 1, (int)std::ceil(centre * std::sqrt(2.0) / perBin) - 1);
            double energy = 0.0;
            for (int k = low; k <= high; k++) {
                const int m = SPECTRUM_SIZE - k;
                energy += (double)re[k] * re[k] + (double)im[k] * im[k] + (double)re[m] * re[m] + (double)im[m] * im[m];
            }
            out[band] = energy * scale;
        }
    }

    struct LevelMeter {
        std::vector<float> ring;
        size_t next = 0;
        unsigned int sampleRate = 48000;
        float windowPeak = 0.0f;
        double peak = 0.0;
        double squares = 0.0;
        uint64_t frames = 0;

        void init(unsigned int rate) {
            sampleRate = rate;
            ring.assign((size_t)SPECTRUM_SIZE * 2, 0.0f);
        }

        void add(const float* interleaved, uint32_t count) {
            float blockPeak = 0.0f;
            double blockSquares = 0.0;
            for (uint32_t i = 0; i < count; i++) {
                const float left = interleaved[i * 2], right = interleaved[i * 2 + 1];
                blockPeak = std::max({blockPeak, std::fabs(left), std::fabs(right)});
                blockSquares += (double)left * left + (double)right * right;
                ring[next * 2] = left;
                ring[next * 2 + 1] = right;
                next = (next + 1) % SPECTRUM_SIZE;
            }
            frames += count;
            peak = std::max(peak, (double)blockPeak);
            windowPeak = std::max(windowPeak, blockPeak);
            squares += blockSquares;
        }

        // {peak_db, rms_db, bands_db[10]}, and the next window starts
        void take(std::string& out) {
            const double samples = (double)std::max<uint64_t>(frames, 1) * 2.0;
            double bands[AudioOctaveBank::BANDS] = {};
            if (windowPeak > 0.0f) octaveMeanSquares(ring, next, sampleRate, bands);
            out += "{\"peak_db\":";
            jsonNumber(out, levelDb(peak), 2);
            out += ",\"rms_db\":";
            jsonNumber(out, aes17Db(squares / samples), 2);
            out += ",\"bands_db\":[";
            for (int b = 0; b < AudioOctaveBank::BANDS; b++) {
                if (b) out += ',';
                jsonNumber(out, aes17Db(bands[b]), 2);
            }
            out += "]}";
            peak = 0.0;
            squares = 0.0;
            windowPeak = 0.0f;
            frames = 0;
        }
    };

    struct GroupCapture {
        std::string id;
        std::string name;
        const MeterFilter* meter = nullptr;
        LevelMeter level;
        // the current chunk, interleaved: what the group's meter saw of it
        std::vector<float> chunk;
        std::unique_ptr<StreamWav> stem;
        uint32_t stemDelay = 0;
    };

    struct ActiveVoice {
        unsigned int handle = 0;
        // what the lines call it: SoLoud's handles count every play since the process began
        uint64_t serial = 0;
        std::string clip;
        std::string entity;
        int64_t entityId = -1;
        std::string scene;
        std::string group;
        std::string groupId;
        const char* owner = "main";
        uint64_t startSample = 0;
        uint32_t delay = 0;
        bool paused = false;
        uint64_t pausedAt = 0;
        uint64_t pausedFrames = 0;
    };

    struct CaptureSession {
        AudioCaptureOptions options;
        std::string id;
        fs::path directory;
        std::string startedUtc;
        unsigned int rate = 48000;
        uint64_t baseFrame = 0;
        double baseGameTime = 0.0;
        uint64_t frames = 0;

        StreamWav master;
        std::ofstream events;
        std::ofstream levels;
        std::string pendingEvents;
        std::string pendingLevels;
        uint64_t lines = 0;
        uint64_t eventsDropped = 0;
        std::map<std::string, uint64_t> counts;

        uint64_t capturedFrames = 0;
        uint64_t maxFrames = 0;
        bool truncated = false;
        uint32_t sinceFlush = 0;
        uint32_t sinceLevel = 0;
        LevelMeter masterLevel;
        std::vector<std::unique_ptr<GroupCapture>> groups;
        std::map<const MeterFilter*, GroupCapture*> groupOfMeter;
        // the mixer's id > name map at begin
        std::string mixerMap;
        std::string mixerParents;
        std::string mixerPath;

        std::vector<ActiveVoice> voices;
        uint64_t nextVoice = 1;
        // what the lines call a Sound Event's play: its counter runs on over every Play of the editor
        std::map<uint64_t, uint64_t> instances;
        int voicesPeak = 0;
        double fixedDelta = -1.0;
        std::set<std::string> notDeterministic;
        std::vector<std::string> warnings;
        std::string failure;
        int tap = 0;
    };

    CaptureSession* captureSession = nullptr;
    fs::path lastSessionPath;

    uint64_t sampleNow() {
        const uint64_t mixed = AudioSystem::getMixedFrames();
        return mixed > captureSession->baseFrame ? mixed - captureSession->baseFrame : 0;
    }

    // the first failed write stops every write and says so; the game goes on
    void failCapture(const std::string& why) {
        if (!captureSession || !captureSession->failure.empty()) return;
        captureSession->failure = why;
        Log::error("Audio capture stopped: %s (session %s): the game goes on uncaptured", why.c_str(), captureSession->id.c_str());
    }

    void flushText(std::ofstream& file, std::string& text, const char* name) {
        if (text.empty() || !captureSession->failure.empty()) {
            text.clear();
            return;
        }
        file.write(text.data(), (std::streamsize)text.size());
        file.flush();
        if (!file) failCapture(std::string("could not write ") + name);
        text.clear();
    }

    std::string sceneNameOf(const Scene* scene) {
        if (!scene) return {};
        if (captureSession && captureSession->options.sceneName) {
            const std::string name = captureSession->options.sceneName(scene);
            if (!name.empty()) return name;
        }
        return SceneManager::getSceneNameOf(scene);
    }

    std::string mixerGroupName(const std::string& id) {
        const std::string name = AudioMixer::getGroupName(id);
        return name.empty() ? "Master" : name;
    }

    // ---- what the mix saw of each group

    std::string safeFileName(const std::string& name) {
        std::string out = name;
        for (char& c : out) {
            if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
        }
        return out;
    }

    GroupCapture* addGroup(const AudioMixer::GroupMeter& info) {
        auto group = std::make_unique<GroupCapture>();
        group->id = info.id;
        group->name = info.name;
        group->meter = info.meter;
        group->level.init(captureSession->rate);
        if (captureSession->options.stems) {
            std::error_code ec;
            fs::create_directories(captureSession->directory / "groups", ec);
            group->stem = std::make_unique<StreamWav>();
            if (ec || !group->stem->open(captureSession->directory / "groups" / (safeFileName(info.name) + ".wav"), captureSession->rate, 2)) {
                failCapture("could not write the stem of group " + info.name);
                group->stem.reset();
            } else {
                // the meter is ahead of the output by the buses left to go: this keeps sample k of a
                // stem on sample k of master.wav
                group->stemDelay = (uint32_t)AudioMixer::getGroupDepth(info.id) + (info.parent.empty() ? 0u : AudioMixer::getOutputLatencyFrames(AudioMixer::findGroupId(info.parent)));
                if (!group->stem->writeSilence((uint32_t)captureSession->capturedFrames + group->stemDelay)) {
                    failCapture("could not write the stem of group " + info.name);
                }
            }
        }
        GroupCapture* raw = group.get();
        captureSession->groups.push_back(std::move(group));
        captureSession->groupOfMeter[info.meter] = raw;
        return raw;
    }

    // a group added or rebuilt during the Play (the mixer edited) has a meter of its own
    void syncGroups() {
        for (const AudioMixer::GroupMeter& info : AudioMixer::getGroupMeters()) {
            GroupCapture* known = nullptr;
            for (const std::unique_ptr<GroupCapture>& group : captureSession->groups) {
                if (group->id == info.id) known = group.get();
            }
            if (!known) {
                addGroup(info);
            } else if (known->meter != info.meter) {
                captureSession->groupOfMeter.erase(known->meter);
                known->meter = info.meter;
                captureSession->groupOfMeter[info.meter] = known;
            }
        }
    }

    void meterHook(const MeterFilter& meter, const float* planar, unsigned int samples, unsigned int stride, unsigned int channels) {
        if (!captureSession || samples == 0) return;
        auto found = captureSession->groupOfMeter.find(&meter);
        if (found == captureSession->groupOfMeter.end()) return;
        std::vector<float>& chunk = found->second->chunk;
        const size_t at = chunk.size();
        chunk.resize(at + (size_t)samples * 2);
        const float* left = planar;
        const float* right = channels > 1 ? planar + stride : planar;
        for (unsigned int i = 0; i < samples; i++) {
            chunk[at + i * 2] = left[i];
            chunk[at + i * 2 + 1] = right[i];
        }
    }

    void writeLevelsLine() {
        CaptureSession& s = *captureSession;
        // S38's limiters: what they took off in these 100 ms, for true_peak's "mix too hot"
        for (const std::unique_ptr<GroupCapture>& group : s.groups) {
            const std::string limiter = AudioMixer::findEffectId(group->id, "limiter");
            if (limiter.empty()) continue;
            const float reduction = AudioMixer::getEffectGainReductionDb(limiter);
            if (reduction <= 0.0f) continue;
            AudioCapture::Line line("limiter");
            line.text("group", group->name).number("gain_reduction_db", reduction, 2);
        }
        std::string& out = s.pendingLevels;
        const uint64_t sample = s.capturedFrames;
        out += "{\"sample\":" + std::to_string(sample) + ",\"t\":";
        jsonNumber(out, (double)sample / s.rate, 6);
        out += ",\"master\":";
        s.masterLevel.take(out);
        out += ",\"groups\":{";
        bool first = true;
        std::vector<GroupCapture*> sorted;
        for (const std::unique_ptr<GroupCapture>& group : s.groups) sorted.push_back(group.get());
        std::sort(sorted.begin(), sorted.end(), [](const GroupCapture* a, const GroupCapture* b) { return a->name < b->name; });
        for (GroupCapture* group : sorted) {
            if (!first) out += ',';
            first = false;
            jsonQuoted(out, group->name);
            out += ':';
            group->level.take(out);
        }
        out += "},\"voices\":[";
        SoLoud::Soloud& soloud = AudioSystem::getSoloud();
        first = true;
        for (const ActiveVoice& voice : s.voices) {
            if (!first) out += ',';
            first = false;
            out += '[' + std::to_string(voice.serial) + ',';
            jsonNumber(out, levelDb(soloud.getOverallVolume(voice.handle)), 2);
            out += ']';
        }
        out += "]}\n";
    }

    // the group chunks of what was mixed last, whatever filled them
    void takeChunk(uint32_t frames, bool write) {
        CaptureSession& s = *captureSession;
        const size_t expected = (size_t)frames * 2;
        for (const std::unique_ptr<GroupCapture>& group : s.groups) {
            std::vector<float>& chunk = group->chunk;
            // a bus the mix skipped (inaudible, no voice) saw nothing of it
            chunk.resize(expected, 0.0f);
            if (write) {
                // the meter is before the fader: the group's level and stem are after it
                const float fader = AudioMixer::getGroupFader(group->id);
                if (fader != 1.0f) {
                    for (float& sample : chunk) sample *= fader;
                }
                group->level.add(chunk.data(), frames);
                if (group->stem && s.failure.empty() && !group->stem->write(chunk.data(), frames)) {
                    failCapture("could not write the stem of group " + group->name);
                }
            }
            chunk.clear();
        }
    }

    void tapChunk(const float* interleaved, uint32_t frames, uint64_t) {
        CaptureSession& s = *captureSession;
        uint32_t offered = frames;
        if (s.capturedFrames >= s.maxFrames) {
            offered = 0;
        } else if (s.capturedFrames + offered > s.maxFrames) {
            offered = (uint32_t)(s.maxFrames - s.capturedFrames);
        }
        if (offered < frames) s.truncated = true;
        if (!s.failure.empty()) offered = 0;
        if (offered == 0) {
            takeChunk(frames, false);
            return;
        }
        if (!s.master.write(interleaved, offered)) {
            failCapture("could not write master.wav");
            takeChunk(frames, false);
            return;
        }
        takeChunk(offered, true);
        s.masterLevel.add(interleaved, offered);
        s.capturedFrames += offered;
        s.sinceLevel += offered;
        if (s.sinceLevel >= LEVEL_FRAMES) {
            s.sinceLevel -= LEVEL_FRAMES;
            writeLevelsLine();
        }
        s.sinceFlush += offered;
        if (s.sinceFlush >= s.rate) {
            s.sinceFlush = 0;
            if (!s.master.patchHeader()) failCapture("could not write master.wav");
            for (const std::unique_ptr<GroupCapture>& group : s.groups) {
                if (group->stem && !group->stem->patchHeader()) failCapture("could not write the stem of group " + group->name);
            }
        }
    }

    // ---- voices

    ActiveVoice* findVoice(unsigned int handle) {
        for (ActiveVoice& voice : captureSession->voices) {
            if (voice.handle == handle) return &voice;
        }
        return nullptr;
    }

    void voiceCommon(AudioCapture::Line& line, const ActiveVoice& voice) {
        line.integer("voice", (int64_t)voice.serial).text("clip", voice.clip);
        if (voice.entity.empty()) line.null("entity").null("entity_id");
        else line.text("entity", voice.entity).integer("entity_id", voice.entityId);
        line.text("scene", voice.scene).text("group", voice.group).text("group_id", voice.groupId).text("owner", voice.owner);
    }

    uint64_t instanceSerial(uint64_t instance) {
        if (instance == 0) return 0;
        std::map<uint64_t, uint64_t>& instances = captureSession->instances;
        auto found = instances.find(instance);
        if (found != instances.end()) return found->second;
        const uint64_t serial = instances.size() + 1;
        instances.emplace(instance, serial);
        return serial;
    }

    void stopLine(const ActiveVoice& voice, const char* reason, uint64_t byInstance = 0) {
        CaptureSession& s = *captureSession;
        const uint64_t sample = sampleNow();
        const uint64_t paused = voice.pausedFrames + (voice.paused && sample > voice.pausedAt ? sample - voice.pausedAt : 0);
        const uint64_t lived = sample > voice.startSample + voice.delay ? sample - voice.startSample - voice.delay : 0;
        AudioCapture::Line line("voice_stop");
        voiceCommon(line, voice);
        line.text("reason", reason);
        if (byInstance) line.integer("by_instance", (int64_t)instanceSerial(byInstance));
        line.number("played_s", (double)(lived > paused ? lived - paused : 0) / s.rate, 6);
    }

    const char* causeOf(const AudioVoiceLine& line) {
        // S37's lines name a clip's one-shot and point voice by their owner
        if (std::strcmp(line.cause, "play") == 0) {
            if (std::strcmp(line.owner, "one_shot") == 0) return "one_shot";
            if (std::strcmp(line.owner, "point") == 0) return "point";
        }
        return line.cause;
    }

    void observeVoice(const AudioVoiceLine& line) {
        if (!captureSession) return;
        CaptureSession& s = *captureSession;
        switch (line.kind) {
            case AudioVoiceLine::Kind::Start: {
                ActiveVoice voice;
                voice.handle = line.handle;
                voice.serial = s.nextVoice++;
                voice.clip = line.clip;
                voice.entity = AudioCapture::entityPath(line.scene, line.entity);
                voice.entityId = voice.entity.empty() ? -1 : (int64_t)line.entity;
                voice.scene = sceneNameOf(line.scene);
                voice.group = mixerGroupName(line.group);
                voice.groupId = line.group;
                voice.owner = line.owner;
                voice.startSample = sampleNow();
                voice.delay = line.delayFrames;
                AudioCapture::Line out("voice_start");
                voiceCommon(out, voice);
                out.number("gain", line.gain, 6).number("gain_db", levelDb(line.gain), 2).number("pitch", line.pitch, 6);
                out.number("pan", line.pan, 6).flag("looping", line.looping).flag("spatial", line.spatial);
                if (line.spatial) {
                    std::string position = "[";
                    jsonNumber(position, line.position.x, 4);
                    position += ',';
                    jsonNumber(position, line.position.y, 4);
                    position += ',';
                    jsonNumber(position, line.position.z, 4);
                    position += ']';
                    out.raw("position", position);
                }
                out.integer("priority", line.priority).text("cause", causeOf(line));
                out.integer("latency_samples", AudioSystem::getDeterministicLatency(line.group));
                if (line.trigger[0]) out.text("trigger", line.trigger);
                if (line.impactSpeed >= 0.0f) out.number("impact_speed", line.impactSpeed, 4);
                if (!line.event.empty()) out.text("event", line.event);
                if (!line.eventPath.empty()) out.text("event_path", line.eventPath);
                if (line.variation >= 0) out.integer("variation", line.variation);
                if (line.layer >= 0) out.integer("layer", line.layer);
                if (line.instance) out.integer("instance", (int64_t)instanceSerial(line.instance));
                if (line.delayFrames) out.integer("delay_samples", line.delayFrames);
                if (line.loadDelay) {
                    out.integer("load_delay", line.loadDelay);
                    s.notDeterministic.insert("a voice waited for its clip to load");
                }
                s.voices.push_back(std::move(voice));
                s.voicesPeak = std::max(s.voicesPeak, (int)s.voices.size());
                break;
            }
            case AudioVoiceLine::Kind::Stop: {
                ActiveVoice* voice = findVoice(line.handle);
                if (!voice) break;
                stopLine(*voice, line.reason, line.byInstance);
                s.voices.erase(s.voices.begin() + (voice - s.voices.data()));
                break;
            }
            case AudioVoiceLine::Kind::Pause:
            case AudioVoiceLine::Kind::Resume: {
                ActiveVoice* voice = findVoice(line.handle);
                const bool pausing = line.kind == AudioVoiceLine::Kind::Pause;
                if (!voice || voice->paused == pausing) break;
                const uint64_t sample = sampleNow();
                if (pausing) voice->pausedAt = sample;
                else voice->pausedFrames += sample > voice->pausedAt ? sample - voice->pausedAt : 0;
                voice->paused = pausing;
                AudioCapture::Line out(pausing ? "voice_pause" : "voice_resume");
                voiceCommon(out, *voice);
                break;
            }
            case AudioVoiceLine::Kind::Reject: {
                AudioCapture::Line out("voice_reject");
                const std::string path = AudioCapture::entityPath(line.scene, line.entity);
                out.text("clip", line.clip);
                if (path.empty()) out.null("entity").null("entity_id");
                else out.text("entity", path).integer("entity_id", (int64_t)line.entity);
                out.text("scene", sceneNameOf(line.scene)).text("group", mixerGroupName(line.group)).text("group_id", line.group);
                out.text("owner", line.owner).text("cause", line.cause).text("reason", line.reason).integer("priority", line.priority);
                if (line.trigger[0]) out.text("trigger", line.trigger);
                if (line.impactSpeed >= 0.0f) out.number("impact_speed", line.impactSpeed, 4);
                if (!line.event.empty()) out.text("event", line.event);
                if (!line.eventPath.empty()) out.text("event_path", line.eventPath);
                break;
            }
        }
    }

    // ---- files

    // the oldest sessions go, so `keep` remain with the one about to be made
    void pruneSessions(const fs::path& root, int keep) {
        std::vector<std::string> names;
        std::error_code ec;
        for (const fs::directory_entry& entry : fs::directory_iterator(root, ec)) {
            const std::string name = entry.path().filename().string();
            if (AudioCapture::isSessionId(name)) names.push_back(name);
        }
        std::sort(names.begin(), names.end());
        const size_t allowed = (size_t)std::max(keep - 1, 0);
        for (size_t i = 0; i + allowed < names.size(); i++) {
            std::error_code removeError;
            fs::remove_all(root / names[i], removeError);
        }
    }

    std::string newSessionId(const fs::path& root) {
        const std::string stamp = utcText("%Y%m%d-%H%M%S", std::time(nullptr));
        for (int counter = 0; counter < 100; counter++) {
            char suffix[8];
            std::snprintf(suffix, sizeof(suffix), "-%02d", counter);
            std::error_code ec;
            if (!fs::exists(root / (stamp + suffix), ec)) return stamp + suffix;
        }
        return {};
    }

    bool writeSessionFile(const char* state, const std::string& endedUtc) {
        CaptureSession& s = *captureSession;
        const auto quoted = [](std::string_view value) {
            std::string out;
            jsonQuoted(out, value);
            return out;
        };
        const auto list = [&quoted](const auto& values) {
            std::string out = "[";
            for (const std::string& value : values) {
                if (out.size() > 1) out += ", ";
                out += quoted(value);
            }
            return out + "]";
        };
        const double fixedDelta = s.fixedDelta > 0.0 ? s.fixedDelta : Engine::getFixedDeltatime();
        const double fixedFps = fixedDelta > 0.0 ? 1.0 / fixedDelta : 0.0;
        std::set<std::string> reasons = s.notDeterministic;
        if (s.frames > 0 && !(s.fixedDelta > 0.0)) reasons.insert("a frame had no fixed delta (play fixed_fps)");

        std::vector<std::string> stems;
        for (const std::unique_ptr<GroupCapture>& group : s.groups) {
            if (group->stem) stems.push_back("groups/" + safeFileName(group->name) + ".wav");
        }
        std::string counts = "{";
        for (const auto& [kind, count] : s.counts) {
            if (counts.size() > 1) counts += ", ";
            counts += quoted(kind) + ": " + std::to_string(count);
        }
        counts += "}";

        SoLoud::Soloud& soloud = AudioSystem::getSoloud();
        std::string out = "{\n";
        const auto field = [&out](const char* key, const std::string& json) {
            out += "  \"";
            out += key;
            out += "\": " + json + ",\n";
        };
        const auto decimal = [](double value, int decimals) {
            std::string text;
            jsonNumber(text, value, decimals);
            return text;
        };
        field("format", quoted(FORMAT_CAPTURE));
        field("version", std::to_string(FORMAT_VERSION));
        field("session", quoted(s.id));
        field("state", quoted(state));
        field("project", quoted(s.options.project));
        field("scene", quoted(s.options.scene));
        field("started_utc", quoted(s.startedUtc));
        field("ended_utc", endedUtc.empty() ? "null" : quoted(endedUtc));
        field("engine_version", quoted(captureEngineVersion));
        field("sample_rate", std::to_string(s.rate));
        field("channels", "2");
        field("buffer_size", "512");
        field("latency_samples", std::to_string(AudioSystem::getDeterministicLatency()));
        field("driver", quoted("deterministic"));
        field("fixed_fps", decimal(fixedFps, 6));
        field("seed", std::to_string(s.options.seed));
        field("deterministic", reasons.empty() ? "true" : "false");
        field("not_deterministic", list(reasons));
        field("frames", std::to_string(s.frames));
        field("samples", std::to_string(sampleNow()));
        field("duration_s", decimal((double)sampleNow() / s.rate, 6));
        field("captured_samples", std::to_string(s.capturedFrames));
        field("truncated", s.truncated ? "true" : "false");
        field("max_seconds", std::to_string(s.options.maxSeconds));
        field("files", "{\"master\": \"master.wav\", \"events\": \"events.jsonl\", \"levels\": \"levels.jsonl\", \"stems\": " + list(stems) + "}");
        field("mixer", "{\"path\": " + quoted(s.mixerPath) + ", \"groups\": " + s.mixerMap + ", \"parents\": " + s.mixerParents + "}");
        field("output_stage", std::string("{\"clipper\": ") + (AudioSystem::getConfig().outputStage == AudioOutputStage::Linear ? "\"linear\"" : "\"roundoff\"") + ", \"post_clip_scaler\": " + decimal(soloud.getPostClipScaler(), 6) + "}");
        field("counts", counts);
        field("events_dropped", std::to_string(s.eventsDropped));
        field("voices_peak", std::to_string(s.voicesPeak));
        field("warnings", list(s.warnings));
        out += "  \"error\": " + (s.failure.empty() ? std::string("null") : quoted(s.failure)) + "\n}\n";

        return io::writeFileAtomic(s.directory / "session.json", out).ok();
    }

}

// ---------------------------------------------------------------- lines

AudioCapture::Line::Line(const char* kind) {
    if (!captureSession) return;
    CaptureSession& s = *captureSession;
    s.counts[kind]++;
    if (s.lines >= MAX_EVENT_LINES) {
        s.eventsDropped++;
        return;
    }
    s.lines++;
    const uint64_t sample = sampleNow();
    out = "{\"n\":" + std::to_string(s.lines) + ",\"kind\":\"";
    out += kind;
    out += "\",\"sample\":" + std::to_string(sample) + ",\"t\":";
    jsonNumber(out, (double)sample / s.rate, 6);
    out += ",\"frame\":" + std::to_string(s.frames) + ",\"game_time\":";
    jsonNumber(out, Engine::getGameTime() - s.baseGameTime, 6);
}

AudioCapture::Line::~Line() {
    if (!captureSession || out.empty()) return;
    out += "}\n";
    captureSession->pendingEvents += out;
}

AudioCapture::Line& AudioCapture::Line::text(const char* key, std::string_view value) {
    if (out.empty()) return *this;
    jsonKey(out, key);
    jsonQuoted(out, value);
    return *this;
}

AudioCapture::Line& AudioCapture::Line::number(const char* key, double value, int decimals) {
    if (out.empty()) return *this;
    jsonKey(out, key);
    jsonNumber(out, value, decimals);
    return *this;
}

AudioCapture::Line& AudioCapture::Line::integer(const char* key, int64_t value) {
    if (out.empty()) return *this;
    jsonKey(out, key);
    out += std::to_string(value);
    return *this;
}

AudioCapture::Line& AudioCapture::Line::flag(const char* key, bool value) {
    if (out.empty()) return *this;
    jsonKey(out, key);
    out += value ? "true" : "false";
    return *this;
}

AudioCapture::Line& AudioCapture::Line::null(const char* key) {
    if (out.empty()) return *this;
    jsonKey(out, key);
    out += "null";
    return *this;
}

AudioCapture::Line& AudioCapture::Line::raw(const char* key, std::string_view json) {
    if (out.empty()) return *this;
    jsonKey(out, key);
    out.append(json);
    return *this;
}

AudioCapture::Line& AudioCapture::Line::texts(const char* key, const std::vector<std::string>& values) {
    if (out.empty()) return *this;
    jsonKey(out, key);
    out += '[';
    for (size_t i = 0; i < values.size(); i++) {
        if (i) out += ',';
        jsonQuoted(out, values[i]);
    }
    out += ']';
    return *this;
}

AudioCapture::Line& AudioCapture::Line::numbers(const char* key, const std::vector<float>& values, int decimals) {
    if (out.empty()) return *this;
    jsonKey(out, key);
    out += '[';
    for (size_t i = 0; i < values.size(); i++) {
        if (i) out += ',';
        jsonNumber(out, values[i], decimals);
    }
    out += ']';
    return *this;
}

AudioCapture::Line& AudioCapture::Line::entity(const Scene* scene, uint32_t entityId, const char* pathKey, const char* idKey) {
    const std::string path = AudioCapture::entityPath(scene, entityId);
    if (path.empty()) return null(pathKey).null(idKey);
    return text(pathKey, path).integer(idKey, (int64_t)entityId);
}

AudioCapture::Line& AudioCapture::Line::scene(const Scene* owner) {
    return text("scene", sceneNameOf(owner));
}

std::string AudioCapture::entityPath(const Scene* constScene, uint32_t entity) {
    Scene* scene = const_cast<Scene*>(constScene);
    if (!scene || entity == NULL_ENTITY || !scene->isEntityCreated(entity)) return {};
    // names from the root joined by '/', "Name#N" for the N-th later sibling of that name
    std::vector<std::string> segments;
    Entity at = entity;
    for (int depth = 0; depth < 256 && at != NULL_ENTITY; depth++) {
        const std::string name = scene->getEntityName(at);
        Entity parent = NULL_ENTITY;
        int index = 0;
        if (const Transform* transform = scene->findComponent<Transform>(at)) {
            parent = transform->parent;
            auto transforms = scene->getComponentArray<Transform>();
            const size_t own = transforms->getIndex(at);
            for (size_t i = 0; i < own; i++) {
                if (transforms->getComponentFromIndex(i).parent == parent && scene->getEntityName(transforms->getEntity(i)) == name) index++;
            }
        }
        segments.push_back(index ? name + "#" + std::to_string(index) : name);
        at = parent;
    }
    std::string path;
    for (auto it = segments.rbegin(); it != segments.rend(); ++it) {
        if (!path.empty()) path += '/';
        path += *it;
    }
    return path;
}

// ---------------------------------------------------------------- the session

bool AudioCapture::isSessionId(const std::string& name) {
    if (name.size() != 18 || name[8] != '-' || name[15] != '-') return false;
    for (size_t i = 0; i < name.size(); i++) {
        if (i != 8 && i != 15 && (name[i] < '0' || name[i] > '9')) return false;
    }
    return true;
}

bool AudioCapture::begin(const AudioCaptureOptions& options, std::string* error) {
    end();
    const auto refuse = [error](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (!AudioSystem::init() || AudioSystem::getOutputMode() != AudioOutputMode::Deterministic) {
        return refuse("the deterministic audio driver is not open");
    }
    std::error_code ec;
    fs::create_directories(options.root, ec);
    if (ec) return refuse("cannot create " + options.root.string() + ": " + ec.message());
    pruneSessions(options.root, options.keep);
    const std::string id = newSessionId(options.root);
    if (id.empty()) return refuse("too many sessions this second in " + options.root.string());
    const fs::path directory = options.root / id;
    fs::create_directories(directory, ec);
    if (ec) return refuse("cannot create " + directory.string() + ": " + ec.message());

    auto created = std::make_unique<CaptureSession>();
    CaptureSession& s = *created;
    s.options = options;
    s.options.maxSeconds = std::clamp(options.maxSeconds, 1, 3600);
    s.id = id;
    s.directory = directory;
    s.startedUtc = utcText("%Y-%m-%dT%H:%M:%SZ", std::time(nullptr));
    s.rate = AudioSystem::getSoloud().getBackendSamplerate();
    s.baseFrame = AudioSystem::getMixedFrames();
    s.baseGameTime = Engine::getGameTime();
    s.maxFrames = (uint64_t)s.options.maxSeconds * s.rate;
    s.masterLevel.init(s.rate);
    s.mixerPath = AudioMixer::getPath();

    s.events.open(directory / "events.jsonl", std::ios::binary | std::ios::trunc);
    s.levels.open(directory / "levels.jsonl", std::ios::binary | std::ios::trunc);
    const bool opened = s.events.is_open() && s.levels.is_open() && s.master.open(directory / "master.wav", s.rate, 2);
    if (!opened) {
        created.reset();
        fs::remove_all(directory, ec);
        return refuse("cannot write in " + directory.string());
    }

    captureSession = created.release();
    lastSessionPath = directory;

    std::string mixerMap = "{";
    std::string parents = "{";
    for (const AudioMixer::GroupMeter& info : AudioMixer::getGroupMeters()) {
        addGroup(info);
        if (mixerMap.size() > 1) mixerMap += ", ";
        jsonQuoted(mixerMap, info.id);
        mixerMap += ": ";
        jsonQuoted(mixerMap, info.name);
        if (info.parent.empty()) continue;
        if (parents.size() > 1) parents += ", ";
        jsonQuoted(parents, info.name);
        parents += ": ";
        jsonQuoted(parents, info.parent);
    }
    s.mixerMap = mixerMap + "}";
    s.mixerParents = parents + "}";

    s.pendingEvents = std::string("{\"format\":\"") + FORMAT_EVENTS + "\",\"version\":" + std::to_string(FORMAT_VERSION) + "}\n";
    s.pendingLevels = std::string("{\"format\":\"") + FORMAT_LEVELS + "\",\"version\":" + std::to_string(FORMAT_VERSION) + "}\n";

    const unsigned int alreadyPlaying = AudioSystem::getVoiceCount();
    if (alreadyPlaying > 0) {
        s.notDeterministic.insert(std::to_string(alreadyPlaying) + " voice(s) started before the capture played into it");
    }

    if (!writeSessionFile("capturing", std::string())) {
        const fs::path failed = s.directory;
        delete captureSession;
        captureSession = nullptr;
        fs::remove_all(failed, ec);
        return refuse("cannot write session.json in " + failed.string());
    }

    active.store(true, std::memory_order_relaxed);
    AudioMarkers::setEnabled(true);
    AudioSystem::setVoiceObserver(observeVoice);
    MeterFilter::captureHook = meterHook;
    s.tap = AudioTap::subscribe("capture", tapChunk);
    AudioMarkers::sceneLoad(options.scene, "start");
    const std::string snapshot = AudioMixer::getCurrentSnapshot();
    if (!snapshot.empty()) {
        Line line("mixer");
        line.text("action", "start_snapshot").text("snapshot", snapshot);
    }
    return true;
}

void AudioCapture::frameMixed() {
    if (!captureSession) return;
    CaptureSession& s = *captureSession;
    s.frames++;
    const double fixedDelta = Engine::getFixedDeltatime();
    if (s.fixedDelta < 0.0) s.fixedDelta = fixedDelta;
    else if (s.fixedDelta != fixedDelta) s.notDeterministic.insert("the frame delta changed during the capture");

    if (!s.voices.empty()) {
        SoLoud::Soloud& soloud = AudioSystem::getSoloud();
        for (size_t i = 0; i < s.voices.size();) {
            if (soloud.isValidVoiceHandle(s.voices[i].handle)) {
                i++;
                continue;
            }
            stopLine(s.voices[i], "ended");
            s.voices.erase(s.voices.begin() + (std::ptrdiff_t)i);
        }
    }
    if (s.sinceLevel < 800) syncGroups();
    flushText(s.events, s.pendingEvents, "events.jsonl");
    flushText(s.levels, s.pendingLevels, "levels.jsonl");
    if (!s.failure.empty()) end();
}

void AudioCapture::end() {
    if (!captureSession) return;
    CaptureSession& s = *captureSession;
    active.store(false, std::memory_order_relaxed);
    AudioMarkers::setEnabled(false);
    AudioSystem::setVoiceObserver(nullptr);
    AudioTap::unsubscribe(s.tap);
    MeterFilter::captureHook = nullptr;

    for (const ActiveVoice& voice : s.voices) stopLine(voice, "scene");
    s.voices.clear();
    flushText(s.events, s.pendingEvents, "events.jsonl");
    flushText(s.levels, s.pendingLevels, "levels.jsonl");
    s.events.close();
    if (s.events.fail()) failCapture("could not write events.jsonl");
    s.levels.close();
    if (s.levels.fail()) failCapture("could not write levels.jsonl");

    std::string why;
    if (!s.master.close(&why)) failCapture(why);
    for (const std::unique_ptr<GroupCapture>& group : s.groups) {
        if (group->stem && !group->stem->close(&why, group->stemDelay)) failCapture(why);
    }

    const std::string endedUtc = utcText("%Y-%m-%dT%H:%M:%SZ", std::time(nullptr));
    if (!writeSessionFile(s.failure.empty() ? "complete" : "failed", endedUtc)) {
        Log::error("Audio capture: could not write session.json in %s", s.directory.string().c_str());
    }
    delete captureSession;
    captureSession = nullptr;
}

fs::path AudioCapture::lastSession() {
    return lastSessionPath;
}

std::string AudioCapture::sessionId() {
    return captureSession ? captureSession->id : std::string();
}

uint64_t AudioCapture::cursor() {
    return captureSession ? sampleNow() : 0;
}

void AudioCapture::mark(std::string_view label, bool fromLua) {
    if (!isActive()) return;
    Line line("marker");
    line.text("label", cutUtf8(label, MAX_LABEL_BYTES)).text("source", fromLua ? "lua" : "cpp");
}

#endif
