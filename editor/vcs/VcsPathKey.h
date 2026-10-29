// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Repository-relative keys computed lexically, after OpenEngine's VCSPathKey.h
// (Engine/Modules/VCSIntegration/Include/VCSIntegration/VCSPathKey.h, MIT, Copyright (c) 2026
// Gamecrafters Guild Limited and contributors; NOTICE.md): a status lookup runs per drawn row,
// and std::filesystem::relative stats every component of both operands.

#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace jasperengine::editor::vcs {

    // A path relative to the repository's working tree root, '/' separated, UTF-8, without a
    // leading "./" or a trailing '/' (the root itself is "")
    using RepoPath = std::string;

    // Windows and macOS file systems ignore case by default: their keys compare without it
    // (ASCII; the providers report the case the file has)
#if defined(_WIN32) || defined(__APPLE__)
    inline constexpr bool kCaseInsensitivePaths = true;
#else
    inline constexpr bool kCaseInsensitivePaths = false;
#endif

    inline char foldPathChar(char c) {
        if constexpr (kCaseInsensitivePaths) {
            if (c >= 'A' && c <= 'Z') return static_cast<char>(c - 'A' + 'a');
        }
        return c;
    }

    // <0, 0, >0: the order of every sorted vector of the snapshot, bytes (case-folded where
    // the file system ignores case). A directory's entries share the prefix "<dir>/", so they
    // form one run in this order.
    inline int comparePathKeys(std::string_view a, std::string_view b) {
        const size_t n = a.size() < b.size() ? a.size() : b.size();
        for (size_t i = 0; i < n; i++) {
            const char ca = foldPathChar(a[i]);
            const char cb = foldPathChar(b[i]);
            if (ca == cb) continue;
            return static_cast<unsigned char>(ca) < static_cast<unsigned char>(cb) ? -1 : 1;
        }
        return a.size() == b.size() ? 0 : (a.size() < b.size() ? -1 : 1);
    }

    struct PathKeyLess {
        using is_transparent = void;
        bool operator()(std::string_view a, std::string_view b) const { return comparePathKeys(a, b) < 0; }
    };

    inline bool pathKeysEqual(std::string_view a, std::string_view b) {
        return a.size() == b.size() && comparePathKeys(a, b) == 0;
    }

    // True when `path` is `dir` or lies below it ("" is the root, above everything)
    inline bool pathKeyIsWithin(std::string_view path, std::string_view dir) {
        if (dir.empty()) return true;
        if (path.size() < dir.size()) return false;
        if (comparePathKeys(path.substr(0, dir.size()), dir) != 0) return false;
        return path.size() == dir.size() || path[dir.size()] == '/';
    }

    // "a/b/c.png" -> "a/b"; "c.png" -> ""
    inline std::string_view parentKey(std::string_view path) {
        const size_t slash = path.rfind('/');
        return slash == std::string_view::npos ? std::string_view() : path.substr(0, slash);
    }

    // UTF-8 <-> path (u8path is deprecated in C++20)
    inline std::filesystem::path pathFromUtf8(std::string_view text) {
        return std::filesystem::path(std::u8string(text.begin(), text.end()));
    }
    inline std::string genericUtf8(const std::filesystem::path& path) {
        const std::u8string utf8 = path.generic_u8string();
        return std::string(utf8.begin(), utf8.end());
    }

    // The key of an absolute path below `root` (both lexically normal, the same form: what the
    // editor holds), computed without a syscall; false when the path is not below the root
    inline bool repoRelative(const std::filesystem::path& path, const std::filesystem::path& root, RepoPath& out) {
        if (path.empty() || root.empty()) return false;
        const std::filesystem::path relative = path.lexically_normal().lexically_relative(root.lexically_normal());
        if (relative.empty()) return false;
        if (*relative.begin() == "..") return false;
        std::string key = genericUtf8(relative);
        if (key == ".") key.clear();
        while (!key.empty() && key.back() == '/') key.pop_back();
        if (key.rfind("./", 0) == 0) key.erase(0, 2);
        out = std::move(key);
        return true;
    }

    // The absolute path of a key below `root`
    inline std::filesystem::path absoluteOf(const std::filesystem::path& root, std::string_view key) {
        if (key.empty()) return root;
        return root / pathFromUtf8(key);
    }

}
