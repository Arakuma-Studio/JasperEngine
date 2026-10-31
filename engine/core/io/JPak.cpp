// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "JPak.h"

#include "System.h"
#include "util/Fnv.h"

#include <algorithm>
#include <cstring>
#include <limits>

#if !defined(_WIN32)
#include <unistd.h>
#endif

using namespace jasperengine;

namespace {

    uint32_t le32(const uint8_t* p) {
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }

    uint64_t le64(const uint8_t* p) {
        uint64_t value = 0;
        for (int i = 0; i < 8; i++) value |= (uint64_t)p[i] << (i * 8);
        return value;
    }

    bool seekTo(std::FILE* file, uint64_t offset) {
#ifdef _WIN32
        return _fseeki64(file, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
        return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
    }

    bool fileSize(std::FILE* file, uint64_t& size) {
#ifdef _WIN32
        if (_fseeki64(file, 0, SEEK_END) != 0) return false;
        const __int64 end = _ftelli64(file);
#else
        if (fseeko(file, 0, SEEK_END) != 0) return false;
        const off_t end = ftello(file);
#endif
        if (end < 0) return false;
        size = static_cast<uint64_t>(end);
        return seekTo(file, 0);
    }

    void fail(std::string* error, const std::string& message) {
        if (error) *error = message;
    }

}

JPak::JPak() = default;

JPak::~JPak() {
    close();
}

void JPak::close() {
    if (file) std::fclose(file);
    file = nullptr;
    memory = {};
    list.clear();
    byPath.clear();
    verified.reset();
    open = false;
}

std::string JPak::checkPath(std::string_view path) {
    if (path.empty()) return "empty path";
    if (path.front() == '/') return "absolute path";
    if (path.size() > 1 && path[1] == ':') return "absolute path";
    if (path.find('\\') != std::string_view::npos) return "backslash in path";
    if (path.find('\0') != std::string_view::npos) return "NUL in path";
    size_t start = 0;
    while (start <= path.size()) {
        const size_t end = path.find('/', start);
        const std::string_view part = path.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (part.empty()) return "empty path part";
        if (part == "." || part == "..") return "'" + std::string(part) + "' in path";
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return "";
}

std::string JPak::foldCase(std::string_view path) {
    std::string folded(path);
    std::transform(folded.begin(), folded.end(), folded.begin(),
                   [](unsigned char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); });
    return folded;
}

bool JPak::readAt(uint64_t offset, void* out, uint64_t size) const {
    if (size == 0) return true;
    if (!memory.empty()) {
        if (offset > memory.size() || size > memory.size() - offset) return false;
        std::memcpy(out, memory.data() + offset, static_cast<size_t>(size));
        return true;
    }
    if (!file) return false;
#if !defined(_WIN32)
    const int fd = fileno(file);
    if (fd >= 0) {
        uint8_t* dst = static_cast<uint8_t*>(out);
        uint64_t done = 0;
        while (done < size) {
            const ssize_t got = ::pread(fd, dst + done, static_cast<size_t>(size - done), static_cast<off_t>(offset + done));
            if (got <= 0) return false;
            done += static_cast<uint64_t>(got);
        }
        return true;
    }
#endif
    // a FILE without a descriptor (an Android asset): one reader at a time
    std::lock_guard<std::mutex> lock(fileMutex);
    return seekTo(file, offset) && std::fread(out, 1, static_cast<size_t>(size), file) == size;
}

bool JPak::parse(uint64_t totalSize, std::string* error) {
    uint8_t header[HEADER_BYTES];
    if (totalSize < HEADER_BYTES || !readAt(0, header, HEADER_BYTES)) {
        fail(error, "too small to be a .jpak");
        return false;
    }
    if (std::memcmp(header, MAGIC, sizeof(MAGIC)) != 0) {
        fail(error, "not a .jpak (bad magic)");
        return false;
    }
    const uint32_t version = le32(header + 8);
    if (version != VERSION) {
        fail(error, "a .jpak of format version " + std::to_string(version) + "; this player reads version " +
                    std::to_string(VERSION) + " (build it with the editor of this player's version)");
        return false;
    }
    const uint32_t count = le32(header + 12);
    const uint64_t indexBytes = le64(header + 16);
    if (indexBytes > totalSize - HEADER_BYTES || indexBytes > std::numeric_limits<uint32_t>::max()) {
        fail(error, "corrupt .jpak index size");
        return false;
    }
    std::vector<uint8_t> index(static_cast<size_t>(indexBytes));
    if (!readAt(HEADER_BYTES, index.data(), indexBytes)) {
        fail(error, "cannot read the .jpak index");
        return false;
    }
    const uint64_t dataStart = HEADER_BYTES + indexBytes;
    std::vector<Entry> entries;
    entries.reserve(count);
    std::unordered_map<std::string, size_t> paths;
    std::unordered_map<std::string, std::string> folded;
    size_t at = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (index.size() - at < 4) {
            fail(error, "truncated .jpak index");
            return false;
        }
        const uint32_t length = le32(&index[at]);
        at += 4;
        if (length == 0 || index.size() - at < (size_t)length + 24) {
            fail(error, "truncated .jpak index");
            return false;
        }
        Entry entry;
        entry.path.assign(reinterpret_cast<const char*>(&index[at]), length);
        at += length;
        entry.offset = le64(&index[at]);
        entry.size = le64(&index[at + 8]);
        entry.fnv1a64 = le64(&index[at + 16]);
        at += 24;
        const std::string illegal = checkPath(entry.path);
        if (!illegal.empty()) {
            fail(error, "illegal .jpak entry \"" + entry.path + "\": " + illegal);
            return false;
        }
        if (entry.offset < dataStart || entry.offset % ALIGNMENT != 0 || entry.offset > totalSize ||
            entry.size > totalSize - entry.offset) {
            fail(error, ".jpak entry \"" + entry.path + "\" lies outside the pack");
            return false;
        }
        const std::string key = foldCase(entry.path);
        auto clash = folded.find(key);
        if (clash != folded.end()) {
            fail(error, ".jpak entries \"" + clash->second + "\" and \"" + entry.path + "\" differ only in case");
            return false;
        }
        folded.emplace(key, entry.path);
        paths.emplace(entry.path, entries.size());
        entries.push_back(std::move(entry));
    }
    if (at != index.size()) {
        fail(error, ".jpak index size does not match its entries");
        return false;
    }
    list = std::move(entries);
    byPath = std::move(paths);
    verified.reset(new std::atomic<bool>[list.size()]);
    for (size_t i = 0; i < list.size(); i++) verified[i].store(false, std::memory_order_relaxed);
    open = true;
    return true;
}

bool JPak::openFile(const std::filesystem::path& path, std::string* error) {
    close();
    file = System::hasInstance() ? System::instance().platformFopen(path.string().c_str(), "rb")
                                 : std::fopen(path.string().c_str(), "rb");
    if (!file) {
        fail(error, "cannot open " + path.string());
        return false;
    }
    uint64_t size = 0;
    if (!fileSize(file, size)) {
        fail(error, "cannot size " + path.string());
        close();
        return false;
    }
    if (!parse(size, error)) {
        close();
        return false;
    }
    return true;
}

bool JPak::openMemory(std::span<const uint8_t> bytes, std::string* error) {
    close();
    memory = bytes;
    if (memory.empty()) {
        fail(error, "empty .jpak");
        return false;
    }
    if (!parse(memory.size(), error)) {
        close();
        return false;
    }
    return true;
}

const JPak::Entry* JPak::find(std::string_view path) const {
    auto it = byPath.find(std::string(path));
    return it == byPath.end() ? nullptr : &list[it->second];
}

bool JPak::read(const Entry& entry, uint8_t* out, std::string* error) const {
    if (!readAt(entry.offset, out, entry.size)) {
        fail(error, "cannot read .jpak entry \"" + entry.path + "\"");
        return false;
    }
    if (fnv1a64(out, static_cast<size_t>(entry.size)) != entry.fnv1a64) {
        fail(error, ".jpak entry \"" + entry.path + "\" is corrupt (FNV-1a 64 mismatch)");
        return false;
    }
    return true;
}

bool JPak::read(std::string_view path, std::vector<uint8_t>& out, std::string* error) const {
    const Entry* entry = find(path);
    if (!entry) {
        fail(error, "no .jpak entry \"" + std::string(path) + "\"");
        return false;
    }
    out.resize(static_cast<size_t>(entry->size));
    if (!read(*entry, out.data(), error)) {
        out.clear();
        return false;
    }
    return true;
}

bool JPak::view(std::string_view path, std::span<const uint8_t>& out, std::string* error) const {
    if (memory.empty()) {
        fail(error, "a file-backed .jpak has no in-place view");
        return false;
    }
    auto it = byPath.find(std::string(path));
    if (it == byPath.end()) {
        fail(error, "no .jpak entry \"" + std::string(path) + "\"");
        return false;
    }
    const Entry& entry = list[it->second];
    const std::span<const uint8_t> bytes = memory.subspan(static_cast<size_t>(entry.offset), static_cast<size_t>(entry.size));
    if (!verified[it->second].load(std::memory_order_acquire)) {
        if (fnv1a64(bytes.data(), bytes.size()) != entry.fnv1a64) {
            fail(error, ".jpak entry \"" + entry.path + "\" is corrupt (FNV-1a 64 mismatch)");
            return false;
        }
        verified[it->second].store(true, std::memory_order_release);
    }
    out = bytes;
    return true;
}
