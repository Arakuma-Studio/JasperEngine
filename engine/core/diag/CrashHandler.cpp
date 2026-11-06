// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The platform-independent half of the crash handler (CrashHandler.h): install-time
// preparation, the crash ring and its log sink, session markers, report folders, retention, the
// report hook and the deliberate crashes. Nothing here runs inside a signal handler; the
// handler-side code is CrashHandlerPosix.cpp, CrashHandlerWin.cpp and CrashHandlerWeb.cpp.

#include "CrashHandler.h"
#include "detail/CrashState.h"

#include "io/AtomicFile.h"
#include "log/LogSinks.h"
#include "log/Logger.h"
#include "thread/ThreadUtil.h"

#include "json.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>

#if defined(__EMSCRIPTEN__)
    #include <emscripten.h>
#elif defined(_WIN32)
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #include <windows.h>
#elif defined(__APPLE__)
    #include <TargetConditionals.h>
    #include <mach-o/dyld.h>
    #include <mach-o/loader.h>
    #include <csignal>
#else
    #include <link.h>
    #include <elf.h>
    #include <csignal>
#endif

using namespace jasperengine;
using namespace jasperengine::diag;
namespace fs = std::filesystem;

namespace je_crash_common {}
using namespace je_crash_common;

namespace jasperengine::diag::crash::detail {

    // Constant-initialized and never destroyed: a crash during exit still finds it
    CrashState crashState;

    CrashState& state() {
        return crashState;
    }

}

namespace je_crash_common {

    using crash::ReportInfo;
    using crash::detail::CrashState;

    // What install keeps that the handlers never read (normal locking is fine here)
    struct Common {
        std::mutex mutex;
        bool installed = false;
        fs::path root;
        fs::path reportsDir;
        fs::path sessionsDir;
        fs::path markerFile;
        std::string session;
        std::string product;
        std::string version;
        std::string buildId;
        bool abnormalPrevious = false;
        std::string previousSession;
        crash::ReportHook hook;
        std::set<std::string> delivered;
        bool ringSinkAdded = false;
    };

    Common& common() {
        // never destroyed: a report hook or a late reader at exit keeps working
        static Common* instance = new Common();
        return *instance;
    }

    // UTF-8 on every platform, which the Windows handler widens with CP_UTF8: path.string() is
    // the ANSI code page there before Windows 10 1903 (a user folder named "José" would get no
    // report). Cut between characters.
    void copyPath(char* out, size_t capacity, const fs::path& path) {
        const std::u8string utf8 = path.u8string();
        const std::string text(utf8.begin(), utf8.end());
        size_t n = std::min(text.size(), capacity - 1);
        while (n > 0 && n < text.size() && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80) n--;
        std::memcpy(out, text.data(), n);
        out[n] = '\0';
    }

    void copyText(char* out, size_t capacity, std::string_view text) {
        const size_t n = std::min(text.size(), capacity - 1);
        std::memcpy(out, text.data(), n);
        out[n] = '\0';
    }

    std::tm utcTime(std::time_t time) {
        std::tm out{};
#if defined(_WIN32)
        gmtime_s(&out, &time);
#else
        gmtime_r(&time, &out);
#endif
        return out;
    }

    // yyyymmddThhmmssZ
    std::string compactUtc(std::time_t time) {
        const std::tm tm = utcTime(time);
        char text[80];
        std::snprintf(text, sizeof(text), "%04d%02d%02dT%02d%02d%02dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                      tm.tm_hour, tm.tm_min, tm.tm_sec);
        return text;
    }

    // yyyy-mm-ddThh:mm:ssZ
    std::string isoUtc(std::time_t time) {
        const std::tm tm = utcTime(time);
        char text[80];
        std::snprintf(text, sizeof(text), "%04d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                      tm.tm_hour, tm.tm_min, tm.tm_sec);
        return text;
    }

    std::time_t fileTime(const fs::path& path) {
        std::error_code ec;
        const auto written = fs::last_write_time(path, ec);
        if (ec) return 0;
        const auto system = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            written - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
        return std::chrono::system_clock::to_time_t(system);
    }

    // The session folder name's time, 0 when it is not one
    std::time_t sessionTime(const std::string& id) {
        int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
        if (id.size() < 16 || std::sscanf(id.c_str(), "%4d%2d%2dT%2d%2d%2dZ", &year, &month, &day, &hour, &minute, &second) != 6) {
            return 0;
        }
        std::tm tm{};
        tm.tm_year = year - 1900;
        tm.tm_mon = month - 1;
        tm.tm_mday = day;
        tm.tm_hour = hour;
        tm.tm_min = minute;
        tm.tm_sec = second;
#if defined(_WIN32)
        return _mkgmtime(&tm);
#else
        return timegm(&tm);
#endif
    }

    std::string readFile(const fs::path& path, size_t limit = 1 << 20) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return {};
        std::string text;
        text.resize(limit);
        in.read(text.data(), static_cast<std::streamsize>(limit));
        text.resize(static_cast<size_t>(in.gcount()));
        return text;
    }

    // The value of "key: value" in a report's text (first match), empty if absent
    std::string reportValue(const std::string& text, const char* key) {
        const std::string prefix = std::string(key) + ": ";
        size_t at = 0;
        while (at < text.size()) {
            const size_t end = text.find('\n', at);
            const size_t lineEnd = end == std::string::npos ? text.size() : end;
            if (text.compare(at, prefix.size(), prefix) == 0) {
                std::string value = text.substr(at + prefix.size(), lineEnd - at - prefix.size());
                while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) value.pop_back();
                return value;
            }
            if (end == std::string::npos) break;
            at = end + 1;
        }
        return {};
    }

    std::string buildIdOfMainModule() {
#if defined(__EMSCRIPTEN__)
    #ifdef JASPERENGINE_BUILD_HASH
        return JASPERENGINE_BUILD_HASH;
    #else
        // the build's compile time, hashed: stable for one build, differs between builds
        const char* stamp = __DATE__ " " __TIME__;
        uint64_t hash = 1469598103934665603ull;
        for (const char* c = stamp; *c; c++) hash = (hash ^ static_cast<uint8_t>(*c)) * 1099511628211ull;
        char text[24];
        std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
        return text;
    #endif
#elif defined(_WIN32)
        // the CodeView RSDS record: the GUID and age a symbol server files the .pdb under
        const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
        if (!base) return {};
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return {};
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return {};
        const IMAGE_DATA_DIRECTORY& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
        if (directory.VirtualAddress == 0 || directory.Size == 0) return {};
        const auto* entries = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(base + directory.VirtualAddress);
        const size_t count = directory.Size / sizeof(IMAGE_DEBUG_DIRECTORY);
        for (size_t i = 0; i < count; i++) {
            if (entries[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW || entries[i].AddressOfRawData == 0) continue;
            const uint8_t* record = base + entries[i].AddressOfRawData;
            if (std::memcmp(record, "RSDS", 4) != 0) continue;
            GUID guid;
            DWORD age = 0;
            std::memcpy(&guid, record + 4, sizeof(guid));
            std::memcpy(&age, record + 4 + sizeof(guid), sizeof(age));
            char text[64];
            std::snprintf(text, sizeof(text), "%08lX%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X%lX",
                          static_cast<unsigned long>(guid.Data1), guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1],
                          guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7],
                          static_cast<unsigned long>(age));
            return text;
        }
        return {};
#elif defined(__APPLE__)
        // LC_UUID, what dwarfdump --uuid prints for the matching dSYM
        const mach_header* header = _dyld_get_image_header(0);
        if (!header) return {};
        const uint8_t* command = reinterpret_cast<const uint8_t*>(header) +
            (header->magic == MH_MAGIC_64 ? sizeof(mach_header_64) : sizeof(mach_header));
        for (uint32_t i = 0; i < header->ncmds; i++) {
            const auto* load = reinterpret_cast<const load_command*>(command);
            if (load->cmd == LC_UUID) {
                const auto* uuid = reinterpret_cast<const uuid_command*>(command);
                char text[48];
                const uint8_t* u = uuid->uuid;
                std::snprintf(text, sizeof(text), "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                              u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
                return text;
            }
            command += load->cmdsize;
        }
        return {};
#else
        // NT_GNU_BUILD_ID of the executable (the first object dl_iterate_phdr reports)
        std::string id;
        dl_iterate_phdr([](struct dl_phdr_info* info, size_t, void* data) -> int {
            std::string& out = *static_cast<std::string*>(data);
#if defined(__ANDROID__)
            // NativeActivity lives in the app's .so; app_process is only the OS launcher.
            // Its build ID cannot locate this game's symbols, so find the module owning us.
            const uintptr_t owner = reinterpret_cast<uintptr_t>(&buildIdOfMainModule);
            bool ownsCode = false;
            for (ElfW(Half) i = 0; i < info->dlpi_phnum; i++) {
                const auto& phdr = info->dlpi_phdr[i];
                const uintptr_t begin = info->dlpi_addr + phdr.p_vaddr;
                if (phdr.p_type == PT_LOAD && owner >= begin && owner - begin < phdr.p_memsz) ownsCode = true;
            }
            if (!ownsCode) return 0;
#endif
            for (ElfW(Half) i = 0; i < info->dlpi_phnum; i++) {
                const ElfW(Phdr)& header = info->dlpi_phdr[i];
                if (header.p_type != PT_NOTE) continue;
                const uint8_t* note = reinterpret_cast<const uint8_t*>(info->dlpi_addr + header.p_vaddr);
                const uint8_t* end = note + header.p_memsz;
                while (note + sizeof(ElfW(Nhdr)) <= end) {
                    const auto* nhdr = reinterpret_cast<const ElfW(Nhdr)*>(note);
                    const uint8_t* name = note + sizeof(ElfW(Nhdr));
                    const uint8_t* desc = name + ((nhdr->n_namesz + 3) & ~3u);
                    if (nhdr->n_type == NT_GNU_BUILD_ID && nhdr->n_namesz == 4 && std::memcmp(name, "GNU", 4) == 0) {
                        static const char kHex[] = "0123456789abcdef";
                        for (uint32_t b = 0; b < nhdr->n_descsz; b++) {
                            out += kHex[desc[b] >> 4];
                            out += kHex[desc[b] & 0xf];
                        }
                        return 1;
                    }
                    note = desc + ((nhdr->n_descsz + 3) & ~3u);
                }
            }
            // the executable comes first; a build without the note has none
            return 1;
        }, &id);
        return id;
#endif
    }

    const char* architectureName() {
#if defined(__x86_64__) || defined(_M_X64)
        return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
        return "arm64";
#elif defined(__arm__) || defined(_M_ARM)
        return "arm";
#elif defined(__i386__) || defined(_M_IX86)
        return "x86";
#elif defined(__wasm32__)
        return "wasm32";
#elif defined(__wasm64__)
        return "wasm64";
#else
        return "unknown";
#endif
    }

    // ---- the crash ring

    void ringWriteParts(const std::string_view* parts, size_t count) {
        CrashState& s = crash::detail::state();
        char* ring = s.ring;
        const size_t capacity = s.ringCapacity;
        if (!ring || capacity == 0) return;
        size_t total = 0;
        for (size_t i = 0; i < count; i++) total += parts[i].size();
        if (total == 0) return;
        // a line longer than the ring keeps its end
        size_t skip = total > capacity ? total - capacity : 0;
        const size_t length = total - skip;
        uint64_t position = s.ringCursor.fetch_add(length, std::memory_order_relaxed);
        for (size_t i = 0; i < count; i++) {
            const char* data = parts[i].data();
            size_t size = parts[i].size();
            if (skip >= size) {
                skip -= size;
                continue;
            }
            data += skip;
            size -= skip;
            skip = 0;
            while (size > 0) {
                const size_t at = static_cast<size_t>(position % capacity);
                const size_t n = std::min(size, capacity - at);
                std::memcpy(ring + at, data, n);
                position += n;
                data += n;
                size -= n;
            }
        }
    }

    // Every record the logger delivers, as "hh:mm:ss.mmmZ LEVEL [category] text" (UTC)
    class CrashRingSink final : public LogSink {
    public:
        void write(const LogRecord& record) override {
            char prefix[96];
            size_t n = 0;
            const int64_t ms = static_cast<int64_t>(record.wallTime * 1000.0);
            const int64_t dayMs = ((ms % 86400000) + 86400000) % 86400000;
            const int hour = static_cast<int>(dayMs / 3600000);
            const int minute = static_cast<int>(dayMs / 60000 % 60);
            const int second = static_cast<int>(dayMs / 1000 % 60);
            const int milli = static_cast<int>(dayMs % 1000);
            prefix[n++] = static_cast<char>('0' + hour / 10);
            prefix[n++] = static_cast<char>('0' + hour % 10);
            prefix[n++] = ':';
            prefix[n++] = static_cast<char>('0' + minute / 10);
            prefix[n++] = static_cast<char>('0' + minute % 10);
            prefix[n++] = ':';
            prefix[n++] = static_cast<char>('0' + second / 10);
            prefix[n++] = static_cast<char>('0' + second % 10);
            prefix[n++] = '.';
            prefix[n++] = static_cast<char>('0' + milli / 100);
            prefix[n++] = static_cast<char>('0' + milli / 10 % 10);
            prefix[n++] = static_cast<char>('0' + milli % 10);
            prefix[n++] = 'Z';
            prefix[n++] = ' ';
            const char* label = logFileLabel(record);
            for (const char* c = label; *c && n < 40; c++) prefix[n++] = *c;
            prefix[n++] = ' ';
            if (record.category && logShowsCategory(*record.category)) {
                prefix[n++] = '[';
                for (const char* c = record.category->name; *c && n < sizeof(prefix) - 3; c++) prefix[n++] = *c;
                prefix[n++] = ']';
                prefix[n++] = ' ';
            }
            const std::string_view parts[] = {std::string_view(prefix, n), record.text, std::string_view("\n", 1)};
            ringWriteParts(parts, 3);
        }

        Delivery delivery() const override { return Delivery::Inline; }
        const char* name() const override { return "crash-ring"; }
    };

    // ---- session markers and report folders

    std::string markerText(const Common& c, std::time_t started) {
        nlohmann::json marker = {
            {"pid", io::currentProcessId()},
            {"started", isoUtc(started)},
            {"version", c.version},
            {"product", c.product},
            {"session", c.session},
        };
        return marker.dump(2) + "\n";
    }

    std::string classify(const fs::path& folder) {
        std::error_code ec;
        if (fs::exists(folder / "crash.txt", ec)) return "crash";
        for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = it->path().filename().string();
            if (name.rfind("hang-", 0) == 0 && name.size() > 9 && name.compare(name.size() - 4, 4, ".txt") == 0) return "hang";
        }
        return "abnormal_exit";
    }

    // The sessions a previous run left behind: a marker whose process is gone ended without a
    // clean shutdown. Its folder says how (crash.txt, a hang report, or nothing at all: a kill,
    // the OOM killer, a power loss); the last case gets a folder too, with the marker in it, so
    // it is listed. Returns the newest such session.
    std::string sweepSessions(Common& c) {
        std::string newest;
        std::error_code ec;
        const int64_t self = io::currentProcessId();
        for (fs::directory_iterator it(c.sessionsDir, ec), end; !ec && it != end; it.increment(ec)) {
            const fs::path file = it->path();
            if (file.extension() != ".json") continue;
            nlohmann::json marker = nlohmann::json::parse(readFile(file, 64 << 10), nullptr, false);
            if (!marker.is_object()) {
                std::error_code removeError;
                fs::remove(file, removeError);
                continue;
            }
            const int64_t pid = marker.value("pid", int64_t(0));
            const std::string session = marker.value("session", std::string());
            // another instance sharing this root is still running (a pid reused by us is not)
            if (pid != self && io::isProcessRunning(pid)) continue;
            if (!session.empty() && session.find_first_of("/\\.") == std::string::npos) {
                const fs::path folder = c.reportsDir / session;
                std::error_code folderError;
                fs::create_directories(folder, folderError);
                if (classify(folder) == "abnormal_exit") {
                    io::AtomicWriteOptions options;
                    options.durability = io::Durability::Cache;
                    io::writeFileAtomic(folder / "session.json", marker.dump(2) + "\n", options);
                }
                if (newest.empty() || session > newest) newest = session;
            }
            std::error_code removeError;
            fs::remove(file, removeError);
        }
        return newest;
    }

    uint64_t folderBytes(const fs::path& folder) {
        uint64_t total = 0;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code sizeError;
            if (it->is_regular_file(sizeError)) {
                const uint64_t size = it->file_size(sizeError);
                if (!sizeError) total += size;
            }
        }
        return total;
    }

    // Offers every pending report not offered yet to the hook. Called without the common lock:
    // the hook and the report readers take it themselves.
    void deliverPending(Common& c) {
        crash::ReportHook hook;
        {
            std::lock_guard<std::mutex> lock(c.mutex);
            hook = c.hook;
        }
        if (!hook) return;
        // The public report APIs lock Common too, and a hook can itself call those APIs.
        // Keep the lock only around the once-per-session delivery bookkeeping.
        for (const ReportInfo& report : crash::pendingReports()) {
            {
                std::lock_guard<std::mutex> lock(c.mutex);
                if (!c.delivered.insert(report.id).second) continue;
            }
            bool handled = false;
            try {
                handled = hook(report);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "crash report hook: %s\n", e.what());
            } catch (...) {
                std::fprintf(stderr, "crash report hook: unknown exception\n");
            }
            if (handled) crash::markHandled(report.id);
        }
    }

    // ---- std::terminate: name the exception, then the SIGABRT path reports

    std::terminate_handler previousTerminate = nullptr;

    void onTerminate() {
        CrashState& s = crash::detail::state();
        char reason[sizeof(s.pendingReason)];
        std::snprintf(reason, sizeof(reason), "std::terminate with no active exception");
        // Rethrowing is what names the exception. A throw that finds no handler calls terminate
        // before unwinding, so the abort's backtrace still reaches the throw site.
        if (std::exception_ptr active = std::current_exception()) {
            try {
                std::rethrow_exception(active);
            } catch (const std::exception& e) {
                std::snprintf(reason, sizeof(reason), "unhandled exception: %s", e.what());
            } catch (...) {
                std::snprintf(reason, sizeof(reason), "unhandled exception of unknown type");
            }
        }
        if (!s.pendingReasonSet.exchange(true)) {
            copyText(s.pendingReason, sizeof(s.pendingReason), reason);
        }
        // a handler installed before ours (the editor's) still writes its part, then aborts
        if (previousTerminate && previousTerminate != onTerminate) previousTerminate();
        std::abort();
    }

    // ---- deliberate crashes

    std::atomic<bool> keepRecursing{true};

    #if defined(_MSC_VER)
        #define JE_CRASH_NOINLINE __declspec(noinline)
    #else
        #define JE_CRASH_NOINLINE __attribute__((noinline))
    #endif

    JE_CRASH_NOINLINE void debugSegv() {
#if defined(JASPERENGINE_SANITIZE_UNDEFINED) && !defined(_WIN32)
        // UBSan stops at a null store before the handler sees the fault: the same signal, raised
        std::raise(SIGSEGV);
#else
        volatile int* nothing = nullptr;
        *nothing = 1;
#endif
    }

    JE_CRASH_NOINLINE uint64_t debugRecurse(uint64_t depth) {
        volatile char pad[2048];
        pad[0] = static_cast<char>(depth);
        pad[sizeof(pad) - 1] = static_cast<char>(depth >> 8);
        if (keepRecursing.load(std::memory_order_relaxed)) {
            return debugRecurse(depth + 1) + static_cast<uint64_t>(pad[depth % sizeof(pad)]);
        }
        return pad[0];
    }

    JE_CRASH_NOINLINE void debugFpe() {
#if defined(JASPERENGINE_SANITIZE_UNDEFINED) && !defined(_WIN32)
        std::raise(SIGFPE);
#else
        volatile int zero = 0;
        volatile int value = 1 / zero;
        (void)value;
    #if !defined(_WIN32)
        // integer division does not trap on every CPU (arm64 returns 0)
        std::raise(SIGFPE);
    #endif
#endif
    }

    // A call of a pure virtual function: the base's destructor runs while the object is a Base
    struct PureBase {
        virtual ~PureBase() { callIt(); }
        virtual void pure() = 0;
        JE_CRASH_NOINLINE void callIt() { pure(); }
    };

    struct PureDerived final : PureBase {
        void pure() override {}
    };

}

namespace jasperengine::diag::crash {

    Config editorConfig(const fs::path& root, const std::string& version) {
        Config config;
        config.product = "JasperEngine Editor";
        config.root = root;
        config.ringBytes = 256 << 10;
        config.dumpKind = DumpKind::Editor;
        config.version = version;
        config.keepReports = 20;
        config.keepBytes = 2ull << 30;
        config.keepUnhandledDays = 7;
        return config;
    }

    bool install(const Config& input) {
        Common& c = common();
        std::unique_lock<std::mutex> lock(c.mutex);
        CrashState& s = detail::state();
        if (c.installed) return false;
        c.installed = true;

        Config config = input;
        if (const char* override = std::getenv("JASPERENGINE_CRASH_REPORT_DIR"); override && override[0] != '\0') {
            config.root = fs::path(override);
        }
        // absolute: the report path in crash.txt and the stderr line must mean the same thing to
        // whoever reads them, whatever their working directory (a game's user data path can be
        // relative to its own)
        if (!config.root.empty() && config.root.is_relative()) {
            std::error_code absoluteError;
            const fs::path absolute = fs::absolute(config.root, absoluteError);
            if (!absoluteError) config.root = absolute.lexically_normal();
        }

        const std::time_t started = std::time(nullptr);
        c.session = compactUtc(started) + "-" + std::to_string(io::currentProcessId());
        c.product = config.product ? config.product : "JasperEngine";
        c.version = config.version;
        c.buildId = buildIdOfMainModule();
        s.startMonotonicNs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        s.includeLogTail = config.includeLogTail;
        s.dumpKind = config.dumpKind;
        copyText(s.sessionName, sizeof(s.sessionName), c.session);

        if (!config.root.empty()) {
            std::error_code ec;
            c.root = config.root;
            c.reportsDir = config.root / "reports";
            c.sessionsDir = config.root / "sessions";
            fs::create_directories(c.reportsDir, ec);
            fs::create_directories(c.sessionsDir, ec);
            const fs::path session = c.reportsDir / c.session;
            copyPath(s.reportsDir, sizeof(s.reportsDir), c.reportsDir);
            copyPath(s.sessionDir, sizeof(s.sessionDir), session);
            copyPath(s.crashFile, sizeof(s.crashFile), session / "crash.txt");
            copyPath(s.dumpFile, sizeof(s.dumpFile), session / "crash.dmp");
            copyPath(s.tailFile, sizeof(s.tailFile), session / "log-tail.txt");
        }
        if (!config.logFile.empty()) {
            copyPath(s.logFile, sizeof(s.logFile), config.logFile);
        }

        std::string header = "JasperEngine crash report v1\n";
        header += "product: " + c.product + "\n";
        header += "version: " + (c.version.empty() ? std::string("unknown") : c.version) + "\n";
        header += "build_id: " + (c.buildId.empty() ? std::string("none") : c.buildId) + "\n";
        header += std::string("platform: ") + platformName() + "\n";
        header += "backend: " + (config.backend.empty() ? std::string("none") : config.backend) + "\n";
        header += "session: " + c.session + "\n";
        copyText(s.header, sizeof(s.header), header);
        s.headerLength = detail::textLength(s.header);
        copyText(s.banner, sizeof(s.banner), c.product + (c.version.empty() ? std::string() : " " + c.version));

        if (config.ringBytes > 0) {
            // never freed: the handler reads it until the process is gone
            s.ring = new char[config.ringBytes];
            std::memset(s.ring, 0, config.ringBytes);
            s.ringCapacity = config.ringBytes;
        }

        if (!c.root.empty()) {
            c.previousSession = sweepSessions(c);
            c.abnormalPrevious = !c.previousSession.empty();
            pruneReports(c.reportsDir, config.keepReports, config.keepBytes, config.keepUnhandledDays);
            c.markerFile = c.sessionsDir / (std::to_string(io::currentProcessId()) + ".json");
            io::AtomicWriteOptions options;
            options.durability = io::Durability::Cache;
            io::writeFileAtomic(c.markerFile, markerText(c, started), options);
        }

        const bool armed = detail::installPlatform(config);
        // only with a backend to report the abort that follows; otherwise the process's own
        // terminate handler stays (chained to by onTerminate when there is one)
        if (armed) previousTerminate = std::set_terminate(onTerminate);
        s.installed.store(true, std::memory_order_release);

        if (!c.ringSinkAdded && s.ring) {
            c.ringSinkAdded = Logger::addSink(std::make_unique<CrashRingSink>()) != 0;
        }
        lock.unlock();
        deliverPending(c);
        return armed;
    }

    bool isInstalled() {
        return detail::state().installed.load(std::memory_order_acquire);
    }

    void shutdown() {
        Common& c = common();
        std::lock_guard<std::mutex> lock(c.mutex);
        if (c.markerFile.empty()) return;
        std::error_code ec;
        fs::remove(c.markerFile, ec);
        c.markerFile.clear();
    }

    void prepareThread() {
        if (!isInstalled()) return;
        detail::prepareThreadPlatform();
    }

    void ringWrite(std::string_view line) {
        if (line.empty()) return;
        if (line.back() == '\n') {
            ringWriteParts(&line, 1);
            return;
        }
        const std::string_view parts[] = {line, std::string_view("\n", 1)};
        ringWriteParts(parts, 2);
    }

    std::string ringContents() {
        const CrashState& s = detail::state();
        if (!s.ring || s.ringCapacity == 0) return {};
        const uint64_t cursor = s.ringCursor.load(std::memory_order_acquire);
        if (cursor <= s.ringCapacity) return std::string(s.ring, static_cast<size_t>(cursor));
        const size_t start = static_cast<size_t>(cursor % s.ringCapacity);
        std::string out(s.ring + start, s.ringCapacity - start);
        out.append(s.ring, start);
        // the oldest line was overwritten in part: start at the next whole one
        const size_t firstLine = out.find('\n');
        return firstLine == std::string::npos ? out : out.substr(firstLine + 1);
    }

    void setTag(std::string_view key, std::string_view value) {
        if (key.empty()) return;
        CrashState& s = detail::state();
        detail::Tag* slot = nullptr;
        for (detail::Tag& tag : s.tags) {
            if (tag.key[0] != '\0' && key == std::string_view(tag.key)) {
                slot = &tag;
                break;
            }
        }
        if (!slot && value.empty()) return;
        if (!slot) {
            for (detail::Tag& tag : s.tags) {
                if (tag.key[0] == '\0') {
                    slot = &tag;
                    break;
                }
            }
        }
        if (!slot) return;
        const uint32_t seq = slot->seq.load(std::memory_order_relaxed);
        slot->seq.store(seq + 1, std::memory_order_release);
        if (value.empty()) {
            slot->key[0] = '\0';
            slot->value[0] = '\0';
        } else {
            // one line each: a newline would forge a key of crash.txt
            char cleanKey[detail::kTagKeyBytes] = {0};
            char cleanValue[detail::kTagValueBytes] = {0};
            copyText(cleanKey, sizeof(cleanKey), key);
            copyText(cleanValue, sizeof(cleanValue), value);
            for (char& ch : cleanKey) {
                if (ch == '\n' || ch == '\r' || ch == ':' || ch == ' ') ch = '_';
            }
            for (char& ch : cleanValue) {
                if (ch == '\n' || ch == '\r') ch = ' ';
            }
            std::memcpy(slot->key, cleanKey, sizeof(cleanKey));
            std::memcpy(slot->value, cleanValue, sizeof(cleanValue));
        }
        slot->seq.store(seq + 2, std::memory_order_release);
    }

    fs::path reportsDirectory() {
        Common& c = common();
        std::lock_guard<std::mutex> lock(c.mutex);
        return c.reportsDir;
    }

    fs::path sessionFolder() {
        Common& c = common();
        std::lock_guard<std::mutex> lock(c.mutex);
        return c.reportsDir.empty() ? fs::path() : c.reportsDir / c.session;
    }

    const std::string& sessionId() {
        return common().session;
    }

    std::optional<ReportInfo> readReport(const fs::path& folder) {
        std::error_code ec;
        if (!fs::is_directory(folder, ec)) return std::nullopt;
        ReportInfo info;
        info.id = folder.filename().string();
        info.folder = folder;
        info.kind = classify(folder);
        info.handled = fs::exists(folder / "handled", ec);
        for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code fileError;
            if (it->is_regular_file(fileError)) info.files.push_back(it->path());
        }
        std::sort(info.files.begin(), info.files.end());
        std::time_t when = 0;
        if (info.kind == "crash") {
            const std::string text = readFile(folder / "crash.txt", 64 << 10);
            info.reason = reportValue(text, "reason");
            info.product = reportValue(text, "product");
            info.version = reportValue(text, "version");
            when = fileTime(folder / "crash.txt");
        } else if (info.kind == "hang") {
            fs::path last;
            for (const fs::path& file : info.files) {
                const std::string name = file.filename().string();
                if (name.rfind("hang-", 0) == 0 && file.extension() == ".txt") last = file;
            }
            const std::string text = readFile(last, 64 << 10);
            const std::string phase = reportValue(text, "phase");
            const std::string scopes = reportValue(text, "busy_scopes");
            info.reason = "not responding (" + (scopes.empty() || scopes == "none" ? phase : scopes) + ")";
            info.product = reportValue(text, "product");
            info.version = reportValue(text, "version");
            when = fileTime(last);
        } else {
            nlohmann::json marker = nlohmann::json::parse(readFile(folder / "session.json", 64 << 10), nullptr, false);
            if (marker.is_object()) {
                info.product = marker.value("product", std::string());
                info.version = marker.value("version", std::string());
            }
            info.reason = "ended without a report (killed, out of memory or power loss)";
            when = sessionTime(info.id);
        }
        if (when == 0) when = sessionTime(info.id);
        info.timeUnix = static_cast<int64_t>(when);
        info.time = isoUtc(when);
        return info;
    }

    std::vector<ReportInfo> listReports(size_t limit) {
        const fs::path dir = reportsDirectory();
        std::vector<std::string> names;
        std::error_code ec;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code typeError;
            if (it->is_directory(typeError) && sessionTime(it->path().filename().string()) != 0) {
                names.push_back(it->path().filename().string());
            }
        }
        std::sort(names.rbegin(), names.rend());
        std::vector<ReportInfo> out;
        for (const std::string& name : names) {
            if (limit != 0 && out.size() >= limit) break;
            if (std::optional<ReportInfo> info = readReport(dir / name)) out.push_back(std::move(*info));
        }
        return out;
    }

    std::vector<ReportInfo> pendingReports() {
        const std::string current = sessionId();
        std::vector<ReportInfo> out;
        for (ReportInfo& info : listReports()) {
            if (!info.handled && info.id != current) out.push_back(std::move(info));
        }
        return out;
    }

    std::optional<ReportInfo> findReport(std::string_view id) {
        if (id.empty() || id.find_first_of("/\\") != std::string_view::npos || id == "." || id == "..") return std::nullopt;
        const fs::path dir = reportsDirectory();
        if (dir.empty()) return std::nullopt;
        return readReport(dir / std::string(id));
    }

    bool markHandled(std::string_view id) {
        std::optional<ReportInfo> info = findReport(id);
        if (!info) return false;
        if (info->handled) return true;
        io::AtomicWriteOptions options;
        options.durability = io::Durability::Cache;
        return io::writeFileAtomic(info->folder / "handled", std::string_view(), options).ok();
    }

    bool lastSessionEndedAbnormally() {
        Common& c = common();
        std::lock_guard<std::mutex> lock(c.mutex);
        return c.abnormalPrevious;
    }

    std::optional<ReportInfo> lastSessionReport() {
        std::string session;
        {
            Common& c = common();
            std::lock_guard<std::mutex> lock(c.mutex);
            session = c.previousSession;
        }
        if (session.empty()) return std::nullopt;
        std::optional<ReportInfo> info = findReport(session);
        if (!info || info->handled) return std::nullopt;
        return info;
    }

    void setReportHook(ReportHook hook) {
        Common& c = common();
        bool installed = false;
        {
            std::lock_guard<std::mutex> lock(c.mutex);
            c.hook = std::move(hook);
            installed = c.installed;
        }
        if (installed) deliverPending(c);
    }

    bool isDebugCrashMode(std::string_view mode) {
        static const char* const kModes[] = {"segv", "abort", "terminate", "stack_overflow", "worker_segv", "fpe", "pure_call", "hang"};
        for (const char* known : kModes) {
            if (mode == known) return true;
        }
        return false;
    }

    bool debugCrash(std::string_view mode, uint32_t hangMs) {
        if (!isDebugCrashMode(mode)) return false;
        detail::state().deliberate.store(true, std::memory_order_relaxed);
        Logger::write(mode == "hang" ? LogLevel::Warn : LogLevel::Error, logCategoryEngine(),
                      "debug crash: " + std::string(mode) + (mode == "hang" ? " for " + std::to_string(hangMs) + " ms" : std::string()));
        if (mode == "hang") {
            detail::state().deliberate.store(false, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(hangMs));
            return true;
        }
        if (detail::platformDebugCrash(mode)) return true;
        if (mode == "segv") {
            debugSegv();
        } else if (mode == "abort") {
            std::abort();
        } else if (mode == "terminate") {
            try {
                throw std::runtime_error("debug crash (terminate)");
            } catch (...) {
                std::terminate();
            }
        } else if (mode == "stack_overflow") {
            debugRecurse(0);
        } else if (mode == "worker_segv") {
            std::thread worker = thread::spawnNamed("je-debug-crash", []() { debugSegv(); });
            worker.join();
        } else if (mode == "fpe") {
            debugFpe();
        } else if (mode == "pure_call") {
            // the pure virtual's handler terminates the process from the base's destructor
            PureBase* object = new PureDerived();
            delete object;
        }
        // not reached unless the platform ignored the fault
        std::abort();
    }

    std::optional<DebugCrashRequest> debugCrashRequest() {
        std::string spec;
#if defined(__EMSCRIPTEN__)
        // a static buffer of the runtime: copied at once
        const char* query = emscripten_run_script_string(
            "(function(){try{return new URLSearchParams(location.search).get('jasperengine_debug_crash')||''}"
            "catch(e){return ''}})()");
        if (query) spec = query;
#else
        if (const char* env = std::getenv("JASPERENGINE_DEBUG_CRASH")) spec = env;
#endif
        if (spec.empty()) return std::nullopt;
        DebugCrashRequest request;
        const size_t at = spec.find('@');
        request.mode = spec.substr(0, at);
        // 0: once every scene loaded (Engine::systemDraw)
        request.frame = 0;
        if (at != std::string::npos) {
            request.frame = std::strtoull(spec.c_str() + at + 1, nullptr, 10);
        }
        if (!isDebugCrashMode(request.mode)) {
            std::fprintf(stderr, "JASPERENGINE_DEBUG_CRASH: unknown mode '%s'\n", request.mode.c_str());
            return std::nullopt;
        }
        return request;
    }

    fs::path writeSessionFile(const std::string& name, std::string_view text) {
        const fs::path folder = sessionFolder();
        if (folder.empty()) return {};
        io::AtomicWriteOptions options;
        options.durability = io::Durability::Cache;
        const fs::path file = folder / name;
        return io::writeFileAtomic(file, text, options).ok() ? file : fs::path();
    }

    const std::string& buildId() {
        return common().buildId;
    }

    const char* platformName() {
        static const std::string name = []() {
#if defined(__EMSCRIPTEN__)
            const char* os = "web";
#elif defined(_WIN32)
            const char* os = "windows";
#elif defined(__ANDROID__)
            const char* os = "android";
#elif defined(__APPLE__)
    #if TARGET_OS_IPHONE
            const char* os = "ios";
    #else
            const char* os = "macos";
    #endif
#elif defined(__linux__)
            const char* os = "linux";
#else
            const char* os = "unknown";
#endif
            return std::string(os) + "-" + architectureName();
        }();
        return name.c_str();
    }

    void pruneReports(const fs::path& reportsDir, size_t keep, uint64_t maxBytes, int keepUnhandledDays) {
        struct Folder {
            fs::path path;
            std::string name;
            uint64_t bytes;
            bool handled;
            std::time_t time;
        };
        std::vector<Folder> folders;
        std::error_code ec;
        const std::string current = common().session;
        for (fs::directory_iterator it(reportsDir, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code typeError;
            const std::string name = it->path().filename().string();
            if (!it->is_directory(typeError) || sessionTime(name) == 0) continue;
            std::error_code existsError;
            folders.push_back({it->path(), name, folderBytes(it->path()), fs::exists(it->path() / "handled", existsError), sessionTime(name)});
        }
        // newest first: the names start with their UTC time
        std::sort(folders.begin(), folders.end(), [](const Folder& a, const Folder& b) { return a.name > b.name; });
        const std::time_t now = std::time(nullptr);
        uint64_t total = 0;
        for (size_t i = 0; i < folders.size(); i++) {
            const Folder& folder = folders[i];
            if (folder.name == current) {
                total += folder.bytes;
                continue;
            }
            bool remove = i >= keep;
            if (!remove && total + folder.bytes > maxBytes) {
                const bool recentUnhandled = keepUnhandledDays > 0 && !folder.handled &&
                    now - folder.time < static_cast<std::time_t>(keepUnhandledDays) * 86400;
                remove = !recentUnhandled;
            }
            if (remove) {
                std::error_code removeError;
                fs::remove_all(folder.path, removeError);
            } else {
                total += folder.bytes;
            }
        }
    }

}
