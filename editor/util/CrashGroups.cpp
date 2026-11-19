// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CrashGroups.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace jasperengine::editor::crashgroups {

namespace {

    namespace fs = std::filesystem;

    // the frames and the first module line come before this (symbolicate.py reads as much)
    constexpr size_t kReadLimit = 256 << 10;

    struct Frame {
        std::string module;
        uint64_t offset = 0;
        std::string symbol;
    };

    bool isIdent(char c) {
        const unsigned char u = static_cast<unsigned char>(c);
        return (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || c == '_';
    }

    bool isAlpha(char c) {
        const unsigned char u = static_cast<unsigned char>(c);
        return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z');
    }

    bool isHex(char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }

    bool isSpace(char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
    }

    bool allHex(std::string_view text) {
        return !text.empty() && std::all_of(text.begin(), text.end(), isHex);
    }

    bool isOperatorSymbol(char c) {
        return std::string_view("+-*/%^&|~!=<>,").find(c) != std::string_view::npos;
    }

    std::string_view trim(std::string_view text) {
        while (!text.empty() && isSpace(text.front())) text.remove_prefix(1);
        while (!text.empty() && isSpace(text.back())) text.remove_suffix(1);
        return text;
    }

    bool startsWith(std::string_view text, std::string_view prefix, size_t at = 0) {
        return text.size() >= at + prefix.size() && text.compare(at, prefix.size(), prefix) == 0;
    }

    bool endsWith(std::string_view text, std::string_view suffix) {
        return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    void replaceAll(std::string& text, std::string_view from, std::string_view to) {
        for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) text.replace(at, from.size(), to);
    }

    // the index after the bracket group that opens at text[i] ('<' or '(', nested in either)
    size_t skipBalanced(std::string_view text, size_t i) {
        int depth = 0;
        for (; i < text.size(); i++) {
            if (text[i] == '<' || text[i] == '(') {
                depth++;
            } else if (text[i] == '>' || text[i] == ')') {
                if (--depth == 0) return i + 1;
            }
        }
        return i;
    }

    std::string readText(const fs::path& file) {
        std::ifstream in(file, std::ios::binary);
        std::string text(kReadLimit, '\0');
        in.read(text.data(), static_cast<std::streamsize>(kReadLimit));
        text.resize(static_cast<size_t>(in.gcount()));
        return text;
    }

    // each line, trimmed unless `raw`; fn returns false to stop
    template <typename Fn>
    void forEachLine(std::string_view text, Fn fn, bool raw = false) {
        size_t start = 0;
        while (start < text.size()) {
            size_t end = text.find('\n', start);
            if (end == std::string_view::npos) end = text.size();
            std::string_view line = text.substr(start, end - start);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (!fn(raw ? line : trim(line))) return;
            start = end + 1;
        }
    }

    // the first "key: value" line's value (a key at the start of its line)
    std::string headerValue(std::string_view text, std::string_view key) {
        std::string value;
        forEachLine(text, [&](std::string_view line) {
            if (line.size() >= key.size() + 2 && startsWith(line, key) && line[key.size()] == ':' && line[key.size() + 1] == ' ') {
                value = std::string(trim(line.substr(key.size() + 2)));
                return false;
            }
            return true;
        }, true);
        return value;
    }

    // "#N <module>+0x<offset>[ [<symbol>]]", as symbolicate.py's FRAME
    bool parseFrame(std::string_view line, Frame& frame) {
        if (line.size() < 2 || line[0] != '#') return false;
        size_t at = 1;
        while (at < line.size() && line[at] >= '0' && line[at] <= '9') at++;
        if (at == 1 || at >= line.size() || !isSpace(line[at])) return false;
        while (at < line.size() && isSpace(line[at])) at++;
        size_t tokenEnd = at;
        while (tokenEnd < line.size() && !isSpace(line[tokenEnd])) tokenEnd++;
        const std::string_view token = line.substr(at, tokenEnd - at);
        size_t split = std::string_view::npos;
        for (size_t k = token.find("+0x", 1); k != std::string_view::npos; k = token.find("+0x", k + 1)) {
            if (allHex(token.substr(k + 3))) {
                split = k;
                break;
            }
        }
        if (split == std::string_view::npos) return false;
        const std::string_view rest = trim(line.substr(tokenEnd));
        std::string_view symbol;
        if (!rest.empty()) {
            if (tokenEnd == line.size() || rest.front() != '[' || rest.back() != ']' || rest.size() < 2) return false;
            symbol = rest.substr(1, rest.size() - 2);
        }
        const std::string offset(token.substr(split + 3));
        if (offset.size() > 16) return false;
        frame.module = std::string(token.substr(0, split));
        frame.offset = std::strtoull(offset.c_str(), nullptr, 16);
        frame.symbol = std::string(symbol);
        return true;
    }

    std::string faultName(std::string_view text) {
        const std::string reason = headerValue(text, "reason");
        if (startsWith(reason, "std::terminate") || startsWith(reason, "unhandled exception")) return "terminate";
        const std::string first = reason.substr(0, reason.find(' '));
        if (first == "SIGABRT") return "abort";
        if (startsWith(first, "SIG")) return first;
        const std::string code = headerValue(text, "exception_code");
        std::string_view digits = code;
        if (startsWith(digits, "0x") || startsWith(digits, "0X")) digits.remove_prefix(2);
        if (allHex(digits) && digits.size() <= 16) {
            const uint64_t value = std::strtoull(std::string(digits).c_str(), nullptr, 16);
            static const std::pair<uint64_t, const char*> kExceptions[] = {
                {0xC0000005, "EXCEPTION_ACCESS_VIOLATION"}, {0xC00000FD, "EXCEPTION_STACK_OVERFLOW"},
                {0xC0000094, "EXCEPTION_INT_DIVIDE_BY_ZERO"}, {0xC000008E, "EXCEPTION_FLT_DIVIDE_BY_ZERO"},
                {0xC000001D, "EXCEPTION_ILLEGAL_INSTRUCTION"}, {0xC0000096, "EXCEPTION_PRIV_INSTRUCTION"},
                {0x80000003, "EXCEPTION_BREAKPOINT"}, {0xC0000006, "EXCEPTION_IN_PAGE_ERROR"},
                {0xC0000409, "STATUS_STACK_BUFFER_OVERRUN"}, {0xC0000025, "EXCEPTION_NONCONTINUABLE_EXCEPTION"},
                {0xC000000D, "STATUS_INVALID_PARAMETER"},
            };
            for (const auto& [known, name] : kExceptions) {
                if (known == value) return name;
            }
            char hex[24];
            std::snprintf(hex, sizeof(hex), "0x%08llx", static_cast<unsigned long long>(value));
            return hex;
        }
        const std::string signal = headerValue(text, "signal");
        if (!signal.empty() && std::all_of(signal.begin(), signal.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            return "signal " + signal;
        }
        return "unknown";
    }

    std::string moduleLinePath(std::string_view line) {
        if (const size_t size = line.find(" size="); size != std::string_view::npos) {   // Windows: <path> <base> size=<size>
            const std::string_view head = line.substr(0, size);
            const size_t space = head.rfind(' ');
            return space == std::string_view::npos ? std::string() : std::string(head.substr(0, space));
        }
        std::vector<std::string_view> parts;
        for (size_t at = 0; at < line.size();) {
            while (at < line.size() && isSpace(line[at])) at++;
            const size_t start = at;
            while (at < line.size() && !isSpace(line[at])) at++;
            if (at > start) parts.push_back(line.substr(start, at - start));
        }
        if (parts.size() >= 6 && parts[0].find('-') != std::string_view::npos) {   // /proc/self/maps
            std::string path(parts[5]);
            for (size_t i = 6; i < parts.size(); i++) path += " " + std::string(parts[i]);
            return startsWith(path, "/") ? path : std::string();
        }
        if (parts.size() >= 2 && startsWith(parts[0], "0x")) {   // Apple: <base> <path>
            return std::string(trim(line.substr(parts[0].size())));
        }
        return {};
    }

    // the basename of the modules section's first module: the game's executable
    std::string mainModule(std::string_view text) {
        bool seen = false;
        std::string out;
        forEachLine(text, [&](std::string_view line) {
            if (line == "modules:") {
                seen = true;
            } else if (seen && !line.empty()) {
                const std::string path = moduleLinePath(line);
                if (!path.empty()) {
                    out = path.substr(path.find_last_of("/\\") == std::string::npos ? 0 : path.find_last_of("/\\") + 1);
                    return false;
                }
            }
            return true;
        });
        return out;
    }

    bool skipped(const std::string& name) {
        if (name == "abort" || name == "raise" || name == "std::terminate") return true;
        for (std::string_view prefix : {"__pthread_kill", "__cxa_", "jasperengine::diag::", "jasperengine::CrashGuard"}) {
            if (startsWith(name, prefix)) return true;
        }
        return false;
    }

    fs::path primaryFile(const diag::crash::ReportInfo& report) {
        if (report.kind == "crash") return report.folder / "crash.txt";
        if (report.kind == "user") return report.folder / "user.txt";
        fs::path last;
        for (const fs::path& file : report.files) {
            const std::string name = file.filename().string();
            if (startsWith(name, "hang-") && name.size() > 9 && endsWith(name, ".txt")) last = file;
        }
        return last;
    }

}

std::string shortName(std::string_view symbol) {
    std::string_view trimmed = trim(symbol);
    if (const size_t plus = trimmed.rfind("+0x"); plus != std::string_view::npos && plus > 0 && allHex(trimmed.substr(plus + 3))) {
        trimmed = trimmed.substr(0, plus);
    }
    // lambdas: GCC {lambda(int)#1}, LLVM 'lambda'(int) / 'lambda0'(), MSVC <lambda_1f2e>, clang $_0
    const std::string_view s = trimmed;
    std::string marked;
    for (size_t i = 0; i < s.size();) {
        if (startsWith(s, "{lambda(", i)) {
            int depth = 0;
            size_t j = i;
            while (j < s.size()) {
                if (s[j] == '{') depth++;
                if (s[j] == '}') depth--;
                j++;
                if (depth == 0) break;
            }
            marked += '\x02';
            i = j;
        } else if (startsWith(s, "'lambda", i)) {
            size_t j = s.find('\'', i + 1);
            j = j == std::string_view::npos ? s.size() : j + 1;
            if (j < s.size() && s[j] == '(') j = skipBalanced(s, j);
            marked += '\x02';
            i = j;
        } else if (startsWith(s, "<lambda_", i)) {
            const size_t j = s.find('>', i);
            marked += '\x02';
            i = j == std::string_view::npos ? s.size() : j + 1;
        } else if (startsWith(s, "$_", i) && i + 2 < s.size() && s[i + 2] >= '0' && s[i + 2] <= '9') {
            size_t j = i + 2;
            while (j < s.size() && s[j] >= '0' && s[j] <= '9') j++;
            marked += '\x02';
            i = j;
        } else {
            marked += s[i++];
        }
    }
    replaceAll(marked, "(anonymous namespace)", "\x01");
    replaceAll(marked, "`anonymous namespace'", "\x01");

    // drop every <...> and (...), [abi:...] and [clone ...]; keep operator names
    const std::string_view m = marked;
    std::string out;
    for (size_t i = 0; i < m.size();) {
        const char c = m[i];
        if (startsWith(m, "operator", i) && (i == 0 || !isIdent(m[i - 1])) && (i + 8 == m.size() || !isIdent(m[i + 8]))) {
            out += "operator";
            i += 8;
            if (startsWith(m, "()", i) || startsWith(m, "[]", i)) {
                out += m.substr(i, 2);
                i += 2;
            } else if (i < m.size() && isOperatorSymbol(m[i])) {
                while (i < m.size() && isOperatorSymbol(m[i])) out += m[i++];
            } else if (i + 1 < m.size() && m[i] == ' ' && (isAlpha(m[i + 1]) || m[i + 1] == '_')) {
                out += ' ';
                i++;
                while (i < m.size() && (isIdent(m[i]) || m[i] == ':')) out += m[i++];
            }
        } else if (c == '<' || c == '(') {
            i = skipBalanced(m, i);
        } else if (c == '[') {
            if (startsWith(m, "[]", i)) {
                out += "[]";
                i += 2;
            } else {
                const size_t j = m.find(']', i);
                i = j == std::string_view::npos ? m.size() : j + 1;
            }
        } else {
            out += c;
            i++;
        }
    }
    // whitespace collapsed to single spaces
    std::vector<std::string> tokens;
    for (size_t at = 0; at < out.size();) {
        while (at < out.size() && isSpace(out[at])) at++;
        const size_t start = at;
        while (at < out.size() && !isSpace(out[at])) at++;
        if (at > start) tokens.push_back(out.substr(start, at - start));
    }
    std::string joined;
    for (const std::string& token : tokens) joined += (joined.empty() ? "" : " ") + token;
    for (bool changed = true; changed;) {
        changed = false;
        for (std::string_view suffix : {" const", " volatile", " noexcept", " &&", " &", "&&", "&"}) {
            if (endsWith(joined, suffix) && joined.size() > suffix.size()) {
                joined.resize(joined.size() - suffix.size());
                while (!joined.empty() && isSpace(joined.back())) joined.pop_back();
                changed = true;
            }
        }
    }
    tokens.clear();
    for (size_t at = 0;;) {
        const size_t space = joined.find(' ', at);
        tokens.push_back(joined.substr(at, space == std::string::npos ? std::string::npos : space - at));
        if (space == std::string::npos) break;
        at = space + 1;
    }
    std::string name = tokens.back();
    if (tokens.size() >= 2 && endsWith(tokens[tokens.size() - 2], "operator")) name = tokens[tokens.size() - 2] + " " + name;
    if (endsWith(name, "\x02::operator()")) name.resize(name.size() - std::string_view("::operator()").size());
    replaceAll(name, "\x01", "(anonymous namespace)");
    replaceAll(name, "\x02", "<lambda>");
    return name;
}

Signature signatureOf(const diag::crash::ReportInfo& report) {
    Signature out;
    if (report.kind == "abnormal_exit") {
        out.text = "abnormal_exit";
        return out;
    }
    const std::string text = readText(primaryFile(report));
    out.platform = headerValue(text, "platform");
    out.buildId = headerValue(text, "build_id");
    if (report.kind == "user") {
        const std::string category = headerValue(text, "category");
        out.text = "user | " + (category.empty() ? std::string("other") : category);
        return out;
    }
    std::vector<std::string> parts;
    if (report.kind == "crash") {
        parts = {"crash", faultName(text)};
    } else {
        const std::string phase = headerValue(text, "phase");
        const std::string scopes = headerValue(text, "busy_scopes");
        parts = {"hang", phase.empty() ? std::string("unknown") : phase};
        if (!scopes.empty() && scopes != "none") {
            const size_t last = scopes.rfind(" > ");
            parts.push_back(last == std::string::npos ? scopes : scopes.substr(last + 3));
        }
    }
    const std::string main = mainModule(text);
    // a frame's name, "" when the report gives none
    std::vector<std::pair<Frame, std::string>> candidates;
    std::vector<std::pair<Frame, std::string>> own;
    forEachLine(text, [&](std::string_view line) {
        Frame frame;
        if (!parseFrame(line, frame)) return true;
        const std::string name = frame.symbol.empty() ? std::string() : shortName(frame.symbol);
        if (!name.empty() && skipped(name)) return true;
        const bool isOwn = frame.module == main || startsWith(frame.module, "libjasperengine");
        candidates.emplace_back(frame, name);
        if (isOwn) own.emplace_back(frame, name);
        return true;
    });
    const std::vector<std::pair<Frame, std::string>>& chosen = own.empty() ? candidates : own;
    for (size_t i = 0; i < chosen.size() && i < 3; i++) {
        const auto& [frame, name] = chosen[i];
        if (!name.empty()) {
            parts.push_back(name);
            continue;
        }
        out.unresolved = true;
        char offset[24];
        std::snprintf(offset, sizeof(offset), "+0x%llx", static_cast<unsigned long long>(frame.offset));
        parts.push_back(frame.module + offset);
    }
    for (const std::string& part : parts) out.text += (out.text.empty() ? "" : " | ") + part;
    if (out.unresolved) out.text += " | build " + (out.buildId.empty() ? std::string("none") : out.buildId);
    return out;
}

std::vector<Group> group(const std::vector<diag::crash::ReportInfo>& reports) {
    std::vector<Group> groups;
    std::map<std::string, size_t> bySignature;
    std::vector<std::vector<std::pair<int64_t, std::string>>> members;
    for (const diag::crash::ReportInfo& report : reports) {
        const Signature signature = signatureOf(report);
        auto [it, added] = bySignature.emplace(signature.text, groups.size());
        if (added) {
            Group fresh;
            fresh.signature = signature.text;
            fresh.kind = report.kind;
            fresh.unresolved = signature.unresolved;
            fresh.firstSeen = fresh.lastSeen = report.timeUnix;
            groups.push_back(std::move(fresh));
            members.emplace_back();
        }
        Group& g = groups[it->second];
        g.count++;
        g.versions[report.version.empty() ? std::string("unknown") : report.version]++;
        g.platforms[signature.platform.empty() ? std::string("unknown") : signature.platform]++;
        g.firstSeen = std::min(g.firstSeen, report.timeUnix);
        g.lastSeen = std::max(g.lastSeen, report.timeUnix);
        members[it->second].emplace_back(report.timeUnix, report.id);
    }
    for (size_t i = 0; i < groups.size(); i++) {
        std::sort(members[i].rbegin(), members[i].rend());
        for (const auto& member : members[i]) groups[i].reportIds.push_back(member.second);
    }
    std::sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) {
        if (a.count != b.count) return a.count > b.count;
        if (a.lastSeen != b.lastSeen) return a.lastSeen > b.lastSeen;
        return a.signature < b.signature;
    });
    return groups;
}

}
