// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Gameplay Journal (Journal.h). log() formats into a stack buffer (fields sorted by an
// insertion sort over views, numbers through std::to_chars and snprintf) and copies the line
// into the ring as the crash ring does: no lock, no heap allocation.

#include "Journal.h"
#include "detail/CrashState.h"

#include "log/Logger.h"
#include "profiler/FrameTimes.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>

using namespace jasperengine;
using namespace jasperengine::diag;

namespace je_journal {

    struct JournalState {
        // relaxed: a disabled journal costs this one load per log
        std::atomic<size_t> capacity{0};
        char* ring = nullptr;
        std::atomic<uint64_t> cursor{0};
        std::atomic<uint64_t> written{0};
        std::atomic<bool> attached{false};
        std::atomic<bool> engineEvents{true};
        std::atomic<journal::Listener> listener{nullptr};
        std::atomic<bool> warnedKind{false};
        int64_t startNs = 0;
        std::time_t startedWall = 0;
        char product[128] = "unknown";
        char version[64] = "unknown";
        char session[64] = "-";
    };

    // constant-initialized and never destroyed: a crash during exit still finds the ring
    JournalState journalState;

    int64_t nowNs() {
        return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    void copyField(char* out, size_t capacity, std::string_view text) {
        const size_t n = std::min(text.size(), capacity - 1);
        std::memcpy(out, text.data(), n);
        out[n] = '\0';
    }

    // The entry being formatted: at most kMaxEntryBytes, the rest dropped and marked
    struct Line {
        char data[journal::kMaxEntryBytes + 1];
        size_t size = 0;
        bool cut = false;

        void put(char c) {
            if (size < journal::kMaxEntryBytes) data[size++] = c;
            else cut = true;
        }
        void put(std::string_view text) {
            const size_t room = journal::kMaxEntryBytes - size;
            const size_t n = text.size() < room ? text.size() : room;
            std::memcpy(data + size, text.data(), n);
            size += n;
            if (n < text.size()) cut = true;
        }
    };

    bool kindChar(char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == ':' || c == '-';
    }

    bool bareChar(char c) {
        return kindChar(c) || c == '/' || c == '+';
    }

    bool validKind(std::string_view kind) {
        if (kind.empty() || kind.size() > journal::kMaxKindBytes) return false;
        for (char c : kind) {
            if (!kindChar(c)) return false;
        }
        return true;
    }

    void putString(Line& line, std::string_view text) {
        bool bare = !text.empty();
        for (char c : text) {
            if (!bareChar(c)) {
                bare = false;
                break;
            }
        }
        if (bare) {
            line.put(text);
            return;
        }
        line.put('"');
        for (char c : text) {
            if (c == '"' || c == '\\') {
                line.put('\\');
                line.put(c);
            } else if (c == '\n') {
                line.put("\\n");
            } else if (c == '\r') {
                line.put("\\r");
            } else {
                line.put(c);
            }
        }
        line.put('"');
    }

    void putInteger(Line& line, int64_t value) {
        char digits[24];
        const auto result = std::to_chars(digits, digits + sizeof(digits), value);
        line.put(std::string_view(digits, static_cast<size_t>(result.ptr - digits)));
    }

    // %.9g. The common range, 1e-4 <= |value| < 1e9, without snprintf (a third of an entry's
    // cost): the 9 significant digits rounded, the trailing zeros dropped. Elsewhere snprintf, whose
    // decimal comma under a device's locale would make the text depend on the device.
    void putDouble(Line& line, double value) {
        const double magnitude = value < 0.0 ? -value : value;
        if (std::isfinite(value) && magnitude >= 1e-4 && magnitude < 1e9) {
            static constexpr double kPowers[] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12};
            int exponent = static_cast<int>(std::floor(std::log10(magnitude)));
            auto significand = [magnitude](int e) { return static_cast<uint64_t>(std::llround(magnitude * kPowers[8 - e])); };
            if (exponent >= -4 && exponent <= 8) {
                uint64_t digits = significand(exponent);
                if (digits >= 1000000000ull && exponent < 8) digits = significand(++exponent);
                else if (digits < 100000000ull && exponent > -4) digits = significand(--exponent);
                if (digits >= 100000000ull && digits < 1000000000ull) {
                    char text[9];
                    for (int i = 8; i >= 0; i--, digits /= 10) text[i] = static_cast<char>('0' + digits % 10);
                    int significant = 9;
                    while (significant > 1 && text[significant - 1] == '0') significant--;
                    if (value < 0.0) line.put('-');
                    if (exponent >= 0) {
                        line.put(std::string_view(text, static_cast<size_t>(exponent + 1)));
                        if (significant > exponent + 1) {
                            line.put('.');
                            line.put(std::string_view(text + exponent + 1, static_cast<size_t>(significant - exponent - 1)));
                        }
                    } else {
                        line.put("0.");
                        for (int i = -1; i > exponent; i--) line.put('0');
                        line.put(std::string_view(text, static_cast<size_t>(significant)));
                    }
                    return;
                }
            }
        }
        char digits[40];
        const int n = std::snprintf(digits, sizeof(digits), "%.9g", value);
        for (int i = 0; i < n && i < static_cast<int>(sizeof(digits)); i++) line.put(digits[i] == ',' ? '.' : digits[i]);
    }

    void putValue(Line& line, const journal::Field& field) {
        if (const auto* text = std::get_if<std::string_view>(&field.value)) {
            if (field.verbatim) line.put(*text);
            else putString(line, *text);
        } else if (const auto* integer = std::get_if<int64_t>(&field.value)) {
            putInteger(line, *integer);
        } else if (const auto* number = std::get_if<double>(&field.value)) {
            putDouble(line, *number);
        } else {
            line.put(std::get<bool>(field.value) ? "true" : "false");
        }
    }

    // A key prints as its [A-Za-z0-9_.:-] characters, others as '_' (a space or '=' would make
    // the line ambiguous)
    void putKey(Line& line, std::string_view key) {
        for (char c : key) line.put(kindChar(c) ? c : '_');
    }

    void ringWrite(JournalState& s, const char* data, size_t size, size_t capacity) {
        // the line was cut to kMaxEntryBytes + 1, far below the smallest ring
        uint64_t position = s.cursor.fetch_add(size, std::memory_order_relaxed);
        while (size > 0) {
            const size_t at = static_cast<size_t>(position % capacity);
            const size_t n = std::min(size, capacity - at);
            std::memcpy(s.ring + at, data, n);
            position += n;
            data += n;
            size -= n;
        }
        s.written.fetch_add(1, std::memory_order_release);
    }

    // The ring's whole entries, oldest first
    std::vector<std::string> wholeEntries(const JournalState& s) {
        std::vector<std::string> out;
        const size_t capacity = s.capacity.load(std::memory_order_acquire);
        if (capacity == 0 || !s.ring) return out;
        const uint64_t cursor = s.cursor.load(std::memory_order_acquire);
        const crash::detail::JournalSpan span = crash::detail::journalSpan(s.ring, capacity, cursor);
        std::string line;
        for (uint64_t at = span.begin; at < span.end; at++) {
            const char c = s.ring[at % capacity];
            if (c != '\n') {
                line += c;
                continue;
            }
            if (!line.empty() && line[0] == '+') out.push_back(line);
            line.clear();
        }
        return out;
    }

    std::string isoUtc(std::time_t time) {
        std::tm tm{};
#if defined(_WIN32)
        gmtime_s(&tm, &time);
#else
        gmtime_r(&time, &tm);
#endif
        char text[40];
        std::snprintf(text, sizeof(text), "%04d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                      tm.tm_hour, tm.tm_min, tm.tm_sec);
        return text;
    }

}

namespace jasperengine::diag::journal {

    // inside this namespace only: a unity build's next source must not see je_journal's names
    using namespace je_journal;

    void configure(size_t bytes) {
        JournalState& s = journalState;
        if (s.attached.load(std::memory_order_acquire)) return;
        // a ring smaller than a few entries keeps none whole
        if (bytes > 0 && bytes < 4 * (kMaxEntryBytes + 1)) bytes = 4 * (kMaxEntryBytes + 1);
        s.capacity.store(0, std::memory_order_release);
        delete[] s.ring;
        s.ring = nullptr;
        s.cursor.store(0, std::memory_order_relaxed);
        s.written.store(0, std::memory_order_relaxed);
        s.startNs = nowNs();
        s.startedWall = std::time(nullptr);
        if (bytes == 0) return;
        s.ring = new char[bytes];
        std::memset(s.ring, 0, bytes);
        s.capacity.store(bytes, std::memory_order_release);
    }

    bool enabled() {
        return journalState.capacity.load(std::memory_order_relaxed) != 0;
    }

    size_t capacity() {
        return journalState.capacity.load(std::memory_order_relaxed);
    }

    void log(std::string_view kind, std::span<const Field> fields) {
        JournalState& s = journalState;
        const size_t capacity = s.capacity.load(std::memory_order_relaxed);
        if (capacity == 0) return;

        Field invalid;
        const bool kindOk = validKind(kind);
        if (!kindOk) {
            invalid.key = "kind";
            invalid.value = kind.substr(0, kMaxKindBytes);
            if (!s.warnedKind.exchange(true, std::memory_order_relaxed)) {
                // once per process: a shipped game never raises from a diagnostics call
                Logger::write(LogLevel::Warn, logCategoryEngine(),
                              "Journal.log: kind \"" + std::string(kind.substr(0, kMaxKindBytes)) + "\" is not [A-Za-z0-9_.:-]{1,48}");
            }
        }

        // the kMaxFields smallest keys, sorted bytewise (the same text whatever the caller's order)
        const Field* sorted[kMaxFields + 1];
        size_t count = 0;
        auto insert = [&](const Field* field, size_t limit) {
            size_t at = count;
            while (at > 0 && field->key < sorted[at - 1]->key) at--;
            if (at >= limit) return;
            const size_t last = count < limit ? count : limit - 1;
            for (size_t i = last; i > at; i--) sorted[i] = sorted[i - 1];
            sorted[at] = field;
            if (count < limit) count++;
        };
        const size_t limit = kindOk ? kMaxFields : kMaxFields + 1;
        if (!kindOk) insert(&invalid, limit);
        for (const Field& field : fields) {
            if (!field.key.empty()) insert(&field, limit);
        }

        Line line;
        const int64_t ms = (nowNs() - s.startNs) / 1000000;
        line.put('+');
        putInteger(line, ms / 1000);
        line.put('.');
        const int64_t fraction = ms % 1000;
        line.put(static_cast<char>('0' + fraction / 100));
        line.put(static_cast<char>('0' + fraction / 10 % 10));
        line.put(static_cast<char>('0' + fraction % 10));
        line.put(" f");
        putInteger(line, static_cast<int64_t>(FrameTimes::currentFrame()));
        line.put(' ');
        line.put(kindOk ? kind : std::string_view("invalid"));
        for (size_t i = 0; i < count; i++) {
            line.put(' ');
            putKey(line, sorted[i]->key);
            line.put('=');
            putValue(line, *sorted[i]);
        }
        if (line.cut) {
            size_t end = kMaxEntryBytes - 1;
            // not inside a UTF-8 sequence
            while (end > 0 && (static_cast<unsigned char>(line.data[end]) & 0xC0) == 0x80) end--;
            line.data[end] = '~';
            line.size = end + 1;
        }
        line.data[line.size] = '\n';
        ringWrite(s, line.data, line.size + 1, capacity);

        if (Listener listener = s.listener.load(std::memory_order_acquire)) {
            const std::string_view entry(line.data, line.size);
            const size_t space = entry.find(' ');
            listener(space == std::string_view::npos ? entry : entry.substr(space + 1));
        }
    }

    std::string text() {
        const JournalState& s = journalState;
        if (s.capacity.load(std::memory_order_acquire) == 0) return {};
        const std::vector<std::string> lines = wholeEntries(s);
        const uint64_t written = s.written.load(std::memory_order_acquire);
        std::string out = detail::headerPrefix();
        out += "dropped: " + std::to_string(written > lines.size() ? written - lines.size() : 0) + "\n---\n";
        for (const std::string& line : lines) out += line + "\n";
        return out;
    }

    std::vector<std::string> entries(size_t newest) {
        std::vector<std::string> lines = wholeEntries(journalState);
        if (lines.size() > newest) lines.erase(lines.begin(), lines.end() - static_cast<std::ptrdiff_t>(newest));
        return lines;
    }

    void clear() {
        JournalState& s = journalState;
        s.cursor.store(0, std::memory_order_release);
        s.written.store(0, std::memory_order_release);
    }

    uint64_t dropped() {
        const JournalState& s = journalState;
        const uint64_t written = s.written.load(std::memory_order_acquire);
        const size_t kept = wholeEntries(s).size();
        return written > kept ? written - kept : 0;
    }

    void setEngineEvents(bool on) {
        journalState.engineEvents.store(on, std::memory_order_relaxed);
    }

    bool engineEvents() {
        return journalState.engineEvents.load(std::memory_order_relaxed);
    }

    void logEngine(std::string_view kind, std::span<const Field> fields) {
        if (!enabled() || !engineEvents()) return;
        log(kind, fields);
    }

    void setListener(Listener listener) {
        journalState.listener.store(listener, std::memory_order_release);
    }

    void setIdentity(std::string_view product, std::string_view version, std::string_view session) {
        JournalState& s = journalState;
        copyField(s.product, sizeof(s.product), product.empty() ? std::string_view("unknown") : product);
        copyField(s.version, sizeof(s.version), version.empty() ? std::string_view("unknown") : version);
        copyField(s.session, sizeof(s.session), session.empty() ? std::string_view("-") : session);
    }

    std::string identityVersion() {
        return journalState.version;
    }

    namespace detail {

        char* ring() {
            return journalState.ring;
        }

        std::atomic<uint64_t>* cursor() {
            return &journalState.cursor;
        }

        std::atomic<uint64_t>* written() {
            return &journalState.written;
        }

        std::string headerPrefix() {
            const JournalState& s = journalState;
            std::string out = "JasperEngine journal v1\n";
            out += std::string("product: ") + s.product + "\n";
            out += std::string("version: ") + s.version + "\n";
            out += std::string("session: ") + s.session + "\n";
            out += "started: " + isoUtc(s.startedWall) + "\n";
            out += "capacity: " + std::to_string(s.capacity.load(std::memory_order_relaxed)) + "\n";
            return out;
        }

        void markAttached() {
            journalState.attached.store(true, std::memory_order_release);
        }

    }

}
