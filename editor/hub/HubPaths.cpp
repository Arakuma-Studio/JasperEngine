// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// isValidCatalogId and isSafeRelativePath are copied from OpenEngine's
// Apps/Editor/Source/Projects/ProjectCatalog.cpp (IsValidCatalogId, IsSafeRelativePath):
//
//   MIT License
//   Copyright (c) 2026 Gamecrafters Guild Limited and contributors
//
//   Permission is hereby granted, free of charge, to any person obtaining a copy of this software
//   and associated documentation files (the "Software"), to deal in the Software without
//   restriction, including without limitation the rights to use, copy, modify, merge, publish,
//   distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
//   Software is furnished to do so, subject to the following conditions: The above copyright
//   notice and this permission notice shall be included in all copies or substantial portions of
//   the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
//   PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
//   LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
//   OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
//   DEALINGS IN THE SOFTWARE.
//
// See NOTICE.md. The rest of the file is JasperEngine's.

#include "hub/HubPaths.h"

#include <cctype>

namespace fs = std::filesystem;

namespace jasperengine::editor::hub {

// Manifest ids name cache files and staging directories, so they must be
// plain slugs — anything else could escape those directories.
bool isValidCatalogId(const std::string& id)
{
    if (id.empty() || id.size() > 64)
        return false;
    for (const char c : id)
    {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok)
            return false;
    }
    return true;
}

// Source paths and relative thumbnail refs are joined under directories we
// own; reject traversal, absolute forms, and option-lookalikes ('-' prefix
// would be parsed as a git flag).
bool isSafeRelativePath(const std::string& path)
{
    if (path.empty() || path.front() == '-' ||
        path.find('\\') != std::string::npos || path.find(':') != std::string::npos)
        return false;
    if (std::filesystem::path(path).is_absolute())
        return false;
    size_t start = 0;
    while (start <= path.size())
    {
        const size_t end = path.find('/', start);
        const std::string segment =
            path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (segment.empty() || segment == "." || segment == "..")
            return false;
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return true;
}

std::string slugify(std::string_view name) {
    std::string slug;
    bool dash = false;
    for (const char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u)) {
            if (dash && !slug.empty()) slug.push_back('-');
            slug.push_back(static_cast<char>(std::tolower(u)));
            dash = false;
        } else {
            dash = true;
        }
        if (slug.size() >= 64) break;
    }
    return slug;
}

bool isSafeArchiveEntry(std::string_view name, std::string& why) {
    std::string entry(name);
    // a directory entry: the same rules without its trailing '/'
    while (!entry.empty() && entry.back() == '/') entry.pop_back();
    if (entry.empty()) {
        why = "an entry without a name";
        return false;
    }
    if (entry.find('\\') != std::string::npos) {
        why = "\"" + std::string(name) + "\" has a backslash";
        return false;
    }
    if (entry.front() == '/' || (entry.size() >= 2 && entry[1] == ':')) {
        why = "\"" + std::string(name) + "\" is an absolute path";
        return false;
    }
    size_t start = 0;
    while (start <= entry.size()) {
        const size_t end = entry.find('/', start);
        const std::string segment = entry.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (segment == "..") {
            why = "\"" + std::string(name) + "\" leaves the folder (..)";
            return false;
        }
        if (segment.empty() || segment == ".") {
            why = "\"" + std::string(name) + "\" has an empty or '.' part";
            return false;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}

bool isAcceptedGitUrl(std::string_view url, std::string& why) {
    if (url.empty()) {
        why = "no URL";
        return false;
    }
    if (url.front() == '-') {
        why = "a URL cannot start with '-'";
        return false;
    }
    for (const char c : url) {
        if (static_cast<unsigned char>(c) < 32 || c == ' ') {
            why = "a URL has no spaces or control characters";
            return false;
        }
    }
    auto starts = [&](std::string_view prefix) { return url.substr(0, prefix.size()) == prefix; };
    if (starts("ext::") || url.find("::") != std::string_view::npos) {
        why = "remote helpers (\"<transport>::\") are refused";
        return false;
    }
    if (starts("file:") ) {
        why = "file:// URLs are refused";
        return false;
    }
    if (starts("https://") || starts("ssh://")) {
        const std::string_view rest = url.substr(url.find("://") + 3);
        if (rest.empty() || rest.front() == '-' || rest.front() == '/') {
            why = "the URL has no host";
            return false;
        }
        return true;
    }
    // scp-like: user@host:path (the host before the first ':', no '/' before it)
    const size_t at = url.find('@');
    const size_t colon = url.find(':');
    if (at != std::string_view::npos && colon != std::string_view::npos && at > 0 && colon > at + 1 && colon + 1 < url.size() &&
        url.substr(0, colon).find('/') == std::string_view::npos) {
        return true;
    }
    why = "only https://, ssh:// and user@host:path URLs are accepted";
    return false;
}

std::string folderName(std::string_view projectName) {
    std::string out;
    for (const char c : projectName) {
        const bool bad = c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' ||
                         static_cast<unsigned char>(c) < 32;
        if (!bad) out.push_back(c);
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    while (!out.empty() && (out.front() == ' ' || out.front() == '.')) out.erase(out.begin());
    return out;
}

std::string targetProblem(const fs::path& dir) {
    std::error_code ec;
    if (dir.empty()) return "No location";
    if (!fs::exists(dir, ec)) return std::string();
    if (!fs::is_directory(dir, ec)) return dir.string() + " is a file";
    if (fs::directory_iterator(dir, ec) != fs::directory_iterator()) return dir.filename().string() + " exists and is not empty";
    return std::string();
}

}
