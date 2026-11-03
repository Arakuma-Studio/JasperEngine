// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "FileWatcher.h"

#include "backends/Backends.h"
#include "AtomicFile.h"

#include <algorithm>
#include <cctype>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

using namespace jasperengine;

namespace fs = std::filesystem;

namespace {

    std::string lowered(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    bool endsWith(const std::string& text, const char* suffix) {
        const size_t n = std::char_traits<char>::length(suffix);
        return text.size() >= n && text.compare(text.size() - n, n, suffix) == 0;
    }

    bool globAt(const std::string& p, size_t pi, const std::string& t, size_t ti) {
        while (pi < p.size()) {
            const char c = p[pi];
            if (c == '*') {
                const bool any = pi + 1 < p.size() && p[pi + 1] == '*';
                const size_t next = pi + (any ? 2 : 1);
                // "**/" also matches no folder at all
                if (any && next < p.size() && p[next] == '/' && globAt(p, next + 1, t, ti)) return true;
                for (size_t k = ti; k <= t.size(); k++) {
                    if (globAt(p, next, t, k)) return true;
                    if (k < t.size() && !any && t[k] == '/') break;
                }
                return false;
            }
            if (ti >= t.size()) return false;
            if (c != '?' && std::tolower(static_cast<unsigned char>(c)) != std::tolower(static_cast<unsigned char>(t[ti]))) return false;
            if (c == '?' && t[ti] == '/') return false;
            pi++;
            ti++;
        }
        return ti == t.size();
    }

}

const char* jasperengine::fileChangeKindName(FileChangeKind kind) {
    switch (kind) {
        case FileChangeKind::Added: return "added";
        case FileChangeKind::Modified: return "modified";
        case FileChangeKind::Removed: return "removed";
        case FileChangeKind::Moved: return "moved";
        case FileChangeKind::Rescan: return "rescan";
    }
    return "modified";
}

const char* jasperengine::watchBackendKindName(WatchBackendKind kind) {
    switch (kind) {
        case WatchBackendKind::Auto: return "auto";
        case WatchBackendKind::Native: return "native";
        case WatchBackendKind::Polling: return "polling";
        case WatchBackendKind::Off: return "off";
    }
    return "auto";
}

bool jasperengine::parseWatchBackendKind(const std::string& text, WatchBackendKind& out) {
    const std::string value = lowered(text);
    if (value == "auto") out = WatchBackendKind::Auto;
    else if (value == "native") out = WatchBackendKind::Native;
    else if (value == "polling") out = WatchBackendKind::Polling;
    else if (value == "off") out = WatchBackendKind::Off;
    else return false;
    return true;
}

bool watch::statPath(const fs::path& path, PathStat& out, bool identity) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
    ULARGE_INTEGER time;
    time.LowPart = data.ftLastWriteTime.dwLowDateTime;
    time.HighPart = data.ftLastWriteTime.dwHighDateTime;
    // 100 ns ticks since 1601-01-01 -> ns since 1970-01-01
    out.mtimeNs = (static_cast<int64_t>(time.QuadPart) - 116444736000000000LL) * 100;
    out.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    out.directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (out.directory) out.size = 0;
    out.identity = identity ? readFileIdentity(path) : FileIdentity{};
    return true;
#else
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return false;
#if defined(__APPLE__)
    out.mtimeNs = static_cast<int64_t>(st.st_mtimespec.tv_sec) * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
    out.mtimeNs = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
#endif
    out.directory = S_ISDIR(st.st_mode);
    out.size = out.directory ? 0 : static_cast<uint64_t>(st.st_size);
    out.identity = FileIdentity{};
    if (identity) {
        out.identity.volume = static_cast<uint64_t>(st.st_dev);
        out.identity.index = static_cast<uint64_t>(st.st_ino);
        out.identity.valid = true;
    }
    return true;
#endif
}

bool watch::isRacyStamp(int64_t mtimeNs) {
    const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return now - mtimeNs < std::chrono::duration_cast<std::chrono::nanoseconds>(RACY_STAMP_WINDOW).count();
}

bool watch::isExcludedName(const std::string& name) {
    if (name.empty() || name[0] == '.') return true;   // .jasperengine, .git, .vscode, .trash, .#lock...
    const std::string lower = lowered(name);
    return lower == "build" || lower == "node_modules" || lower == "trash";
}

bool watch::relativeTo(const fs::path& root, const fs::path& path, std::string& out) {
    const std::string base = pathToUtf8(root.lexically_normal());
    const std::string full = pathToUtf8(path.lexically_normal());
    std::string prefix = base;
    while (prefix.size() > 1 && prefix.back() == '/') prefix.pop_back();
    if (full.size() < prefix.size()) return false;
#ifdef _WIN32
    // drive letters and folder names are case-insensitive there
    if (lowered(full.substr(0, prefix.size())) != lowered(prefix)) return false;
#else
    if (full.compare(0, prefix.size(), prefix) != 0) return false;
#endif
    if (full.size() == prefix.size()) {
        out.clear();
        return true;
    }
    if (prefix.back() == '/') {
        out = full.substr(prefix.size());
    } else {
        if (full[prefix.size()] != '/') return false;
        out = full.substr(prefix.size() + 1);
    }
    while (!out.empty() && out.back() == '/') out.pop_back();
    return true;
}

bool watch::isExcludedPath(const fs::path& root, const fs::path& path) {
    std::string rel;
    if (!relativeTo(root, path, rel)) return true;
    size_t start = 0;
    while (start <= rel.size()) {
        const size_t slash = rel.find('/', start);
        const std::string part = rel.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (!part.empty() && (isExcludedName(part) || part == "..")) return true;
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return false;
}

bool watch::isTemporaryName(const std::string& filename) {
    if (filename.empty()) return false;
    if (io::isTemporarySiblingName(filename)) return true;   // the atomic writer's (S03)
    if (filename == "4913") return true;                       // vim's write probe
    if (filename.rfind(".#", 0) == 0) return true;             // emacs lock
    if (filename.back() == '~') return true;                   // backups (*~, *.kra~)
    const std::string lower = lowered(filename);
    return endsWith(lower, ".tmp") || endsWith(lower, ".swp") || endsWith(lower, ".swx") || endsWith(lower, ".crdownload") ||
           endsWith(filename, "___jb_tmp___") || endsWith(filename, "___jb_old___");
}

std::string watch::pathToUtf8(const fs::path& path) {
#if defined(__cpp_char8_t)
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
#else
    return path.generic_u8string();
#endif
}

fs::path watch::pathFromUtf8(const std::string& text) {
#if defined(__cpp_char8_t)
    return fs::path(std::u8string(text.begin(), text.end()));
#else
    return fs::u8path(text);
#endif
}

std::string watch::lowerExtension(const fs::path& path) {
    return lowered(pathToUtf8(path.extension()));
}

bool watch::globMatch(const std::string& pattern, const std::string& text) {
    return globAt(pattern, 0, text, 0);
}

bool FileWatcher::hasNative() {
#if (defined(__linux__) && !defined(__ANDROID__)) || defined(_WIN32) || defined(JASPERENGINE_WATCH_FSEVENTS)
    return true;
#else
    return false;
#endif
}

std::unique_ptr<FileWatcher> FileWatcher::createNative() {
#if defined(__linux__) && !defined(__ANDROID__)
    return watch::createInotifyBackend();
#elif defined(_WIN32)
    return watch::createRdcwBackend();
#elif defined(JASPERENGINE_WATCH_FSEVENTS)
    return watch::createFsEventsBackend();
#else
    return nullptr;
#endif
}

std::unique_ptr<FileWatcher> FileWatcher::createPolling(std::chrono::milliseconds interval) {
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__) || (defined(__APPLE__) && !defined(JASPERENGINE_WATCH_FSEVENTS))
    // players never watch (S05 R20): the Null backend stands in
    (void)interval;
    return watch::createNullBackend();
#else
    return watch::createPollingBackend(interval);
#endif
}

std::unique_ptr<FileWatcher> FileWatcher::createNull() {
    return watch::createNullBackend();
}
