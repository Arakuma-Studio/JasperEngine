// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AudioWaveform.h"

#include "EditorHost.h"
#include "Project.h"
#include "io/AtomicFile.h"
#include "util/SHA1.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

// "JEWF", then version, channels, sample rate (u32), frames (u64), columns (u32), then the peaks
// (int16), every number little endian
const char MAGIC[4] = {'J', 'E', 'W', 'F'};
const size_t HEADER_BYTES = 4 + 4 + 4 + 4 + 8 + 4;

void putU32(std::string& out, uint32_t value) {
    for (int i = 0; i < 4; i++) out.push_back((char)((value >> (8 * i)) & 0xFF));
}

void putU64(std::string& out, uint64_t value) {
    for (int i = 0; i < 8; i++) out.push_back((char)((value >> (8 * i)) & 0xFF));
}

uint64_t getLE(const std::string& in, size_t at, int bytes) {
    uint64_t value = 0;
    for (int i = 0; i < bytes; i++) value |= (uint64_t)(unsigned char)in[at + i] << (8 * i);
    return value;
}

int16_t toInt16(float sample) {
    const double clamped = std::max(-1.0, std::min(1.0, (double)sample));
    return (int16_t)std::lround(clamped * 32767.0);
}

bool readBytes(const fs::path& file, std::string& bytes) {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    const std::streamoff size = in ? (std::streamoff)in.tellg() : -1;
    if (size < 0) return false;
    bytes.assign((size_t)size, '\0');
    in.seekg(0);
    return size == 0 || (bool)in.read(&bytes[0], size);
}

// SHA-1 by path, size and modification time: an unchanged file is read once per session
struct Memo {
    uintmax_t size = 0;
    fs::file_time_type mtime;
    std::string sha1;
};
std::mutex memoMutex;
std::unordered_map<std::string, Memo> memos;

} // namespace

AudioWaveform::Data AudioWaveform::compute(const AudioAnalysis::Signal& signal) {
    Data data;
    data.channels = signal.channels;
    data.sampleRate = signal.sampleRate;
    data.frames = signal.frames;
    if (signal.channels == 0 || signal.frames == 0) {
        data.channels = 0;
        return data;
    }
    data.peaks.assign((size_t)signal.channels * COLUMNS * 2, 0);
    const size_t frames = signal.frames;
    for (unsigned int c = 0; c < signal.channels; c++) {
        const float* samples = signal.channel(c);
        for (int column = 0; column < COLUMNS; column++) {
            size_t start = (size_t)((uint64_t)column * frames / COLUMNS);
            size_t end = (size_t)((uint64_t)(column + 1) * frames / COLUMNS);
            start = std::min(start, frames - 1);
            end = std::max(end, start + 1);
            float lo = samples[start], hi = samples[start];
            for (size_t i = start + 1; i < end; i++) {
                lo = std::min(lo, samples[i]);
                hi = std::max(hi, samples[i]);
            }
            data.peaks[((size_t)c * COLUMNS + column) * 2] = toInt16(lo);
            data.peaks[((size_t)c * COLUMNS + column) * 2 + 1] = toInt16(hi);
        }
    }
    return data;
}

fs::path AudioWaveform::cacheFile(const fs::path& cacheDirectory, const std::string& sha1) {
    return cacheDirectory / (sha1 + "-wave-v" + std::to_string(VERSION) + ".bin");
}

bool AudioWaveform::read(const fs::path& cacheDirectory, const std::string& sha1, Data& out) {
    if (cacheDirectory.empty() || sha1.size() != 40) return false;
    std::string bytes;
    if (!readBytes(cacheFile(cacheDirectory, sha1), bytes) || bytes.size() < HEADER_BYTES) return false;
    if (std::memcmp(bytes.data(), MAGIC, 4) != 0 || getLE(bytes, 4, 4) != (uint64_t)VERSION) return false;
    Data data;
    data.channels = (unsigned int)getLE(bytes, 8, 4);
    data.sampleRate = (unsigned int)getLE(bytes, 12, 4);
    data.frames = getLE(bytes, 16, 8);
    const uint64_t columns = getLE(bytes, 24, 4);
    if (columns != (uint64_t)COLUMNS || data.channels == 0 || data.channels > 64) return false;
    const size_t count = (size_t)data.channels * COLUMNS * 2;
    if (bytes.size() != HEADER_BYTES + count * 2) return false;
    data.peaks.resize(count);
    for (size_t i = 0; i < count; i++) data.peaks[i] = (int16_t)(uint16_t)getLE(bytes, HEADER_BYTES + i * 2, 2);
    out = std::move(data);
    return true;
}

bool AudioWaveform::write(const fs::path& cacheDirectory, const std::string& sha1, const Data& data) {
    if (cacheDirectory.empty() || sha1.size() != 40 || data.empty()) return false;
    std::string bytes(MAGIC, 4);
    putU32(bytes, (uint32_t)VERSION);
    putU32(bytes, data.channels);
    putU32(bytes, data.sampleRate);
    putU64(bytes, data.frames);
    putU32(bytes, (uint32_t)COLUMNS);
    for (int16_t value : data.peaks) {
        const uint16_t bits = (uint16_t)value;
        bytes.push_back((char)(bits & 0xFF));
        bytes.push_back((char)(bits >> 8));
    }
    io::AtomicWriteOptions options;
    options.durability = io::Durability::Cache;
    return io::writeFileAtomic(cacheFile(cacheDirectory, sha1), bytes, options).ok();
}

bool AudioWaveform::ofBytes(const std::string& bytes, const fs::path& cacheDirectory, Data& out, std::string& error) {
    const std::string sha1 = SHA1::hash(bytes);
    if (read(cacheDirectory, sha1, out)) return true;
    AudioAnalysis::Signal signal;
    if (!AudioAnalysis::decode(bytes, signal, error)) return false;
    out = compute(signal);
    if (out.empty()) {
        error = "the file holds no audio";
        return false;
    }
    write(cacheDirectory, sha1, out);
    return true;
}

bool AudioWaveform::ofFile(const fs::path& file, const fs::path& cacheDirectory, Data& out, std::string& error) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(file, ec);
    if (ec) {
        error = "the file can't be read";
        return false;
    }
    if (size > AudioAnalysis::MAX_FILE_BYTES) {
        error = "larger than " + std::to_string(AudioAnalysis::MAX_FILE_BYTES / (1024 * 1024)) + " MB";
        return false;
    }
    const fs::file_time_type mtime = fs::last_write_time(file, ec);
    {
        std::lock_guard<std::mutex> lock(memoMutex);
        auto it = memos.find(file.string());
        if (it != memos.end() && it->second.size == size && it->second.mtime == mtime && read(cacheDirectory, it->second.sha1, out)) {
            return true;
        }
    }
    std::string bytes;
    if (!readBytes(file, bytes)) {
        error = "the file can't be read";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(memoMutex);
        memos[file.string()] = Memo{size, mtime, SHA1::hash(bytes)};
    }
    return ofBytes(bytes, cacheDirectory, out, error);
}

// -- the windows' queue ---------------------------------------------------------------------------

namespace {

struct Key {
    // a file's absolute path
    std::string name;
    uintmax_t size = 0;
    int64_t modified = 0;

    bool operator==(const Key& other) const { return name == other.name && size == other.size && modified == other.modified; }
};

struct Job {
    Key key;
    fs::path file;
    fs::path cacheDirectory;
};

struct Entry {
    Key key;
    bool done = false;
    AudioWaveform::Result result;
};

// The overviews, the queue and the worker, behind one mutex; made at the first request (after
// the editor host, so destroyed before it), its destructor stops the worker and joins a job
struct State {
    std::mutex mutex;
    std::condition_variable wake;
    std::thread thread;
    bool stopping = false;
    fs::path projectDirectory;
    // newest first
    std::deque<Entry> entries;
    std::deque<Job> queue;

    ~State() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
            queue.clear();
        }
        wake.notify_all();
        if (thread.joinable()) thread.join();
    }
};

State& state() {
    static State instance;
    return instance;
}

void work() {
    State& s = state();
    std::unique_lock<std::mutex> lock(s.mutex);
    while (true) {
        s.wake.wait(lock, [&]() { return s.stopping || !s.queue.empty(); });
        if (s.stopping) return;
        const Job job = std::move(s.queue.front());
        s.queue.pop_front();
        lock.unlock();
        AudioWaveform::Result result;
        result.pending = false;
        auto data = std::make_shared<AudioWaveform::Data>();
        std::string error;
        if (AudioWaveform::ofFile(job.file, job.cacheDirectory, *data, error)) result.data = data;
        else result.error = error;
        lock.lock();
        if (s.stopping) return;
        auto found = std::find_if(s.entries.begin(), s.entries.end(), [&](const Entry& e) { return e.key == job.key; });
        if (found == s.entries.end()) continue;  // superseded, or another project since
        found->done = true;
        found->result = result;
        // the editor loop draws a frame for it (the task itself has nothing to do)
        getEditorHost().enqueueMainThreadTask([]() {});
    }
}

AudioWaveform::Result request(const Project* project, Job job) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.projectDirectory != project->getProjectPath()) {
        // another project: nothing of the last one is kept
        s.entries.clear();
        s.queue.clear();
        s.projectDirectory = project->getProjectPath();
    }
    auto found = std::find_if(s.entries.begin(), s.entries.end(), [&](const Entry& e) { return e.key == job.key; });
    if (found != s.entries.end()) {
        if (found->done) return found->result;
        return AudioWaveform::Result();
    }
    // the file's older overview goes; the request goes first in the queue
    s.entries.erase(std::remove_if(s.entries.begin(), s.entries.end(), [&](const Entry& e) { return e.key.name == job.key.name; }),
                    s.entries.end());
    Entry entry;
    entry.key = job.key;
    s.entries.push_front(entry);
    while (s.entries.size() > AudioWaveform::KEPT) s.entries.pop_back();
    s.queue.erase(std::remove_if(s.queue.begin(), s.queue.end(), [&](const Job& queued) { return queued.key.name == job.key.name; }),
                  s.queue.end());
    s.queue.push_front(std::move(job));
    if (!s.thread.joinable()) s.thread = std::thread(work);
    s.wake.notify_one();
    return AudioWaveform::Result();
}

} // namespace

AudioWaveform::Result AudioWaveform::get(const Project* project, const fs::path& file) {
    Result none;
    none.pending = false;
    if (!project || file.empty()) return none;
    std::error_code ec;
    const uintmax_t size = fs::file_size(file, ec);
    if (ec) {
        none.error = "the file can't be read";
        return none;
    }
    const auto modified = fs::last_write_time(file, ec);
    Job job;
    job.key.name = file.lexically_normal().string();
    job.key.size = size;
    job.key.modified = ec ? 0 : (int64_t)modified.time_since_epoch().count();
    job.file = file.lexically_normal();
    job.cacheDirectory = AudioAnalysis::cacheDirectory(project->getProjectPath());
    return request(project, std::move(job));
}

void AudioWaveform::clear() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.entries.clear();
    s.queue.clear();
    s.projectDirectory.clear();
}
