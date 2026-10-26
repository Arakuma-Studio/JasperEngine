// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef DIAG_DETAIL_CRASHSTATE_H
#define DIAG_DETAIL_CRASHSTATE_H

// What the crash handler's install (CrashHandler.cpp) prepares for the platform backends
// (CrashHandlerPosix.cpp, CrashHandlerWin.cpp): fixed buffers only, filled before any handler
// is armed and never written again but for the ring, the tags and the entry bookkeeping. The
// backends read it from a signal handler or a dying process, so nothing here allocates.

#include "Export.h"
#include "diag/CrashHandler.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace jasperengine::diag::crash::detail {

    constexpr size_t kPathBytes = 1024;
    constexpr size_t kHeaderBytes = 4096;
    constexpr size_t kTagCount = 16;
    constexpr size_t kTagKeyBytes = 32;
    constexpr size_t kTagValueBytes = 128;

    struct Tag {
        std::atomic<uint32_t> seq{0};   // odd while written (a seqlock)
        char key[kTagKeyBytes] = {0};
        char value[kTagValueBytes] = {0};
    };

    struct CrashState {
        std::atomic<bool> installed{false};

        // <root>/reports, <root>/reports/<session>, its files, and the optional log file
        char reportsDir[kPathBytes] = {0};
        char sessionDir[kPathBytes] = {0};
        char crashFile[kPathBytes] = {0};
        char dumpFile[kPathBytes] = {0};
        char tailFile[kPathBytes] = {0};
        char logFile[kPathBytes] = {0};
        char sessionName[64] = {0};

        // "JasperEngine crash report v1\nproduct: ...\n...session: ...\n"
        char header[kHeaderBytes] = {0};
        size_t headerLength = 0;
        // the log file's banner line ("JasperEngine Editor 1.2.3"), and where cores go
        char banner[256] = {0};
        char coreHint[512] = {0};

        bool includeLogTail = true;
        DumpKind dumpKind = DumpKind::Small;
        uint64_t startMonotonicNs = 0;

        // the crash ring: writers reserve with one fetch_add on cursor, then memcpy
        char* ring = nullptr;
        size_t ringCapacity = 0;
        std::atomic<uint64_t> ringCursor{0};

        Tag tags[kTagCount];

        // The first entry reports. Other threads wait up to eight seconds for reportDone
        // before chaining; recursive faults on the reporting thread terminate immediately.
        std::atomic<int> entries{0};
        std::atomic<int64_t> reportingThread{0};
        std::atomic<bool> reportDone{false};
        // Internal subprocess-test seam; production leaves this null. A test callback must
        // obey the fatal path's signal-safety rules. Set only before any fault.
        void (*beforeReportForTesting)() = nullptr;
        // set by debugCrash: the report says it was deliberate
        std::atomic<bool> deliberate{false};
        // the terminate handler's reason ("unhandled exception: ..."), reported by the SIGABRT
        // path that follows
        std::atomic<bool> pendingReasonSet{false};
        char pendingReason[512] = {0};
    };

    JASPERENGINE_INTERNAL CrashState& state();

    // The platform backend: arm the handlers once state() is filled (false: nothing armed)
    JASPERENGINE_INTERNAL bool installPlatform(const Config& config);
    JASPERENGINE_INTERNAL void prepareThreadPlatform();
    // A deliberate crash the platform does its own way (pure_call on Windows): false elsewhere
    JASPERENGINE_INTERNAL bool platformDebugCrash(std::string_view mode);

    // Small formatters with no locale, no allocation and no libc beyond memcpy/strlen: the
    // signal path's only way to print numbers
    inline size_t appendText(char* out, size_t capacity, size_t length, const char* text, size_t textLength) {
        if (length >= capacity) return length;
        const size_t room = capacity - 1 - length;
        const size_t n = textLength < room ? textLength : room;
        for (size_t i = 0; i < n; i++) out[length + i] = text[i];
        length += n;
        out[length] = '\0';
        return length;
    }

    inline size_t textLength(const char* text) {
        size_t n = 0;
        while (text && text[n]) n++;
        return n;
    }

    inline size_t appendText(char* out, size_t capacity, size_t length, const char* text) {
        return appendText(out, capacity, length, text, textLength(text));
    }

    inline size_t appendDecimal(char* out, size_t capacity, size_t length, uint64_t value) {
        char digits[24];
        size_t n = 0;
        do {
            digits[n++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value != 0 && n < sizeof(digits));
        char reversed[24];
        for (size_t i = 0; i < n; i++) reversed[i] = digits[n - 1 - i];
        return appendText(out, capacity, length, reversed, n);
    }

    inline size_t appendSigned(char* out, size_t capacity, size_t length, int64_t value) {
        if (value < 0) {
            length = appendText(out, capacity, length, "-", 1);
            return appendDecimal(out, capacity, length, static_cast<uint64_t>(-(value + 1)) + 1);
        }
        return appendDecimal(out, capacity, length, static_cast<uint64_t>(value));
    }

    // "0x" and the lowercase hex digits, no padding
    inline size_t appendHex(char* out, size_t capacity, size_t length, uint64_t value) {
        static const char kDigits[] = "0123456789abcdef";
        char digits[18];
        size_t n = 0;
        do {
            digits[n++] = kDigits[value & 0xf];
            value >>= 4;
        } while (value != 0 && n < 16);
        char text[20] = {'0', 'x'};
        for (size_t i = 0; i < n; i++) text[2 + i] = digits[n - 1 - i];
        return appendText(out, capacity, length, text, 2 + n);
    }

}

#endif // DIAG_DETAIL_CRASHSTATE_H
