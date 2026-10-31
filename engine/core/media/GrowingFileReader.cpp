// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "GrowingFileReader.h"

#include <algorithm>
#include <chrono>
#include <thread>

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    #include <fcntl.h>
    #include <io.h>
#endif

using namespace jasperengine;

namespace {

    bool seekTo(FILE* file, int64_t offset){
#if defined(_WIN32)
        return _fseeki64(file, offset, SEEK_SET) == 0;
#else
        return fseeko(file, (off_t)offset, SEEK_SET) == 0;
#endif
    }

    // a snapshot of what has arrived: never waits
    class SnapshotReader : public MediaByteSource {
    public:
        SnapshotReader(std::shared_ptr<GrowingFileState> state, std::string name): state(std::move(state)), name(std::move(name)){}
        ~SnapshotReader() override{
            if (file)
                fclose(file);
        }
        int64_t readAt(int64_t offset, void* dst, size_t size) override{
            if (offset < 0 || state->isFailed())
                return -1;
            const bool complete = state->isComplete();
            const int64_t readable = complete ? state->getTotal() : state->getAvailable();
            if (offset >= readable || size == 0)
                return 0;
            std::lock_guard<std::mutex> lock(mutex);
            if (!file)
                file = GrowingFileReader::openShared(complete ? state->getFinalPath() : state->getPartPath());
            if (!file || !seekTo(file, offset))
                return -1;
            const size_t wanted = (size_t)std::min<int64_t>((int64_t)size, readable - offset);
            return (int64_t)fread(dst, 1, wanted, file);
        }
        int64_t size() const override{
            return state->isComplete() ? state->getTotal() : state->getAvailable();
        }
        const std::string& path() const override{ return name; }
        bool isGrowing() const override{ return !state->isComplete(); }

    private:
        std::shared_ptr<GrowingFileState> state;
        std::string name;
        std::mutex mutex;
        FILE* file = nullptr;
    };

}

// ---------------------------------------------------------------- GrowingFileState

GrowingFileState::GrowingFileState(std::string part, std::string final): partPath(std::move(part)), finalPath(std::move(final)){
}

void GrowingFileState::update(int64_t bytes, int64_t size){
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (done || failed)
            return;
        available = std::max<int64_t>(bytes, 0);
        if (size >= 0)
            total = size;
    }
    changed.notify_all();
}

void GrowingFileState::complete(int64_t size){
    {
        std::lock_guard<std::mutex> lock(mutex);
        done = true;
        failed = false;
        total = size;
        available = size;
    }
    changed.notify_all();
}

void GrowingFileState::fail(const std::string& message){
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (done)
            return;
        failed = true;
        error = message;
    }
    changed.notify_all();
}

int64_t GrowingFileState::getAvailable() const{
    std::lock_guard<std::mutex> lock(mutex);
    return available;
}

int64_t GrowingFileState::getTotal() const{
    std::lock_guard<std::mutex> lock(mutex);
    return total;
}

bool GrowingFileState::isComplete() const{
    std::lock_guard<std::mutex> lock(mutex);
    return done;
}

bool GrowingFileState::isFailed() const{
    std::lock_guard<std::mutex> lock(mutex);
    return failed;
}

std::string GrowingFileState::getError() const{
    std::lock_guard<std::mutex> lock(mutex);
    return error;
}

// ---------------------------------------------------------------- GrowingFileReader

GrowingFileReader::GrowingFileReader(std::shared_ptr<GrowingFileState> fileState, std::string readerName):
    state(std::move(fileState)), name(std::move(readerName)){
}

GrowingFileReader::~GrowingFileReader(){
    abort();
    std::lock_guard<std::mutex> lock(fileMutex);
    if (file)
        fclose(file);
}

FILE* GrowingFileReader::openShared(const std::string& systemPath){
#if defined(_WIN32)
    // the downloader appends to the file and renames it when done: both need our handle to share
    const int size = MultiByteToWideChar(CP_UTF8, 0, systemPath.data(), (int)systemPath.size(), nullptr, 0);
    if (size <= 0)
        return nullptr;
    std::wstring wide((size_t)size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, systemPath.data(), (int)systemPath.size(), &wide[0], size);
    HANDLE handle = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return nullptr;
    const int fd = _open_osfhandle((intptr_t)handle, _O_RDONLY | _O_BINARY);
    if (fd < 0){
        CloseHandle(handle);
        return nullptr;
    }
    FILE* file = _fdopen(fd, "rb");
    if (!file)
        _close(fd);
    return file;
#else
    return fopen(systemPath.c_str(), "rb");
#endif
}

int64_t GrowingFileReader::readAt(int64_t offset, void* dst, size_t size){
    if (offset < 0)
        return -1;
    if (size == 0)
        return 0;
    int64_t readable = 0;
    bool complete = false;
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        // the bytes asked for, or the end of a finished file
        state->changed.wait(lock, [&]{
            return aborted || state->failed || state->done || state->available >= offset + (int64_t)size ||
                   (state->total >= 0 && state->available >= state->total);
        });
        if (aborted || state->failed)
            return -1;
        complete = state->done;
        readable = complete ? state->total : state->available;
    }
    if (offset >= readable)
        return 0;
    const size_t wanted = (size_t)std::min<int64_t>((int64_t)size, readable - offset);

    std::lock_guard<std::mutex> lock(fileMutex);
    if (!file){
        // the .part while it grows; the finished file once renamed (a handle on the .part stays
        // valid across the rename)
        file = openShared(complete ? state->getFinalPath() : state->getPartPath());
        if (!file && !complete && state->isComplete())
            file = openShared(state->getFinalPath());
        if (!file)
            return -1;
    }
    if (!seekTo(file, offset))
        return -1;
    const size_t got = fread(dst, 1, wanted, file);
    if (got < wanted)
        clearerr(file);
    return (int64_t)got;
}

int64_t GrowingFileReader::size() const{
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->total;
}

const std::string& GrowingFileReader::path() const{
    return name;
}

bool GrowingFileReader::isGrowing() const{
    return !state->isComplete();
}

int64_t GrowingFileReader::availableBytes() const{
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->done ? state->total : state->available;
}

void GrowingFileReader::abort(){
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        aborted = true;
    }
    state->changed.notify_all();
}

std::unique_ptr<MediaByteSource> GrowingFileReader::snapshot(std::shared_ptr<GrowingFileState> state, std::string name){
    return std::make_unique<SnapshotReader>(std::move(state), std::move(name));
}
