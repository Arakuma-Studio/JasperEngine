// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "MediaByteSource.h"

#include "System.h"
#include "io/FileData.h"
#include "io/ResourcePack.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>

using namespace jasperengine;

namespace {

    bool seekTo(FILE* file, int64_t offset){
#if defined(_WIN32)
        return _fseeki64(file, offset, SEEK_SET) == 0;
#else
        return fseeko(file, (off_t)offset, SEEK_SET) == 0;
#endif
    }

    int64_t tellEnd(FILE* file){
#if defined(_WIN32)
        if (_fseeki64(file, 0, SEEK_END) != 0)
            return -1;
        return _ftelli64(file);
#else
        if (fseeko(file, 0, SEEK_END) != 0)
            return -1;
        return (int64_t)ftello(file);
#endif
    }

    // a file through the platform's fopen (desktop files, user files, Android assets)
    class StdioSource : public MediaByteSource {
    public:
        StdioSource(FILE* file, std::string path, std::string systemPath, int64_t size):
            file(file), givenPath(std::move(path)), osPath(std::move(systemPath)), total(size){}
        ~StdioSource() override{
            if (file)
                fclose(file);
        }

        int64_t readAt(int64_t offset, void* dst, size_t size) override{
            if (offset < 0)
                return -1;
            if (offset >= total || size == 0)
                return 0;
            const size_t wanted = (size_t)std::min<int64_t>((int64_t)size, total - offset);
            std::lock_guard<std::mutex> lock(mutex);
            if (!seekTo(file, offset))
                return -1;
            const size_t got = fread(dst, 1, wanted, file);
            if (got < wanted && ferror(file))
                return -1;
            return (int64_t)got;
        }
        int64_t size() const override{ return total; }
        const std::string& path() const override{ return givenPath; }
        std::string systemPath() const override{ return osPath; }

    private:
        std::mutex mutex;
        FILE* file;
        std::string givenPath;
        std::string osPath;
        int64_t total;
    };

    class MemorySource : public MediaByteSource {
    public:
        MemorySource(std::vector<uint8_t> bytes, std::string name): data(std::move(bytes)), name(std::move(name)){}
        int64_t readAt(int64_t offset, void* dst, size_t size) override{
            if (offset < 0)
                return -1;
            if ((uint64_t)offset >= data.size())
                return 0;
            const size_t count = std::min(size, data.size() - (size_t)offset);
            std::memcpy(dst, data.data() + offset, count);
            return (int64_t)count;
        }
        int64_t size() const override{ return (int64_t)data.size(); }
        const std::string& path() const override{ return name; }

    private:
        std::vector<uint8_t> data;
        std::string name;
    };

}

std::unique_ptr<MediaByteSource> MediaByteSource::openFile(const std::string& path, std::string& error){
    if (path.empty()){
        error = "no file";
        return nullptr;
    }
    // a packed asset has no file on disk: its bytes in memory
    if (ResourcePack::contains(path)){
        std::vector<unsigned char> bytes;
        if (!ResourcePack::read(path, bytes)){
            error = "'" + path + "' could not be read from the resource pack";
            return nullptr;
        }
        return std::make_unique<MemorySource>(std::vector<uint8_t>(bytes.begin(), bytes.end()), path);
    }
    const std::string systemPath = FileData::getSystemPath(path);
    FILE* file = System::instance().platformFopen(systemPath.c_str(), "rb");
    if (!file){
        error = "'" + path + "' could not be read (no such file)";
        return nullptr;
    }
    const int64_t size = tellEnd(file);
    if (size <= 0){
        fclose(file);
        error = "'" + path + "' could not be read (" + std::string(size == 0 ? "empty file" : "no size") + ")";
        return nullptr;
    }
    // Android assets go through funopen (no file the OS media APIs can open by path)
#if defined(__ANDROID__)
    const std::string userData = System::instance().getUserDataPath();
    const bool onDisk = !userData.empty() && systemPath.compare(0, userData.size(), userData) == 0;
    return std::make_unique<StdioSource>(file, path, onDisk ? systemPath : std::string(), size);
#else
    return std::make_unique<StdioSource>(file, path, systemPath, size);
#endif
}

std::unique_ptr<MediaByteSource> MediaByteSource::openStdio(FILE* file, const std::string& name){
    if (!file)
        return nullptr;
    const int64_t size = tellEnd(file);
    if (size <= 0){
        fclose(file);
        return nullptr;
    }
    return std::make_unique<StdioSource>(file, name, std::string(), size);
}

std::unique_ptr<MediaByteSource> MediaByteSource::openMemory(std::vector<uint8_t> bytes, const std::string& name){
    return std::make_unique<MemorySource>(std::move(bytes), name);
}

bool MediaByteSource::readAll(MediaByteSource& source, std::vector<uint8_t>& out, size_t maxBytes){
    const int64_t total = source.size();
    if (total < 0 || (uint64_t)total > maxBytes)
        return false;
    out.resize((size_t)total);
    int64_t done = 0;
    while (done < total){
        const int64_t got = source.readAt(done, out.data() + done, (size_t)(total - done));
        if (got <= 0)
            return false;
        done += got;
    }
    return true;
}
