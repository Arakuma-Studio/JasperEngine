// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Gameplay Journal (Journal.h). log() formats into a stack buffer (fields sorted by an
// insertion sort over views, numbers through std::to_chars and snprintf) and copies the line
// into the ring as the crash ring does: no lock, no heap allocation. R29's 250 ns leave no room
// for a call per piece: short copies are inline moves and the cut is decided once, at the end.

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
#if defined(__linux__)
#include <time.h>
#endif

using namespace jasperengine;

namespace je_journal {

    struct JournalState {
        // relaxed: a disabled journal costs this one load per log
        std::atomic<size_t> capacity{0};
        char* ring = nullptr;
        // 1 / capacity: the ring offset of a cursor without a 64-bit division (ringOffset)
        double inverseCapacity = 0.0;
        // entries stamped from the coarse monotonic clock (journalNowNs)
        bool coarseClock = false;
        std::atomic<uint64_t> cursor{0};
        std::atomic<uint64_t> written{0};
        std::atomic<bool> attached{false};
        std::atomic<bool> engineEvents{true};
        std::atomic<diag::journal::Listener> listener{nullptr};
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

    // An entry's time (R2 prints milliseconds). Where the kernel's coarse monotonic clock ticks at
    // 1 ms or finer (Linux at HZ=1000) it is read instead: a stamp lags by less than the millisecond
    // it prints, and the read costs a quarter of the precise one (a tenth of a log on a VM's
    // kvm-clock). Coarser kernels (most Android ones) and other systems keep the precise clock.
    bool coarseClockFine() {
#if defined(__linux__)
        timespec resolution{};
        return clock_getres(CLOCK_MONOTONIC_COARSE, &resolution) == 0 && resolution.tv_sec == 0 && resolution.tv_nsec <= 1000000;
#else
        return false;
#endif
    }

    int64_t journalNowNs(const JournalState& s) {
#if defined(__linux__)
        if (s.coarseClock) {
            timespec now{};
            clock_gettime(CLOCK_MONOTONIC_COARSE, &now);
            return static_cast<int64_t>(now.tv_sec) * 1000000000 + now.tv_nsec;
        }
#endif
        (void)s;
        return nowNs();
    }

    void copyField(char* out, size_t capacity, std::string_view text) {
        const size_t n = std::min(text.size(), capacity - 1);
        std::memcpy(out, text.data(), n);
        out[n] = '\0';
    }

    // The entry being formatted, in a stack buffer: at most kMaxEntryBytes, the rest dropped and
    // marked. Each writer takes the position and returns the next one, so it stays in a register (a
    // position kept in memory made every piece wait for the last one's store). Writing stops once
    // the line is past the limit (`end`, one byte beyond it), and kLineSlack bytes after it let a
    // number be written without a check of its own: the cut is decided once, at the end
    constexpr size_t kLineSlack = 64;

    // keys and values are short: fixed-width moves (overlapping, never past the source) instead
    // of a call to memcpy
    void copyShort(char* out, const char* in, size_t n) {
        if (n >= 8 && n <= 16) {
            std::memcpy(out, in, 8);
            std::memcpy(out + n - 8, in + n - 8, 8);
        } else if (n >= 4 && n < 8) {
            std::memcpy(out, in, 4);
            std::memcpy(out + n - 4, in + n - 4, 4);
        } else if (n < 4) {
            for (size_t i = 0; i < n; i++) out[i] = in[i];
        } else {
            std::memcpy(out, in, n);
        }
    }

    char* putChar(char* at, const char* end, char c) {
        if (at < end) *at++ = c;
        return at;
    }

    char* putText(char* at, const char* end, std::string_view text) {
        if (at >= end) return at;
        const size_t room = static_cast<size_t>(end - at);
        const size_t n = text.size() < room ? text.size() : room;
        copyShort(at, text.data(), n);
        return at + n;
    }

    // a piece from a small local buffer: memcpy, whose bounds the compiler can follow
    char* putDigits(char* at, const char* end, const char* text, size_t size) {
        if (at >= end) return at;
        const size_t room = static_cast<size_t>(end - at);
        const size_t n = size < room ? size : room;
        std::memcpy(at, text, n);
        return at + n;
    }

    // "00".."99"
    constexpr char kDigitPairs[] =
        "0001020304050607080910111213141516171819202122232425262728293031323334353637383940414243444546474849"
        "5051525354555657585960616263646566676869707172737475767778798081828384858687888990919293949596979899";

    // per byte: 1 a kind's and a key's characters [A-Za-z0-9_.:-], 2 also a bare value's (/ and +)
    constexpr uint8_t kKindClass = 1, kBareClass = 2;
    struct CharClasses {
        uint8_t of[256] = {};
        constexpr CharClasses() {
            for (int c = 0; c < 256; c++) {
                const bool kind = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
                                  c == ':' || c == '-';
                of[c] = static_cast<uint8_t>((kind ? kKindClass | kBareClass : 0) | (c == '/' || c == '+' ? kBareClass : 0));
            }
        }
    };
    constexpr CharClasses kCharClasses;

    bool kindChar(char c) {
        return (kCharClasses.of[static_cast<unsigned char>(c)] & kKindClass) != 0;
    }

    bool bareChar(char c) {
        return (kCharClasses.of[static_cast<unsigned char>(c)] & kBareClass) != 0;
    }

    bool validKind(std::string_view kind) {
        if (kind.empty() || kind.size() > diag::journal::kMaxKindBytes) return false;
        for (char c : kind) {
            if (!kindChar(c)) return false;
        }
        return true;
    }

    char* putString(char* at, const char* end, std::string_view text) {
        // copied, then checked where it landed: most values are bare
        if (at >= end) return at;
        {
            const size_t room = static_cast<size_t>(end - at);
            const size_t n = text.size() < room ? text.size() : room;
            uint8_t all = text.empty() ? 0 : kBareClass;
            for (size_t i = 0; i < n; i++) {
                at[i] = text[i];
                all &= kCharClasses.of[static_cast<unsigned char>(text[i])];
            }
            // a cut value is bare when its whole text is
            for (size_t i = n; i < text.size() && all; i++) all &= kCharClasses.of[static_cast<unsigned char>(text[i])];
            if (all & kBareClass) return at + n;
        }
        at = putChar(at, end, '"');
        for (char c : text) {
            if (at >= end) break;
            if (c == '"' || c == '\\') {
                at = putChar(at, end, '\\');
                at = putChar(at, end, c);
            } else if (c == '\n') {
                at = putText(at, end, "\\n");
            } else if (c == '\r') {
                at = putText(at, end, "\\r");
            } else {
                at = putChar(at, end, c);
            }
        }
        return putChar(at, end, '"');
    }

    char* putInteger(char* at, const char* end, int64_t value) {
        // at most 20 characters, in the slack
        return at < end ? std::to_chars(at, at + 24, value).ptr : at;
    }

    // %.9g. The common range, 1e-4 <= |value| < 1e9, without snprintf (a third of an entry's
    // cost): the 9 significant digits rounded, the trailing zeros dropped. Elsewhere snprintf, whose
    // decimal comma under a device's locale would make the text depend on the device.
    char* putDouble(char* at, const char* end, double value) {
        const double magnitude = value < 0.0 ? -value : value;
        if (std::isfinite(value) && magnitude >= 1e-4 && magnitude < 1e9) {
            static constexpr double kPowers[] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12};
            static constexpr double kExponents[] = {1e-4, 1e-3, 1e-2, 1e-1, 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8};
            // the decimal exponent estimated from the binary one (log10(2) ~ 1233 / 4096), then made
            // exact by comparisons (log10 costs more than the whole lookup)
            uint64_t bits;
            std::memcpy(&bits, &magnitude, sizeof(bits));
            const int binary = static_cast<int>((bits >> 52) & 0x7ff) - 1023;
            int exponent = (binary * 1233) >> 12;
            if (exponent > 8) exponent = 8;
            if (exponent < -4) exponent = -4;
            while (exponent < 8 && magnitude >= kExponents[exponent + 5]) exponent++;
            while (exponent > -4 && magnitude < kExponents[exponent + 4]) exponent--;
            // rounded half away from zero as llround does: the sum is exact for these magnitudes
            // (at least 1e7, so 0.5 is a multiple of their ulp), and a call less
            auto significand = [magnitude](int e) { return static_cast<uint64_t>(magnitude * kPowers[8 - e] + 0.5); };
            if (exponent >= -4 && exponent <= 8) {
                uint64_t digits = significand(exponent);
                if (digits >= 1000000000ull && exponent < 8) digits = significand(++exponent);
                else if (digits < 100000000ull && exponent > -4) digits = significand(--exponent);
                if (digits >= 100000000ull && digits < 1000000000ull) {
                    // two digits a step: a shorter chain of divisions
                    char text[10];
                    for (int i = 7; i >= 1; i -= 2, digits /= 100) std::memcpy(text + i, kDigitPairs + 2 * (digits % 100), 2);
                    text[0] = static_cast<char>('0' + digits);
                    int significant = 9;
                    while (significant > 1 && text[significant - 1] == '0') significant--;
                    if (value < 0.0) at = putChar(at, end, '-');
                    if (exponent >= 0) {
                        at = putDigits(at, end, text, static_cast<size_t>(exponent + 1));
                        if (significant > exponent + 1) {
                            at = putChar(at, end, '.');
                            at = putDigits(at, end, text + exponent + 1, static_cast<size_t>(significant - exponent - 1));
                        }
                    } else {
                        at = putText(at, end, "0.");
                        for (int i = -1; i > exponent; i--) at = putChar(at, end, '0');
                        at = putDigits(at, end, text, static_cast<size_t>(significant));
                    }
                    return at;
                }
            }
        }
        char digits[40];
        const int n = std::snprintf(digits, sizeof(digits), "%.9g", value);
        for (int i = 0; i < n && i < static_cast<int>(sizeof(digits)); i++) at = putChar(at, end, digits[i] == ',' ? '.' : digits[i]);
        return at;
    }

    char* putValue(char* at, const char* end, const diag::journal::Field& field) {
        if (const auto* text = std::get_if<std::string_view>(&field.value)) {
            return field.verbatim ? putText(at, end, *text) : putString(at, end, *text);
        } else if (const auto* integer = std::get_if<int64_t>(&field.value)) {
            return putInteger(at, end, *integer);
        } else if (const auto* number = std::get_if<double>(&field.value)) {
            return putDouble(at, end, *number);
        }
        return putText(at, end, std::get<bool>(field.value) ? "true" : "false");
    }

    // bytewise as string_view's <, the first byte compared before a memcmp call (keys mostly differ there)
    bool keyLess(std::string_view a, std::string_view b) {
        if (!a.empty() && !b.empty() && a[0] != b[0]) return static_cast<unsigned char>(a[0]) < static_cast<unsigned char>(b[0]);
        return a < b;
    }

    // A key prints as its [A-Za-z0-9_.:-] characters, others as '_' (a space or '=' would make
    // the line ambiguous)
    char* putKey(char* at, const char* end, std::string_view key) {
        if (at >= end) return at;
        const size_t room = static_cast<size_t>(end - at);
        const size_t n = key.size() < room ? key.size() : room;
        for (size_t i = 0; i < n; i++) at[i] = kindChar(key[i]) ? key[i] : '_';
        return at + n;
    }

    // position % capacity through the reciprocal (a 64-bit div was a tenth of a log): the quotient's
    // estimate is off by at most one for any cursor below 2^53 bytes, and the remainder corrects it
    size_t ringOffset(uint64_t position, size_t capacity, double inverse) {
        const uint64_t quotient = static_cast<uint64_t>(static_cast<double>(static_cast<int64_t>(position)) * inverse);
        int64_t at = static_cast<int64_t>(position - quotient * capacity);
        const int64_t size = static_cast<int64_t>(capacity);
        while (at < 0) at += size;
        while (at >= size) at -= size;
        return static_cast<size_t>(at);
    }

    void ringWrite(JournalState& s, const char* data, size_t size, size_t capacity) {
        // the line was cut to kMaxEntryBytes + 1, far below the smallest ring
        uint64_t position = s.cursor.fetch_add(size, std::memory_order_relaxed);
        // counted as it is reserved: the locked add after the copy waited for the copy's stores. An
        // entry still being copied at a crash counts as dropped, which it is
        s.written.fetch_add(1, std::memory_order_relaxed);
        while (size > 0) {
            const size_t at = ringOffset(position, capacity, s.inverseCapacity);
            const size_t n = std::min(size, capacity - at);
            std::memcpy(s.ring + at, data, n);
            position += n;
            data += n;
            size -= n;
        }
    }

    // The ring's whole entries, oldest first
    std::vector<std::string> wholeEntries(const JournalState& s) {
        std::vector<std::string> out;
        const size_t capacity = s.capacity.load(std::memory_order_acquire);
        if (capacity == 0 || !s.ring) return out;
        const uint64_t cursor = s.cursor.load(std::memory_order_acquire);
        const diag::crash::detail::JournalSpan span = diag::crash::detail::journalSpan(s.ring, capacity, cursor);
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

    void configure(size_t bytes) {
        je_journal::JournalState& s = je_journal::journalState;
        if (s.attached.load(std::memory_order_acquire)) return;
        // a ring smaller than a few entries keeps none whole
        if (bytes > 0 && bytes < 4 * (kMaxEntryBytes + 1)) bytes = 4 * (kMaxEntryBytes + 1);
        s.capacity.store(0, std::memory_order_release);
        delete[] s.ring;
        s.ring = nullptr;
        s.cursor.store(0, std::memory_order_relaxed);
        s.written.store(0, std::memory_order_relaxed);
        s.coarseClock = je_journal::coarseClockFine();
        s.startNs = je_journal::journalNowNs(s);
        s.startedWall = std::time(nullptr);
        if (bytes == 0) return;
        s.ring = new char[bytes];
        std::memset(s.ring, 0, bytes);
        s.inverseCapacity = 1.0 / static_cast<double>(bytes);
        s.capacity.store(bytes, std::memory_order_release);
    }

    bool enabled() {
        return je_journal::journalState.capacity.load(std::memory_order_relaxed) != 0;
    }

    size_t capacity() {
        return je_journal::journalState.capacity.load(std::memory_order_relaxed);
    }

    void log(std::string_view kind, std::span<const Field> fields) {
        je_journal::JournalState& s = je_journal::journalState;
        const size_t capacity = s.capacity.load(std::memory_order_relaxed);
        if (capacity == 0) return;

        Field invalid;
        const bool kindOk = je_journal::validKind(kind);
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
            while (at > 0 && je_journal::keyLess(field->key, sorted[at - 1]->key)) at--;
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

        char line[kMaxEntryBytes + je_journal::kLineSlack];
        const char* const end = line + kMaxEntryBytes + 1;
        const int64_t ms = (je_journal::journalNowNs(s) - s.startNs) / 1000000;
        // the header fits the slack: two numbers of at most 20 digits and the kind (48 bytes)
        char* at = line;
        *at++ = '+';
        at = std::to_chars(at, at + 20, ms / 1000).ptr;
        const int64_t fraction = ms % 1000;
        at[0] = '.';
        at[1] = static_cast<char>('0' + fraction / 100);
        std::memcpy(at + 2, je_journal::kDigitPairs + 2 * (fraction % 100), 2);
        at[4] = ' ';
        at[5] = 'f';
        at = std::to_chars(at + 6, at + 26, FrameTimes::currentFrame()).ptr;
        *at++ = ' ';
        const std::string_view shown = kindOk ? kind : std::string_view("invalid");
        je_journal::copyShort(at, shown.data(), shown.size());
        at += shown.size();
        for (size_t i = 0; i < count; i++) {
            at = je_journal::putChar(at, end, ' ');
            at = je_journal::putKey(at, end, sorted[i]->key);
            at = je_journal::putChar(at, end, '=');
            at = je_journal::putValue(at, end, *sorted[i]);
        }
        size_t size = static_cast<size_t>(at - line);
        if (size > kMaxEntryBytes) {
            size_t cut = kMaxEntryBytes - 1;
            // not inside a UTF-8 sequence
            while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80) cut--;
            line[cut] = '~';
            size = cut + 1;
        }
        line[size] = '\n';
        je_journal::ringWrite(s, line, size + 1, capacity);

        if (Listener listener = s.listener.load(std::memory_order_acquire)) {
            const std::string_view entry(line, size);
            const size_t space = entry.find(' ');
            listener(space == std::string_view::npos ? entry : entry.substr(space + 1));
        }
    }

    std::string text() {
        const je_journal::JournalState& s = je_journal::journalState;
        if (s.capacity.load(std::memory_order_acquire) == 0) return {};
        const std::vector<std::string> lines = je_journal::wholeEntries(s);
        const uint64_t written = s.written.load(std::memory_order_acquire);
        std::string out = detail::headerPrefix();
        out += "dropped: " + std::to_string(written > lines.size() ? written - lines.size() : 0) + "\n---\n";
        for (const std::string& line : lines) out += line + "\n";
        return out;
    }

    std::vector<std::string> entries(size_t newest) {
        std::vector<std::string> lines = je_journal::wholeEntries(je_journal::journalState);
        if (lines.size() > newest) lines.erase(lines.begin(), lines.end() - static_cast<std::ptrdiff_t>(newest));
        return lines;
    }

    void clear() {
        je_journal::JournalState& s = je_journal::journalState;
        s.cursor.store(0, std::memory_order_release);
        s.written.store(0, std::memory_order_release);
    }

    uint64_t dropped() {
        const je_journal::JournalState& s = je_journal::journalState;
        const uint64_t written = s.written.load(std::memory_order_acquire);
        const size_t kept = je_journal::wholeEntries(s).size();
        return written > kept ? written - kept : 0;
    }

    void setEngineEvents(bool on) {
        je_journal::journalState.engineEvents.store(on, std::memory_order_relaxed);
    }

    bool engineEvents() {
        return je_journal::journalState.engineEvents.load(std::memory_order_relaxed);
    }

    void logEngine(std::string_view kind, std::span<const Field> fields) {
        if (!enabled() || !engineEvents()) return;
        log(kind, fields);
    }

    void setListener(Listener listener) {
        je_journal::journalState.listener.store(listener, std::memory_order_release);
    }

    void setIdentity(std::string_view product, std::string_view version, std::string_view session) {
        je_journal::JournalState& s = je_journal::journalState;
        je_journal::copyField(s.product, sizeof(s.product), product.empty() ? std::string_view("unknown") : product);
        je_journal::copyField(s.version, sizeof(s.version), version.empty() ? std::string_view("unknown") : version);
        je_journal::copyField(s.session, sizeof(s.session), session.empty() ? std::string_view("-") : session);
    }

    std::string identityVersion() {
        return je_journal::journalState.version;
    }

    namespace detail {

        char* ring() {
            return je_journal::journalState.ring;
        }

        std::atomic<uint64_t>* cursor() {
            return &je_journal::journalState.cursor;
        }

        std::atomic<uint64_t>* written() {
            return &je_journal::journalState.written;
        }

        std::string headerPrefix() {
            const je_journal::JournalState& s = je_journal::journalState;
            std::string out = "JasperEngine journal v1\n";
            out += std::string("product: ") + s.product + "\n";
            out += std::string("version: ") + s.version + "\n";
            out += std::string("session: ") + s.session + "\n";
            out += "started: " + je_journal::isoUtc(s.startedWall) + "\n";
            out += "capacity: " + std::to_string(s.capacity.load(std::memory_order_relaxed)) + "\n";
            return out;
        }

        void markAttached() {
            je_journal::journalState.attached.store(true, std::memory_order_release);
        }

    }

}
